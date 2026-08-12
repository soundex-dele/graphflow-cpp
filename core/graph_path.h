// core/graph_path.h
#pragma once
#include <memory>
#include <mutex>
#include <vector>
#include "core/common.h"           // NodeBase / Lifecycle
#include "core/event_bus.h"

namespace graphflow::core {

// GraphPath 作为管线基类（不再抽出 IPipeline 接口）。
// 生命周期由 Lifecycle 托管：doInit/doStart/doStop/doRelease 遍历 m_nodes 委托给各节点。
class GraphPath : public Lifecycle {
public:
    GraphPath(EventBus* eventBus, const std::vector<std::shared_ptr<NodeBase>>& nodes);
    ~GraphPath() override;

    void addNode(std::shared_ptr<NodeBase> node);
    void retainEventBus(std::shared_ptr<EventBus> eventBus);
    std::vector<std::shared_ptr<EventBus>> eventBusesSnapshot() const;

    EventBus* getEventBus() noexcept { return m_eventBus; }
    const std::vector<std::shared_ptr<NodeBase>>& getNodes() const noexcept {
        return m_nodes;
    }

protected:
    bool doInit() override;
    bool doStart() override;
    void doStop() override;
    void doRelease() override;

    // NOTE: doInit/doStart/doStop/doRelease 在实现里走"锁内拷贝 m_nodes 快照 → 锁外调虚"
    // 模式，回调可安全回灌 addNode（addNode 走另一把锁内 push）。
    EventBus* m_eventBus;
    std::vector<std::shared_ptr<NodeBase>> m_nodes;
    std::vector<std::shared_ptr<EventBus>> m_ownedEventBuses;
    mutable std::mutex m_mutex;
};

}  // namespace graphflow::core
