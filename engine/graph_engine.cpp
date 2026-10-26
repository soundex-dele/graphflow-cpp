// engine/graph_engine.cpp
#include "engine/graph_engine.h"
#include "engine/json_graph_builder.h"
#include "utils/logger.h"

#include <unordered_set>
#include <utility>

namespace graphflow::engine {

static const std::string TAG = "GraphEngine";

namespace {

using GraphPaths = std::vector<std::shared_ptr<core::GraphPath>>;

std::vector<std::shared_ptr<core::EventBus>> collectEdgeBuses(const GraphPaths& paths) {
    std::vector<std::shared_ptr<core::EventBus>> buses;
    std::unordered_set<core::EventBus*> seen;
    for (const auto& path : paths) {
        if (!path) continue;
        for (auto& bus : path->eventBusesSnapshot()) {
            if (bus && seen.insert(bus.get()).second) {
                buses.push_back(std::move(bus));
            }
        }
    }
    return buses;
}

void closeAndDrainEdgeBuses(const GraphPaths& paths) {
    auto buses = collectEdgeBuses(paths);
    for (const auto& bus : buses) bus->close();
    for (const auto& bus : buses) bus->drain();
}

void releasePaths(GraphPaths& paths) {
    closeAndDrainEdgeBuses(paths);
    for (auto& path : paths) {
        if (path) path->release();
    }
    paths.clear();
}

bool initPaths(const GraphPaths& paths) {
    for (const auto& path : paths) {
        if (!path || !path->init()) return false;
    }
    return true;
}

bool startPaths(const GraphPaths& paths) {
    for (const auto& path : paths) {
        if (!path || !path->start()) return false;
    }
    return true;
}

}  // namespace

GraphEngine::GraphEngine(size_t threadPoolSize)
    : m_builder(std::make_unique<JsonGraphBuilder>()),
      m_threadPool(std::make_shared<core::EventThreadPool>(threadPoolSize)),
      m_eventBus(std::make_unique<core::EventBus>(m_threadPool)) {
    m_builder->setEventBus(m_eventBus.get());
    m_builder->setNodeFactory(&m_factory);
    m_builder->setBlackboard(&m_blackboard);
}

GraphEngine::~GraphEngine() {
    release();
}

bool GraphEngine::loadFromJsonFile(const std::string& configFile) {
    if (state() != core::LifecycleState::Created) {
        LOG_ERROR(TAG, "load requires Created state, current=" << static_cast<int>(state()));
        return false;
    }
    return applyBuildResult(m_builder->buildFromJsonFile(configFile));
}

bool GraphEngine::loadFromJson(const nlohmann::json& configs) {
    if (state() != core::LifecycleState::Created) {
        LOG_ERROR(TAG, "load requires Created state, current=" << static_cast<int>(state()));
        return false;
    }
    return applyBuildResult(m_builder->buildFromJson(configs));
}

BuildResult GraphEngine::buildFromNodes(
    const std::vector<std::shared_ptr<core::GraphNode>>& nodes,
    const std::vector<NodeConnection>& connections, GraphBuilder::EdgeTransport transport) {
    BuildResult result;
    std::unordered_set<const core::NodeBase*> oldNodes;
    for (const auto& path : m_paths) {
        for (const auto& node : path->getNodes()) oldNodes.insert(node.get());
    }
    std::unordered_set<std::string> ids;
    for (size_t i = 0; i < nodes.size(); ++i) {
        const auto& node = nodes[i];
        const auto ptr = "/nodes/" + std::to_string(i);
        if (!node || node->state() != core::LifecycleState::Created || oldNodes.count(node.get())) {
            result.errors.emplace_back("node must be fresh and independent of the current graph", ptr);
            continue;
        }
        if (node->getName().empty() || !ids.insert(node->getName()).second) {
            result.errors.emplace_back("node name must be nonempty and unique", ptr);
        }
        const auto validateEdges = [&](const auto& edges) {
            for (const auto& edge : edges) {
                if (edge.isConnection()) {
                    result.errors.emplace_back("node already has connection edges", ptr);
                    break;
                }
            }
        };
        validateEdges(node->getPubEdges());
        validateEdges(node->getSubEdges());
    }
    for (size_t i = 0; i < connections.size(); ++i) {
        const auto& edge = connections[i];
        if (!ids.count(edge.from) || !ids.count(edge.to)) {
            result.errors.emplace_back("unknown connection endpoint: " + edge.from + "->" + edge.to,
                                       "/connections/" + std::to_string(i));
        }
    }
    if (!result.ok() || nodes.empty()) return result;

    GraphBuilder builder;
    builder.setEventBus(m_eventBus.get());
    std::vector<std::string> names;
    for (const auto& node : nodes) {
        builder.registerNode(node->getName(), node);
        names.push_back(node->getName());
    }
    auto path = builder.buildGraph(names, connections, transport);
    for (const auto& node : nodes) node->setEventBus(m_eventBus.get());
    result.paths.push_back(std::move(path));
    return result;
}

bool GraphEngine::loadFromNodes(
    const std::vector<std::shared_ptr<core::GraphNode>>& nodes,
    const std::vector<NodeConnection>& connections, GraphBuilder::EdgeTransport transport) {
    if (state() != core::LifecycleState::Created) return false;
    return applyBuildResult(buildFromNodes(nodes, connections, transport));
}

bool GraphEngine::reloadFromNodes(
    const std::vector<std::shared_ptr<core::GraphNode>>& nodes,
    const std::vector<NodeConnection>& connections, GraphBuilder::EdgeTransport transport) {
    if (state() == core::LifecycleState::Released) return false;
    if (state() == core::LifecycleState::Created) return loadFromNodes(nodes, connections, transport);
    auto result = buildFromNodes(nodes, connections, transport);
    if (!result.ok()) {
        for (const auto& error : result.errors) {
            LOG_ERROR(TAG, "build error @ " << error.jsonPointer << ": " << error.message);
        }
        return false;
    }
    const auto previousState = state();
    if (previousState == core::LifecycleState::Running) stop();
    return applyReloadResult(std::move(result), previousState);
}

bool GraphEngine::reloadFromJsonFile(const std::string& configFile) {
    if (state() == core::LifecycleState::Released) {
        LOG_ERROR(TAG, "reload after release not allowed");
        return false;
    }
    if (state() == core::LifecycleState::Created) {
        return loadFromJsonFile(configFile);
    }
    const auto previousState = state();
    if (previousState == core::LifecycleState::Running) stop();
    return applyReloadResult(m_builder->buildFromJsonFile(configFile), previousState);
}

bool GraphEngine::reloadFromJson(const nlohmann::json& configs) {
    if (state() == core::LifecycleState::Released) {
        LOG_ERROR(TAG, "reload after release not allowed");
        return false;
    }
    if (state() == core::LifecycleState::Created) {
        return loadFromJson(configs);
    }
    const auto previousState = state();
    if (previousState == core::LifecycleState::Running) stop();
    return applyReloadResult(m_builder->buildFromJson(configs), previousState);
}

bool GraphEngine::applyReloadResult(BuildResult result,
                                    core::LifecycleState previousState) {
    const auto restoreOldGraph = [this, previousState] {
        if (previousState == core::LifecycleState::Running && !start()) {
            LOG_ERROR(TAG, "reload rollback: old graph failed to restart");
            return false;
        }
        return true;
    };

    if (!result.ok()) {
        LOG_ERROR(TAG, "reload failed, keeping old graph");
        restoreOldGraph();
        return false;
    }

    GraphPaths newPaths = std::move(result.paths);
    if (!initPaths(newPaths)) {
        LOG_ERROR(TAG, "reload: new graph init failed, keeping old graph");
        releasePaths(newPaths);
        restoreOldGraph();
        return false;
    }
    if (previousState == core::LifecycleState::Running && !startPaths(newPaths)) {
        LOG_ERROR(TAG, "reload: new graph start failed, keeping old graph");
        releasePaths(newPaths);
        restoreOldGraph();
        return false;
    }

    GraphPaths oldPaths = std::move(m_paths);
    m_paths = std::move(newPaths);
    setState(previousState == core::LifecycleState::Running
                 ? core::LifecycleState::Running
                 : core::LifecycleState::Initialized);
    releasePaths(oldPaths);
    return true;
}

bool GraphEngine::applyBuildResult(BuildResult result) {
    if (!result.ok()) {
        for (const auto& error : result.errors) {
            LOG_ERROR(TAG, "build error @ " << error.jsonPointer << ": " << error.message);
        }
        return false;
    }
    GraphPaths oldPaths = std::move(m_paths);
    m_paths = std::move(result.paths);
    releasePaths(oldPaths);
    return true;
}

bool GraphEngine::doInit() {
    std::vector<std::shared_ptr<core::GraphPath>> initialized;
    for (auto& path : m_paths) {
        if (!path->init()) {
            LOG_ERROR(TAG, "path init failed; rolling back " << initialized.size() << " path(s)");
            for (auto it = initialized.rbegin(); it != initialized.rend(); ++it) {
                try { (*it)->release(); } catch (const std::exception& e) {
                    LOG_ERROR(TAG, "rollback release exception: " << e.what());
                }
            }
            return false;
        }
        initialized.push_back(path);
    }
    return true;
}

bool GraphEngine::doStart() {
    std::vector<std::shared_ptr<core::GraphPath>> started;
    for (auto& path : m_paths) {
        if (!path->start()) {
            LOG_ERROR(TAG, "path start failed; rolling back " << started.size() << " path(s)");
            for (auto it = started.rbegin(); it != started.rend(); ++it) {
                try { (*it)->stop(); } catch (const std::exception& e) {
                    LOG_ERROR(TAG, "rollback stop exception: " << e.what());
                }
            }
            return false;
        }
        started.push_back(path);
    }
    return true;
}

void GraphEngine::doStop() {
    for (auto& path : m_paths) path->stop();
}

void GraphEngine::doRelease() {
    auto edgeBuses = collectEdgeBuses(m_paths);
    if (m_eventBus) m_eventBus->close();
    for (const auto& bus : edgeBuses) bus->close();

    if (m_eventBus) m_eventBus->drain();
    for (const auto& bus : edgeBuses) bus->drain();

    for (auto& path : m_paths) path->release();
    m_paths.clear();
}

const std::vector<std::shared_ptr<core::GraphPath>>& GraphEngine::paths() const noexcept {
    return m_paths;
}

core::EventBus* GraphEngine::eventBus() noexcept {
    return m_eventBus.get();
}

void GraphEngine::registerCreator(const std::string& nodeClass,
                                  std::shared_ptr<NodeCreator> creator) {
    m_factory.registerCreator(nodeClass, std::move(creator));
}

void GraphEngine::setConfigBuilder(ConfigBuilderCallback builder) noexcept {
    if (m_builder) m_builder->setConfigBuilder(std::move(builder));
}

}  // namespace graphflow::engine
