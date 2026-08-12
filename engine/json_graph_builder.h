// engine/json_graph_builder.h
#pragma once
#include <functional>
#include <memory>
#include <nlohmann/json.hpp>
#include <string>
#include <unordered_map>
#include <vector>
#include "core/blackboard.h"
#include "core/event_bus.h"
#include "core/graph_node.h"
#include "core/graph_path.h"
#include "engine/build_result.h"
#include "engine/graph_builder.h"
#include "engine/node_config.h"
#include "engine/node_factory.h"

namespace graphflow::engine {

using ConfigBuilderCallback =
    std::function<NodeConfig(const std::string& nodeClass,
                              const nlohmann::json& params)>;

// JSON → 图装配器。
// 审查 3.3 / 3.5 / 4.2 修复：返回 BuildResult（paths + errors），
//   内部按 validate → instantiate → connect → commit 事务化，失败时不修改上一次有效图。
//   依赖注入：EventBus / Blackboard / NodeFactory 均由外部注入（GraphEngine 持有）。
class JsonGraphBuilder {
public:
    JsonGraphBuilder();

    // 返回 BuildResult：成功 ok()=true 且 paths 为装配好的路径；失败 errors 非空、paths 为空。
    BuildResult buildFromJsonFile(const std::string& configFile);
    BuildResult buildFromJson(const nlohmann::json& configs);

    void setEventBus(core::EventBus* bus);
    void setBlackboard(core::Blackboard* blackboard) { m_blackboard = blackboard; }
    void setConfigBuilder(ConfigBuilderCallback builder) { m_configBuilder = std::move(builder); }
    void setNodeFactory(NodeFactory* factory) { m_factory = factory; }

    // 直接注册预构造节点（绕过 factory），主要用于测试与预定义节点。
    void registerNode(const std::string& nodeName,
                      std::shared_ptr<core::GraphNode> node) {
        m_graphBuilder.registerNode(nodeName, node);
    }
    GraphBuilder& getGraphBuilder() { return m_graphBuilder; }

private:
    core::EventBus* m_eventBus = nullptr;
    core::Blackboard* m_blackboard = nullptr;
    NodeFactory* m_factory = nullptr;
    GraphBuilder m_graphBuilder;
    ConfigBuilderCallback m_configBuilder;

    BuildResult buildFromNewFormat(const nlohmann::json& configs);
};

}  // namespace graphflow::engine
