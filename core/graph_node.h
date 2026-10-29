// core/graph_node.h
#pragma once
#include <atomic>
#include <memory>
#include <exception>
#include <functional>
#include <mutex>
#include <sstream>
#include <string>
#include <type_traits>
#include <vector>
#include "utils/logger.h"
#include "core/blackboard.h"
#include "core/common.h"              // NodeBase
#include "core/event_bus.h"           // EventBus
#include "core/graph_edge.h"          // GraphEdge（已集成 eventBus/dataType，取代 GraphPortInfo）
#include "core/node_runtime_context.h"

namespace graphflow::core {

// GraphPortInfo 已删除：其三字段（edge / eventBus / dataType）已全部并入 GraphEdge。
//   pub/sub 信息现在直接以 GraphEdge 表示：info.name()=topic, info.eventBus()=信道, info.dataType()=数据类型。

class GraphNode : public NodeBase {
public:
    GraphNode(const std::string& nodeName, EventBus* eventBus,
              Blackboard* blackboard = nullptr);
    virtual ~GraphNode() override = default;

    void addSubEdge(const GraphEdge& edge);
    void addPubEdge(const GraphEdge& edge);
    void clearSubEdges();
    void clearPubEdges();
    void setEventBus(EventBus* eventBus);

    // Failure delivery runs on the reporting thread. Owners must enqueue
    // lifecycle work, never stop/join this node from its failure handler.
    // The first failure is retained, including failures before registration.
    using FailureHandler = std::function<void(std::exception_ptr)>;
    void setFailureHandler(FailureHandler handler);
    void reportFailure(std::exception_ptr failure) noexcept;
    // Retainable report handle for shared devices. It owns only failure state,
    // never the node; late device reports cannot dereference a destroyed node.
    FailureHandler failureReporter() const;
    std::exception_ptr failure() const;
    void clearFailure();  // Only after workers are quiescent, before a restart.

    const std::vector<GraphEdge>& getSubEdges() const;
    const std::vector<GraphEdge>& getPubEdges() const;

    // === 软中断(通用原语,spec §3.1)===
    // halt():non-virtual 模板方法。置 m_halted=true(仅首次) + 调 doHalt()。
    //   不碰 Lifecycle m_state,与整图 stop() 正交。
    //   幂等:已 halted 时再调为 no-op(doHalt 不重入)。
    void halt();
    // clearHalt():清 m_halted 标志,供派生类在"新一轮"信号时调用(BtNode 在新 tick 重置)。
    //   不调 doHalt。core 不规定调用时机。
    void clearHalt();
    bool isHalted() const noexcept;

protected:
    // 派生类 override:实际的中断清理(取消异步任务、重置 Running 状态)。
    virtual void doHalt() {}

    // === 便捷 subscribe/publish(spec 2026-07-25)===
    // subscribe<E>(cb):遍历 m_subEdges,校验 edge.dataType() 与 E::typeName() 一致后订阅。
    //   dataType 空:跳过校验直接订阅(宽松兼容旧 JSON)。
    //   不一致:LOG_ERROR 并跳过该边(不中止整次调用)。
    //   edge.eventBus() null:LOG_ERROR 跳过。
    //   回调签名 void(E&),透传既有 EventBus::subscribe。
    //   注:EventBus::subscribe(topic, Callback&&) 是转发引用:传左值会推导 Callback 为
    //   Lambda&、handler 内 m_callback 成引用,引用指向本函数栈帧 cb,函数返回即悬空。
    //   故每条边用 std::decay_t<Callback>(cb) 显式构造一个右值副本传入,EventBus 推导为
    //   非引用类型、按值持有,多边各自独立拷贝,生命周期安全。
    template <typename EventType, typename Callback>
    void subscribe(Callback&& cb) {
        using CB = std::decay_t<Callback>;
        const std::string expected = detail::prettyName<EventType>();
        for (const auto& edge : m_subEdges) {
            if (!edge.eventBus()) {
                LOG_ERROR(TAG, "subscribe<" << expected << "> edge " << edge.name()
                          << " has no eventBus, skip");
                continue;
            }
            if (!edge.dataType().empty() && edge.dataType() != expected) {
                LOG_ERROR(TAG, "subscribe<" << expected << "> edge " << edge.name()
                          << " declared dataType=" << edge.dataType() << ", mismatch, skip");
                continue;
            }
            edge.eventBus()->template subscribe<EventType>(edge.name(),
                [this, callback = CB(cb)](EventType& event) mutable {
                    try {
                        callback(event);
                    } catch (...) {
                        reportFailure(std::current_exception());
                        throw;
                    }
                });
        }
    }

    // publish<E>(e, condition):遍历 m_pubEdges,按 condition 过滤后发布。
    //   condition 空:不过滤,所有出边都发。
    //   condition 非空:只发 edge.condition()==condition 的出边。
    //   edge.eventBus() null:LOG_ERROR 跳过。
    //   e 按 const 引用转发,每条边各自拷贝(多边发布安全)。
    //   注:因 EventBus::publish 的转发引用从实参推导 EventType,若直接传 const 左值会
    //   推导出 const T&、下游 handle(Event&) 拒绝 const,故每条边本地拷贝一份 mutable
    //   副本再发,既保证多边独立拷贝又满足非 const 引用契约。
    template <typename EventType>
    void publish(const EventType& event, const std::string& condition = "") {
        for (const auto& edge : m_pubEdges) {
            if (!edge.eventBus()) {
                LOG_ERROR(TAG, "publish edge " << edge.name() << " has no eventBus, skip");
                continue;
            }
            if (edge.condition() != condition) continue;
            EventType copy(event);   // 每条边独立拷贝,避免跨边共享状态
            edge.eventBus()->publish(edge.name(), std::move(copy));
        }
    }

    // Asynchronous counterpart of publish(). Preserve the same condition
    // routing and make an independent event copy for every matching edge.
    // This is especially important for events containing movable payloads:
    // moving one event repeatedly would leave later edges with empty data.
    template <typename EventType>
    void publishAsync(const EventType& event,
                      const std::string& condition = "") {
        for (const auto& edge : m_pubEdges) {
            if (!edge.eventBus()) {
                LOG_ERROR(TAG, "publishAsync edge " << edge.name()
                          << " has no eventBus, skip");
                continue;
            }
            if (edge.condition() != condition) continue;
            EventType copy(event);
            edge.eventBus()->publishAsync(edge.name(), std::move(copy));
        }
    }

    EventBus* m_eventBus;
    Blackboard* m_blackboard;
    std::vector<GraphEdge> m_pubEdges;
    std::vector<GraphEdge> m_subEdges;
    std::atomic<bool> m_halted{false};   // 软中断标志;halt() 置,clearHalt() 清
    const std::string TAG = "GraphNode";

private:
    struct FailureState {
        mutable std::mutex mutex;
        std::exception_ptr failure;
        std::shared_ptr<FailureHandler> handler;
        void report(std::exception_ptr value) noexcept;
    };
    std::shared_ptr<FailureState> m_failureState = std::make_shared<FailureState>();
};

}  // namespace graphflow::core
