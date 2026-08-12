// engine/node_factory.cpp
#include "engine/node_factory.h"

namespace graphflow::engine {

void NodeFactory::registerCreator(const std::string& nodeClass,
                                  std::shared_ptr<NodeCreator> creator) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_creators.find(nodeClass) != m_creators.end()) {
        LOG_WARN("NodeFactory", "override existing creator: " << nodeClass);
    } else {
        LOG_INFO("NodeFactory", "register creator: " << nodeClass);
    }
    m_creators[nodeClass] = std::move(creator);
}

std::shared_ptr<core::GraphNode> NodeFactory::createNode(
    const std::string& nodeClass, core::EventBus* eventBus,
    const nlohmann::json& params) {
    NodeConfig config = NodeConfig::builder()
        .nodeClass(nodeClass)
        .eventBus(eventBus)
        .params(params)
        .build();
    return createNode(config);
}

std::shared_ptr<core::GraphNode> NodeFactory::createNode(const NodeConfig& config) {
    std::shared_ptr<NodeCreator> creator;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        auto it = m_creators.find(config.nodeClass);
        if (it != m_creators.end()) {
            creator = it->second;
        }
    }
    if (!creator) {
        LOG_ERROR("NodeFactory", "Unknown node type: " << config.nodeClass);
        return nullptr;
    }
    return creator->createNode(config);
}

bool NodeFactory::hasCreator(const std::string& nodeClass) const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_creators.find(nodeClass) != m_creators.end();
}

size_t NodeFactory::size() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_creators.size();
}

}  // namespace graphflow::engine
