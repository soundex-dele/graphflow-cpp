// engine/json_graph_builder.cpp
#include "engine/json_graph_builder.h"
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <utility>
#include "utils/logger.h"

namespace graphflow::engine {

static const std::string TAG = "JsonGraphBuilder";

JsonGraphBuilder::JsonGraphBuilder() {}

void JsonGraphBuilder::setEventBus(core::EventBus* bus) {
    m_eventBus = bus;
    m_graphBuilder.setEventBus(bus);
}

BuildResult JsonGraphBuilder::buildFromJsonFile(const std::string& configFile) {
    BuildResult result;
    std::ifstream file(configFile);
    if (!file.is_open()) {
        LOG_ERROR(TAG, "cannot open config file: " << configFile);
        result.errors.emplace_back("cannot open config file: " + configFile, "");
        return result;
    }
    nlohmann::json configs;
    try {
        file >> configs;
    } catch (const std::exception& e) {
        LOG_ERROR(TAG, "parse config error: " << e.what());
        result.errors.emplace_back(std::string("parse error: ") + e.what(), "");
        return result;
    }
    return buildFromJson(configs);
}

BuildResult JsonGraphBuilder::buildFromJson(const nlohmann::json& configs) {
    BuildResult result;
    if (!configs.is_object()) {
        result.errors.emplace_back("graph config must be an object", "");
        return result;
    }
    if (!configs.contains("nodes") || !configs.contains("paths")) {
        result.errors.emplace_back("unsupported json format: need {nodes, paths}", "");
        return result;
    }
    if (!configs.at("nodes").is_array()) {
        result.errors.emplace_back("nodes must be an array", "/nodes");
        return result;
    }
    if (!configs.at("paths").is_array()) {
        result.errors.emplace_back("paths must be an array", "/paths");
        return result;
    }

    LOG_INFO(TAG, "buildFromJson: nodes=" << configs.at("nodes").size()
             << ", paths=" << configs.at("paths").size());
    try {
        return buildFromNewFormat(configs);
    } catch (const nlohmann::json::exception& e) {
        result.errors.emplace_back(std::string("invalid graph config: ") + e.what(), "");
    } catch (const std::exception& e) {
        result.errors.emplace_back(std::string("graph build failed: ") + e.what(), "");
    }
    return result;
}

BuildResult JsonGraphBuilder::buildFromNewFormat(const nlohmann::json& configs) {
    // 事务化：先在临时上下文（nodes 表 + 临时 GraphBuilder）中校验与装配，
    //   全程成功才提交（move 进持久 m_graphBuilder）；任一失败丢弃临时，不污染旧图。
    BuildResult result;

    // 节点定义索引（id 是否已声明）
    std::unordered_map<std::string, size_t> declaredNodeIds;
    // 临时节点表（id -> GraphNode），失败时直接丢弃
    std::unordered_map<std::string, std::shared_ptr<core::GraphNode>> tempNodes;
    // 临时 builder：复用 setEventBus，不暴露持久表
    GraphBuilder tempBuilder;
    if (m_eventBus) tempBuilder.setEventBus(m_eventBus);

    // === validate nodes（schema + 唯一 id）===
    const auto& nodesArr = configs.at("nodes");
    for (size_t i = 0; i < nodesArr.size(); ++i) {
        const auto& nc = nodesArr[i];
        std::string ptr = "/nodes/" + std::to_string(i);
        if (!nc.is_object() || !nc.contains("id") || !nc.contains("class")) {
            result.errors.emplace_back("node missing id or class", ptr);
            continue;
        }
        if (!nc.at("id").is_string()) {
            result.errors.emplace_back("node id must be a string", ptr + "/id");
            continue;
        }
        if (!nc.at("class").is_string()) {
            result.errors.emplace_back("node class must be a string", ptr + "/class");
            continue;
        }
        std::string nodeId = nc.at("id").get<std::string>();
        std::string nodeClass = nc.at("class").get<std::string>();
        if (nodeId.empty()) {
            result.errors.emplace_back("node id is empty", ptr + "/id");
            continue;
        }
        if (nodeClass.empty()) {
            result.errors.emplace_back("node class is empty", ptr + "/class");
            continue;
        }
        if (declaredNodeIds.count(nodeId)) {
            result.errors.emplace_back("duplicate node id: " + nodeId, ptr + "/id");
            continue;
        }
        declaredNodeIds[nodeId] = i;
    }
    if (!result.errors.empty()) {
        LOG_ERROR(TAG, "node validation failed: " << result.errors.size() << " errors");
        return result;
    }

    // === instantiate nodes（用注入的 factory 创建）===
    for (size_t i = 0; i < nodesArr.size(); ++i) {
        const auto& nc = nodesArr[i];
        std::string ptr = "/nodes/" + std::to_string(i);
        std::string nodeId = nc.at("id").get<std::string>();
        std::string nodeClass = nc.at("class").get<std::string>();

        std::shared_ptr<core::GraphNode> node;
        if (!m_factory) {
            LOG_ERROR(TAG, "no NodeFactory injected; cannot create: " << nodeClass);
            result.errors.emplace_back("no NodeFactory injected for node: " + nodeClass, ptr);
            continue;
        }
        try {
            NodeConfig cfg;
            if (m_configBuilder) {
                cfg = m_configBuilder(nodeClass, nc);
            } else {
                cfg = NodeConfig::builder()
                    .nodeClass(nodeClass)
                    .eventBus(m_eventBus)
                    .blackboard(m_blackboard)
                    .params(nc.value("params", nlohmann::json::object()))
                    .nodeName(nodeId)
                    .build();
            }
            node = m_factory->createNode(cfg);
        } catch (const std::exception& e) {
            result.errors.emplace_back(
                std::string("node creation failed: ") + e.what(), ptr);
            continue;
        }
        if (!node) {
            LOG_ERROR(TAG, "cannot create node: " << nodeClass << " (" << nodeId << ")");
            result.errors.emplace_back("cannot create node: " + nodeClass + " (" + nodeId + ")", ptr);
            continue;
        }
        LOG_INFO(TAG, "created node: " << nodeClass << " (" << nodeId << ")");
        tempNodes[nodeId] = node;
        tempBuilder.registerNode(nodeId, node);
    }
    if (!result.errors.empty()) {
        LOG_ERROR(TAG, "node instantiation failed");
        return result;
    }

    // === validate + connect paths ===
    const auto& pathsArr = configs.at("paths");
    for (size_t i = 0; i < pathsArr.size(); ++i) {
        const auto& pc = pathsArr[i];
        std::string ptr = "/paths/" + std::to_string(i);
        if (!pc.is_object() || !pc.contains("name") || !pc.contains("path")) {
            result.errors.emplace_back("path missing name or path", ptr);
            continue;
        }
        if (!pc.at("name").is_string()) {
            result.errors.emplace_back("path name must be a string", ptr + "/name");
            continue;
        }
        std::string pathName = pc.at("name").get<std::string>();
        const auto& pathArr = pc.at("path");
        if (!pathArr.is_array() || pathArr.empty()) {
            result.errors.emplace_back("path is empty or not array: " + pathName, ptr + "/path");
            continue;
        }

        std::vector<graphflow::engine::PathNodeSpec> specs;
        specs.reserve(pathArr.size());
        for (size_t j = 0; j < pathArr.size(); ++j) {
            const auto& pn = pathArr[j];
            std::string pptr = ptr + "/path/" + std::to_string(j);
            if (!pn.is_object() || !pn.contains("id")) {
                result.errors.emplace_back("path node missing id", pptr + "/id");
                break;
            }
            if (!pn.at("id").is_string()) {
                result.errors.emplace_back("path node id must be a string", pptr + "/id");
                break;
            }
            std::string nodeId = pn.at("id").get<std::string>();
            if (!declaredNodeIds.count(nodeId)) {
                // 审查 3.5：未知节点不再 continue 静默跳过，使整路径失败
                result.errors.emplace_back("unknown node referenced: " + nodeId, pptr + "/id");
                break;
            }
            if (pn.contains("edge") && !pn.at("edge").is_string()) {
                result.errors.emplace_back("path edge must be a string", pptr + "/edge");
                break;
            }
            // route：缺失为空串；字符串原样传入；其他类型构建失败并附准确 JSON Pointer。
            if (pn.contains("condition") && !pn.at("condition").is_string()) {
                result.errors.emplace_back("path condition must be a string", pptr + "/condition");
                break;
            }
            std::string edge = pn.value("edge", "");
            std::string condition = pn.value("condition", "");
            specs.push_back({nodeId, edge, condition});
        }
        if (!result.errors.empty()) continue;

        std::shared_ptr<core::GraphPath> path;
        try {
            path = tempBuilder.build(specs);
        } catch (const std::exception& e) {
            result.errors.emplace_back(
                std::string("build path failed: ") + e.what(), ptr);
            continue;
        }
        if (path) {
            result.paths.push_back(path);
            LOG_INFO(TAG, "path built: " << pathName);
        } else {
            result.errors.emplace_back("build path returned null: " + pathName, ptr);
        }
    }
    if (!result.errors.empty()) {
        LOG_ERROR(TAG, "path build failed: " << result.errors.size() << " errors");
        return result;
    }

    // === dataType 连线 warn(spec §3.3:修 README §229 债)===
    // 对每条边 from->to,比对 from 节点 pub 边 dataType 与 to 节点 sub 边 dataType。
    //   不一致 LOG_WARN(不 fail,保持宽松)。空 dataType(未声明)跳过。
    for (auto& [id, node] : tempNodes) {
        for (const auto& pub : node->getPubEdges()) {
            if (pub.dataType().empty()) continue;
            if (!pub.isConnection()) continue;   // 仅连接边(from->to)有意义
            auto it = tempNodes.find(pub.to());
            if (it == tempNodes.end()) continue;
            for (const auto& sub : it->second->getSubEdges()) {
                if (sub.name() == pub.name() && !sub.dataType().empty()
                    && sub.dataType() != pub.dataType()) {
                    LOG_WARN(TAG, "dataType mismatch on edge " << pub.name()
                             << ": pub=" << pub.dataType()
                             << " vs sub=" << sub.dataType());
                }
            }
        }
    }

    // === commit ===
    // 成功：把临时 builder 的节点表与边总线表整体移交到持久 m_graphBuilder。
    //   边总线 shared_ptr 随 absorb 迁移，GraphEdge 裸指针仍有效；节点 shared_ptr
    //   同时被 paths 与 m_graphBuilder 持有，生命周期安全。
    //   失败路径在上方已提前 return（临时 builder 析构，持久图不变）。
    m_graphBuilder.absorb(tempBuilder);
    return result;
}

}  // namespace graphflow::engine
