// tests/engine/graph_builder_test.cpp
// GraphBuilder 按位置建边 / 重复节点不同出边类型 / 自环 / reset / condition 语义路由测试。
// 审查 3.2：旧实现按节点名折叠邻接表，导致重复节点位置被合并，
//   A(EventA)->B->A(EventB)->C 会被错配成 A→C dataType=EventA。
//   修复后按相邻位置建边，每个位置可独立 dataType。
// condition 扩展：同起终点、同 dataType、不同 condition 是不同边（各自独立 EventBus）；
//   完全相同身份 (from,to,dataType,condition) 的边去重。
#include <memory>
#include <string>
#include <vector>
#include <gtest/gtest.h>
#include "common/test_nodes.h"
#include "engine/graph_builder.h"

using graphflow::engine::GraphBuilder;
using graphflow::engine::PathNodeSpec;
using graphflow::core::EventBus;
using graphflow::core::GraphEdge;
using graphflow_test::TestNode;

// 路径节点表类型别名，避免 build 重载歧义。
using NodeDataTypes = std::vector<std::pair<std::string, std::string>>;

namespace {
// 工具：注册节点表
struct BuilderFixture {
    EventBus eb{1};
    GraphBuilder builder;
    void reg(const std::vector<std::string>& names) {
        builder.setEventBus(&eb);
        for (const auto& n : names) {
            builder.registerNode(n, std::make_shared<TestNode>(n, &eb));
        }
    }
    // 在 node 的 pub 边中查找 (from->to, dataType, condition="")；返回命中边指针或 nullptr
    static const GraphEdge* findPub(const TestNode& n,
                                    const std::string& fromTo,
                                    const std::string& dataType) {
        return findPub(n, fromTo, dataType, "");
    }
    static const GraphEdge* findPub(const TestNode& n,
                                    const std::string& fromTo,
                                    const std::string& dataType,
                                    const std::string& condition) {
        for (const auto& e : n.getPubEdges()) {
            if (e.name() == fromTo && e.dataType() == dataType && e.condition() == condition)
                return &e;
        }
        return nullptr;
    }
    static const GraphEdge* findSub(const TestNode& n,
                                    const std::string& fromTo,
                                    const std::string& dataType) {
        return findSub(n, fromTo, dataType, "");
    }
    static const GraphEdge* findSub(const TestNode& n,
                                    const std::string& fromTo,
                                    const std::string& dataType,
                                    const std::string& condition) {
        for (const auto& e : n.getSubEdges()) {
            if (e.name() == fromTo && e.dataType() == dataType && e.condition() == condition)
                return &e;
        }
        return nullptr;
    }
};

// 取路径中指定名字的节点（按位置遍历，第一个匹配）
TestNode* nodeByName(const std::shared_ptr<graphflow::core::GraphPath>& path,
                     const std::string& name) {
    for (auto& n : path->getNodes()) {
        if (n->getName() == name) return static_cast<TestNode*>(n.get());
    }
    return nullptr;
}
}  // namespace

TEST(GraphBuilderTest, LinearBuildCreatesOneEdge) {
    BuilderFixture f;
    f.reg({"A", "B"});
    auto path = f.builder.build(NodeDataTypes{{"A", "EventA"}, {"B", ""}});
    ASSERT_NE(path, nullptr);
    auto* a = nodeByName(path, "A");
    auto* b = nodeByName(path, "B");
    ASSERT_NE(a, nullptr);
    ASSERT_NE(b, nullptr);
    EXPECT_NE(BuilderFixture::findPub(*a, "A->B", "EventA"), nullptr);
    EXPECT_NE(BuilderFixture::findSub(*b, "A->B", "EventA"), nullptr);
    EXPECT_EQ(a->getPubEdges().size(), 1u);
    EXPECT_EQ(b->getSubEdges().size(), 1u);
}

// 审查 3.2 核心：重复节点 + 不同出边类型，必须不交叉合并。
//   A(EventA)->B->A(EventB)->C
//   期望：A 的 pub 有 A->B(EventA) 与 A->C(EventB)；B sub A->B(EventA)；C sub A->C(EventB)。
//   旧实现会错误给 A->C 的 dataType=EventA（取 A 第一次出现的 dataType）。
TEST(GraphBuilderTest, RepeatedNodeDifferentEdgeTypes) {
    BuilderFixture f;
    f.reg({"A", "B", "C"});
    auto path = f.builder.build(NodeDataTypes{
        {"A", "EventA"},
        {"B", ""},
        {"A", "EventB"},
        {"C", ""},
    });
    ASSERT_NE(path, nullptr);
    auto* a = nodeByName(path, "A");
    auto* b = nodeByName(path, "B");
    auto* c = nodeByName(path, "C");
    ASSERT_NE(a, nullptr);
    ASSERT_NE(b, nullptr);
    ASSERT_NE(c, nullptr);

    // A 出两条 pub，dataType 各异
    EXPECT_NE(BuilderFixture::findPub(*a, "A->B", "EventA"), nullptr);
    EXPECT_NE(BuilderFixture::findPub(*a, "A->C", "EventB"), nullptr);
    EXPECT_EQ(a->getPubEdges().size(), 2u);
    // 不应出现错配的 A->C(EventA)
    EXPECT_EQ(BuilderFixture::findPub(*a, "A->C", "EventA"), nullptr);

    // B 订 A->B(EventA)，C 订 A->C(EventB)
    EXPECT_NE(BuilderFixture::findSub(*b, "A->B", "EventA"), nullptr);
    EXPECT_NE(BuilderFixture::findSub(*c, "A->C", "EventB"), nullptr);
    EXPECT_EQ(BuilderFixture::findSub(*c, "A->C", "EventA"), nullptr);
}

// 自环：A(EventA)->A：同一节点同时是 from 与 to，单边。
TEST(GraphBuilderTest, SelfLoop) {
    BuilderFixture f;
    f.reg({"A"});
    auto path = f.builder.build(NodeDataTypes{{"A", "EventA"}, {"A", ""}});
    ASSERT_NE(path, nullptr);
    auto* a = nodeByName(path, "A");
    ASSERT_NE(a, nullptr);
    EXPECT_NE(BuilderFixture::findPub(*a, "A->A", "EventA"), nullptr);
    EXPECT_NE(BuilderFixture::findSub(*a, "A->A", "EventA"), nullptr);
}

// 重复边：A->B 两次（同 dataType），应去重为单条。
TEST(GraphBuilderTest, DuplicateEdgeDedup) {
    BuilderFixture f;
    f.reg({"A", "B"});
    auto path = f.builder.build(NodeDataTypes{
        {"A", "EventA"}, {"B", ""},
        {"A", "EventA"}, {"B", ""},
    });
    ASSERT_NE(path, nullptr);
    auto* a = nodeByName(path, "A");
    auto* b = nodeByName(path, "B");
    ASSERT_NE(a, nullptr);
    ASSERT_NE(b, nullptr);
    EXPECT_EQ(a->getPubEdges().size(), 1u);
    EXPECT_EQ(b->getSubEdges().size(), 1u);
}

// reset 清空节点表与边总线：reset 后 registerNode 同名应成功。
TEST(GraphBuilderTest, ResetClearsState) {
    BuilderFixture f;
    f.reg({"A"});
    f.builder.reset();
    // reset 后 A 不再占用，可重新注册
    EXPECT_TRUE(f.builder.registerNode("A", std::make_shared<TestNode>("A", &f.eb)));
    std::shared_ptr<graphflow::core::GraphNode> n;
    EXPECT_TRUE(f.builder.findNode("A", n));
    EXPECT_NE(n, nullptr);
}

// 多路径共享节点：同节点出现在多次 build，pub/sub 累加。
TEST(GraphBuilderTest, MultiBuildAccumulatesEdgesOnSharedNode) {
    BuilderFixture f;
    f.reg({"A", "B", "C"});
    f.builder.build(NodeDataTypes{{"A", "EventA"}, {"B", ""}});
    f.builder.build(NodeDataTypes{{"A", "EventA"}, {"C", ""}});
    // A 的 pub 现在应有 A->B 与 A->C 两条
    std::shared_ptr<graphflow::core::GraphNode> ga;
    ASSERT_TRUE(f.builder.findNode("A", ga));
    auto* a = static_cast<graphflow_test::TestNode*>(ga.get());
    EXPECT_EQ(a->getPubEdges().size(), 2u);
}

// 空输入返回 nullptr。
TEST(GraphBuilderTest, EmptyInputReturnsNull) {
    BuilderFixture f;
    EXPECT_EQ(f.builder.build(NodeDataTypes{}), nullptr);
}

// 未注册节点抛异常。
TEST(GraphBuilderTest, UnregisteredNodeThrows) {
    BuilderFixture f;
    f.reg({"A"});
    EXPECT_THROW(f.builder.build(NodeDataTypes{{"A", "X"}, {"Missing", ""}}), std::runtime_error);
}

TEST(GraphBuilderTest, PathRetainsEdgeBusAfterBuilderReset) {
    EventBus eb(1);
    GraphBuilder builder;
    builder.setEventBus(&eb);
    auto a = std::make_shared<TestNode>("A", &eb);
    auto b = std::make_shared<TestNode>("B", &eb);
    ASSERT_TRUE(builder.registerNode("A", a));
    ASSERT_TRUE(builder.registerNode("B", b));

    auto path = builder.build(
        std::vector<std::pair<std::string, std::string>>{{"A", "data"}, {"B", ""}});
    ASSERT_NE(path, nullptr);
    auto buses = path->eventBusesSnapshot();
    ASSERT_EQ(buses.size(), 1u);

    builder.reset();
    EXPECT_FALSE(buses.front()->isClosed());
    EXPECT_EQ(path->eventBusesSnapshot().front().get(), buses.front().get());
}

// === condition 语义路由测试 ===

// 策略 1：GraphEdge::connect 写入并返回 condition；默认 condition 为空。
TEST(GraphBuilderRouteTest, ConnectPreservesRoute) {
    EventBus eb(1);
    auto e1 = GraphEdge::connect("A", "B", "EventA", &eb, "move");
    EXPECT_EQ(e1.condition(), "move");
    auto e2 = GraphEdge::connect("A", "B", "EventA", &eb);
    EXPECT_EQ(e2.condition(), "");
    // 裸名边 condition 为空
    GraphEdge bare("Topic", &eb);
    EXPECT_EQ(bare.condition(), "");
}

// 策略 2：旧 pair 接口生成的边 condition 为空。
TEST(GraphBuilderRouteTest, PairInterfaceProducesEmptyRoute) {
    BuilderFixture f;
    f.reg({"A", "B"});
    auto path = f.builder.build(NodeDataTypes{{"A", "EventA"}, {"B", ""}});
    ASSERT_NE(path, nullptr);
    auto* a = nodeByName(path, "A");
    ASSERT_NE(a, nullptr);
    const GraphEdge* e = BuilderFixture::findPub(*a, "A->B", "EventA");
    ASSERT_NE(e, nullptr);
    EXPECT_EQ(e->condition(), "");
}

// 策略 5：同一源节点同 condition 可连接多个目标，遍历全部命中（多播候选）。
TEST(GraphBuilderRouteTest, SameRouteReachesMultipleTargets) {
    BuilderFixture f;
    f.reg({"S", "X", "Y"});
    // S --(EventA,condition=r)--> X，S --(EventA,condition=r)--> Y：同 condition 两条出边。
    f.builder.build(std::vector<PathNodeSpec>{
        {"S", "EventA", "r"}, {"X", "", ""}
    });
    f.builder.build(std::vector<PathNodeSpec>{
        {"S", "EventA", "r"}, {"Y", "", ""}
    });
    std::shared_ptr<graphflow::core::GraphNode> gs;
    ASSERT_TRUE(f.builder.findNode("S", gs));
    auto* s = static_cast<TestNode*>(gs.get());
    // S 应有两条 pub 边，condition 均为 r，分别到 X 与 Y
    EXPECT_NE(BuilderFixture::findPub(*s, "S->X", "EventA", "r"), nullptr);
    EXPECT_NE(BuilderFixture::findPub(*s, "S->Y", "EventA", "r"), nullptr);
    EXPECT_EQ(s->getPubEdges().size(), 2u);
}

// 策略 6：同起终点、同 dataType、不同 condition 不被去重，且使用不同 EventBus。
TEST(GraphBuilderRouteTest, DifferentRouteNotDedupedAndDistinctBus) {
    BuilderFixture f;
    f.reg({"A", "B"});
    // A --(EventA,condition=r1)--> B 与 A --(EventA,condition=r2)--> B：两条独立边。
    f.builder.build(std::vector<PathNodeSpec>{
        {"A", "EventA", "r1"}, {"B", "", ""}
    });
    f.builder.build(std::vector<PathNodeSpec>{
        {"A", "EventA", "r2"}, {"B", "", ""}
    });
    std::shared_ptr<graphflow::core::GraphNode> ga;
    ASSERT_TRUE(f.builder.findNode("A", ga));
    auto* a = static_cast<TestNode*>(ga.get());
    EXPECT_EQ(a->getPubEdges().size(), 2u);
    const GraphEdge* e1 = BuilderFixture::findPub(*a, "A->B", "EventA", "r1");
    const GraphEdge* e2 = BuilderFixture::findPub(*a, "A->B", "EventA", "r2");
    ASSERT_NE(e1, nullptr);
    ASSERT_NE(e2, nullptr);
    // 不同 condition 必须挂在不同 EventBus，避免跨路由串流
    EXPECT_NE(e1->eventBus(), e2->eventBus());
}

// 策略 7：完全相同 (from,to,dataType,condition) 的边仍然去重。
TEST(GraphBuilderRouteTest, FullIdentityEdgeDeduped) {
    BuilderFixture f;
    f.reg({"A", "B"});
    // 同一条 A--(EventA,r)-->B 构建两次，应去重为单条。
    f.builder.build(std::vector<PathNodeSpec>{
        {"A", "EventA", "r"}, {"B", "", ""}
    });
    f.builder.build(std::vector<PathNodeSpec>{
        {"A", "EventA", "r"}, {"B", "", ""}
    });
    std::shared_ptr<graphflow::core::GraphNode> ga;
    ASSERT_TRUE(f.builder.findNode("A", ga));
    auto* a = static_cast<TestNode*>(ga.get());
    EXPECT_EQ(a->getPubEdges().size(), 1u);
    // 同 condition 去重后应复用同一 EventBus
    const GraphEdge* e = BuilderFixture::findPub(*a, "A->B", "EventA", "r");
    ASSERT_NE(e, nullptr);
    EXPECT_EQ(e->condition(), "r");
}
