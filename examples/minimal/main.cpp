// examples/minimal/main.cpp
// End-to-end verification: SourceNode publishes a PayloadEvent, SinkNode (subscribed
// in init(), before SourceNode's start() publishes) receives it through the EventBus.
// Driven entirely by examples/minimal/graph.json through the engine::GraphEngine facade.
#include <atomic>
#include <iostream>
#include <memory>
#include <string>
#include <vector>
#include "engine/graph_engine.h"
#include "core/event_bus.h"
#include "core/graph_node.h"

using graphflow::engine::GraphEngine;
using graphflow::core::Event;
using graphflow::core::EventT;
using graphflow::core::EventBus;
using graphflow::core::GraphNode;
using graphflow::engine::NodeConfig;
using graphflow::engine::NodeCreator;

// 自定义事件
struct PayloadEvent : EventT<PayloadEvent> {
    std::string payload;
    explicit PayloadEvent(std::string p) : payload(std::move(p)) {}
};

// 源节点：start 后发布一条 PayloadEvent 到自己注册的 pubInfo edge
struct SourceNode : GraphNode {
    explicit SourceNode(const std::string& n, EventBus* eb) : GraphNode(n, eb) {}
    bool doInit() override { return true; }
    bool doStart() override {
        publish<PayloadEvent>(PayloadEvent("hello from source"));
        return true;
    }
};

// 汇节点：init 时订阅自己的 sub edge（保证 start 前 sink 已订阅）
struct SinkNode : GraphNode {
    std::atomic<bool> received{false};
    std::string captured;
    explicit SinkNode(const std::string& n, EventBus* eb) : GraphNode(n, eb) {}
    bool doInit() override {
        subscribe<PayloadEvent>([this](PayloadEvent& e) {
            captured = e.payload;
            received = true;
        });
        return true;
    }
    bool doStart() override { return true; }
};

class SourceCreator : public NodeCreator {
public:
    std::shared_ptr<GraphNode> createNode(const NodeConfig& cfg) override {
        return std::make_shared<SourceNode>(cfg.nodeName, cfg.eventBus);
    }
};

class SinkCreator : public NodeCreator {
public:
    std::shared_ptr<GraphNode> createNode(const NodeConfig& cfg) override {
        return std::make_shared<SinkNode>(cfg.nodeName, cfg.eventBus);
    }
};

int main(int argc, char** argv) {
    std::string configFile = "examples/minimal/graph.json";
    if (argc > 1) configFile = argv[1];

    GraphEngine eng;
    eng.registerCreator("SourceNode", std::make_shared<SourceCreator>());
    eng.registerCreator("SinkNode", std::make_shared<SinkCreator>());

    if (!eng.loadFromJsonFile(configFile)) {
        std::cerr << "failed to load " << configFile << "\n";
        return 1;
    }
    if (!eng.init()) {
        std::cerr << "engine init failed\n";
        return 1;
    }
    if (!eng.start()) {
        std::cerr << "engine start failed\n";
        return 1;
    }
    // 在 release() 之前验证：release() 会清空 eng.paths()，导致无法遍历节点
    bool ok = false;
    for (auto& path : eng.paths()) {
        for (auto& nodeBase : path->getNodes()) {
            if (nodeBase->getName() == "sink") {
                auto* sink = dynamic_cast<SinkNode*>(nodeBase.get());
                if (sink && sink->received.load() && sink->captured == "hello from source") {
                    ok = true;
                }
            }
        }
    }
    eng.stop();
    eng.release();
    std::cout << (ok ? "minimal example: DATA FLOWED source->sink OK"
                     : "minimal example: NO DATA FLOW") << "\n";
    return ok ? 0 : 2;
}
