// core/lifecycle.h
#pragma once
#include "utils/logger.h"

namespace graphflow::core {

// 生命周期状态机。单向迁移 Created→Initialized→Running→Stopped，release 为终态。
enum class LifecycleState {
    Created,       // 构造完，未 init
    Initialized,   // init 成功
    Running,       // start 成功
    Stopped,       // stop 过（可再 start）
    Released       // release 过（终态）
};

// 生命周期基类（模板方法模式）。
// init/start/stop/release 为 non-virtual，封装状态迁移 + 幂等 + 乱序防护；
// 派生类 override doInit/doStart/doStop/doRelease 实现实际逻辑。
// TODO: 当前生命周期调用假定主线程线性串行，m_state 为非原子。若将来并发，
//       换 std::atomic<LifecycleState> + CAS。
class Lifecycle {
public:
    virtual ~Lifecycle() = default;

    // 幂等：已 Initialized/Running/Stopped → true；已 Released → false。
    bool init() {
        if (m_state == LifecycleState::Initialized ||
            m_state == LifecycleState::Running ||
            m_state == LifecycleState::Stopped) {
            return true;
        }
        if (m_state == LifecycleState::Released) return false;
        bool ok = doInit();
        m_state = ok ? LifecycleState::Initialized : LifecycleState::Created;
        return ok;
    }

    // 幂等：已 Running → true；未就绪（Created）或已 Released → false。
    bool start() {
        if (m_state == LifecycleState::Running) return true;
        if (m_state != LifecycleState::Initialized &&
            m_state != LifecycleState::Stopped) {
            return false;
        }
        bool ok = doStart();
        if (ok) m_state = LifecycleState::Running;
        return ok;
    }

    // 幂等：非 Running → no-op。
    void stop() {
        if (m_state != LifecycleState::Running) return;
        doStop();
        m_state = LifecycleState::Stopped;
    }

    // 幂等：已 Released → no-op；若 Running 先 stop 再 release（兜底防漏 stop）。
    void release() {
        if (m_state == LifecycleState::Released) return;
        if (m_state == LifecycleState::Running) stop();
        doRelease();
        m_state = LifecycleState::Released;
    }

    LifecycleState state() const noexcept { return m_state; }

protected:
    // 供派生组合对象（如 GraphEngine reload）显式重置状态机。
    //   仅在事务化 reload 重走 init/start 流程时使用；正常流程不应调用。
    void setState(LifecycleState s) noexcept { m_state = s; }

    virtual bool doInit() { return true; }
    virtual bool doStart() { return true; }
    virtual void doStop() {}
    virtual void doRelease() {}

private:
    LifecycleState m_state = LifecycleState::Created;
};

}  // namespace graphflow::core
