// core/event_bus.h
#pragma once
#include <any>
#include <chrono>
#include <cstddef>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <type_traits>
#include <typeinfo>
#include <unordered_map>
#include <vector>
#include "utils/logger.h"
#include "core/event.h"
#include "utoolkit/threadpool/threadpool.h"

namespace graphflow::core {

namespace detail {
struct EventBusAsyncState;
}  // namespace detail

class EventThreadPool {
public:
    // One GraphEngine owns one pool and injects it into its main and edge buses.
    // A topic always hashes to the same single-thread worker within this pool.
    explicit EventThreadPool(size_t workerCount = 1);
    ~EventThreadPool();
    EventThreadPool(const EventThreadPool&) = delete;
    EventThreadPool& operator=(const EventThreadPool&) = delete;
    void enqueue(const std::string& topic, std::function<void()> task);
private:
    class Impl;
    std::unique_ptr<Impl> m_impl;
};

class EventBus {
public:
    // threadPoolSize: 一旦懒创建池时的 worker 数。0 被 utoolkit 当作 hardware 哨兵，
    // GraphFlow 不使用——构造时兜底为 1，避免误传 0 起一堆线程。
    explicit EventBus(size_t threadPoolSize = 1);
    explicit EventBus(std::shared_ptr<EventThreadPool> threadPool);
    ~EventBus();

    // === 生命周期 ===
    // close(): 关闭总线，拒绝后续异步入队。close 后 publishAsync 记 LOG_WARN 并直接返回，
    //   不 enqueue。同步 publish 不受影响（同步派发不依赖池）。
    //   release 场景下先 close 阻止新异步任务进入队列，再 drain 等待已入队任务完成，
    //   避免回调在节点已 release 后访问已释放对象（use-after-free）。
    void close();
    // drain(): 若懒创建的 ThreadPool 已存在，调用其 shutdown() 等待已入队异步任务完成。
    //   drain 不强制 close（可独立调用），但配合 close 使用可确保 release 期间无遗留回调在跑。
    //   utoolkit ThreadPool::shutdown 语义：停止接收新任务并 join 已入队任务。
    void drain();
    bool isClosed() const;
    std::shared_ptr<EventThreadPool> threadPool();

    // === 订阅 ===
    // 同 topic 仅一个回调（重复订阅默认忽略）
    template<typename EventType, typename Callback>
    void subscribe(const std::string& topic, Callback&& cb) {
        std::lock_guard<std::mutex> lock(m_mutex);
        LOG_INFO("EventBus", "subscribe: " << topic);
        if (m_handlers.find(topic) != m_handlers.end()) {
            LOG_INFO("EventBus", "topic already subscribed: " << topic);
            return;
        }
        auto handler = std::make_shared<ConcreteEventHandler<EventType, Callback>>(
            std::forward<Callback>(cb));
        m_handlers[topic].push_back(handler);
    }

    // 同 topic 多回调
    template<typename EventType, typename Callback>
    void subscribeAll(const std::string& topic, Callback&& cb) {
        std::lock_guard<std::mutex> lock(m_mutex);
        LOG_INFO("EventBus", "subscribeAll: " << topic);
        auto handler = std::make_shared<ConcreteEventHandler<EventType, Callback>>(
            std::forward<Callback>(cb));
        m_handlers[topic].push_back(handler);
    }

    // 不带 topic：用事件类型名作默认 topic
    template<typename EventType, typename Callback>
    void subscribe(Callback&& cb) {
        subscribe<EventType>(typeid(EventType).name(),
                             std::forward<Callback>(cb));
    }

    // === 发布 ===
    // 同步发布（调用线程直接派发，转发引用）。
    // 注意：同步派发不转移所有权——handler->handle(event) 取 Event&，不 move。
    //       故左值（如 ServiceRequestEvent）与右值（临时 EventA{}）均可，靠引用推导绑定。
    template<typename EventType>
    void publish(const std::string& topic, EventType&& event) {
        static_assert(std::is_base_of_v<Event, std::decay_t<EventType>>,
                      "publish: EventType must derive from Event");
        std::vector<std::shared_ptr<EventHandler>> handlersCopy;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            auto it = m_handlers.find(topic);
            if (it != m_handlers.end()) {
                handlersCopy = it->second;
            }
        }
        LOG_DEBUG("EventBus", "publish to: " << topic
                 << ", handlers: " << handlersCopy.size());
        for (auto& handler : handlersCopy) {
            try {
                handler->handle(event);
            } catch (const std::exception& e) {
                LOG_ERROR("EventBus", "handler exception: " << e.what());
            } catch (...) {
                LOG_ERROR("EventBus", "unknown handler exception");
            }
        }
    }

    // 异步发布（ThreadPool 派发，复制一份事件）
    // 懒初始化：首次调用才创建池。在此之前 EventBus 不占任何线程。
    template<typename EventType>
    void publishAsync(const std::string& topic, EventType&& event) {
        static_assert(std::is_base_of_v<Event, std::decay_t<EventType>>,
                      "publishAsync: EventType must derive from Event");
        using EventT = std::decay_t<EventType>;
        EventT eventStorage(std::forward<EventType>(event));
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_closed) {
            LOG_WARN("EventBus", "publishAsync after close, dropped: " << topic);
            return;
        }
        auto it = m_handlers.find(topic);
        if (it == m_handlers.end() || it->second.empty()) {
            return;
        }

        // Keep the bus lock through shared-pool enqueue. close() takes the same
        // lock, so each task is either accepted and counted by drain(), or
        // rejected after close.
        for (const auto& handler : it->second) {
            LOG_DEBUG("EventBus", "publishAsync to: " << topic);
            enqueueTopicTaskLocked(
                topic, [handler, eventCopy = eventStorage]() mutable {
                    try {
                        handler->handle(eventCopy);
                    } catch (const std::exception& e) {
                        LOG_ERROR("EventBus", "handler exception: " << e.what());
                    } catch (...) {
                        LOG_ERROR("EventBus", "unknown handler exception");
                    }
                });
        }
    }

    // 不带 topic：用事件类型名作默认 topic
    template<typename EventType>
    void publish(EventType&& event) {
        publish(typeid(EventType).name(),
                std::forward<EventType>(event));
    }

    // 不带 topic 异步
    template<typename EventType>
    void publishAsync(EventType&& event) {
        publishAsync(typeid(EventType).name(),
                     std::forward<EventType>(event));
    }

    // === 管理 ===
    void unsubscribe(const std::string& topic);
    void clear();
    size_t handlerCount(const std::string& topic) const;

    // === 服务调用（通用）===
    template<typename RequestType, typename ResponseType>
    void createService(const std::string& topic,
                       ServiceHandler<RequestType, ResponseType> handler) {
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_services[topic] = handler;
        }
        subscribe<ServiceRequestEvent<RequestType, ResponseType>>(
            topic,
            [this, topic, handler](
                ServiceRequestEvent<RequestType, ResponseType>& ev) {
                if (ev.getServiceName() == topic) {
                    ResponseType resp;
                    handler(ev.getRequest(), resp);
                    ev.setResponse(resp);
                }
            });
        LOG_INFO("EventBus", "service created: " << topic);
    }

    template<typename RequestType, typename ResponseType>
    bool call(const std::string& topic, const RequestType& req, ResponseType& resp,
              std::chrono::milliseconds timeout = std::chrono::milliseconds(5000)) {
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (m_services.find(topic) == m_services.end()) {
                LOG_ERROR("EventBus", "service not found: " << topic);
                return false;
            }
        }
        auto ev = std::make_shared<ServiceRequestEvent<RequestType, ResponseType>>(
            topic, req);
        // 同步派发服务请求（publish 走同 topic 的订阅者，即 createService 注册的 handler）。
        // 不显式指定模板参数：让转发引用从 *ev（左值）推导为左值引用——
        // 旧实现显式 publish<ServiceRequestEvent<...>> 把 EventType 钉为非引用，
        // 使 EventType&& 成右值引用，无法绑定左值 *ev，导致实例化即编译失败。
        publish(topic, *ev);
        return ev->waitForResponse(resp, timeout);
    }

private:
    // Lazily create a private pool for standalone EventBus instances.
    // GraphEngine-owned buses receive the engine's shared pool instead.
    // Caller must hold m_mutex.
    EventThreadPool& ensurePoolLocked();
    void enqueueTopicTaskLocked(const std::string& topic,
                                std::function<void()> task);

    std::unordered_map<std::string, std::vector<std::shared_ptr<EventHandler>>> m_handlers;
    std::unordered_map<std::string, std::any> m_services;
    mutable std::mutex m_mutex;
    size_t m_poolSize = 1;
    std::shared_ptr<EventThreadPool> m_threadPool;
    std::shared_ptr<detail::EventBusAsyncState> m_asyncState;
    // 受 m_mutex 保护。close() 置 true 后 publishAsync 拒绝入队。
    bool m_closed = false;
};

}  // namespace graphflow::core
