// tests/engine/json_graph_builder_test.cpp
// JsonGraphBuilder 校验/事务化/BuildError 测试。
//
// 覆盖：
//   1. 合法配置：ok()=true，paths 非空，路径节点数正确
//   2. 空对象 {}：ok()=false（unsupported format）
//   3. 缺 nodes 或 paths：ok()=false
//   4. 节点缺 id 或 class：ok()=false，errors 含 /nodes/i jsonPointer
//   5. 重复 node id：ok()=false
//   6. 未知节点引用（审查 3.5）：path 引用未声明 id，ok()=false，paths 为空（不产生部分图）
//   7. 空 path 数组：ok()=false
//   8. 节点 class 未注册 creator：ok()=false（createNode 返 nullptr）
//   9. 多路径合法：paths.size()==路径数
//   10. 事务性：合法→非法→合法，第三次仍成功，不污染状态
//
// 依赖注入：EventBus + NodeFactory + 自定义 NodeCreator（把 "TestNode" 映射到
//   graphflow_test::TestNode）。
#include <memory>
#include <string>
#include <gtest/gtest.h>
#include <nlohmann/json.hpp>
#include "common/test_nodes.h"
#include "core/event_bus.h"
#include "engine/json_graph_builder.h"
#include "engine/node_factory.h"

using graphflow::core::EventBus;
using graphflow::core::GraphNode;
using graphflow::core::GraphPath;
using graphflow::engine::BuildResult;
using graphflow::engine::JsonGraphBuilder;
using graphflow::engine::NodeCreator;
using graphflow::engine::NodeFactory;
using graphflow_test::TestNode;
using json = nlohmann::json;

namespace {

// 把 "TestNode" class 映射到 graphflow_test::TestNode 的创建器。
class TestNodeCreator : public NodeCreator {
public:
    std::shared_ptr<GraphNode> createNode(
        const graphflow::engine::NodeConfig& cfg) override {
        return std::make_shared<TestNode>(cfg.nodeName, cfg.eventBus);
    }
};

// 返回一个注册了 TestNode creator 的 NodeFactory（每次调用独立实例）。
std::shared_ptr<NodeFactory> makeFactory() {
    auto f = std::make_shared<NodeFactory>();
    f->registerCreator("TestNode", std::make_shared<TestNodeCreator>());
    return f;
}

}  // namespace

TEST(JsonGraphBuilderTest, ValidConfigBuildsOnePathWithTwoNodes) {
    EventBus eb{1};
    auto factory = makeFactory();
    JsonGraphBuilder builder;
    builder.setEventBus(&eb);
    builder.setNodeFactory(factory.get());

    auto j = json::parse(R"({
        "nodes":[{"id":"A","class":"TestNode"},{"id":"B","class":"TestNode"}],
        "paths":[{"name":"p1","path":[{"id":"A","edge":"EventA"},{"id":"B","edge":""}]}]
    })");
    auto result = builder.buildFromJson(j);

    EXPECT_TRUE(result.ok()) << "valid config should succeed";
    EXPECT_EQ(result.paths.size(), 1u);
    ASSERT_FALSE(result.paths.empty());
    EXPECT_EQ(result.paths[0]->getNodes().size(), 2u);
}

TEST(JsonGraphBuilderTest, EmptyObjectRejectedAsUnsupportedFormat) {
    EventBus eb{1};
    auto factory = makeFactory();
    JsonGraphBuilder builder;
    builder.setEventBus(&eb);
    builder.setNodeFactory(factory.get());

    auto result = builder.buildFromJson(json::object());

    EXPECT_FALSE(result.ok());
    EXPECT_FALSE(result.errors.empty());
    EXPECT_TRUE(result.paths.empty());
}

TEST(JsonGraphBuilderTest, MissingNodesRejected) {
    EventBus eb{1};
    auto factory = makeFactory();
    JsonGraphBuilder builder;
    builder.setEventBus(&eb);
    builder.setNodeFactory(factory.get());

    auto j = json::parse(R"({
        "paths":[{"name":"p1","path":[{"id":"A","edge":"EventA"},{"id":"B","edge":""}]}]
    })");
    auto result = builder.buildFromJson(j);

    EXPECT_FALSE(result.ok());
    EXPECT_TRUE(result.paths.empty());
}

TEST(JsonGraphBuilderTest, MissingPathsRejected) {
    EventBus eb{1};
    auto factory = makeFactory();
    JsonGraphBuilder builder;
    builder.setEventBus(&eb);
    builder.setNodeFactory(factory.get());

    auto j = json::parse(R"({
        "nodes":[{"id":"A","class":"TestNode"},{"id":"B","class":"TestNode"}]
    })");
    auto result = builder.buildFromJson(j);

    EXPECT_FALSE(result.ok());
    EXPECT_TRUE(result.paths.empty());
}

TEST(JsonGraphBuilderTest, NodeMissingIdReportsJsonPointer) {
    EventBus eb{1};
    auto factory = makeFactory();
    JsonGraphBuilder builder;
    builder.setEventBus(&eb);
    builder.setNodeFactory(factory.get());

    // 第二个节点缺 id（仅 class）
    auto j = json::parse(R"({
        "nodes":[{"id":"A","class":"TestNode"},{"class":"TestNode"}],
        "paths":[{"name":"p1","path":[{"id":"A","edge":"EventA"},{"id":"B","edge":""}]}]
    })");
    auto result = builder.buildFromJson(j);

    EXPECT_FALSE(result.ok());
    ASSERT_FALSE(result.errors.empty());
    // jsonPointer 指向 /nodes/1
    bool hasNodesPtr = false;
    for (const auto& e : result.errors) {
        if (e.jsonPointer.find("/nodes/1") != std::string::npos) {
            hasNodesPtr = true;
            break;
        }
    }
    EXPECT_TRUE(hasNodesPtr) << "error should carry /nodes/1 jsonPointer";
    EXPECT_TRUE(result.paths.empty());
}

TEST(JsonGraphBuilderTest, NodeMissingClassReportsJsonPointer) {
    EventBus eb{1};
    auto factory = makeFactory();
    JsonGraphBuilder builder;
    builder.setEventBus(&eb);
    builder.setNodeFactory(factory.get());

    // 第一个节点缺 class（仅 id）
    auto j = json::parse(R"({
        "nodes":[{"id":"A"},{"id":"B","class":"TestNode"}],
        "paths":[{"name":"p1","path":[{"id":"A","edge":"EventA"},{"id":"B","edge":""}]}]
    })");
    auto result = builder.buildFromJson(j);

    EXPECT_FALSE(result.ok());
    ASSERT_FALSE(result.errors.empty());
    bool hasNodesPtr = false;
    for (const auto& e : result.errors) {
        if (e.jsonPointer.find("/nodes/0") != std::string::npos) {
            hasNodesPtr = true;
            break;
        }
    }
    EXPECT_TRUE(hasNodesPtr) << "error should carry /nodes/0 jsonPointer";
    EXPECT_TRUE(result.paths.empty());
}

TEST(JsonGraphBuilderTest, DuplicateNodeIdRejected) {
    EventBus eb{1};
    auto factory = makeFactory();
    JsonGraphBuilder builder;
    builder.setEventBus(&eb);
    builder.setNodeFactory(factory.get());

    auto j = json::parse(R"({
        "nodes":[{"id":"A","class":"TestNode"},{"id":"A","class":"TestNode"}],
        "paths":[{"name":"p1","path":[{"id":"A","edge":"EventA"},{"id":"A","edge":""}]}]
    })");
    auto result = builder.buildFromJson(j);

    EXPECT_FALSE(result.ok());
    ASSERT_FALSE(result.errors.empty());
    bool foundDup = false;
    for (const auto& e : result.errors) {
        if (e.message.find("duplicate") != std::string::npos) {
            foundDup = true;
            break;
        }
    }
    EXPECT_TRUE(foundDup) << "error message should mention duplicate";
    EXPECT_TRUE(result.paths.empty());
}

// 审查 3.5 核心：path 引用未在 nodes 声明的 id 必须失败，且不产生部分图。
TEST(JsonGraphBuilderTest, UnknownNodeReferenceRejectedAndNoPartialGraph) {
    EventBus eb{1};
    auto factory = makeFactory();
    JsonGraphBuilder builder;
    builder.setEventBus(&eb);
    builder.setNodeFactory(factory.get());

    // A、B 已声明；path 引用了未声明的 "C"
    auto j = json::parse(R"({
        "nodes":[{"id":"A","class":"TestNode"},{"id":"B","class":"TestNode"}],
        "paths":[{"name":"p1","path":[{"id":"A","edge":"EventA"},{"id":"C","edge":""}]}]
    })");
    auto result = builder.buildFromJson(j);

    EXPECT_FALSE(result.ok());
    ASSERT_FALSE(result.errors.empty());
    bool foundUnknown = false;
    for (const auto& e : result.errors) {
        if (e.message.find("unknown") != std::string::npos) {
            foundUnknown = true;
            break;
        }
    }
    EXPECT_TRUE(foundUnknown) << "error should mention unknown node";
    // 关键：失败时不产生部分图（paths 必须为空）
    EXPECT_TRUE(result.paths.empty())
        << "unknown node reference must not produce a partial graph";
}

TEST(JsonGraphBuilderTest, EmptyPathArrayRejected) {
    EventBus eb{1};
    auto factory = makeFactory();
    JsonGraphBuilder builder;
    builder.setEventBus(&eb);
    builder.setNodeFactory(factory.get());

    auto j = json::parse(R"({
        "nodes":[{"id":"A","class":"TestNode"},{"id":"B","class":"TestNode"}],
        "paths":[{"name":"p1","path":[]}]
    })");
    auto result = builder.buildFromJson(j);

    EXPECT_FALSE(result.ok());
    ASSERT_FALSE(result.errors.empty());
    bool foundEmpty = false;
    for (const auto& e : result.errors) {
        if (e.message.find("empty") != std::string::npos) {
            foundEmpty = true;
            break;
        }
    }
    EXPECT_TRUE(foundEmpty) << "error should mention empty path";
    EXPECT_TRUE(result.paths.empty());
}

TEST(JsonGraphBuilderTest, UnregisteredNodeClassRejected) {
    EventBus eb{1};
    auto factory = makeFactory();
    JsonGraphBuilder builder;
    builder.setEventBus(&eb);
    builder.setNodeFactory(factory.get());

    // class "UnknownNode" 没有注册 creator，createNode 返 nullptr
    auto j = json::parse(R"({
        "nodes":[{"id":"A","class":"UnknownNode"},{"id":"B","class":"TestNode"}],
        "paths":[{"name":"p1","path":[{"id":"A","edge":"EventA"},{"id":"B","edge":""}]}]
    })");
    auto result = builder.buildFromJson(j);

    EXPECT_FALSE(result.ok());
    ASSERT_FALSE(result.errors.empty());
    bool foundCannotCreate = false;
    for (const auto& e : result.errors) {
        if (e.message.find("cannot create") != std::string::npos) {
            foundCannotCreate = true;
            break;
        }
    }
    EXPECT_TRUE(foundCannotCreate) << "error should mention cannot create node";
    EXPECT_TRUE(result.paths.empty());
}

TEST(JsonGraphBuilderTest, NoFactoryInjectedRejected) {
    EventBus eb{1};
    JsonGraphBuilder builder;
    builder.setEventBus(&eb);
    // 故意不 setNodeFactory

    auto j = json::parse(R"({
        "nodes":[{"id":"A","class":"TestNode"},{"id":"B","class":"TestNode"}],
        "paths":[{"name":"p1","path":[{"id":"A","edge":"EventA"},{"id":"B","edge":""}]}]
    })");
    auto result = builder.buildFromJson(j);

    EXPECT_FALSE(result.ok());
    ASSERT_FALSE(result.errors.empty());
    bool foundNoFactory = false;
    for (const auto& e : result.errors) {
        if (e.message.find("NodeFactory") != std::string::npos) {
            foundNoFactory = true;
            break;
        }
    }
    EXPECT_TRUE(foundNoFactory) << "error should mention no NodeFactory injected";
    EXPECT_TRUE(result.paths.empty());
}

TEST(JsonGraphBuilderTest, MultiplePathsAllValid) {
    EventBus eb{1};
    auto factory = makeFactory();
    JsonGraphBuilder builder;
    builder.setEventBus(&eb);
    builder.setNodeFactory(factory.get());

    // 三条 path，共享节点 A
    auto j = json::parse(R"({
        "nodes":[{"id":"A","class":"TestNode"},{"id":"B","class":"TestNode"},{"id":"C","class":"TestNode"}],
        "paths":[
            {"name":"p1","path":[{"id":"A","edge":"EventA"},{"id":"B","edge":""}]},
            {"name":"p2","path":[{"id":"A","edge":"EventB"},{"id":"C","edge":""}]},
            {"name":"p3","path":[{"id":"B","edge":"EventC"},{"id":"C","edge":""}]}
        ]
    })");
    auto result = builder.buildFromJson(j);

    EXPECT_TRUE(result.ok()) << "multi-path valid config should succeed";
    EXPECT_EQ(result.paths.size(), 3u);
}

// 事务性：合法→非法→合法，第三次仍成功，失败不污染后续。
TEST(JsonGraphBuilderTest, TransactionalFailureDoesNotPoisonSubsequentBuilds) {
    EventBus eb{1};
    auto factory = makeFactory();
    JsonGraphBuilder builder;
    builder.setEventBus(&eb);
    builder.setNodeFactory(factory.get());

    // 第一次：合法
    auto good = json::parse(R"({
        "nodes":[{"id":"A","class":"TestNode"},{"id":"B","class":"TestNode"}],
        "paths":[{"name":"p1","path":[{"id":"A","edge":"EventA"},{"id":"B","edge":""}]}]
    })");
    auto r1 = builder.buildFromJson(good);
    EXPECT_TRUE(r1.ok()) << "first valid build should succeed";
    EXPECT_EQ(r1.paths.size(), 1u);

    // 第二次：非法（未知节点引用）
    auto bad = json::parse(R"({
        "nodes":[{"id":"A","class":"TestNode"},{"id":"B","class":"TestNode"}],
        "paths":[{"name":"p1","path":[{"id":"A","edge":"EventA"},{"id":"Missing","edge":""}]}]
    })");
    auto r2 = builder.buildFromJson(bad);
    EXPECT_FALSE(r2.ok()) << "invalid build should fail";
    EXPECT_TRUE(r2.paths.empty());

    // 第三次：再次合法——必须仍成功，证明失败未污染 builder 状态
    auto r3 = builder.buildFromJson(good);
    EXPECT_TRUE(r3.ok()) << "third valid build should still succeed after a failure";
    EXPECT_EQ(r3.paths.size(), 1u);
    ASSERT_FALSE(r3.paths.empty());
    EXPECT_EQ(r3.paths[0]->getNodes().size(), 2u);
}

TEST(JsonGraphBuilderTest, RejectsNonArrayNodesWithoutThrowing) {
    EventBus eb{1};
    auto factory = makeFactory();
    JsonGraphBuilder builder;
    builder.setEventBus(&eb);
    builder.setNodeFactory(factory.get());

    BuildResult result;
    EXPECT_NO_THROW(result = builder.buildFromJson(json{{"nodes", json::object()},
                                                        {"paths", json::array()}}));
    ASSERT_FALSE(result.ok());
    ASSERT_FALSE(result.errors.empty());
    EXPECT_EQ(result.errors.front().jsonPointer, "/nodes");
}

TEST(JsonGraphBuilderTest, RejectsWrongFieldTypesWithJsonPointer) {
    EventBus eb{1};
    auto factory = makeFactory();
    JsonGraphBuilder builder;
    builder.setEventBus(&eb);
    builder.setNodeFactory(factory.get());

    auto result = builder.buildFromJson(json{
        {"nodes", json::array({json{{"id", 7}, {"class", "TestNode"}}})},
        {"paths", json::array()}});

    ASSERT_FALSE(result.ok());
    ASSERT_FALSE(result.errors.empty());
    EXPECT_EQ(result.errors.front().jsonPointer, "/nodes/0/id");
}

TEST(JsonGraphBuilderTest, RejectsNonStringEdgeWithJsonPointer) {
    EventBus eb{1};
    auto factory = makeFactory();
    JsonGraphBuilder builder;
    builder.setEventBus(&eb);
    builder.setNodeFactory(factory.get());

    auto result = builder.buildFromJson(json{
        {"nodes", json::array({json{{"id", "A"}, {"class", "TestNode"}}})},
        {"paths", json::array({json{{"name", "p"},
                                     {"path", json::array({json{{"id", "A"}, {"edge", 3}}})}}})}});

    ASSERT_FALSE(result.ok());
    ASSERT_FALSE(result.errors.empty());
    EXPECT_EQ(result.errors.front().jsonPointer, "/paths/0/path/0/edge");
}

// === condition 语义路由测试 ===

// 策略 3：JSON 未配置 condition 时生成空 condition（兼容旧配置）。
TEST(JsonGraphBuilderRouteTest, MissingRouteDefaultsToEmpty) {
    EventBus eb{1};
    auto factory = makeFactory();
    JsonGraphBuilder builder;
    builder.setEventBus(&eb);
    builder.setNodeFactory(factory.get());

    // 旧式配置：无 condition 字段
    auto j = json::parse(R"({
        "nodes":[{"id":"A","class":"TestNode"},{"id":"B","class":"TestNode"}],
        "paths":[{"name":"p1","path":[{"id":"A","edge":"EventA"},{"id":"B","edge":""}]}]
    })");
    auto result = builder.buildFromJson(j);
    EXPECT_TRUE(result.ok());
    ASSERT_EQ(result.paths.size(), 1u);
    // A 的 pub 边 condition 应为空
    auto& nodes = result.paths[0]->getNodes();
    ASSERT_EQ(nodes.size(), 2u);
    bool foundEmptyRoute = false;
    for (const auto& e : static_cast<TestNode*>(nodes[0].get())->getPubEdges()) {
        if (e.name() == "A->B" && e.dataType() == "EventA") {
            EXPECT_EQ(e.condition(), "");
            foundEmptyRoute = true;
        }
    }
    EXPECT_TRUE(foundEmptyRoute);
}

// 策略 3b：JSON 配置 condition 字符串时原样传入构建器。
TEST(JsonGraphBuilderRouteTest, RouteStringPassedThrough) {
    EventBus eb{1};
    auto factory = makeFactory();
    JsonGraphBuilder builder;
    builder.setEventBus(&eb);
    builder.setNodeFactory(factory.get());

    auto j = json::parse(R"({
        "nodes":[{"id":"A","class":"TestNode"},{"id":"B","class":"TestNode"}],
        "paths":[{"name":"p1","path":[{"id":"A","edge":"EventA","condition":"move"},{"id":"B","edge":""}]}]
    })");
    auto result = builder.buildFromJson(j);
    EXPECT_TRUE(result.ok());
    ASSERT_EQ(result.paths.size(), 1u);
    auto& nodes = result.paths[0]->getNodes();
    bool foundRoute = false;
    for (const auto& e : static_cast<TestNode*>(nodes[0].get())->getPubEdges()) {
        if (e.name() == "A->B" && e.dataType() == "EventA") {
            EXPECT_EQ(e.condition(), "move");
            foundRoute = true;
        }
    }
    EXPECT_TRUE(foundRoute);
}

// 策略 4：condition 非字符串时构建失败，并返回准确 JSON Pointer。
TEST(JsonGraphBuilderRouteTest, NonStringRouteRejectedWithJsonPointer) {
    EventBus eb{1};
    auto factory = makeFactory();
    JsonGraphBuilder builder;
    builder.setEventBus(&eb);
    builder.setNodeFactory(factory.get());

    auto result = builder.buildFromJson(json{
        {"nodes", json::array({json{{"id", "A"}, {"class", "TestNode"}}})},
        {"paths", json::array({json{{"name", "p"},
                                     {"path", json::array({json{{"id", "A"}, {"edge", "EventA"}, {"condition", 3}}})}}})}});

    ASSERT_FALSE(result.ok());
    ASSERT_FALSE(result.errors.empty());
    EXPECT_EQ(result.errors.front().jsonPointer, "/paths/0/path/0/condition");
}
