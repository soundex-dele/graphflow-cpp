// core/graph_path.cpp
#include "core/graph_path.h"
#include <algorithm>
#include <stdexcept>
#include "utils/logger.h"

namespace graphflow::core {

static const std::string TAG = "GraphPath";

GraphPath::GraphPath(EventBus* eventBus,
                     const std::vector<std::shared_ptr<NodeBase>>& nodes)
    : m_eventBus(eventBus), m_nodes(nodes) {}

GraphPath::~GraphPath() {
    release();  // 状态机保证幂等：已 Released 则 no-op
}

void GraphPath::addNode(std::shared_ptr<NodeBase> node) {
    if (!node) {
        throw std::invalid_argument("Invalid node pointer");
    }
    std::lock_guard<std::mutex> lock(m_mutex);
    m_nodes.push_back(node);
}

void GraphPath::retainEventBus(std::shared_ptr<EventBus> eventBus) {
    if (!eventBus) return;
    std::lock_guard<std::mutex> lock(m_mutex);
    const auto raw = eventBus.get();
    const auto found = std::find_if(
        m_ownedEventBuses.begin(), m_ownedEventBuses.end(),
        [raw](const auto& existing) { return existing.get() == raw; });
    if (found == m_ownedEventBuses.end()) {
        m_ownedEventBuses.push_back(std::move(eventBus));
    }
}

std::vector<std::shared_ptr<EventBus>> GraphPath::eventBusesSnapshot() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_ownedEventBuses;
}

bool GraphPath::doInit() {
    std::vector<std::shared_ptr<NodeBase>> snapshot;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        snapshot = m_nodes;
    }
    std::vector<std::shared_ptr<NodeBase>> started;
    for (const auto& node : snapshot) {
        const auto previousState = node->state();
        if (!node->init()) {
            LOG_ERROR(TAG, "node init failed: " << node->getName()
                        << ", rolling back " << started.size() << " initialized node(s)");
            for (auto it = started.rbegin(); it != started.rend(); ++it) {
                try {
                    (*it)->release();  // Initialized → Released（恢复终态）
                } catch (const std::exception& e) {
                    LOG_ERROR(TAG, "rollback release exception: " << e.what());
                }
            }
            return false;
        }
        if (previousState == LifecycleState::Created &&
            node->state() == LifecycleState::Initialized) {
            started.push_back(node);
        }
    }
    return true;
}

bool GraphPath::doStart() {
    std::vector<std::shared_ptr<NodeBase>> snapshot;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        snapshot = m_nodes;
    }
    std::vector<std::shared_ptr<NodeBase>> started;
    for (const auto& node : snapshot) {
        const auto previousState = node->state();
        if (!node->start()) {
            LOG_ERROR(TAG, "node start failed: " << node->getName()
                        << ", rolling back " << started.size() << " started node(s)");
            for (auto it = started.rbegin(); it != started.rend(); ++it) {
                try {
                    (*it)->stop();  // Running → Stopped
                } catch (const std::exception& e) {
                    LOG_ERROR(TAG, "rollback stop exception: " << e.what());
                }
            }
            return false;
        }
        if (previousState != LifecycleState::Running &&
            node->state() == LifecycleState::Running) {
            started.push_back(node);
        }
    }
    return true;
}

void GraphPath::doStop() {
    // 不再在此调 node->release()：release 是独立阶段，由 doRelease 负责。
    // 旧实现把 release 塞进 stop 导致节点 release 被多次调用。
    std::vector<std::shared_ptr<NodeBase>> snapshot;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        snapshot = m_nodes;
    }
    for (const auto& node : snapshot) {
        node->stop();
    }
}

void GraphPath::doRelease() {
    std::vector<std::shared_ptr<NodeBase>> snapshot;
    std::vector<std::shared_ptr<EventBus>> buses;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        snapshot = m_nodes;
        buses = m_ownedEventBuses;
    }
    for (const auto& bus : buses) bus->close();
    for (const auto& bus : buses) bus->drain();
    for (const auto& node : snapshot) {
        node->release();
    }
}

}  // namespace graphflow::core
