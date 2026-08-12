// tests/core/lifecycle_test.cpp
// 生命周期状态机 + 失败回滚测试。
// 覆盖：单节点状态迁移、init/start/stop/release 幂等与乱序防护、
//       GraphPath doInit/doStart 失败时对已推进节点的逆序回滚。
#include <memory>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <future>
#include <mutex>
#include <vector>
#include <gtest/gtest.h>
#include "common/test_nodes.h"
#include "core/graph_path.h"

using graphflow_test::TestNode;
using graphflow::core::EventBus;
using graphflow::core::GraphPath;
using graphflow::core::LifecycleState;
using graphflow::core::NodeBase;

// ---------------------------------------------------------------------------
// 1. 单节点状态迁移：Created→init→Initialized→start→Running→stop→Stopped
//    →release→Released
// ---------------------------------------------------------------------------
TEST(LifecycleTest, SingleNodeStateTransitions) {
    EventBus eb(1);
    auto n = std::make_shared<TestNode>("n", &eb);

    EXPECT_EQ(n->state(), LifecycleState::Created);

    EXPECT_TRUE(n->init());
    EXPECT_EQ(n->state(), LifecycleState::Initialized);
    EXPECT_EQ(n->initCount.load(), 1);

    EXPECT_TRUE(n->start());
    EXPECT_EQ(n->state(), LifecycleState::Running);
    EXPECT_EQ(n->startCount.load(), 1);

    n->stop();
    EXPECT_EQ(n->state(), LifecycleState::Stopped);
    EXPECT_EQ(n->stopCount.load(), 1);

    n->release();
    EXPECT_EQ(n->state(), LifecycleState::Released);
    EXPECT_EQ(n->releaseCount.load(), 1);
}

// ---------------------------------------------------------------------------
// 2. 幂等：已 Initialized 再 init→true（doInit 不再调用，initCount 不增）
// ---------------------------------------------------------------------------
TEST(LifecycleTest, InitIsIdempotentWhenInitialized) {
    EventBus eb(1);
    auto n = std::make_shared<TestNode>("n", &eb);
    ASSERT_TRUE(n->init());
    ASSERT_EQ(n->initCount.load(), 1);

    EXPECT_TRUE(n->init());                       // 幂等短路
    EXPECT_EQ(n->state(), LifecycleState::Initialized);
    EXPECT_EQ(n->initCount.load(), 1);            // doInit 未被再次调用
}

// ---------------------------------------------------------------------------
// 2. 幂等：已 Running 再 start→true（doStart 不再调用，startCount 不增）
// ---------------------------------------------------------------------------
TEST(LifecycleTest, StartIsIdempotentWhenRunning) {
    EventBus eb(1);
    auto n = std::make_shared<TestNode>("n", &eb);
    ASSERT_TRUE(n->init());
    ASSERT_TRUE(n->start());
    ASSERT_EQ(n->startCount.load(), 1);

    EXPECT_TRUE(n->start());                       // 幂等短路
    EXPECT_EQ(n->state(), LifecycleState::Running);
    EXPECT_EQ(n->startCount.load(), 1);           // doStart 未被再次调用
}

// ---------------------------------------------------------------------------
// 2. 幂等：非 Running 的 stop→no-op（stopCount 不增）
// ---------------------------------------------------------------------------
TEST(LifecycleTest, StopIsNoOpWhenNotRunning) {
    EventBus eb(1);
    auto n = std::make_shared<TestNode>("n", &eb);
    ASSERT_TRUE(n->init());                        // Initialized（非 Running）

    n->stop();                                     // no-op
    EXPECT_EQ(n->state(), LifecycleState::Initialized);
    EXPECT_EQ(n->stopCount.load(), 0);            // doStop 未被调用

    // Stopped 态再 stop 同样 no-op
    ASSERT_TRUE(n->start());
    n->stop();                                     // Running→Stopped
    ASSERT_EQ(n->stopCount.load(), 1);
    n->stop();                                     // 已 Stopped，no-op
    EXPECT_EQ(n->stopCount.load(), 1);
}

// ---------------------------------------------------------------------------
// 2. 幂等：已 Released 再 release→no-op（releaseCount 不增）
// ---------------------------------------------------------------------------
TEST(LifecycleTest, ReleaseIsNoOpAfterReleased) {
    EventBus eb(1);
    auto n = std::make_shared<TestNode>("n", &eb);
    ASSERT_TRUE(n->init());
    n->release();
    ASSERT_EQ(n->releaseCount.load(), 1);
    ASSERT_EQ(n->state(), LifecycleState::Released);

    n->release();                                  // 终态 no-op
    EXPECT_EQ(n->state(), LifecycleState::Released);
    EXPECT_EQ(n->releaseCount.load(), 1);
}

// ---------------------------------------------------------------------------
// 3. 乱序防护：Created 直接 start→false（state 不变）
// ---------------------------------------------------------------------------
TEST(LifecycleTest, StartRejectedFromCreated) {
    EventBus eb(1);
    auto n = std::make_shared<TestNode>("n", &eb);
    ASSERT_EQ(n->state(), LifecycleState::Created);

    EXPECT_FALSE(n->start());                      // 未 init，拒绝
    EXPECT_EQ(n->state(), LifecycleState::Created);
    EXPECT_EQ(n->startCount.load(), 0);           // doStart 未被调用
}

// ---------------------------------------------------------------------------
// 3. 乱序防护：Released 后 init→false
// ---------------------------------------------------------------------------
TEST(LifecycleTest, InitRejectedFromReleased) {
    EventBus eb(1);
    auto n = std::make_shared<TestNode>("n", &eb);
    ASSERT_TRUE(n->init());
    n->release();
    ASSERT_EQ(n->state(), LifecycleState::Released);

    EXPECT_FALSE(n->init());                       // 终态不可回退
    EXPECT_EQ(n->state(), LifecycleState::Released);
    EXPECT_EQ(n->initCount.load(), 1);            // 未再次调用 doInit
}

// ---------------------------------------------------------------------------
// 4. 失败回滚（核心）：单节点 doStart 失败。
//    failOnStartIndex=0 → 第一次 start 即返回 false。
//    GraphPath::doStart 无已推进节点，started 为空，不触发逆序 stop。
//    断言：path.start() 返回 false；该节点 startCount==1（失败那次也算）；
//          stopCount==0；节点未进入 Running（仍 Initialized）；path 仍 Initialized。
// ---------------------------------------------------------------------------
TEST(LifecycleTest, SingleNodeStartFailureRollback) {
    EventBus eb(1);
    auto n = std::make_shared<TestNode>("n", &eb);
    n->failOnStartIndex = 0;                       // 第一次 start 即失败
    std::vector<std::shared_ptr<NodeBase>> nodes = {n};
    GraphPath path(&eb, nodes);

    ASSERT_TRUE(path.init());                      // 所有节点 init 成功
    ASSERT_EQ(n->state(), LifecycleState::Initialized);

    EXPECT_FALSE(path.start());                    // start 失败
    EXPECT_EQ(n->startCount.load(), 1);           // 失败那次 doStart 被调用
    EXPECT_EQ(n->stopCount.load(), 0);            // 无已推进节点，未触发回滚 stop
    EXPECT_EQ(n->state(), LifecycleState::Initialized);  // doStart 失败不改状态
    EXPECT_EQ(path.state(), LifecycleState::Initialized);
}

// ---------------------------------------------------------------------------
// 4. 失败回滚（核心）：多节点 doStart 失败，前 N-1 个被逆序 stop。
//    3 个节点：n0、n1 正常，n2 failOnStartIndex=0。
//    GraphPath::doStart 顺序 start n0(OK)、n1(OK)、n2(FAIL)；
//    失败后逆序 stop started=[n0,n1] → 先 stop n1 再 stop n0。
//    断言：n0/n1 stopCount>=1；n2 startCount==1、stopCount==0；
//          无节点残留 Running；逆序（n0.lastStopSeq > n1.lastStopSeq）。
// ---------------------------------------------------------------------------
TEST(LifecycleTest, MultiNodeStartFailureRollback) {
    EventBus eb(1);
    auto n0 = std::make_shared<TestNode>("n0", &eb);
    auto n1 = std::make_shared<TestNode>("n1", &eb);
    auto n2 = std::make_shared<TestNode>("n2", &eb);
    n2->failOnStartIndex = 0;                       // n2 第一次 start 即失败
    std::vector<std::shared_ptr<NodeBase>> nodes = {n0, n1, n2};
    GraphPath path(&eb, nodes);

    ASSERT_TRUE(path.init());                       // 所有节点 init 成功
    ASSERT_EQ(n0->state(), LifecycleState::Initialized);
    ASSERT_EQ(n1->state(), LifecycleState::Initialized);
    ASSERT_EQ(n2->state(), LifecycleState::Initialized);

    EXPECT_FALSE(path.start());                     // n2 start 失败 → 整体失败

    // 前 N-1 个（n0、n1）被回滚 stop
    EXPECT_GE(n0->stopCount.load(), 1);
    EXPECT_GE(n1->stopCount.load(), 1);
    // 失败节点 n2：失败那次 doStart 被调用，但未被 stop
    EXPECT_EQ(n2->startCount.load(), 1);
    EXPECT_EQ(n2->stopCount.load(), 0);

    // 无节点残留 Running：n0/n1 已 Stopped，n2 仍 Initialized
    EXPECT_EQ(n0->state(), LifecycleState::Stopped);
    EXPECT_EQ(n1->state(), LifecycleState::Stopped);
    EXPECT_EQ(n2->state(), LifecycleState::Initialized);
    EXPECT_EQ(path.state(), LifecycleState::Initialized);

    // 逆序：n0 后被 stop（序号更大），n1 先被 stop（序号更小）
    EXPECT_GT(n0->lastStopSeq.load(), n1->lastStopSeq.load());
}

// ---------------------------------------------------------------------------
// 4. 失败回滚（核心）：单节点 doInit 失败。
//    failOnInitIndex=0 → 第一次 init 即返回 false。
//    GraphPath::doInit 无已推进节点，不触发逆序 release。
//    断言：path.init() 返回 false；initCount==1；releaseCount==0；
//          节点仍 Created；path 仍 Created。
// ---------------------------------------------------------------------------
TEST(LifecycleTest, SingleNodeInitFailureRollback) {
    EventBus eb(1);
    auto n = std::make_shared<TestNode>("n", &eb);
    n->failOnInitIndex = 0;                         // 第一次 init 即失败
    std::vector<std::shared_ptr<NodeBase>> nodes = {n};
    GraphPath path(&eb, nodes);

    EXPECT_FALSE(path.init());                      // init 失败
    EXPECT_EQ(n->initCount.load(), 1);            // 失败那次 doInit 被调用
    EXPECT_EQ(n->releaseCount.load(), 0);         // 无已推进节点，未触发回滚 release
    EXPECT_EQ(n->state(), LifecycleState::Created);  // doInit 失败回 Created
    EXPECT_EQ(path.state(), LifecycleState::Created);
}

// ---------------------------------------------------------------------------
// 4. 失败回滚（核心）：多节点 doInit 失败，前 N-1 个被逆序 release。
//    3 个节点：n0、n1 正常，n2 failOnInitIndex=0。
//    GraphPath::doInit 顺序 init n0(OK)、n1(OK)、n2(FAIL)；
//    失败后逆序 release started=[n0,n1] → 先 release n1 再 release n0。
//    断言：n0/n1 releaseCount>=1；n2 initCount==1、releaseCount==0；
//          n0/n1 终态 Released，n2 仍 Created；逆序（n0.lastReleaseSeq > n1.lastReleaseSeq）。
// ---------------------------------------------------------------------------
TEST(LifecycleTest, MultiNodeInitFailureRollback) {
    EventBus eb(1);
    auto n0 = std::make_shared<TestNode>("n0", &eb);
    auto n1 = std::make_shared<TestNode>("n1", &eb);
    auto n2 = std::make_shared<TestNode>("n2", &eb);
    n2->failOnInitIndex = 0;                        // n2 第一次 init 即失败
    std::vector<std::shared_ptr<NodeBase>> nodes = {n0, n1, n2};
    GraphPath path(&eb, nodes);

    EXPECT_FALSE(path.init());                      // n2 init 失败 → 整体失败

    // 前 N-1 个（n0、n1）被回滚 release
    EXPECT_GE(n0->releaseCount.load(), 1);
    EXPECT_GE(n1->releaseCount.load(), 1);
    // 失败节点 n2：doInit 被调用一次，未被 release
    EXPECT_EQ(n2->initCount.load(), 1);
    EXPECT_EQ(n2->releaseCount.load(), 0);

    // n0/n1 已 Released，n2 仍 Created
    EXPECT_EQ(n0->state(), LifecycleState::Released);
    EXPECT_EQ(n1->state(), LifecycleState::Released);
    EXPECT_EQ(n2->state(), LifecycleState::Created);
    EXPECT_EQ(path.state(), LifecycleState::Created);

    // 逆序：n0 后被 release（序号更大），n1 先被 release（序号更小）
    EXPECT_GT(n0->lastReleaseSeq.load(), n1->lastReleaseSeq.load());
}

TEST(LifecycleTest, InitRollbackDoesNotReleasePreviouslyInitializedSharedNode) {
    EventBus eb(1);
    auto shared = std::make_shared<TestNode>("shared", &eb);
    auto failing = std::make_shared<TestNode>("failing", &eb);
    failing->failOnInitIndex = 0;
    ASSERT_TRUE(shared->init());

    GraphPath path(&eb, std::vector<std::shared_ptr<NodeBase>>{shared, failing});
    EXPECT_FALSE(path.init());

    EXPECT_EQ(shared->state(), LifecycleState::Initialized);
    EXPECT_EQ(shared->releaseCount.load(), 0);
}

TEST(LifecycleTest, StartRollbackDoesNotStopPreviouslyRunningSharedNode) {
    EventBus eb(1);
    auto shared = std::make_shared<TestNode>("shared", &eb);
    auto failing = std::make_shared<TestNode>("failing", &eb);
    ASSERT_TRUE(shared->init());
    ASSERT_TRUE(shared->start());
    ASSERT_TRUE(failing->init());
    failing->failOnStartIndex = 0;

    GraphPath path(&eb, std::vector<std::shared_ptr<NodeBase>>{shared, failing});
    ASSERT_TRUE(path.init());
    EXPECT_FALSE(path.start());

    EXPECT_EQ(shared->state(), LifecycleState::Running);
    EXPECT_EQ(shared->stopCount.load(), 0);
}

TEST(LifecycleTest, PathReleaseDrainsOwnedBusesBeforeReleasingNodes) {
    struct ReleaseObservingNode : TestNode {
        ReleaseObservingNode(EventBus* bus, std::atomic<bool>& callbackDone,
                             std::atomic<bool>& releaseSawDone)
            : TestNode("observer", bus),
              callbackDone(callbackDone),
              releaseSawDone(releaseSawDone) {}
        void doRelease() override {
            releaseSawDone = callbackDone.load();
            TestNode::doRelease();
        }
        std::atomic<bool>& callbackDone;
        std::atomic<bool>& releaseSawDone;
    };

    auto edgeBus = std::make_shared<EventBus>(1);
    std::atomic<bool> callbackDone{false};
    std::atomic<bool> releaseSawDone{false};
    std::mutex mutex;
    std::condition_variable cv;
    bool started = false;
    bool mayFinish = false;
    edgeBus->subscribe<graphflow_test::StringEvent>("edge", [&](auto&) {
        {
            std::lock_guard<std::mutex> lock(mutex);
            started = true;
        }
        cv.notify_all();
        std::unique_lock<std::mutex> lock(mutex);
        cv.wait(lock, [&] { return mayFinish; });
        callbackDone = true;
    });

    auto node = std::make_shared<ReleaseObservingNode>(
        edgeBus.get(), callbackDone, releaseSawDone);
    GraphPath path(nullptr, std::vector<std::shared_ptr<NodeBase>>{node});
    path.retainEventBus(edgeBus);
    edgeBus->publishAsync("edge", graphflow_test::StringEvent("data"));
    {
        std::unique_lock<std::mutex> lock(mutex);
        ASSERT_TRUE(cv.wait_for(lock, std::chrono::seconds(2), [&] { return started; }));
    }

    auto releasing = std::async(std::launch::async, [&] { path.release(); });
    EXPECT_EQ(releasing.wait_for(std::chrono::milliseconds(50)), std::future_status::timeout);
    {
        std::lock_guard<std::mutex> lock(mutex);
        mayFinish = true;
    }
    cv.notify_all();
    EXPECT_EQ(releasing.wait_for(std::chrono::seconds(2)), std::future_status::ready);
    EXPECT_TRUE(releaseSawDone.load());
}
