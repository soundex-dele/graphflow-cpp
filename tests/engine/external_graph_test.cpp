#include <gtest/gtest.h>
#include "common/test_nodes.h"
#include "engine/graph_engine.h"

using graphflow::core::LifecycleState;
using graphflow::engine::GraphBuilder;
using graphflow::engine::GraphEngine;
using graphflow_test::TestNode;
using graphflow_test::StringEvent;

namespace {
struct FlowNode : TestNode {
    using TestNode::TestNode;
    using GraphNode::publish;
    using GraphNode::publishAsync;
    std::vector<std::string> received;
    graphflow::core::EventBus* controlBus() const { return m_eventBus; }
    bool doInit() override {
        subscribe<StringEvent>([this](StringEvent& event) { received.push_back(event.payload); });
        return TestNode::doInit();
    }
};

struct Creator : graphflow::engine::NodeCreator {
    std::shared_ptr<graphflow::core::GraphNode> createNode(
        const graphflow::engine::NodeConfig& cfg) override {
        return std::make_shared<TestNode>(cfg.nodeName, cfg.eventBus);
    }
};

nlohmann::json jsonGraph() {
    return nlohmann::json::parse(R"({"nodes":[{"id":"json","class":"Test"}],
        "paths":[{"name":"main","path":[{"id":"json"}]}]})");
}
}

TEST(ExternalGraphTest, NodesAndRelationsDriveDataAndLifecycleWithoutFactory) {
    GraphEngine engine(1);
    auto source = std::make_shared<FlowNode>("source", nullptr);
    auto left = std::make_shared<FlowNode>("left", nullptr);
    auto right = std::make_shared<FlowNode>("right", nullptr);
    auto isolated = std::make_shared<FlowNode>("isolated", nullptr);
    ASSERT_TRUE(engine.loadFromNodes({source, left, right, isolated}, {
        {"source", "left"}, {"source", "right"}, {"source", "left"},
        {"source", "left", "", "selected"}, {"left", "right"}, {"right", "source"}}));
    ASSERT_EQ(engine.paths().size(), 1u);
    EXPECT_EQ(engine.paths()[0]->getNodes()[0].get(), source.get());
    EXPECT_EQ(engine.paths()[0]->getNodes().size(), 4u);
    EXPECT_EQ(source->controlBus(), engine.eventBus());
    EXPECT_EQ(source->getPubEdges().size(), 3u);
    EXPECT_EQ(right->getSubEdges().size(), 2u);
    EXPECT_TRUE(isolated->getPubEdges().empty());
    EXPECT_EQ(source->state(), LifecycleState::Created);
    ASSERT_TRUE(engine.init());
    ASSERT_TRUE(engine.start());
    source->publish(StringEvent("broadcast"));
    source->publish(StringEvent("route"), "selected");
    source->publishAsync(StringEvent("async"));
    for (const auto& bus : engine.paths()[0]->eventBusesSnapshot()) {
        EXPECT_EQ(bus->threadPool(), engine.eventBus()->threadPool());
        bus->drain();
    }
    EXPECT_EQ(left->received, (std::vector<std::string>{"broadcast", "route", "async"}));
    EXPECT_EQ(right->received, (std::vector<std::string>{"broadcast", "async"}));
    engine.stop();
    ASSERT_TRUE(engine.start());
    engine.release();
    engine.release();
    for (const auto& node : {source, left, right, isolated}) {
        EXPECT_EQ(node->initCount, 1);
        EXPECT_EQ(node->startCount, 2);
        EXPECT_EQ(node->stopCount, 2);
        EXPECT_EQ(node->releaseCount, 1);
    }
}

TEST(ExternalGraphTest, ExternalTransportCreatesOnlyTopology) {
    GraphEngine engine(1);
    auto a = std::make_shared<TestNode>("A", nullptr);
    auto b = std::make_shared<TestNode>("B", nullptr);
    ASSERT_TRUE(engine.loadFromNodes({a, b}, {{"A", "B", "Frame", "video"}},
                                    GraphBuilder::EdgeTransport::External));
    ASSERT_EQ(a->getPubEdges().size(), 1u);
    EXPECT_EQ(a->getPubEdges()[0].dataType(), "Frame");
    EXPECT_EQ(a->getPubEdges()[0].condition(), "video");
    EXPECT_EQ(a->getPubEdges()[0].eventBus(), nullptr);
    EXPECT_TRUE(engine.paths()[0]->eventBusesSnapshot().empty());
    ASSERT_TRUE(engine.init());
    ASSERT_TRUE(engine.start());
}

TEST(ExternalGraphTest, InvalidNodesAndRelationsDoNotMutateCandidateOrOldGraph) {
    GraphEngine engine(1);
    auto old = std::make_shared<TestNode>("old", nullptr);
    ASSERT_TRUE(engine.loadFromNodes({old}, {}));
    auto a = std::make_shared<FlowNode>("A", nullptr);
    auto b = std::make_shared<FlowNode>("B", nullptr);
    auto duplicate = std::make_shared<TestNode>("A", nullptr);
    auto unnamed = std::make_shared<TestNode>("", nullptr);
    EXPECT_FALSE(engine.loadFromNodes({a, nullptr}, {}));
    EXPECT_FALSE(engine.loadFromNodes({a, a}, {}));
    EXPECT_FALSE(engine.loadFromNodes({a, duplicate}, {}));
    EXPECT_FALSE(engine.loadFromNodes({unnamed}, {}));
    EXPECT_FALSE(engine.loadFromNodes({old}, {}));
    EXPECT_FALSE(engine.loadFromNodes({a, b}, {{"A", "B"}, {"B", "missing"}}));
    EXPECT_FALSE(engine.loadFromNodes({}, {{"A", "B"}}));
    EXPECT_TRUE(a->getPubEdges().empty());
    EXPECT_TRUE(b->getSubEdges().empty());
    EXPECT_EQ(a->controlBus(), nullptr);
    EXPECT_EQ(a->state(), LifecycleState::Created);
    EXPECT_EQ(old->releaseCount, 0);
    ASSERT_EQ(engine.paths().size(), 1u);
    EXPECT_EQ(engine.paths()[0]->getNodes()[0], old);
    ASSERT_TRUE(engine.loadFromNodes({a, b}, {{"A", "B"}}));
    EXPECT_EQ(old->releaseCount, 1);
}

TEST(ExternalGraphTest, RejectsUsedOrAlreadyConnectedNodes) {
    GraphEngine engine(1);
    for (const auto target : {LifecycleState::Initialized, LifecycleState::Running,
                              LifecycleState::Stopped, LifecycleState::Released}) {
        auto node = std::make_shared<TestNode>("used", nullptr);
        ASSERT_TRUE(node->init());
        if (target == LifecycleState::Running || target == LifecycleState::Stopped) {
            ASSERT_TRUE(node->start());
        }
        if (target == LifecycleState::Stopped) node->stop();
        if (target == LifecycleState::Released) node->release();
        EXPECT_FALSE(engine.loadFromNodes({node}, {}));
        EXPECT_EQ(node->state(), target);
        node->release();
    }
    auto node = std::make_shared<TestNode>("A", nullptr);
    node->addSubEdge(graphflow::core::GraphEdge::connect("B", "A"));
    EXPECT_FALSE(engine.loadFromNodes({node}, {}));
    EXPECT_EQ(node->getSubEdges().size(), 1u);
}

TEST(ExternalGraphTest, LoadStateGuardsAndEmptyGraph) {
    GraphEngine engine(1);
    ASSERT_TRUE(engine.reloadFromNodes({}, {}));
    EXPECT_TRUE(engine.paths().empty());
    EXPECT_EQ(engine.state(), LifecycleState::Created);
    auto node = std::make_shared<TestNode>("A", nullptr);
    ASSERT_TRUE(engine.init());
    EXPECT_FALSE(engine.loadFromNodes({node}, {}));
    ASSERT_TRUE(engine.start());
    EXPECT_FALSE(engine.loadFromNodes({node}, {}));
    engine.stop();
    EXPECT_FALSE(engine.loadFromNodes({node}, {}));
    engine.release();
    EXPECT_FALSE(engine.loadFromNodes({node}, {}));
    EXPECT_FALSE(engine.reloadFromNodes({node}, {}));
    EXPECT_EQ(node->state(), LifecycleState::Created);
}

TEST(ExternalGraphTest, SwitchesBetweenJsonAndExternalNodesWhileRunning) {
    GraphEngine engine(1);
    engine.registerCreator("Test", std::make_shared<Creator>());
    ASSERT_TRUE(engine.loadFromJson(jsonGraph()));
    auto jsonNode = engine.paths()[0]->getNodes()[0];
    ASSERT_TRUE(engine.init());
    ASSERT_TRUE(engine.start());
    auto external = std::make_shared<TestNode>("external", nullptr);
    ASSERT_TRUE(engine.reloadFromNodes({external}, {}));
    EXPECT_EQ(jsonNode->state(), LifecycleState::Released);
    EXPECT_EQ(engine.state(), LifecycleState::Running);
    EXPECT_EQ(external->state(), LifecycleState::Running);
    ASSERT_TRUE(engine.reloadFromJson(jsonGraph()));
    EXPECT_EQ(external->releaseCount, 1);
    EXPECT_EQ(engine.paths()[0]->getNodes()[0]->getName(), "json");
    EXPECT_EQ(engine.state(), LifecycleState::Running);
    ASSERT_TRUE(engine.reloadFromNodes({}, {}));
    EXPECT_TRUE(engine.paths().empty());
    EXPECT_EQ(engine.state(), LifecycleState::Running);
}

TEST(ExternalGraphTest, InvalidReloadDoesNotStopOldGraph) {
    GraphEngine engine(1);
    auto old = std::make_shared<TestNode>("old", nullptr);
    ASSERT_TRUE(engine.loadFromNodes({old}, {}));
    ASSERT_TRUE(engine.init());
    ASSERT_TRUE(engine.start());
    auto candidate = std::make_shared<TestNode>("new", nullptr);
    EXPECT_FALSE(engine.reloadFromNodes({old}, {}));
    EXPECT_FALSE(engine.reloadFromNodes({candidate}, {{"new", "missing"}}));
    EXPECT_EQ(old->stopCount, 0);
    EXPECT_EQ(old->state(), LifecycleState::Running);
    EXPECT_EQ(candidate->state(), LifecycleState::Created);
}

TEST(ExternalGraphTest, ReloadInitAndStartFailuresReleaseCandidateAndRestartOldGraph) {
    for (bool failInit : {true, false}) {
        GraphEngine engine(1);
        auto old = std::make_shared<TestNode>("old", nullptr);
        ASSERT_TRUE(engine.loadFromNodes({old}, {}));
        ASSERT_TRUE(engine.init());
        ASSERT_TRUE(engine.start());
        auto good = std::make_shared<TestNode>("good", nullptr);
        auto bad = std::make_shared<TestNode>("bad", nullptr);
        if (failInit) bad->failOnInitIndex = 0;
        else bad->failOnStartIndex = 0;
        EXPECT_FALSE(engine.reloadFromNodes({good, bad}, {{"good", "bad"}}));
        EXPECT_EQ(engine.state(), LifecycleState::Running);
        EXPECT_EQ(old->state(), LifecycleState::Running);
        EXPECT_EQ(old->startCount, 2);
        EXPECT_EQ(old->releaseCount, 0);
        EXPECT_EQ(good->releaseCount, 1);
        EXPECT_EQ(bad->releaseCount, 1);
        EXPECT_EQ(engine.paths()[0]->getNodes()[0], old);
    }
}

TEST(ExternalGraphTest, ReloadNonRunningGraphDoesNotStartCandidate) {
    for (bool stopped : {false, true}) {
        GraphEngine engine(1);
        auto old = std::make_shared<TestNode>("old", nullptr);
        ASSERT_TRUE(engine.reloadFromNodes({old}, {}));
        ASSERT_TRUE(engine.init());
        if (stopped) { ASSERT_TRUE(engine.start()); engine.stop(); }
        auto next = std::make_shared<TestNode>("next", nullptr);
        ASSERT_TRUE(engine.reloadFromNodes({next}, {}));
        EXPECT_EQ(engine.state(), LifecycleState::Initialized);
        EXPECT_EQ(next->initCount, 1);
        EXPECT_EQ(next->startCount, 0);
        EXPECT_EQ(old->releaseCount, 1);
        ASSERT_TRUE(engine.start());
    }
}
