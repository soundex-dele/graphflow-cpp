// examples/multi/main.cpp
// 四种图拓扑演示：JSON 直接定义在代码里（不经文件），用 GraphEngine facade 装配。
//
//   场景1：一个节点(A)发送两个相同事件给不同节点(B,C)
//          A→B、A→C 都发 EventA
//   场景2：一个节点(A)发送两个不同事件给不同节点(B,C)
//          A→B 发 EventA，A→C 发 EventB
//   场景3：一个节点(C)收两个不同节点(A,B)的两个相同事件
//          A→C、B→C 都发 EventA
//   场景4：一个节点(C)收两个不同节点(A,B)的两个不同事件
//          A→C 发 EventA，B→C 发 EventB
//   场景5：循环图 A→B→C→A→D
//          单条 path 内 A 出现两次：A→B、B→C、C→A（回环到 A）、A→D
//          A 既发布(A→B、A→D)又订阅(C→A)——闭合环 + 分支
//
// 关键机制：
//   - GraphEngine::loadFromJson(nlohmann::json&) 直接吃内存 JSON，无需文件。
//   - GraphBuilder::build 把每条 path 当线性链，相邻节点建边 topic="from->to"，
//     每条边独立 EventBus。
//   - 同一 id 的节点跨 path 共享（同一 shared_ptr），故一个节点可同时挂多条
//     pub/sub 边——多扇出/多扇入靠"同一节点出现在多条 path"实现。
//   - 节点 doInit/doStart 受 Lifecycle 状态机幂等保护：跨 path 只生效一次；
//     但边在 build 阶段已全部挂上，故那一次 doInit/doStart 能遍历到全部边。
//   - edge 字段(dataType) 决定该边承载的事件类型；发布/订阅按它分发。
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>
#include "engine/graph_engine.h"
#include "core/event_bus.h"
#include "core/graph_node.h"

using graphflow::engine::GraphEngine;
using graphflow::engine::NodeConfig;
using graphflow::engine::NodeCreator;
using graphflow::core::Event;
using graphflow::core::EventT;
using graphflow::core::EventBus;
using graphflow::core::GraphNode;
using json = nlohmann::json;

// === 两种事件类型，各承载一个字符串 payload ===
struct EventA : EventT<EventA> {
    std::string payload;
    explicit EventA(std::string p) : payload(std::move(p)) {}
};
struct EventB : EventT<EventB> {
    std::string payload;
    explicit EventB(std::string p) : payload(std::move(p)) {}
};

// === 通用节点：按 edge.dataType() 决定订阅/发布哪种事件 ===
// 有 pub 边即源、有 sub 边即汇，两者兼有即中继。
struct MultiNode : GraphNode {
    mutable std::mutex m_mtx;
    std::vector<std::pair<std::string, std::string>> m_received;  // (事件类型, payload)

    explicit MultiNode(const std::string& n, EventBus* eb) : GraphNode(n, eb) {}

    void onEvent(EventA& e) {
        std::lock_guard<std::mutex> lk(m_mtx);
        m_received.emplace_back("EventA", e.payload);
        std::cout << "    [" << getName() << "] recv EventA: " << e.payload << "\n";
    }
    void onEvent(EventB& e) {
        std::lock_guard<std::mutex> lk(m_mtx);
        m_received.emplace_back("EventB", e.payload);
        std::cout << "    [" << getName() << "] recv EventB: " << e.payload << "\n";
    }

    // doInit：在每条 sub 边上订阅对应类型事件（init 在 start 之前，保证订阅先于发布）
    bool doInit() override {
        for (const auto& info : getSubEdges()) {
            const std::string& dt = info.dataType();
            auto* bus = info.eventBus();
            const std::string& topic = info.name();
            if (dt == "EventA") {
                bus->subscribe<EventA>(topic, [this](EventA& e) { onEvent(e); });
            } else if (dt == "EventB") {
                bus->subscribe<EventB>(topic, [this](EventB& e) { onEvent(e); });
            } else {
                std::cout << "    [" << getName() << "] skip sub unknown dataType: " << dt << "\n";
            }
        }
        return true;
    }

    // doStart：在每条 pub 边上发布对应类型事件，payload 标明来源
    bool doStart() override {
        for (const auto& info : getPubEdges()) {
            const std::string& dt = info.dataType();
            auto* bus = info.eventBus();
            const std::string& topic = info.name();
            std::string payload = "from " + getName();
            if (dt == "EventA") {
                bus->publish<EventA>(topic, EventA(payload));
            } else if (dt == "EventB") {
                bus->publish<EventB>(topic, EventB(payload));
            } else {
                std::cout << "    [" << getName() << "] skip pub unknown dataType: " << dt << "\n";
            }
        }
        return true;
    }

    std::vector<std::pair<std::string, std::string>> received() const {
        std::lock_guard<std::mutex> lk(m_mtx);
        return m_received;
    }
};

class MultiCreator : public NodeCreator {
public:
    std::shared_ptr<GraphNode> createNode(const NodeConfig& cfg) override {
        return std::make_shared<MultiNode>(cfg.nodeName, cfg.eventBus);
    }
};

// === 循环节点：事件驱动中继 ===
// 与 MultiNode 不同：不在 doStart 里主动发布，而是收到上游事件后才向下游转发。
//   - doStart：仅当 params.seed=true 时，发布第一条 pub 边，作为循环的"种子"触发。
//   - handleAndRelay：记录后，发布本节点的 pub 边（seed 节点跳过第一条，因种子边
//     已在 doStart 发过，relay 时不重发——否则会把种子边再触发一遍）。
//
// 用事件类型切换防止死循环（用户方案）：
//   A→B(EventA) → B→C(EventA) → C→A(EventB) → A→D(EventA)
//   环路在 C→A 处把类型从 EventA 切成 EventB；A 收到 EventB 后发 A→D(EventA)。
//   只有 C 会发 EventB 且只发一次，故 A 只被触发一次，循环自然终止——无需计数/标志。
struct CycleNode : GraphNode {
    mutable std::mutex m_mtx;
    std::vector<std::pair<std::string, std::string>> m_received;  // (事件类型, payload)
    bool m_seed = false;   // 是否在 doStart 时播种第一条 pub 边

    explicit CycleNode(const std::string& n, EventBus* eb, bool seed)
        : GraphNode(n, eb), m_seed(seed) {}

    // 发布本节点的 pub 边（按各自 dataType 发）。seed 节点跳过第一条——
    // 种子边已在 doStart 发过，relay 时若再发会重复触发下游，破坏"一次性"语义。
    void relay() {
        const auto& edges = getPubEdges();
        for (size_t i = 0; i < edges.size(); ++i) {
            if (m_seed && i == 0) continue;
            const auto& info = edges[i];
            const std::string& dt = info.dataType();
            auto* bus = info.eventBus();
            const std::string& topic = info.name();
            std::string payload = "from " + getName();
            if (dt == "EventA") {
                bus->publish<EventA>(topic, EventA(payload));
            } else if (dt == "EventB") {
                bus->publish<EventB>(topic, EventB(payload));
            }
        }
    }

    // 事件回调入口：记录后，接力转发到下游。
    template <typename Evt>
    void handleAndRelay(Evt& e, const char* tag) {
        {
            std::lock_guard<std::mutex> lk(m_mtx);
            m_received.emplace_back(tag, e.payload);
            std::cout << "    [" << getName() << "] recv " << tag << ": " << e.payload << "\n";
        }
        relay();   // 锁外：把事件接力到下游
    }

    bool doInit() override {
        for (const auto& info : getSubEdges()) {
            const std::string& dt = info.dataType();
            auto* bus = info.eventBus();
            const std::string& topic = info.name();
            if (dt == "EventA") {
                bus->subscribe<EventA>(topic,
                    [this](EventA& e) { handleAndRelay(e, "EventA"); });
            } else if (dt == "EventB") {
                bus->subscribe<EventB>(topic,
                    [this](EventB& e) { handleAndRelay(e, "EventB"); });
            }
        }
        return true;
    }

    bool doStart() override {
        // 种子节点：在 start 时发布第一条 pub 边，启动循环。其余 pub 边由 onEvent 触发。
        if (m_seed && !getPubEdges().empty()) {
            const auto& info = getPubEdges().front();
            const std::string& dt = info.dataType();
            auto* bus = info.eventBus();
            const std::string& topic = info.name();
            std::string payload = "from " + getName();
            if (dt == "EventA") {
                bus->publish<EventA>(topic, EventA(payload));
            } else if (dt == "EventB") {
                bus->publish<EventB>(topic, EventB(payload));
            }
        }
        return true;
    }

    std::vector<std::pair<std::string, std::string>> received() const {
        std::lock_guard<std::mutex> lk(m_mtx);
        return m_received;
    }
};

class CycleCreator : public NodeCreator {
public:
    std::shared_ptr<GraphNode> createNode(const NodeConfig& cfg) override {
        bool seed = cfg.params.value("seed", false);
        return std::make_shared<CycleNode>(cfg.nodeName, cfg.eventBus, seed);
    }
};

// === 工具 ===
static MultiNode* findNode(GraphEngine& eng, const std::string& name) {
    for (auto& path : eng.paths()) {
        for (auto& nodeBase : path->getNodes()) {
            if (nodeBase->getName() == name) {
                return dynamic_cast<MultiNode*>(nodeBase.get());
            }
        }
    }
    return nullptr;
}
static CycleNode* findCycleNode(GraphEngine& eng, const std::string& name) {
    for (auto& path : eng.paths()) {
        for (auto& nodeBase : path->getNodes()) {
            if (nodeBase->getName() == name) {
                return dynamic_cast<CycleNode*>(nodeBase.get());
            }
        }
    }
    return nullptr;
}

// n 是否收到过 (type, payload)
static bool got(MultiNode* n, const std::string& type, const std::string& payload) {
    if (!n) return false;
    for (const auto& r : n->received()) {
        if (r.first == type && r.second == payload) return true;
    }
    return false;
}
static bool got(CycleNode* n, const std::string& type, const std::string& payload) {
    if (!n) return false;
    for (const auto& r : n->received()) {
        if (r.first == type && r.second == payload) return true;
    }
    return false;
}

// 运行一个场景：装配→init→start→校验→stop→release
static bool runScenario(const std::string& title, const json& configs,
                        bool (*verify)(GraphEngine&)) {
    std::cout << "====== " << title << " ======\n";
    GraphEngine eng;
    eng.registerCreator("MultiNode", std::make_shared<MultiCreator>());
    eng.registerCreator("CycleNode", std::make_shared<CycleCreator>());
    if (!eng.loadFromJson(configs)) {
        std::cout << "  FAIL: loadFromJson\n\n";
        return false;
    }
    if (!eng.init()) {
        std::cout << "  FAIL: init\n\n";
        return false;
    }
    if (!eng.start()) {
        std::cout << "  FAIL: start\n\n";
        return false;
    }
    // release() 前校验：release 会清空 eng.paths()
    bool ok = verify(eng);
    eng.stop();
    eng.release();
    std::cout << "  => " << (ok ? "PASS" : "FAIL") << "\n\n";
    return ok;
}

// === 场景1：A 发送两个相同事件(EventA)给 B 和 C ===
// 两条 path 共享源节点 A：A->B、A->C 两条 pub 边都发 EventA。
static bool scenario1() {
    json configs = json::parse(R"JSON({
      "nodes": [
        { "id": "A", "class": "MultiNode" },
        { "id": "B", "class": "MultiNode" },
        { "id": "C", "class": "MultiNode" }
      ],
      "paths": [
        { "name": "p1", "path": [ { "id": "A", "edge": "EventA" }, { "id": "B", "edge": "" } ] },
        { "name": "p2", "path": [ { "id": "A", "edge": "EventA" }, { "id": "C", "edge": "" } ] }
      ]
    })JSON");
    return runScenario("scenario 1: A sends SAME event (EventA) to B and C",
                        configs, [](GraphEngine& e) {
        return got(findNode(e, "B"), "EventA", "from A")
            && got(findNode(e, "C"), "EventA", "from A");
    });
}

// === 场景2：A 发送两个不同事件给 B 和 C ===
// A->B 发 EventA、A->C 发 EventB（两条 pub 边 dataType 不同）。
static bool scenario2() {
    json configs = json::parse(R"JSON({
      "nodes": [
        { "id": "A", "class": "MultiNode" },
        { "id": "B", "class": "MultiNode" },
        { "id": "C", "class": "MultiNode" }
      ],
      "paths": [
        { "name": "p1", "path": [ { "id": "A", "edge": "EventA" }, { "id": "B", "edge": "" } ] },
        { "name": "p2", "path": [ { "id": "A", "edge": "EventB" }, { "id": "C", "edge": "" } ] }
      ]
    })JSON");
    return runScenario("scenario 2: A sends DIFFERENT events (EventA->B, EventB->C)",
                        configs, [](GraphEngine& e) {
        return got(findNode(e, "B"), "EventA", "from A")
            && got(findNode(e, "C"), "EventB", "from A");
    });
}

// === 场景3：C 收两个不同节点(A,B)的两个相同事件(EventA) ===
// 两条 path 共享汇节点 C：A->C、B->C 两条 sub 边都订 EventA。
static bool scenario3() {
    json configs = json::parse(R"JSON({
      "nodes": [
        { "id": "A", "class": "MultiNode" },
        { "id": "B", "class": "MultiNode" },
        { "id": "C", "class": "MultiNode" }
      ],
      "paths": [
        { "name": "p1", "path": [ { "id": "A", "edge": "EventA" }, { "id": "C", "edge": "" } ] },
        { "name": "p2", "path": [ { "id": "B", "edge": "EventA" }, { "id": "C", "edge": "" } ] }
      ]
    })JSON");
    return runScenario("scenario 3: C receives SAME event (EventA) from A and B",
                        configs, [](GraphEngine& e) {
        MultiNode* c = findNode(e, "C");
        return got(c, "EventA", "from A") && got(c, "EventA", "from B");
    });
}

// === 场景4：C 收两个不同节点(A,B)的两个不同事件 ===
// A->C 发 EventA、B->C 发 EventB。
static bool scenario4() {
    json configs = json::parse(R"JSON({
      "nodes": [
        { "id": "A", "class": "MultiNode" },
        { "id": "B", "class": "MultiNode" },
        { "id": "C", "class": "MultiNode" }
      ],
      "paths": [
        { "name": "p1", "path": [ { "id": "A", "edge": "EventA" }, { "id": "C", "edge": "" } ] },
        { "name": "p2", "path": [ { "id": "B", "edge": "EventB" }, { "id": "C", "edge": "" } ] }
      ]
    })JSON");
    return runScenario("scenario 4: C receives DIFFERENT events (EventA from A, EventB from B)",
                        configs, [](GraphEngine& e) {
        MultiNode* c = findNode(e, "C");
        return got(c, "EventA", "from A") && got(c, "EventB", "from B");
    });
}

// === 场景5：循环图 A→B→C→A→D，且 A→D 必须等 A 收到 C 的事件后才发 ===
// 单条 path：A 出现两次（位置 0 与 3）。GraphBuilder 对相邻节点建边：
//   A→B、B→C、C→A（回环到 A）、A→D。
// 事件类型切换（用户方案）防死循环 + 表达"A 收到 C 才发 D"：
//   A→B(EventA) → B→C(EventA) → C→A(EventB) → A→D(EventA)
//   - A 在 doStart 种子式发 A→B(EventA) 启动；
//   - B/C 收上游事件后接力转发；
//   - C→A 把类型从 EventA 切成 EventB；
//   - A 收到 EventB 后 relay，跳过种子边(A→B)，只发 A→D(EventA)。
//   只有 C 会发 EventB 且只发一次，故 A 只被触发一次，循环自然终止。
static bool scenario5() {
    json configs = json::parse(R"JSON({
      "nodes": [
        { "id": "A", "class": "CycleNode", "params": { "seed": true } },
        { "id": "B", "class": "CycleNode" },
        { "id": "C", "class": "CycleNode" },
        { "id": "D", "class": "CycleNode" }
      ],
      "paths": [
        { "name": "cycle", "path": [
          { "id": "A", "edge": "EventA" },
          { "id": "B", "edge": "EventA" },
          { "id": "C", "edge": "EventB" },
          { "id": "A", "edge": "EventA" },
          { "id": "D", "edge": "" }
        ] }
      ]
    })JSON");
    return runScenario("scenario 5: cycle A->B->C->A->D, A->D fires only after A recv C",
                        configs, [](GraphEngine& e) {
        // 环路：A→B、B→C 同类型接力，C→A 切成 EventB
        bool loopAB = got(findCycleNode(e, "B"), "EventA", "from A");
        bool loopBC = got(findCycleNode(e, "C"), "EventA", "from B");
        bool loopCA = got(findCycleNode(e, "A"), "EventB", "from C");
        // 分支：A 收到 C 的 EventB 后才发 A→D(EventA)
        bool branchAD = got(findCycleNode(e, "D"), "EventA", "from A");
        return loopAB && loopBC && loopCA && branchAD;
    });
}

int main() {
    bool s1 = scenario1();
    bool s2 = scenario2();
    bool s3 = scenario3();
    bool s4 = scenario4();
    bool s5 = scenario5();

    std::cout << "====== summary ======\n"
              << "scenario1 (one -> two, same event):     " << (s1 ? "PASS" : "FAIL") << "\n"
              << "scenario2 (one -> two, diff event):     " << (s2 ? "PASS" : "FAIL") << "\n"
              << "scenario3 (two -> one, same event):     " << (s3 ? "PASS" : "FAIL") << "\n"
              << "scenario4 (two -> one, diff event):     " << (s4 ? "PASS" : "FAIL") << "\n"
              << "scenario5 (cycle A->B->C->A->D):         " << (s5 ? "PASS" : "FAIL") << "\n";
    return (s1 && s2 && s3 && s4 && s5) ? 0 : 1;
}
