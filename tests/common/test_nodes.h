// tests/common/test_nodes.h
// 共享测试夹具：可注入失败位置的 GraphNode 子类，以及最小事件类型。
// 供 lifecycle / graph_builder / graph_engine 测试复用。
#pragma once

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <vector>
#include "core/event.h"
#include "core/event_bus.h"
#include "core/graph_node.h"

namespace graphflow_test {

// 最小事件类型
struct StringEvent : graphflow::core::EventT<StringEvent> {
    std::string payload;
    explicit StringEvent(std::string p) : payload(std::move(p)) {}
};

// 可注入失败位置的测试节点。
//   failOnInitIndex / failOnStartIndex：doInit/doStart 遍历时第 N 个（0-based）返回 false。
//   startCount / stopCount / releaseCount 等计数用于断言回滚逆序与幂等。
struct TestNode : graphflow::core::GraphNode {
    int failOnInitIndex = -1;
    int failOnStartIndex = -1;
    int initOrder = -1;     // 由外部 assigner 赋值，记录期望初始化次序（调试用）
    std::atomic<int> initCount{0};
    std::atomic<int> startCount{0};
    std::atomic<int> stopCount{0};
    std::atomic<int> releaseCount{0};

    // 全局计数器，用于断言"前 N-1 个被逆序回滚"——记录每次调用的顺序号。
    static std::atomic<int>& callSeq() { static std::atomic<int> s{0}; return s; }

    // 最近一次 start/stop/release 的全局序号，用于断言逆序。
    std::atomic<int> lastStartSeq{0};
    std::atomic<int> lastStopSeq{0};
    std::atomic<int> lastReleaseSeq{0};

    TestNode(const std::string& n, graphflow::core::EventBus* eb)
        : graphflow::core::GraphNode(n, eb) {}

    bool doInit() override {
        ++initCount;
        if (failOnInitIndex >= 0 && initCount.load() - 1 == failOnInitIndex) {
            LOG_ERROR("TestNode", "injected init fail: " << getName());
            return false;
        }
        return true;
    }
    bool doStart() override {
        ++startCount;
        if (failOnStartIndex >= 0 && startCount.load() - 1 == failOnStartIndex) {
            LOG_ERROR("TestNode", "injected start fail: " << getName());
            return false;
        }
        lastStartSeq = ++callSeq();   // 记录 start 成功序号，用于断言逆序回滚
        return true;
    }
    void doStop() override {
        ++stopCount;
        lastStopSeq = ++callSeq();
    }
    void doRelease() override {
        ++releaseCount;
        lastReleaseSeq = ++callSeq();
    }
};

}  // namespace graphflow_test
