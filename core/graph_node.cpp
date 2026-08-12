// core/graph_node.cpp
#include "core/graph_node.h"

namespace graphflow::core {

GraphNode::GraphNode(const std::string& nodeName, EventBus* eventBus,
                     Blackboard* blackboard)
    : NodeBase(nodeName), m_eventBus(eventBus), m_blackboard(blackboard) {}

void GraphNode::addSubEdge(const GraphEdge& edge) {
    LOG_INFO(TAG, "addSubEdge: " << edge.name()
             << " eventBus=" << edge.eventBus()
             << " dataType=" << edge.dataType()
             << " condition=" << edge.condition());
    m_subEdges.push_back(edge);
}

void GraphNode::addPubEdge(const GraphEdge& edge) {
    LOG_INFO(TAG, "addPubEdge: " << edge.name()
             << " eventBus=" << edge.eventBus()
             << " dataType=" << edge.dataType()
             << " condition=" << edge.condition());
    m_pubEdges.push_back(edge);
}

void GraphNode::clearSubEdges() { m_subEdges.clear(); }
void GraphNode::clearPubEdges() { m_pubEdges.clear(); }

void GraphNode::setEventBus(EventBus* eventBus) { m_eventBus = eventBus; }

const std::vector<GraphEdge>& GraphNode::getSubEdges() const {
    std::ostringstream oss;
    oss << "getSubEdges count: " << m_subEdges.size();
    for (size_t i = 0; i < m_subEdges.size(); ++i) {
        oss << ", [" << i << "] edge=" << m_subEdges[i].name()
            << " dataType=" << m_subEdges[i].dataType()
            << " condition=" << m_subEdges[i].condition()
            << " eventBus=" << m_subEdges[i].eventBus();
    }
    LOG_DEBUG(TAG, oss.str());
    return m_subEdges;
}

const std::vector<GraphEdge>& GraphNode::getPubEdges() const {
    std::ostringstream oss;
    oss << "getPubEdges count: " << m_pubEdges.size();
    for (size_t i = 0; i < m_pubEdges.size(); ++i) {
        oss << ", [" << i << "] edge=" << m_pubEdges[i].name()
            << " dataType=" << m_pubEdges[i].dataType()
            << " condition=" << m_pubEdges[i].condition()
            << " eventBus=" << m_pubEdges[i].eventBus();
    }
    LOG_DEBUG(TAG, oss.str());
    return m_pubEdges;
}

void GraphNode::halt() {
    bool expected = false;
    if (m_halted.compare_exchange_strong(expected, true)) {
        doHalt();   // 仅首次 halt 调 doHalt,保证幂等
    }
}

void GraphNode::clearHalt() {
    m_halted.store(false);
}

bool GraphNode::isHalted() const noexcept {
    return m_halted.load();
}

}  // namespace graphflow::core
