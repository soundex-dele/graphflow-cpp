// tests/engine/graph_engine_test.cpp
// GraphEngine 状态机 / load / reload / 失败回滚 测试。
//
// 覆盖：
//   1. 新引擎 Created 态、paths() 为空
//   2. loadFromJson 合法配置成功（state 仍 Created，paths 非空）
//   3. init→Initialized / start→Running / stop→Stopped / release→Released
//   4. Running 态调 load 被拒绝（审查 3.3）：旧图不变
//   5. reload 切换图：load A→init→start→Running；reload 成另一合法配置→新图生效
//   6. reload 失败回滚：Running 态 reload 非法配置→返回 false，旧图保持一致
//   7. load 失败不污染：load 合法→load 非法→失败；再 load 合法仍成功
//   8. init 成功后重复 init 幂等
//   9. release 后再 load 返回 false（Released 终态）
//  10. Created 态直接 start→false（未 init）
//
// 关于"路径级失败回滚"：TestNode 的 failOnStartIndex/failOnInitIndex 在节点实例上
//   注入，而 TestNodeCreator 每次 createNode 都 new 一个全新 TestNode，无法从外部
//   指定某个节点实例的 failIndex。要构造"某节点 doStart 返回 false"需自定义 creator
//   或在 NodeConfig.params 里传 failIndex 并由 creator 读取——这超出本测试文件范围
//   （且 doInit/doStart 路径级回滚已由 core/lifecycle_test 与 graph_builder_test 间接
//   覆盖），故此处不单独构造路径级失败回滚用例。
#include <memory>
#include <string>
#include <vector>
#include <gtest/gtest.h>
#include <nlohmann/json.hpp>
#include "common/test_nodes.h"
#include "core/lifecycle.h"
#include "engine/graph_engine.h"
#include "engine/node_factory.h"

using graphflow::core::LifecycleState;
using graphflow::engine::GraphEngine;
using graphflow::engine::NodeConfig;
using graphflow::engine::NodeCreator;
using graphflow_test::TestNode;

namespace {

// 与 json_graph_builder 测试一致的 creator：从 NodeConfig 重建 TestNode。
class TestNodeCreator : public NodeCreator {
public:
    std::shared_ptr<graphflow::core::GraphNode> createNode(const NodeConfig& cfg) override {
        return std::make_shared<TestNode>(cfg.nodeName, cfg.eventBus);
    }
};

class ConfigurableNodeCreator : public NodeCreator {
public:
    std::shared_ptr<graphflow::core::GraphNode> createNode(const NodeConfig& cfg) override {
        auto node = std::make_shared<TestNode>(cfg.nodeName, cfg.eventBus);
        if (cfg.params.value("fail_init", false)) node->failOnInitIndex = 0;
        return node;
    }
};

// 合法两节点单路径配置（A->B，edge=EventA）。
nlohmann::json makeValidConfigAB() {
    return nlohmann::json::parse(R"({
        "nodes":[{"id":"A","class":"TestNode"},{"id":"B","class":"TestNode"}],
        "paths":[{"name":"p1","path":[{"id":"A","edge":"EventA"},{"id":"B","edge":""}]}]
    })");
}

// 另一合法配置（C->D，edge=EventB），用于 reload 切换图断言。
nlohmann::json makeValidConfigCD() {
    return nlohmann::json::parse(R"({
        "nodes":[{"id":"C","class":"TestNode"},{"id":"D","class":"TestNode"}],
        "paths":[{"name":"p2","path":[{"id":"C","edge":"EventB"},{"id":"D","edge":""}]}]
    })");
}

// 非法配置：paths 引用未声明的节点 id。
nlohmann::json makeInvalidConfigUnknownNode() {
    return nlohmann::json::parse(R"({
        "nodes":[{"id":"A","class":"TestNode"}],
        "paths":[{"name":"p1","path":[{"id":"A","edge":"EventA"},{"id":"Missing","edge":""}]}]
    })");
}

// 非法配置：node 缺少 class 字段。
nlohmann::json makeInvalidConfigMissingClass() {
    return nlohmann::json::parse(R"({
        "nodes":[{"id":"A"}],
        "paths":[{"name":"p1","path":[{"id":"A","edge":""}]}]
    })");
}

// 收集某引擎所有路径中所有节点的名字（按路径顺序），用于断言"图是否切换"。
std::vector<std::string> collectNodeNames(const GraphEngine& eng) {
    std::vector<std::string> names;
    for (const auto& p : eng.paths()) {
        for (const auto& n : p->getNodes()) names.push_back(n->getName());
    }
    return names;
}

}  // namespace

TEST(GraphEngineTest, FreshEngineCreatedStateAndEmptyPaths) {
    GraphEngine eng;
    EXPECT_EQ(eng.state(), LifecycleState::Created);
    EXPECT_TRUE(eng.paths().empty());
}

TEST(GraphEngineTest, MainAndEdgeBusesShareThreadPool) {
    GraphEngine eng(2);
    eng.registerCreator("TestNode", std::make_shared<TestNodeCreator>());
    ASSERT_TRUE(eng.loadFromJson(makeValidConfigAB()));
    ASSERT_EQ(eng.paths().size(), 1u);
    auto edgeBuses = eng.paths().front()->eventBusesSnapshot();
    ASSERT_EQ(edgeBuses.size(), 1u);

    EXPECT_EQ(eng.eventBus()->threadPool().get(),
              edgeBuses.front()->threadPool().get());
}

TEST(GraphEngineTest, LoadValidKeepsCreatedStateAndPopulatesPaths) {
    GraphEngine eng;
    eng.registerCreator("TestNode", std::make_shared<TestNodeCreator>());
    ASSERT_TRUE(eng.loadFromJson(makeValidConfigAB()));
    // load 不改 state（仍 Created，需显式 init）。
    EXPECT_EQ(eng.state(), LifecycleState::Created);
    EXPECT_FALSE(eng.paths().empty());
    EXPECT_EQ(collectNodeNames(eng), (std::vector<std::string>{"A", "B"}));
}

TEST(GraphEngineTest, FullLifecycleInitStartStopRelease) {
    GraphEngine eng;
    eng.registerCreator("TestNode", std::make_shared<TestNodeCreator>());
    ASSERT_TRUE(eng.loadFromJson(makeValidConfigAB()));

    EXPECT_TRUE(eng.init());
    EXPECT_EQ(eng.state(), LifecycleState::Initialized);

    EXPECT_TRUE(eng.start());
    EXPECT_EQ(eng.state(), LifecycleState::Running);

    eng.stop();
    EXPECT_EQ(eng.state(), LifecycleState::Stopped);

    eng.release();
    EXPECT_EQ(eng.state(), LifecycleState::Released);
}

TEST(GraphEngineTest, LoadRejectedWhileRunning) {
    GraphEngine eng;
    eng.registerCreator("TestNode", std::make_shared<TestNodeCreator>());
    ASSERT_TRUE(eng.loadFromJson(makeValidConfigAB()));
    ASSERT_TRUE(eng.init());
    ASSERT_TRUE(eng.start());
    ASSERT_EQ(eng.state(), LifecycleState::Running);

    // Running 态调 load 必须被拒绝。
    EXPECT_FALSE(eng.loadFromJson(makeValidConfigCD()));
    // 旧图不变：仍是第一次的 A/B。
    EXPECT_EQ(collectNodeNames(eng), (std::vector<std::string>{"A", "B"}));
    EXPECT_EQ(eng.state(), LifecycleState::Running);
}

TEST(GraphEngineTest, ReloadSwitchesGraphWhileRunning) {
    GraphEngine eng;
    eng.registerCreator("TestNode", std::make_shared<TestNodeCreator>());
    ASSERT_TRUE(eng.loadFromJson(makeValidConfigAB()));
    ASSERT_TRUE(eng.init());
    ASSERT_TRUE(eng.start());
    ASSERT_EQ(eng.state(), LifecycleState::Running);
    EXPECT_EQ(collectNodeNames(eng), (std::vector<std::string>{"A", "B"}));

    // reload 成另一合法配置：新图生效，节点变为 C/D。
    ASSERT_TRUE(eng.reloadFromJson(makeValidConfigCD()));
    EXPECT_EQ(collectNodeNames(eng), (std::vector<std::string>{"C", "D"}));
    // reload 前为 Running，reload 后恢复 Running。
    EXPECT_EQ(eng.state(), LifecycleState::Running);
}

TEST(GraphEngineTest, ReloadInvalidRollsBackWhileRunning) {
    GraphEngine eng;
    eng.registerCreator("TestNode", std::make_shared<TestNodeCreator>());
    ASSERT_TRUE(eng.loadFromJson(makeValidConfigAB()));
    ASSERT_TRUE(eng.init());
    ASSERT_TRUE(eng.start());
    ASSERT_EQ(eng.state(), LifecycleState::Running);
    ASSERT_EQ(collectNodeNames(eng), (std::vector<std::string>{"A", "B"}));

    // reload 非法配置：返回 false，旧图保持一致。
    EXPECT_FALSE(eng.reloadFromJson(makeInvalidConfigUnknownNode()));
    EXPECT_EQ(collectNodeNames(eng), (std::vector<std::string>{"A", "B"}));
    EXPECT_EQ(eng.state(), LifecycleState::Running);
}

TEST(GraphEngineTest, LoadFailureDoesNotPolluteBuilder) {
    GraphEngine eng;
    eng.registerCreator("TestNode", std::make_shared<TestNodeCreator>());

    // 第一次 load 合法。
    ASSERT_TRUE(eng.loadFromJson(makeValidConfigAB()));
    EXPECT_EQ(collectNodeNames(eng), (std::vector<std::string>{"A", "B"}));

    // 第二次 load 非法：失败返回 false（load 要求 Created 态，此时仍 Created）。
    EXPECT_FALSE(eng.loadFromJson(makeInvalidConfigMissingClass()));
    // 旧图不被污染：仍是 A/B。
    EXPECT_EQ(collectNodeNames(eng), (std::vector<std::string>{"A", "B"}));

    // 再 load 合法仍成功（验证 builder 内部事务清理，无残留脏状态）。
    EXPECT_TRUE(eng.loadFromJson(makeValidConfigCD()));
    EXPECT_EQ(collectNodeNames(eng), (std::vector<std::string>{"C", "D"}));
}

TEST(GraphEngineTest, InitIsIdempotentAfterSuccess) {
    GraphEngine eng;
    eng.registerCreator("TestNode", std::make_shared<TestNodeCreator>());
    ASSERT_TRUE(eng.loadFromJson(makeValidConfigAB()));
    ASSERT_TRUE(eng.init());
    EXPECT_EQ(eng.state(), LifecycleState::Initialized);

    // 重复 init 幂等：返回 true，状态不变。
    EXPECT_TRUE(eng.init());
    EXPECT_EQ(eng.state(), LifecycleState::Initialized);
}

TEST(GraphEngineTest, LoadRejectedAfterRelease) {
    GraphEngine eng;
    eng.registerCreator("TestNode", std::make_shared<TestNodeCreator>());
    ASSERT_TRUE(eng.loadFromJson(makeValidConfigAB()));
    ASSERT_TRUE(eng.init());
    eng.release();
    ASSERT_EQ(eng.state(), LifecycleState::Released);

    // Released 为终态：load 必须被拒绝。
    EXPECT_FALSE(eng.loadFromJson(makeValidConfigCD()));
    EXPECT_EQ(eng.state(), LifecycleState::Released);
}

TEST(GraphEngineTest, StartRejectedBeforeInit) {
    GraphEngine eng;
    eng.registerCreator("TestNode", std::make_shared<TestNodeCreator>());
    ASSERT_TRUE(eng.loadFromJson(makeValidConfigAB()));
    ASSERT_EQ(eng.state(), LifecycleState::Created);

    // Created 态直接 start：未 init，必须返回 false。
    EXPECT_FALSE(eng.start());
    EXPECT_EQ(eng.state(), LifecycleState::Created);
}

TEST(GraphEngineTest, ReloadInitFailureKeepsAndRestartsOldGraph) {
    GraphEngine eng;
    eng.registerCreator("TestNode", std::make_shared<ConfigurableNodeCreator>());
    ASSERT_TRUE(eng.loadFromJson(makeValidConfigAB()));
    ASSERT_TRUE(eng.init());
    ASSERT_TRUE(eng.start());

    auto failingConfig = nlohmann::json::parse(R"({
        "nodes":[{"id":"X","class":"TestNode","params":{"fail_init":true}}],
        "paths":[{"name":"failing","path":[{"id":"X","edge":""}]}]
    })");
    EXPECT_FALSE(eng.reloadFromJson(failingConfig));

    EXPECT_EQ(eng.state(), LifecycleState::Running);
    EXPECT_EQ(collectNodeNames(eng), (std::vector<std::string>{"A", "B"}));
}
