// tests/core/graph_node_pubsub_test.cpp
// GraphNode 便捷 subscribe/publish + Event::typeName 单元测试。
#include <atomic>
#include <string>
#include <gtest/gtest.h>
#include "common/test_nodes.h"
#include "core/event.h"
#include "core/event_bus.h"
#include "core/graph_node.h"

using graphflow::core::EventBus;
using graphflow::core::GraphEdge;
using graphflow::core::GraphNode;
using graphflow_test::StringEvent;

// 本文件局部事件（与 minimal 示例同形）
struct PayloadEvent : graphflow::core::EventT<PayloadEvent> {
    std::string payload;
    explicit PayloadEvent(std::string p) : payload(std::move(p)) {}
};

TEST(EventTypeName, ReturnsBareClassName) {
    PayloadEvent e("x");
    EXPECT_EQ(e.typeName(), "PayloadEvent");
}

TEST(EventTypeName, SharedStringEvent) {
    StringEvent e("x");
    EXPECT_EQ(e.typeName(), "StringEvent");
}

TEST(EventTypeName, HaltEventBareName) {
    graphflow::core::HaltEvent e;
    EXPECT_EQ(e.typeName(), "HaltEvent");
}

// ---- GraphNode::subscribe 测试夹具 ----
// 测试用节点：暴露 protected subscribe/publish 给测试调用。
struct TestSubNode : GraphNode {
    explicit TestSubNode(const std::string& n, EventBus* eb) : GraphNode(n, eb) {}
    using GraphNode::subscribe;   // 提升到 public 供测试
    bool doInit() override { return true; }
    bool doStart() override { return true; }
};

TEST(GraphNodeSubscribe, BasicPathDeliversToCallback) {
    EventBus eb(1);
    TestSubNode sink("sink", nullptr);
    sink.addSubEdge(GraphEdge::connect("src", "sink", "PayloadEvent", &eb));

    std::atomic<bool> got{false};
    std::string captured;
    sink.subscribe<PayloadEvent>([&](PayloadEvent& e) {
        captured = e.payload;
        got = true;
    });
    eb.publish<PayloadEvent>("src->sink", PayloadEvent("hi"));
    EXPECT_TRUE(got.load());
    EXPECT_EQ(captured, "hi");
}

TEST(GraphNodeSubscribe, DataTypeMismatchSkipsEdge) {
    EventBus eb(1);
    TestSubNode sink("sink", nullptr);
    sink.addSubEdge(GraphEdge::connect("src", "sink", "WrongType", &eb));

    std::atomic<bool> got{false};
    sink.subscribe<PayloadEvent>([&](PayloadEvent&) { got = true; });
    eb.publish<PayloadEvent>("src->sink", PayloadEvent("hi"));   // 边未订阅,不会触发
    EXPECT_FALSE(got.load());
}

TEST(GraphNodeSubscribe, EmptyDataTypeSubscribesLoosely) {
    EventBus eb(1);
    TestSubNode sink("sink", nullptr);
    sink.addSubEdge(GraphEdge::connect("src", "sink", "", &eb));   // dataType 空

    std::atomic<bool> got{false};
    sink.subscribe<PayloadEvent>([&](PayloadEvent&) { got = true; });
    eb.publish<PayloadEvent>("src->sink", PayloadEvent("hi"));
    EXPECT_TRUE(got.load());
}

TEST(GraphNodeSubscribe, MultipleSubEdgesAllSubscribed) {
    EventBus eb(1);
    TestSubNode sink("sink", nullptr);
    sink.addSubEdge(GraphEdge::connect("a", "sink", "PayloadEvent", &eb));
    sink.addSubEdge(GraphEdge::connect("b", "sink", "PayloadEvent", &eb));

    std::atomic<int> count{0};
    sink.subscribe<PayloadEvent>([&](PayloadEvent&) { ++count; });
    eb.publish<PayloadEvent>("a->sink", PayloadEvent("1"));
    eb.publish<PayloadEvent>("b->sink", PayloadEvent("2"));
    EXPECT_EQ(count.load(), 2);
}

TEST(GraphNodeSubscribe, NullEventBusSkipsEdge) {
    TestSubNode sink("sink", nullptr);
    sink.addSubEdge(GraphEdge::connect("src", "sink", "PayloadEvent", nullptr));   // bus null

    std::atomic<bool> got{false};
    sink.subscribe<PayloadEvent>([&](PayloadEvent&) { got = true; });   // 不崩溃,仅 LOG_ERROR 跳过
    EXPECT_FALSE(got.load());
}

// ---- GraphNode::publish 测试夹具 ----
struct TestPubNode : GraphNode {
    explicit TestPubNode(const std::string& n, EventBus* eb) : GraphNode(n, eb) {}
    using GraphNode::publish;    // 提升到 public 供测试
    bool doInit() override { return true; }
    bool doStart() override { return true; }
};

TEST(GraphNodePublish, NoConditionPublishesToAllPubEdges) {
    EventBus ca(1), cb(1);
    TestPubNode src("src", nullptr);
    src.addPubEdge(GraphEdge::connect("src", "a", "PayloadEvent", &ca));
    src.addPubEdge(GraphEdge::connect("src", "b", "PayloadEvent", &cb));

    std::atomic<int> count{0};
    ca.subscribe<PayloadEvent>("src->a", [&](PayloadEvent&) { ++count; });
    cb.subscribe<PayloadEvent>("src->b", [&](PayloadEvent&) { ++count; });
    src.publish<PayloadEvent>(PayloadEvent("hi"));
    EXPECT_EQ(count.load(), 2);
}

TEST(GraphNodePublish, ConditionFiltersPubEdges) {
    EventBus ca(1), cb(1);
    TestPubNode src("src", nullptr);
    src.addPubEdge(GraphEdge::connect("src", "a", "PayloadEvent", &ca, "gather"));
    src.addPubEdge(GraphEdge::connect("src", "b", "PayloadEvent", &cb, "move"));

    std::atomic<int> gatherCount{0}, moveCount{0};
    ca.subscribe<PayloadEvent>("src->a", [&](PayloadEvent&) { ++gatherCount; });
    cb.subscribe<PayloadEvent>("src->b", [&](PayloadEvent&) { ++moveCount; });
    src.publish<PayloadEvent>(PayloadEvent("hi"), "gather");
    EXPECT_EQ(gatherCount.load(), 1);
    EXPECT_EQ(moveCount.load(), 0);
}

TEST(GraphNodePublish, NoMatchingConditionSilent) {
    EventBus ca(1);
    TestPubNode src("src", nullptr);
    src.addPubEdge(GraphEdge::connect("src", "a", "PayloadEvent", &ca, "gather"));

    std::atomic<int> count{0};
    ca.subscribe<PayloadEvent>("src->a", [&](PayloadEvent&) { ++count; });
    src.publish<PayloadEvent>(PayloadEvent("hi"), "nonexistent");   // 无匹配,静默
    EXPECT_EQ(count.load(), 0);
}

TEST(GraphNodePublish, MultipleEdgesGetIndependentCopies) {
    EventBus ca(1), cb(1);
    TestPubNode src("src", nullptr);
    src.addPubEdge(GraphEdge::connect("src", "a", "PayloadEvent", &ca));
    src.addPubEdge(GraphEdge::connect("src", "b", "PayloadEvent", &cb));

    std::string aPayload, bPayload;
    ca.subscribe<PayloadEvent>("src->a", [&](PayloadEvent& e) { aPayload = e.payload; });
    cb.subscribe<PayloadEvent>("src->b", [&](PayloadEvent& e) { bPayload = e.payload; });
    src.publish<PayloadEvent>(PayloadEvent("shared"));
    EXPECT_EQ(aPayload, "shared");
    EXPECT_EQ(bPayload, "shared");
}

TEST(GraphNodePublish, NullEventBusSkipsEdge) {
    TestPubNode src("src", nullptr);
    src.addPubEdge(GraphEdge::connect("src", "a", "PayloadEvent", nullptr));   // bus null

    std::atomic<int> count{0};
    src.publish<PayloadEvent>(PayloadEvent("hi"));   // 不崩溃,仅 LOG_ERROR 跳过
    EXPECT_EQ(count.load(), 0);
}
