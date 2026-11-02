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

void GraphNode::setFailureHandler(FailureHandler handler) {
    auto installed = handler ? std::make_shared<FailureHandler>(std::move(handler)) : nullptr;
    std::exception_ptr pending;
    {
        std::lock_guard lock(m_failureState->mutex);
        m_failureState->handler = installed;
        pending = m_failureState->failure;
    }
    if (installed && pending) {
        (*installed)(pending);
    }
}

void GraphNode::FailureState::report(std::exception_ptr value) noexcept {
    if (!value) return;
    std::shared_ptr<FailureHandler> notify;
    {
        std::lock_guard lock(mutex);
        if (failure) return;
        failure = value;
        notify = handler;
    }
    if (notify) {
        try {
            (*notify)(value);
        } catch (...) {
            LOG_ERROR("GraphNode", "Failure handler could not dispatch node failure");
        }
    }
}

void GraphNode::reportFailure(std::exception_ptr value) noexcept {
    m_failureState->report(value);
    if (value) {
        GraphError error{"Legacy node failure", {}};
        try {
            std::rethrow_exception(value);
        } catch (const std::exception& cause) {
            error.message = cause.what();
        } catch (...) {
        }
        m_errorState->report(std::move(error));
    }
}

GraphNode::FailureHandler GraphNode::failureReporter() const {
    return [state = m_failureState](std::exception_ptr value) { state->report(value); };
}

std::exception_ptr GraphNode::failure() const {
    std::lock_guard lock(m_failureState->mutex);
    return m_failureState->failure;
}

void GraphNode::clearFailure() {
    std::lock_guard lock(m_failureState->mutex);
    m_failureState->failure = {};
}

void GraphNode::setErrorHandler(ErrorHandler handler) {
    auto installed = handler ? std::make_shared<ErrorHandler>(std::move(handler)) : nullptr;
    std::optional<GraphError> pending;
    {
        std::lock_guard lock(m_errorState->mutex);
        m_errorState->handler = installed;
        pending = m_errorState->error;
    }
    if (installed && pending) (*installed)(*pending);
}

void GraphNode::ErrorState::report(GraphError value) {
    std::shared_ptr<ErrorHandler> notify;
    {
        std::lock_guard lock(mutex);
        if (error) return;
        error = value;
        notify = handler;
    }
    if (notify) (*notify)(value);
}

void GraphNode::reportError(GraphError value) { m_errorState->report(std::move(value)); }

GraphNode::ErrorHandler GraphNode::errorReporter() const {
    return [state = m_errorState](const GraphError& value) { state->report(value); };
}

std::optional<GraphError> GraphNode::error() const {
    std::lock_guard lock(m_errorState->mutex);
    return m_errorState->error;
}

void GraphNode::clearError() {
    std::lock_guard lock(m_errorState->mutex);
    m_errorState->error.reset();
}

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

namespace graphflow::core {
void GraphNode::invokeCallback(void (*callback)(void*), void* context) {
    try {
        callback(context);
    } catch (...) {
        reportFailure(std::current_exception());
        throw;
    }
}
} // namespace graphflow::core
