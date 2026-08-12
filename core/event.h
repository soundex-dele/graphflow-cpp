// core/event.h
#pragma once
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <typeinfo>

namespace graphflow::core { namespace detail {

// 从 __FUNCSIG__(MSVC) / __PRETTY_FUNCTION__(GCC, Clang) 解析 Derived 的裸类名。
//   MSVC:       ... prettyName<class ns::PayloadEvent>(void)
//   GCC:        ... prettyName() [with Derived = ns::PayloadEvent; ...]
//   AppleClang: ... prettyName() [Derived = ns::PayloadEvent]
template <typename Derived>
inline std::string prettyName() {
#if defined(_MSC_VER)
    const char* sig = __FUNCSIG__;
    constexpr const char* marker = "prettyName<";
#elif defined(__GNUC__) || defined(__clang__)
    const char* sig = __PRETTY_FUNCTION__;
    constexpr const char* marker = "Derived = ";
#else
    #error "unsupported compiler: need __FUNCSIG__ or __PRETTY_FUNCTION__"
#endif
    std::string s(sig);
    const auto markerPos = s.find(marker);
    if (markerPos == std::string::npos) return s;

    const auto begin = markerPos + std::char_traits<char>::length(marker);
#if defined(_MSC_VER)
    const auto end = s.find('>', begin);
#else
    // GCC appends aliases after ';'; AppleClang closes the substitution list
    // directly with ']'.
    const auto end = s.find_first_of(";]", begin);
#endif
    if (end == std::string::npos) return s;

    std::string inner = s.substr(begin, end - begin);
    while (!inner.empty() && inner.front() == ' ') inner.erase(inner.begin());
    for (const char* prefix : {"class ", "struct ", "enum "}) {
        const std::string value(prefix);
        if (inner.rfind(value, 0) == 0) {
            inner.erase(0, value.size());
            break;
        }
    }

    // Strip only the namespace qualifying the outer type. Looking before the
    // first template argument avoids mistaking a nested template argument's
    // namespace for the outer namespace.
    const auto templatePos = inner.find('<');
    const auto scope = inner.rfind(
        "::", templatePos == std::string::npos ? inner.size() : templatePos);
    if (scope != std::string::npos) inner.erase(0, scope + 2);
    return inner;
}

} }  // namespace graphflow::core::detail

namespace graphflow::core {

// 事件基类（保留 RTTI）
class Event {
public:
    virtual ~Event() = default;
    virtual const std::type_info& getType() const = 0;
    virtual std::string typeName() const = 0;
};

// CRTP 助手：派生类继承 EventT<自己> 即自动获得 getType()，不必再手写样板。
//   struct FooEvent : EventT<FooEvent> { ... };   // 不用再写 getType()
// 语义等价于手写 `return typeid(FooEvent)`：再从 FooEvent 派生子类且不重写时，
//   getType() 仍返回 typeid(FooEvent)（与手写行为一致），不是子类类型。
template <typename Derived>
class EventT : public Event {
public:
    const std::type_info& getType() const override { return typeid(Derived); }

    std::string typeName() const override {
        static const std::string name = detail::prettyName<Derived>();
        return name;
    }
};

// 事件处理器接口
class EventHandler {
public:
    virtual ~EventHandler() = default;
    virtual void handle(Event& event) = 0;
};

// 具体事件处理器（模板）
template<typename EventType, typename Callback>
class ConcreteEventHandler : public EventHandler {
private:
    Callback m_callback;

public:
    explicit ConcreteEventHandler(Callback callback)
        : m_callback(std::move(callback)) {}

    void handle(Event& event) override {
        // 守卫通过则类型已确认，下转良定义。static_cast 非 dynamic_cast，
        // 不返回 null，故无需 if 包装——安全责任在 getType() 比较上，不骗读者。
        if (event.getType() == typeid(EventType)) {
            m_callback(static_cast<EventType&>(event));
        }
    }
};

// 通用服务请求事件
template<typename RequestType, typename ResponseType>
class ServiceRequestEvent : public Event {
public:
    explicit ServiceRequestEvent(const std::string& serviceName,
                                 const RequestType& request)
        : m_serviceName(serviceName),
          m_request(request),
          m_responseReady(false) {}

    const std::type_info& getType() const override {
        return typeid(ServiceRequestEvent);
    }

    std::string typeName() const override { return "ServiceRequestEvent"; }

    const std::string& getServiceName() const { return m_serviceName; }
    const RequestType& getRequest() const { return m_request; }

    void setResponse(const ResponseType& response) {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_response = response;
        m_responseReady = true;
        m_cv.notify_one();
    }

    bool waitForResponse(
        ResponseType& response,
        std::chrono::milliseconds timeout = std::chrono::milliseconds(5000)) {
        std::unique_lock<std::mutex> lock(m_mutex);
        if (m_cv.wait_for(lock, timeout, [this] { return m_responseReady; })) {
            response = m_response;
            return true;
        }
        return false;
    }

private:
    std::string m_serviceName;
    RequestType m_request;
    ResponseType m_response;
    bool m_responseReady;
    std::mutex m_mutex;
    std::condition_variable m_cv;
};

// 服务响应事件
template<typename ResponseType>
class ServiceResponseEvent : public Event {
public:
    explicit ServiceResponseEvent(const ResponseType& response)
        : m_response(response) {}

    const std::type_info& getType() const override {
        return typeid(ServiceResponseEvent);
    }

    std::string typeName() const override { return "ServiceResponseEvent"; }

    const ResponseType& getResponse() const { return m_response; }

private:
    ResponseType m_response;
};

// 服务处理器类型
template<typename RequestType, typename ResponseType>
using ServiceHandler = std::function<void(const RequestType&, ResponseType&)>;

// 软中断事件(通用原语,非 BT 专用):撤回一次正在流转的执行。
// BT 层用于 Selector 抢占 / Parallel 取消未完成子;其他图用户亦可消费。
// 携带 tickId 供 BT 层关联"中断哪个 tick",core 不解释其语义。
struct HaltEvent : EventT<HaltEvent> {
    uint64_t tickId = 0;
};

}  // namespace graphflow::core
