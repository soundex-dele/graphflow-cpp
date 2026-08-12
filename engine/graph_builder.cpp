// engine/graph_builder.cpp
#include "engine/graph_builder.h"
#include <stdexcept>
#include <tuple>
#include <unordered_set>
#include "utils/logger.h"
#include "core/event_bus.h"

namespace graphflow::engine {

static const std::string TAG = "GraphBuilder";

GraphBuilder::GraphBuilder() : m_eventBus(nullptr) {}
GraphBuilder::~GraphBuilder() = default;

bool GraphBuilder::registerNode(const std::string& nodeName,
                               std::shared_ptr<core::GraphNode> node) {
    if (!node) return false;
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_nodeMap.find(nodeName) != m_nodeMap.end()) return false;
    m_nodeMap[nodeName] = std::move(node);
    return true;
}

GraphBuilder& GraphBuilder::setEventBus(core::EventBus* eventBus) {
    m_eventBus = eventBus;
    return *this;
}

void GraphBuilder::reset() {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_nodeMap.clear();
    m_nodeEventBusMap.clear();
}

void GraphBuilder::absorb(GraphBuilder& other) {
    // 先从 other 取走状态（other 持锁），再替换 this（this 持锁）。避免同时持两把锁。
    decltype(other.m_nodeMap) otherNodes;
    decltype(other.m_nodeEventBusMap) otherBuses;
    core::EventBus* otherEventBus = nullptr;
    {
        std::lock_guard<std::mutex> olock(other.m_mutex);
        otherNodes = std::move(other.m_nodeMap);
        otherBuses = std::move(other.m_nodeEventBusMap);
        otherEventBus = other.m_eventBus;
        other.m_eventBus = nullptr;
    }
    std::lock_guard<std::mutex> lock(m_mutex);
    m_nodeMap = std::move(otherNodes);
    m_nodeEventBusMap = std::move(otherBuses);
    if (otherEventBus) m_eventBus = otherEventBus;
}

std::shared_ptr<core::GraphPath> GraphBuilder::build(
    const std::vector<std::string>& nodeNames, const std::string& dataType) {
    (void)dataType;
    std::lock_guard<std::mutex> lock(m_mutex);
    // 单数据类型重载：仅查找已注册节点 + 创建空管线 GraphPath。
    // 建边逻辑走多数据类型重载 build(nodeDataTypes)。
    std::vector<std::shared_ptr<core::NodeBase>> baseNodes;
    for (const auto& nodeName : nodeNames) {
        auto it = m_nodeMap.find(nodeName);
        if (it == m_nodeMap.end()) {
            throw std::runtime_error("Node not registered: " + nodeName);
        }
        baseNodes.push_back(it->second);
    }
    return std::make_shared<core::GraphPath>(m_eventBus, baseNodes);
}

std::shared_ptr<core::GraphPath> GraphBuilder::build(
    const std::vector<std::pair<std::string, std::string>>& nodeDataTypes) {
    // 旧接口兼容：每一项 (id, dataType) 转为 condition="" 的 PathNodeSpec，转发到新重载。
    std::vector<PathNodeSpec> specs;
    specs.reserve(nodeDataTypes.size());
    for (const auto& nd : nodeDataTypes) {
        specs.push_back({nd.first, nd.second, std::string()});
    }
    return build(specs);
}

std::shared_ptr<core::GraphPath> GraphBuilder::build(const std::vector<PathNodeSpec>& specs) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (specs.empty()) {
        LOG_ERROR(TAG, "No nodes provided for path construction");
        return nullptr;
    }
    if (!m_eventBus) {
        throw std::runtime_error("EventBus not set for GraphBuilder");
    }

    // 按相邻位置建边（审查 3.2 修复 + 语义路由扩展）：不再先按节点名折叠邻接表。
    //   位置 i 的节点作为 from，位置 i+1 作为 to，dataType/condition 取自位置 i 的 spec。
    //   同一节点可在路径中多次出现，每个位置的出边 dataType/condition 可不同。
    //   边总线键 = (fromId+"->"+toId, dataType, condition)；单次 build 内同物理边复用同 EventBus。
    //   同起终点不同 condition 使用各自独立 EventBus，避免订阅/发布跨路由串流。
    std::vector<std::shared_ptr<core::EventBus>> pathEventBuses;
    std::unordered_set<core::EventBus*> seenEventBuses;
    for (size_t i = 0; i + 1 < specs.size(); ++i) {
        const std::string& fromId = specs[i].id;
        const std::string& dataType = specs[i].dataType;
        const std::string& condition = specs[i].condition;
        const std::string& toId = specs[i + 1].id;

        auto fromIt = m_nodeMap.find(fromId);
        if (fromIt == m_nodeMap.end()) {
            throw std::runtime_error("Node not registered: " + fromId);
        }
        auto toIt = m_nodeMap.find(toId);
        if (toIt == m_nodeMap.end()) {
            throw std::runtime_error("Node not registered: " + toId);
        }
        auto& fromNode = fromIt->second;
        auto& toNode = toIt->second;

        const std::string topic = fromId + "->" + toId;
        auto key = std::make_tuple(topic, dataType, condition);
        auto busIt = m_nodeEventBusMap.find(key);
        if (busIt == m_nodeEventBusMap.end()) {
            // Each edge keeps an independent EventBus, while all edge buses
            // reuse the GraphEngine main bus's topic-aware thread pool.
            auto eb = std::make_shared<core::EventBus>(m_eventBus->threadPool());
            busIt = m_nodeEventBusMap.emplace(key, std::move(eb)).first;
            LOG_INFO(TAG, "create edge bus: " << topic
                     << " dataType=" << dataType << " condition=" << condition);
        }
        core::EventBus* bus = busIt->second.get();
        if (seenEventBuses.insert(bus).second) {
            pathEventBuses.push_back(busIt->second);
        }

        // 去重挂边：from 的 pub 边、to 的 sub 边均按完整身份 (name,dataType,condition) 去重。
        const auto& pubEdges = fromNode->getPubEdges();
        bool pubExists = false;
        for (const auto& e : pubEdges) {
            if (e.name() == topic && e.dataType() == dataType && e.condition() == condition) {
                pubExists = true;
                break;
            }
        }
        if (!pubExists) {
            fromNode->addPubEdge(
                core::GraphEdge::connect(fromId, toId, dataType, bus, condition));
        }

        const auto& subEdges = toNode->getSubEdges();
        bool subExists = false;
        for (const auto& e : subEdges) {
            if (e.name() == topic && e.dataType() == dataType && e.condition() == condition) {
                subExists = true;
                break;
            }
        }
        if (!subExists) {
            toNode->addSubEdge(
                core::GraphEdge::connect(fromId, toId, dataType, bus, condition));
        }
    }

    // 收集路径节点（保持位置顺序，含重复节点）。
    std::vector<std::shared_ptr<core::NodeBase>> baseNodes;
    baseNodes.reserve(specs.size());
    for (const auto& spec : specs) {
        auto it = m_nodeMap.find(spec.id);
        if (it == m_nodeMap.end()) {
            throw std::runtime_error("Node not registered: " + spec.id);
        }
        baseNodes.push_back(it->second);
    }
    auto path = std::make_shared<core::GraphPath>(m_eventBus, baseNodes);
    for (auto& bus : pathEventBuses) {
        path->retainEventBus(std::move(bus));
    }
    return path;
}

}  // namespace graphflow::engine
