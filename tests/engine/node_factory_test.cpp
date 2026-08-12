// tests/engine/node_factory_test.cpp
// NodeFactory 去单例/依赖注入/同名覆盖/双引擎独立 测试。
// 覆盖：实例化构造、未注册返回 nullptr、注册后类型匹配、同名覆盖、
//       双引擎注册同名 class 互不影响、hasCreator/size。
#include <memory>
#include <string>
#include <gtest/gtest.h>
#include <nlohmann/json.hpp>
#include "core/graph_node.h"
#include "engine/graph_engine.h"
#include "engine/node_factory.h"

using graphflow::core::EventBus;
using graphflow::core::GraphNode;
using graphflow::engine::GraphEngine;
using graphflow::engine::NodeConfig;
using graphflow::engine::NodeCreator;
using graphflow::engine::NodeFactory;
using nlohmann::json;

namespace {

// 测试用 creator：根据 factory tag + params.name 组装节点名。
// 用来区分两个工厂注册的"同名 class"各自返回的节点。
class TaggedNodeCreator : public NodeCreator {
public:
    explicit TaggedNodeCreator(std::string tag) : m_tag(std::move(tag)) {}

    std::shared_ptr<GraphNode> createNode(const NodeConfig& config) override {
        std::string nodeName = m_tag;
        if (config.params.contains("name") && config.params["name"].is_string()) {
            nodeName = m_tag + "_" + config.params["name"].get<std::string>();
        }
        return std::make_shared<GraphNode>(nodeName, config.eventBus);
    }

private:
    std::string m_tag;
};

class ReentrantNodeCreator : public NodeCreator {
public:
    explicit ReentrantNodeCreator(NodeFactory& factory) : m_factory(factory) {}

    std::shared_ptr<GraphNode> createNode(const NodeConfig& config) override {
        if (!m_factory.hasCreator(config.nodeClass)) return nullptr;
        return std::make_shared<GraphNode>("reentrant", config.eventBus);
    }

private:
    NodeFactory& m_factory;
};

}  // namespace

// ---------------------------------------------------------------------------
// 1. NodeFactory 实例化构造（不再有 getInstance）
// ---------------------------------------------------------------------------
TEST(NodeFactoryTest, IsInstantiable) {
    NodeFactory factory;                            // 实例构造，非单例
    EXPECT_FALSE(factory.hasCreator("X"));
    EXPECT_EQ(factory.size(), 0u);
}

// ---------------------------------------------------------------------------
// 2. 未注册 class 的 createNode 返回 nullptr
// ---------------------------------------------------------------------------
TEST(NodeFactoryTest, CreateUnregisteredReturnsNull) {
    NodeFactory factory;
    EventBus eb(1);

    auto node = factory.createNode("NoSuchClass", &eb, json::object());
    EXPECT_EQ(node, nullptr);
    EXPECT_FALSE(factory.hasCreator("NoSuchClass"));
}

// ---------------------------------------------------------------------------
// 3. 注册 creator 后 createNode 返回非空且类型匹配
// ---------------------------------------------------------------------------
TEST(NodeFactoryTest, RegisteredCreatorProducesNode) {
    NodeFactory factory;
    EventBus eb(1);
    factory.registerCreator("Source",
        std::make_shared<TaggedNodeCreator>("Src"));

    EXPECT_TRUE(factory.hasCreator("Source"));
    EXPECT_EQ(factory.size(), 1u);

    auto node = factory.createNode("Source", &eb, json{{"name", "a"}});
    ASSERT_NE(node, nullptr);
    EXPECT_EQ(node->getName(), "Src_a");            // tag + params.name
}

// ---------------------------------------------------------------------------
// 4. 双引擎独立（核心）：两个 GraphEngine 各自 factory() 注册同名 class 不同 creator，
//    createNode 返回各自类型，互不影响。
//    即使 class 名与 params 完全相同，因各自 factory 持有不同 creator，返回节点名不同。
// ---------------------------------------------------------------------------
TEST(NodeFactoryTest, TwoEnginesRegisterSameClassIndependently) {
    GraphEngine e1, e2;
    e1.registerCreator("X", std::make_shared<TaggedNodeCreator>("E1"));
    e2.registerCreator("X", std::make_shared<TaggedNodeCreator>("E2"));

    // 两个引擎各自的 factory 都已注册 "X"
    EXPECT_TRUE(e1.factory().hasCreator("X"));
    EXPECT_TRUE(e2.factory().hasCreator("X"));

    // 同名 class、相同 params，但返回的节点名因 creator 不同而不同
    json params = json{{"name", "node"}};
    auto n1 = e1.factory().createNode("X", e1.eventBus(), params);
    auto n2 = e2.factory().createNode("X", e2.eventBus(), params);

    ASSERT_NE(n1, nullptr);
    ASSERT_NE(n2, nullptr);
    EXPECT_EQ(n1->getName(), "E1_node");
    EXPECT_EQ(n2->getName(), "E2_node");
    EXPECT_NE(n1->getName(), n2->getName());        // 关键：互不影响

    // 再次验证：e1 不受 e2 注册影响，仍返回 E1 前缀
    auto n1Again = e1.factory().createNode("X", e1.eventBus(), params);
    ASSERT_NE(n1Again, nullptr);
    EXPECT_EQ(n1Again->getName(), "E1_node");
}

// ---------------------------------------------------------------------------
// 5. 同名覆盖：同 factory 先后注册同名 class，后者覆盖前者
//    （createNode 返回后者类型）
// ---------------------------------------------------------------------------
TEST(NodeFactoryTest, SameClassOverride) {
    NodeFactory factory;
    EventBus eb(1);

    factory.registerCreator("X", std::make_shared<TaggedNodeCreator>("First"));
    auto first = factory.createNode("X", &eb, json{{"name", "v"}});
    ASSERT_NE(first, nullptr);
    EXPECT_EQ(first->getName(), "First_v");

    // 后注册覆盖前者（内部记 LOG_WARN）
    factory.registerCreator("X", std::make_shared<TaggedNodeCreator>("Second"));
    EXPECT_EQ(factory.size(), 1u);                  // 覆盖不新增条目

    auto second = factory.createNode("X", &eb, json{{"name", "v"}});
    ASSERT_NE(second, nullptr);
    EXPECT_EQ(second->getName(), "Second_v");       // 取后者 creator
    EXPECT_NE(second->getName(), first->getName());
}

// ---------------------------------------------------------------------------
// 6. hasCreator / size
// ---------------------------------------------------------------------------
TEST(NodeFactoryTest, HasCreatorAndSize) {
    NodeFactory factory;
    EventBus eb(1);

    EXPECT_FALSE(factory.hasCreator("A"));
    EXPECT_FALSE(factory.hasCreator("B"));
    EXPECT_EQ(factory.size(), 0u);

    factory.registerCreator("A", std::make_shared<TaggedNodeCreator>("A"));
    EXPECT_TRUE(factory.hasCreator("A"));
    EXPECT_FALSE(factory.hasCreator("B"));
    EXPECT_EQ(factory.size(), 1u);

    factory.registerCreator("B", std::make_shared<TaggedNodeCreator>("B"));
    EXPECT_TRUE(factory.hasCreator("A"));
    EXPECT_TRUE(factory.hasCreator("B"));
    EXPECT_EQ(factory.size(), 2u);

    // 覆盖同名不增加 size
    factory.registerCreator("A", std::make_shared<TaggedNodeCreator>("A2"));
    EXPECT_EQ(factory.size(), 2u);
}

TEST(NodeFactoryTest, CreatorMayReenterFactoryWithoutDeadlock) {
    NodeFactory factory;
    EventBus eb(1);
    factory.registerCreator("Reentrant", std::make_shared<ReentrantNodeCreator>(factory));

    auto node = factory.createNode("Reentrant", &eb, json::object());

    ASSERT_NE(node, nullptr);
    EXPECT_EQ(node->getName(), "reentrant");
}
