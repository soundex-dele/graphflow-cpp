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
#include "engine/json_graph_builder.h"  // ConfigBuilderCallback（setConfigBuilder 透传用）
#include "engine/node_factory.h"   // NodeCreator（同 namespace，迁后无需 engine:: 前缀）

namespace graphflow::engine {

class JsonGraphBuilder;   // 同 namespace 前向声明

// 用户 JSON 驱动入口（facade）。
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

    // 热重载：Initialized/Running/Stopped 态可调。Created 态等价于 load。
    bool reloadFromJsonFile(const std::string& configFile);
    bool reloadFromJson(const nlohmann::json& configs);

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
};

}  // namespace graphflow::engine
