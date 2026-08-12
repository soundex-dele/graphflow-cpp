// tests/core/event_bus_test.cpp
// EventBus 订阅/发布/服务调用/close/drain 测试。完整用例由子代理补全。
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <future>
#include <mutex>
#include <string>
#include <thread>
#include <gtest/gtest.h>
#include "common/test_nodes.h"

using graphflow::core::EventBus;
using graphflow::core::ServiceRequestEvent;
using graphflow_test::StringEvent;

TEST(EventBusSmoke, SubscribePublish) {
    EventBus eb(1);
    bool got = false;
    eb.subscribe<StringEvent>("topic", [&](StringEvent& e) { got = true; });
    eb.publish<StringEvent>("topic", StringEvent("hi"));
    EXPECT_TRUE(got);
}

// 验证 createService + call 可编译并可同步返回（审查 3.1：旧实现实例化即编译失败）。
TEST(EventBusSmoke, ServiceCallReturnsResponse) {
    EventBus eb(1);
    eb.createService<int, int>("square", [](const int& req, int& resp) { resp = req * req; });
    int resp = 0;
    bool ok = eb.call<int, int>("square", 4, resp, std::chrono::milliseconds(1000));
    EXPECT_TRUE(ok);
    EXPECT_EQ(resp, 16);
}

// 服务不存在立即返回 false。
TEST(EventBusSmoke, ServiceNotFoundReturnsFalse) {
    EventBus eb(1);
    int resp = 0;
    bool ok = eb.call<int, int>("nope", 1, resp, std::chrono::milliseconds(200));
    EXPECT_FALSE(ok);
}

TEST(EventBusLifecycle, DrainWaitsForAcceptedTasksAndCloseRejectsNewTasks) {
    EventBus eb(1);
    std::mutex mutex;
    std::condition_variable cv;
    bool started = false;
    bool mayFinish = false;
    std::atomic<int> completed{0};

    eb.subscribe<StringEvent>("topic", [&](StringEvent&) {
        {
            std::lock_guard<std::mutex> lock(mutex);
            started = true;
        }
        cv.notify_all();
        std::unique_lock<std::mutex> lock(mutex);
        cv.wait(lock, [&] { return mayFinish; });
        ++completed;
    });
    eb.publishAsync("topic", StringEvent("accepted"));

    {
        std::unique_lock<std::mutex> lock(mutex);
        ASSERT_TRUE(cv.wait_for(lock, std::chrono::seconds(2), [&] { return started; }));
    }
    auto draining = std::async(std::launch::async, [&] {
        eb.close();
        eb.drain();
    });
    EXPECT_EQ(draining.wait_for(std::chrono::milliseconds(50)), std::future_status::timeout);

    {
        std::lock_guard<std::mutex> lock(mutex);
        mayFinish = true;
    }
    cv.notify_all();
    EXPECT_EQ(draining.wait_for(std::chrono::seconds(2)), std::future_status::ready);
    EXPECT_EQ(completed.load(), 1);

    eb.publishAsync("topic", StringEvent("dropped"));
    EXPECT_EQ(completed.load(), 1);
}

TEST(EventBusAsyncRouting, SameTopicAcrossBusesUsesSameWorkerThread) {
    auto pool = std::make_shared<graphflow::core::EventThreadPool>(2);
    EventBus first(pool);
    EventBus second(pool);
    std::thread::id firstThread;
    std::thread::id secondThread;

    first.subscribe<StringEvent>("shared-topic", [&](StringEvent&) {
        firstThread = std::this_thread::get_id();
    });
    second.subscribe<StringEvent>("shared-topic", [&](StringEvent&) {
        secondThread = std::this_thread::get_id();
    });

    first.publishAsync("shared-topic", StringEvent("first"));
    second.publishAsync("shared-topic", StringEvent("second"));
    first.close();
    second.close();
    first.drain();
    second.drain();

    EXPECT_NE(firstThread, std::thread::id{});
    EXPECT_EQ(firstThread, secondThread);
}
