// engine/node_config.h
#pragma once
#include <memory>
#include <nlohmann/json.hpp>
#include <string>
#include "core/blackboard.h"
#include "core/event_bus.h"
#include "core/node_runtime_context.h"

namespace graphflow::engine {

class NodeConfig {
public:
    class Builder;

    NodeConfig() = default;
    static Builder builder();

    std::string nodeClass;
    core::EventBus* eventBus = nullptr;
    core::Blackboard* blackboard = nullptr;
    nlohmann::json params;
    std::string nodeId;
    std::string nodeName;
    std::shared_ptr<core::NodeRuntimeContext> runtimeContext;
};

class NodeConfig::Builder {
public:
    Builder() = default;

    Builder& nodeClass(const std::string& cls) { m_config.nodeClass = cls; return *this; }
    Builder& eventBus(core::EventBus* bus) { m_config.eventBus = bus; return *this; }
    Builder& blackboard(core::Blackboard* bb) { m_config.blackboard = bb; return *this; }
    Builder& params(const nlohmann::json& p) { m_config.params = p; return *this; }
    Builder& nodeId(const std::string& id) { m_config.nodeId = id; return *this; }
    Builder& nodeName(const std::string& name) { m_config.nodeName = name; return *this; }
    Builder& runtimeContext(std::shared_ptr<core::NodeRuntimeContext> ctx) {
        m_config.runtimeContext = std::move(ctx);
        return *this;
    }
    Builder& addParam(const std::string& key, const nlohmann::json& value) {
        m_config.params[key] = value;
        return *this;
    }
    Builder& addParams(std::initializer_list<std::pair<const std::string, nlohmann::json>> list) {
        for (const auto& item : list) {
            m_config.params[item.first] = item.second;
        }
        return *this;
    }

    NodeConfig build() const { return m_config; }
    operator NodeConfig() const { return m_config; }

private:
    NodeConfig m_config;
};

inline NodeConfig::Builder NodeConfig::builder() { return Builder(); }

}  // namespace graphflow::engine
