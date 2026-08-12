// engine/node_factory.h
#pragma once
#include <memory>
#include <nlohmann/json.hpp>
#include <mutex>
#include <string>
#include <unordered_map>
#include "utils/logger.h"
#include "core/graph_node.h"
#include "engine/node_config.h"

namespace graphflow::engine {

// 节点创建器抽象基类
class NodeCreator {
public:
    virtual ~NodeCreator() = default;
    virtual std::shared_ptr<core::GraphNode> createNode(const NodeConfig& config) = 0;
};

// 节点工厂（实例，非进程级单例）。
// 审查 3.6 修复：原为 getInstance() 进程级单例，两个 GraphEngine 注册同名 class 会相互
//   覆盖且无同步保护。现改为普通类，由 GraphEngine 持有实例；注册/创建加锁保证线程安全；
//   同名 class 后注册覆盖前者并记日志。
class NodeFactory {
public:
    NodeFactory() = default;
    ~NodeFactory() = default;

    NodeFactory(const NodeFactory&) = delete;
    NodeFactory& operator=(const NodeFactory&) = delete;

    // 注册节点创建器；同名 class 覆盖前者（并记日志）。
    void registerCreator(const std::string& nodeClass,
                         std::shared_ptr<NodeCreator> creator);

    // 便捷创建（旧签名保留）：由 class + eventBus + params 组装 NodeConfig 后委托 createNode(config)。
    std::shared_ptr<core::GraphNode> createNode(
        const std::string& nodeClass, core::EventBus* eventBus,
        const nlohmann::json& params);

    std::shared_ptr<core::GraphNode> createNode(const NodeConfig& config);

    bool hasCreator(const std::string& nodeClass) const;
    size_t size() const;

private:
    mutable std::mutex m_mutex;
    std::unordered_map<std::string, std::shared_ptr<NodeCreator>> m_creators;
};

}  // namespace graphflow::engine
