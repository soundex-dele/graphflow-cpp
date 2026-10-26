// engine/graph_engine.h
#pragma once
#include <memory>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>
#include "core/blackboard.h"
#include "core/event_bus.h"
#include "core/graph_path.h"
#include "engine/build_result.h"
#include "engine/graph_builder.h"
#include "engine/json_graph_builder.h"  // ConfigBuilderCallback（setConfigBuilder 透传用）
#include "engine/node_factory.h"   // NodeCreator（同 namespace，迁后无需 engine:: 前缀）

namespace graphflow::engine {

class JsonGraphBuilder;   // 同 namespace 前向声明

// 用户构图入口（facade）：接受 JSON 或外部节点实例与连接关系。
// 拥有一个 JsonGraphBuilder + 共享 EventBus + 实例级 NodeFactory，对外提供
//   load/reload/init/start/stop/release/paths/eventBus。
// 生命周期由 Lifecycle 托管：doInit/doStart/doStop/doRelease 遍历 m_paths 委托。
//
// 状态机约束（审查 3.3）：
//   - loadFromJson(…) 仅 Created 态可调；其他态拒绝并返回 false。
//   - reloadFromJson(…) 在 Initialized/Running/Stopped 态可调：先 stop 旧图→
//     临时构建→成功替换 m_paths→按 reload 前态恢复；失败回滚到旧图。
//   - loadFromJson 用 BuildResult.ok() 判成功（不再 !paths.empty()）。
//
// 线程安全：主线程线性串行调用（与 Lifecycle 一致）；m_factory 加锁。
class GraphEngine : public core::Lifecycle {
public:
    explicit GraphEngine(size_t threadPoolSize = 5);
    ~GraphEngine() override;

    // 首次加载：仅 Created 态可调。
    bool loadFromJsonFile(const std::string& configFile);
    bool loadFromJson(const nlohmann::json& configs);

    // 外部只传节点和关系，不需要创建 GraphPath 或注册 NodeCreator。
    // getName() 为唯一非空 ID；节点须为 Created 且未挂连接边，不得复用旧图节点。
    // 引擎绑定控制总线、建立连接并共享持有节点，按输入顺序驱动生命周期。
    // 支持孤立节点、分支与环；空节点+空连接表示空图。校验失败不修改节点。
    // 成功后调用方不得独立驱动或修改节点；Blackboard/业务依赖由调用方注入。
    bool loadFromNodes(
        const std::vector<std::shared_ptr<core::GraphNode>>& nodes,
        const std::vector<NodeConnection>& connections,
        GraphBuilder::EdgeTransport transport = GraphBuilder::EdgeTransport::EventBus);

    // 热重载：Initialized/Running/Stopped 态可调。Created 态等价于 load。
    bool reloadFromJsonFile(const std::string& configFile);
    bool reloadFromJson(const nlohmann::json& configs);
    // 初始化/启动失败时释放候选节点并恢复旧图；重试须传入新的节点实例。
    bool reloadFromNodes(
        const std::vector<std::shared_ptr<core::GraphNode>>& nodes,
        const std::vector<NodeConnection>& connections,
        GraphBuilder::EdgeTransport transport = GraphBuilder::EdgeTransport::EventBus);

    // 路径与总线访问。生命周期约束：调用方须在 release() 之前使用返回引用，
    //   返回的 paths() 引用仅在下次 load/reload/release 前有效。
    const std::vector<std::shared_ptr<core::GraphPath>>& paths() const noexcept;
    core::EventBus* eventBus() noexcept;
    core::Blackboard* blackboard() noexcept { return &m_blackboard; }

    // 注册节点创建器到本引擎的实例级工厂（非全局）。
    void registerCreator(const std::string& nodeClass,
                         std::shared_ptr<NodeCreator> creator);
    NodeFactory& factory() noexcept { return m_factory; }

    // ConfigBuilder 透传：允许调用方（如 AgentWorkflow）在 loadFromJson 前
    // 注入 per-node 构建回调（把 runtimeContext 注入每个 NodeConfig）。
    void setConfigBuilder(ConfigBuilderCallback builder) noexcept;

protected:
    bool doInit() override;
    bool doStart() override;
    void doStop() override;
    void doRelease() override;

private:
    std::unique_ptr<JsonGraphBuilder> m_builder;
    std::shared_ptr<core::EventThreadPool> m_threadPool;
    std::unique_ptr<core::EventBus> m_eventBus;
    NodeFactory m_factory;
    std::vector<std::shared_ptr<core::GraphPath>> m_paths;
    core::Blackboard m_blackboard;   // 引擎级共享 Blackboard,注入到每个 NodeConfig(spec §3.2)

    // reload 内部实现：应用 BuildResult，失败返回 false 且不修改 m_paths。
    bool applyBuildResult(BuildResult result);
    bool applyReloadResult(BuildResult result, core::LifecycleState previousState);
    BuildResult buildFromNodes(
        const std::vector<std::shared_ptr<core::GraphNode>>& nodes,
        const std::vector<NodeConnection>& connections, GraphBuilder::EdgeTransport transport);
};

}  // namespace graphflow::engine
