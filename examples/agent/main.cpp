// examples/agent/main.cpp
// 智能体示例:Perceive -> Think(mock LLM) -> Act 闭环,WorldState 做环境/记忆。
// 闭环:Move/Gather 的完成反馈到 Perceive;Stop 为终端汇聚(无反馈边)自然终止。
//
// === 机制(与 spec §5 一致,落地为 condition 语义选路 + 通用 NodeState 返回) ===
//   - Perceive 的"初始化 vs 重感知" = 靠 sub 边的 condition 角色分流(不靠事件字段):
//       condition=seed 的边(goal->perc)收触发 -> 初始化 WorldState;
//       无 condition 的反馈边(move->perc / gather->perc)收触发 -> 只重发感知。
//     Perceive 不认识 goal/move/gather 这些节点名,只认边的 seed/feedback 角色。
//   - Think 的"3 路决策" = 持 N 条 pub 边(各配稳定 condition gather/move/stop),
//     按状态谓词只向 condition 匹配的出边发,不广播到未选分支。
//   - Move/Gather 的"副作用" = 各自改自己的 WorldState 字段(position / nearbyItems),
//     这是动作的固有职责(换动作节点 = 换另一种副作用,预期可替换),非逻辑耦合。
//   - Stop 的"终止" = 拓扑保证:JSON 不含 stop->perc 反馈边,Stop 发完事件无下游,
//     链自然终止;不靠计数/标志/phase 抑制(与 multi sc5 的 D 终端汇聚同构)。
//
// === 与 spec §5 的差异:事件无 phase 字段,改通用 NodeState ===
//   spec §5.3 原本用 4 个事件类型(GoalEvent/PerceptionEvent/DecisionEvent/ActionDoneEvent),
//   靠 subscribe<EventType> 类型分派。后改为单一 AgentStateEvent + phase 字段。
//   本版再删 phase:节点执行完发通用 AgentStateEvent(NodeState + payload),
//   路由靠 condition 选路 + 边的独占性,分流靠 sub 边 condition 角色,
//   节点不硬编码任何阶段字符串——换节点组合只改 JSON,节点代码零改。
//
// 拓扑(所有 edge 统一为 AgentStateEvent,决策/seed 出边各配 condition):
//   goal  --(condition=seed)-----> perc     [首启边,触发初始化]
//   perc  --(AgentStateEvent)-----> think    [发感知快照]
//   think --(condition=gather)---> gather   [决策:有物品]
//   think --(condition=move)-----> move     [决策:未到目标]
//   think --(condition=stop)-----> stop     [决策:到目标,终端]
//   move  --(AgentStateEvent)-----> perc     [反馈:重感知]
//   gather--(AgentStateEvent)-----> perc     [反馈:重感知]
//   stop  [无反馈边,终端汇聚]
//
// 默认参数 startPos=0,target=3,items=2 的轨迹:
//   seed->perc(0,2,F) -> think gather -> perc(0,1,T) -> think gather -> perc(0,0,F)
//   -> think move -> perc(1,0,F) -> Move -> perc(2,0,F) -> Move -> perc(3,0,T)
//   -> think stop (终端,链终止)
//   即 2 Gather + 3 Move + 1 Stop。
#include <cstddef>
#include <functional>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <vector>
#include "engine/graph_engine.h"
#include "core/event_bus.h"
#include "core/graph_node.h"

using graphflow::core::Event;
using graphflow::core::EventT;
using graphflow::core::EventBus;
using graphflow::core::GraphNode;
using graphflow::engine::GraphEngine;
using graphflow::engine::NodeConfig;
using graphflow::engine::NodeCreator;
using json = nlohmann::json;

// === 通用节点状态(行为树/智能体正统三态) ===
enum class NodeState { Success, Failure, Running };

// === 统一事件类型 ===
// 无 phase 字段:节点执行完发 AgentStateEvent 带 state + payload(感知快照/目标)。
// 路由靠 condition 选路 + 边的独占性,分流靠 sub 边 condition 角色;节点不认识具体节点名。
struct AgentStateEvent : EventT<AgentStateEvent> {
    NodeState state = NodeState::Success;
    // 感知快照(Perceive 发的)
    int position = 0;
    int nearbyItems = 0;
    bool atTarget = false;
    // Goal 携带
    std::string goal;
    int target = 0;

    AgentStateEvent() = default;
};

// === 共享环境状态(经 Creator 注入) ===
struct WorldState {
    int position = 0;
    int target = 0;
    int nearbyItems = 0;
    bool atTarget = false;
};

// 执行轨迹打印:统一前缀 "  >> [节点名] 动作",便于观察事件流转。
static void trace(const std::string& node, const std::string& msg) {
    std::cout << "  >> [" << node << "] " << msg << "\n";
}

// === 节点 ===
// GoalSeedNode: doStart 种子式发 AgentStateEvent 启动。带 goal,无 phase。
struct GoalSeedNode : GraphNode {
    std::string m_goal;
    GoalSeedNode(const std::string& n, EventBus* eb, std::string g)
        : GraphNode(n, eb), m_goal(std::move(g)) {}
    bool doInit() override { return true; }
    bool doStart() override {
        trace(getName(), "seed -> start chain");
        for (const auto& info : getPubEdges()) {
            AgentStateEvent e;
            e.state = NodeState::Success;
            e.goal = m_goal;
            // 不显式指定模板参数:让转发引用从 e 推导为左值引用(见 event_bus.h publish 注释)。
            info.eventBus()->publish(info.name(), e);
        }
        return true;
    }
};

// PerceiveNode: 持 2 类 sub 边,靠 condition 角色分流(不靠事件字段):
//   condition=seed 的边(goal->perc) -> 初始化 WorldState(startPos/target/items) + 发感知;
//   无 condition 的反馈边(move->perc/gather->perc) -> 只重发感知(状态已由各 Action 改好)。
//   只读 WorldState 初始化,不改(改动由各 Action 负责自己的字段)。
struct PerceiveNode : GraphNode {
    std::shared_ptr<WorldState> m_state;
    int m_startPos = 0;
    int m_target = 0;
    int m_items = 0;
    PerceiveNode(const std::string& n, EventBus* eb, std::shared_ptr<WorldState> s)
        : GraphNode(n, eb), m_state(std::move(s)) {}
    bool doInit() override {
        for (const auto& info : getSubEdges()) {
            // 按 condition 角色(拓扑层声明)分流:seed=首启初始化,其他=反馈重感知。
            // 不读事件内容分流,故节点不依赖具体节点名。
            if (info.condition() == "seed") {
                info.eventBus()->subscribe<AgentStateEvent>(info.name(),
                    [this](AgentStateEvent&) { onSeed(); });
            } else {
                info.eventBus()->subscribe<AgentStateEvent>(info.name(),
                    [this](AgentStateEvent&) { emitPerception(); });
            }
        }
        return true;
    }
    bool doStart() override { return true; }
    void onSeed() {
        // 初始化环境(仅首启边触发一次)。
        m_state->position = m_startPos;
        m_state->target = m_target;
        m_state->nearbyItems = m_items;
        m_state->atTarget = (m_startPos == m_target);
        trace(getName(), "seed init pos=" + std::to_string(m_state->position) +
              " items=" + std::to_string(m_state->nearbyItems) +
              " target=" + std::to_string(m_state->target));
        emitPerception();
    }
    void emitPerception() {
        trace(getName(), "perceive pos=" + std::to_string(m_state->position) +
              " items=" + std::to_string(m_state->nearbyItems) +
              " atTarget=" + std::string(m_state->atTarget ? "T" : "F"));
        for (const auto& p : getPubEdges()) {
            AgentStateEvent e;
            e.state = NodeState::Success;
            e.position = m_state->position;
            e.nearbyItems = m_state->nearbyItems;
            e.atTarget = m_state->atTarget;
            p.eventBus()->publish(p.name(), e);
        }
    }
};

// ThinkNode: mock LLM。收感知(来自 perc->think),按规则计算 chosenRoute(gather/move/stop),
//   只向 condition 匹配的出边发布(通用 AgentStateEvent,无 phase)。
//   nearbyItems>0   -> condition="gather"(优先采集)
//   nearbyItems==0 && !atTarget -> condition="move"
//   nearbyItems==0 && atTarget  -> condition="stop"(终端)
//   不读取/比较 edge.to(),不广播到未选分支。
struct ThinkNode : GraphNode {
    std::shared_ptr<WorldState> m_state;
    int m_perceived = 0;
    explicit ThinkNode(const std::string& n, EventBus* eb, std::shared_ptr<WorldState> s)
        : GraphNode(n, eb), m_state(std::move(s)) {}
    bool doInit() override {
        for (const auto& info : getSubEdges()) {
            // think 的 sub 边只有 perc->think 一条,收到即感知触发(不靠 phase 校验)。
            info.eventBus()->subscribe<AgentStateEvent>(info.name(),
                [this](AgentStateEvent&) { onPerception(); });
        }
        return true;
    }
    bool doStart() override { return true; }
    void onPerception() {
        // condition 语义选路:chosenRoute 与拓扑声明的 condition 标签对齐。
        std::string chosenRoute;
        if (m_state->nearbyItems > 0) {
            chosenRoute = "gather";
        } else if (!m_state->atTarget) {
            chosenRoute = "move";
        } else {
            chosenRoute = "stop";
        }
        trace(getName(), "recv perceive; items=" + std::to_string(m_state->nearbyItems) +
              " atTarget=" + std::string(m_state->atTarget ? "T" : "F") +
              " -> select " + chosenRoute);
        ++m_perceived;
        AgentStateEvent e;
        e.state = NodeState::Success;
        e.target = m_state->target;
        // 遍历出边,只向 condition 匹配的边发布;循环不 break 以支持同 condition 多播。
        // 未命中 condition 的边不会收到事件(不广播到未选分支)。
        for (const auto& p : getPubEdges()) {
            if (p.condition() == chosenRoute) {
                p.eventBus()->publish(p.name(), e);
            }
        }
    }
};

// Action 节点: 各自改自己的 WorldState 字段(动作固有副作用),发通用 AgentStateEvent 回 perc。
//   对触发无感(不看 phase/state):sub 边由 condition 独占(think->move 的 condition=move 边
//   只有 think 发、只有 move 订阅),收到必是 think 路由给它,不需校验。
struct MoveActionNode : GraphNode {
    std::shared_ptr<WorldState> m_state;
    int m_received = 0;
    MoveActionNode(const std::string& n, EventBus* eb, std::shared_ptr<WorldState> s)
        : GraphNode(n, eb), m_state(std::move(s)) {}
    bool doInit() override {
        for (const auto& info : getSubEdges()) {
            info.eventBus()->subscribe<AgentStateEvent>(info.name(),
                [this](AgentStateEvent&) {
                    ++m_received;
                    ++m_state->position;
                    m_state->atTarget = (m_state->position == m_state->target);
                    trace(getName(), "exec move; pos=" + std::to_string(m_state->position) +
                          " -> emit Success");
                    for (const auto& p : getPubEdges()) {
                        AgentStateEvent d;
                        d.state = NodeState::Success;
                        p.eventBus()->publish(p.name(), d);
                    }
                });
        }
        return true;
    }
    bool doStart() override { return true; }
};
struct GatherActionNode : GraphNode {
    std::shared_ptr<WorldState> m_state;
    int m_received = 0;
    GatherActionNode(const std::string& n, EventBus* eb, std::shared_ptr<WorldState> s)
        : GraphNode(n, eb), m_state(std::move(s)) {}
    bool doInit() override {
        for (const auto& info : getSubEdges()) {
            info.eventBus()->subscribe<AgentStateEvent>(info.name(),
                [this](AgentStateEvent&) {
                    ++m_received;
                    if (m_state->nearbyItems > 0) --m_state->nearbyItems;
                    trace(getName(), "exec gather; items=" + std::to_string(m_state->nearbyItems) +
                          " -> emit Success");
                    for (const auto& p : getPubEdges()) {
                        AgentStateEvent d;
                        d.state = NodeState::Success;
                        p.eventBus()->publish(p.name(), d);
                    }
                });
        }
        return true;
    }
    bool doStart() override { return true; }
};
struct StopActionNode : GraphNode {
    int m_received = 0;
    explicit StopActionNode(const std::string& n, EventBus* eb) : GraphNode(n, eb) {}
    bool doInit() override {
        for (const auto& info : getSubEdges()) {
            info.eventBus()->subscribe<AgentStateEvent>(info.name(),
                [this](AgentStateEvent&) {
                    ++m_received;
                    trace(getName(), "exec stop (terminal)");
                    // 终端汇聚:无反馈边,链路在此自然终止。
                });
        }
        return true;
    }
    bool doStart() override { return true; }
};

// === Creators ===
class GoalSeedCreator : public NodeCreator {
public:
    std::shared_ptr<GraphNode> createNode(const NodeConfig& cfg) override {
        std::string g = cfg.params.value("goal", std::string(""));
        return std::make_shared<GoalSeedNode>(cfg.nodeName, cfg.eventBus, g);
    }
};
class PerceiveCreator : public NodeCreator {
public:
    std::shared_ptr<WorldState> state;
    explicit PerceiveCreator(std::shared_ptr<WorldState> s) : state(std::move(s)) {}
    std::shared_ptr<GraphNode> createNode(const NodeConfig& cfg) override {
        auto n = std::make_shared<PerceiveNode>(cfg.nodeName, cfg.eventBus, state);
        n->m_startPos = cfg.params.value("startPos", 0);
        n->m_target = cfg.params.value("target", 0);
        n->m_items = cfg.params.value("items", 0);
        return n;
    }
};
class ThinkCreator : public NodeCreator {
public:
    std::shared_ptr<WorldState> state;
    explicit ThinkCreator(std::shared_ptr<WorldState> s) : state(std::move(s)) {}
    std::shared_ptr<GraphNode> createNode(const NodeConfig& cfg) override {
        return std::make_shared<ThinkNode>(cfg.nodeName, cfg.eventBus, state);
    }
};
class MoveCreator : public NodeCreator {
public:
    std::shared_ptr<WorldState> state;
    explicit MoveCreator(std::shared_ptr<WorldState> s) : state(std::move(s)) {}
    std::shared_ptr<GraphNode> createNode(const NodeConfig& cfg) override {
        return std::make_shared<MoveActionNode>(cfg.nodeName, cfg.eventBus, state);
    }
};
class GatherCreator : public NodeCreator {
public:
    std::shared_ptr<WorldState> state;
    explicit GatherCreator(std::shared_ptr<WorldState> s) : state(std::move(s)) {}
    std::shared_ptr<GraphNode> createNode(const NodeConfig& cfg) override {
        return std::make_shared<GatherActionNode>(cfg.nodeName, cfg.eventBus, state);
    }
};
class StopCreator : public NodeCreator {
public:
    std::shared_ptr<GraphNode> createNode(const NodeConfig& cfg) override {
        return std::make_shared<StopActionNode>(cfg.nodeName, cfg.eventBus);
    }
};

// === 工具:按名找节点 ===
template <typename T>
static T* findNode(GraphEngine& eng, const std::string& name) {
    for (auto& path : eng.paths()) {
        for (auto& nodeBase : path->getNodes()) {
            if (nodeBase->getName() == name) {
                return dynamic_cast<T*>(nodeBase.get());
            }
        }
    }
    return nullptr;
}

// runScenario:接收注册回调,由调用方在里面创建 WorldState 并注册带状态的 Creator。
static bool runScenario(const std::string& title, const json& configs,
                        std::function<void(GraphEngine&)> registerCreators,
                        bool (*verify)(GraphEngine&)) {
    std::cout << "====== " << title << " ======\n";
    GraphEngine eng;
    registerCreators(eng);
    if (!eng.loadFromJson(configs)) {
        std::cout << "  FAIL: loadFromJson\n\n";
        return false;
    }
    if (!eng.init()) { std::cout << "  FAIL: init\n\n"; return false; }
    if (!eng.start()) { std::cout << "  FAIL: start\n\n"; return false; }
    bool ok = verify(eng);
    eng.stop();
    eng.release();
    std::cout << "  => " << (ok ? "PASS" : "FAIL") << "\n\n";
    return ok;
}

// 拓扑:所有 edge 统一为 AgentStateEvent;goal->perc 配 condition=seed(首启初始化角色);
//   think 三条出边各配 condition(gather/move/stop,决策意图);反馈边无 condition。
//   perc 持 1 seed sub 边 + 2 feedback sub 边,靠 condition 角色分流;think 持 3 决策 pub 边扇出。
static json makeTopology() {
    return {
        {"nodes", json::array({
            { {"id","goal"},  {"class","GoalSeedNode"}, {"params", { {"goal","reach and gather"} }} },
            { {"id","perc"},  {"class","PerceiveNode"}, {"params", { {"startPos",0}, {"target",3}, {"items",2} }} },
            { {"id","think"}, {"class","ThinkNode"} },
            { {"id","move"},  {"class","MoveActionNode"} },
            { {"id","gather"},{"class","GatherActionNode"} },
            { {"id","stop"},  {"class","StopActionNode"} }
        })},
        {"paths", json::array({
            { {"name","seed"},    {"path", json::array({ { {"id","goal"}, {"edge","AgentStateEvent"}, {"condition","seed"} }, { {"id","perc"}, {"edge",""} } })} },
            { {"name","perc"},    {"path", json::array({ { {"id","perc"}, {"edge","AgentStateEvent"} },  { {"id","think"}, {"edge",""} } })} },
            { {"name","move"},    {"path", json::array({ { {"id","think"}, {"edge","AgentStateEvent"}, {"condition","move"} },   { {"id","move"}, {"edge",""} } })} },
            { {"name","gather"},  {"path", json::array({ { {"id","think"}, {"edge","AgentStateEvent"}, {"condition","gather"} }, { {"id","gather"}, {"edge",""} } })} },
            { {"name","stop"},    {"path", json::array({ { {"id","think"}, {"edge","AgentStateEvent"}, {"condition","stop"} },   { {"id","stop"}, {"edge",""} } })} },
            { {"name","fb_move"}, {"path", json::array({ { {"id","move"}, {"edge","AgentStateEvent"} },   { {"id","perc"}, {"edge",""} } })} },
            { {"name","fb_gath"}, {"path", json::array({ { {"id","gather"}, {"edge","AgentStateEvent"} },{ {"id","perc"}, {"edge",""} } })} }
        })}
    };
}

static void registerAll(GraphEngine& eng, std::shared_ptr<WorldState> state) {
    eng.registerCreator("GoalSeedNode",  std::make_shared<GoalSeedCreator>());
    eng.registerCreator("PerceiveNode",  std::make_shared<PerceiveCreator>(state));
    eng.registerCreator("ThinkNode",     std::make_shared<ThinkCreator>(state));
    eng.registerCreator("MoveActionNode", std::make_shared<MoveCreator>(state));
    eng.registerCreator("GatherActionNode", std::make_shared<GatherCreator>(state));
    eng.registerCreator("StopActionNode", std::make_shared<StopCreator>());
}

static bool scenario_reach_gather() {
    auto state = std::make_shared<WorldState>();
    json cfg = makeTopology();
    return runScenario("agent: reach target and gather (2G + 3M + 1Stop)",
                       cfg,
                       [&](GraphEngine& e){ registerAll(e, state); },
                       [](GraphEngine& e) {
                           auto* move = findNode<MoveActionNode>(e, "move");
                           auto* gath = findNode<GatherActionNode>(e, "gather");
                           auto* stop = findNode<StopActionNode>(e, "stop");
                           if (!move || !gath || !stop) return false;
                           return gath->m_received == 2 &&
                                  move->m_received == 3 &&
                                  stop->m_received == 1;
                       });
}

int main() {
    utoolkit::logging::Logger::GetInstance().Initialize(
        "cli_test", "[%Y-%m-%d %H:%M:%S.%e] [%n] [%^%l%$] [%t] %v",
        utoolkit::logging::LogLevel::Trace);

    bool ok = scenario_reach_gather();
    std::cout << "====== summary ======\n"
              << "agent (reach+gather loop, natural stop): "
              << (ok ? "PASS" : "FAIL") << "\n";
    return ok ? 0 : 1;
}
