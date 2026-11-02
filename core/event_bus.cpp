// core/event_bus.cpp
#include "core/event_bus.h"
#include "utoolkit/threadpool/threadpool.h"
#include <condition_variable>

namespace graphflow::core {

namespace detail {

struct EventBusAsyncState {
    std::mutex mutex;
    std::condition_variable drained;
    size_t pending = 0;
};

}  // namespace detail

class EventThreadPool::Impl {
public:
    explicit Impl(size_t workerCount)
        : workerCount_(workerCount == 0 ? 1 : workerCount) {}

    void enqueue(const std::string& topic, std::function<void()> task) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (workers_.empty()) {
            workers_.reserve(workerCount_);
            for (size_t i = 0; i < workerCount_; ++i) {
                workers_.push_back(
                    std::make_unique<utoolkit::threadpool::ThreadPool>(1));
            }
        }
        const auto index = std::hash<std::string>{}(topic) % workers_.size();
        workers_[index]->enqueue(std::move(task));
    }

private:
    size_t workerCount_;
    std::mutex mutex_;
    std::vector<std::unique_ptr<utoolkit::threadpool::ThreadPool>> workers_;
};

EventThreadPool::EventThreadPool(size_t workerCount)
    : m_impl(std::make_unique<Impl>(workerCount)) {}

EventThreadPool::~EventThreadPool() = default;

void EventThreadPool::enqueue(const std::string& topic,
                              std::function<void()> task) {
    m_impl->enqueue(topic, std::move(task));
}

EventBus::EventBus(size_t threadPoolSize)
    : m_poolSize(threadPoolSize == 0 ? 1 : threadPoolSize),
      m_asyncState(std::make_shared<detail::EventBusAsyncState>()) {
    // 不在此创建 ThreadPool：默认 0 线程占用。仅在 publishAsync 首次调用时懒创建。
    LOG_INFO("EventBus", "created (lazy pool, size=" << m_poolSize << " when activated)");
}

EventBus::EventBus(std::shared_ptr<EventThreadPool> threadPool)
    : m_threadPool(std::move(threadPool)),
      m_asyncState(std::make_shared<detail::EventBusAsyncState>()) {
    if (!m_threadPool) m_threadPool = std::make_shared<EventThreadPool>(1);
    LOG_INFO("EventBus", "created with shared topic thread pool");
}

EventBus::~EventBus() {
    close();
    drain();
}

void EventBus::close() {
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_closed) {
            LOG_INFO("EventBus", "close: already closed");
            return;
        }
        m_closed = true;
    }
    LOG_INFO("EventBus", "closed: publishAsync will drop new async events");
}

void EventBus::drain() {
    // 共享池不由单个 EventBus 关闭；只等待本总线已接受的任务完成。
    auto state = m_asyncState;
    std::unique_lock<std::mutex> lock(state->mutex);
    state->drained.wait(lock, [&] { return state->pending == 0; });
    LOG_INFO("EventBus", "drain: queued async tasks completed");
}

bool EventBus::isClosed() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_closed;
}

std::shared_ptr<EventThreadPool> EventBus::threadPool() {
    std::lock_guard<std::mutex> lock(m_mutex);
    ensurePoolLocked();
    return m_threadPool;
}

EventThreadPool& EventBus::ensurePoolLocked() {
    if (!m_threadPool) {
        m_threadPool = std::make_shared<EventThreadPool>(m_poolSize);
        LOG_INFO("EventBus", "topic thread pool activated: size=" << m_poolSize);
    }
    return *m_threadPool;
}

void EventBus::enqueueTopicTaskLocked(const std::string& topic,
                                      std::function<void()> task) {
    auto state = m_asyncState;
    {
        std::lock_guard<std::mutex> lock(state->mutex);
        ++state->pending;
    }

    try {
        ensurePoolLocked().enqueue(
            topic, [state, task = std::move(task)]() mutable {
                try {
                    task();
                } catch (...) {
                    {
                        std::lock_guard<std::mutex> lock(state->mutex);
                        --state->pending;
                    }
                    state->drained.notify_all();
                    throw;
                }
                {
                    std::lock_guard<std::mutex> lock(state->mutex);
                    --state->pending;
                }
                state->drained.notify_all();
            });
    } catch (...) {
        {
            std::lock_guard<std::mutex> lock(state->mutex);
            --state->pending;
        }
        state->drained.notify_all();
        throw;
    }
}

void EventBus::unsubscribe(const std::string& topic) {
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_handlers.find(topic);
    if (it != m_handlers.end()) {
        m_handlers.erase(it);
    }
}

void EventBus::clear() {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_handlers.clear();
    m_services.clear();
}

size_t EventBus::handlerCount(const std::string& topic) const {
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_handlers.find(topic);
    return it != m_handlers.end() ? it->second.size() : 0;
}

}  // namespace graphflow::core
