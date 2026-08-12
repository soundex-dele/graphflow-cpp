// core/graph_edge.h
#pragma once
#include <string>
#include <functional>

namespace graphflow::core {

class EventBus;   // 前向声明：GraphEdge 持有裸指针，不形成环（EventBus 仅以 std::string 为 key）

// GraphEdge：节点间连接边 + 承载信道。
// 集成了 EventBus*（该边对应的信道总线）与 dataType（边承载的数据类型标识），
// 取代旧的 GraphPortInfo 三元组 {edge, eventBus, dataType}。
class GraphEdge {
public:
    GraphEdge() = default;

    // 从任意字符串隐式构造（含 typeid 名/服务名/连接边名）—— 不加 explicit，迁移友好。
    // eventBus 默认 nullptr：用于事件类型名/裸名等不绑定信道的场景。
    GraphEdge(const std::string& name, EventBus* bus = nullptr)             // NOLINT(google-explicit-constructor)
        : m_name(name), m_eventBus(bus) {}
    GraphEdge(std::string&& name, EventBus* bus = nullptr)                    // NOLINT(google-explicit-constructor)
        : m_name(std::move(name)), m_eventBus(bus) {}
    GraphEdge(const char* name, EventBus* bus = nullptr)                      // NOLINT(google-explicit-constructor)
        : m_name(name), m_eventBus(bus) {}

    // 节点间连接边 "from->to"，承载 dataType 类型数据，挂在 bus 信道上
    // condition 为业务路由标签，可选（缺失时为空串）；同起终点、同 dataType、不同 condition
    //   视为不同边（不同信道），用于决策节点按语义选路而非按目标节点名选路。
    static GraphEdge connect(const std::string& from,
                             const std::string& to,
                             const std::string& dataType = "",
                             EventBus* bus = nullptr,
                             const std::string& condition = "") {
        GraphEdge e;
        e.m_from = from;
        e.m_to = to;
        e.m_dataType = dataType;
        e.m_name = from + "->" + to;
        e.m_eventBus = bus;
        e.m_route = condition;
        return e;
    }

    // 访问
    const std::string& name() const noexcept { return m_name; }
    const std::string& from() const noexcept { return m_from; }
    const std::string& to() const noexcept { return m_to; }
    const std::string& dataType() const noexcept { return m_dataType; }
    const std::string& condition() const noexcept { return m_route; }
    EventBus* eventBus() const noexcept { return m_eventBus; }
    bool isConnection() const noexcept { return !m_from.empty(); }

    // 设置/重绑信道（GraphNode / GraphEngine 在装配阶段使用）
    void setEventBus(EventBus* bus) noexcept { m_eventBus = bus; }
    void setDataType(const std::string& dataType) noexcept { m_dataType = dataType; }
    void setRoute(const std::string& condition) noexcept { m_route = condition; }

    // 显式转 string（不提供隐式 operator string，保证类型安全）
    const std::string& toString() const noexcept { return m_name; }

    // 解析 "from->to" 形式，或裸名。解析得到的边 condition 为空。
    static GraphEdge fromString(const std::string& s, EventBus* bus = nullptr) {
        GraphEdge e;
        e.m_eventBus = bus;
        auto pos = s.find("->");
        if (pos != std::string::npos) {
            e.m_from = s.substr(0, pos);
            e.m_to = s.substr(pos + 2);
            e.m_name = s;
        } else {
            e.m_name = s;
        }
        return e;
    }

    // 比较（唯一性判据为完整身份 m_name + dataType + condition）
    bool operator==(const GraphEdge& o) const noexcept {
        return m_name == o.m_name && m_dataType == o.m_dataType && m_route == o.m_route;
    }
    bool operator!=(const GraphEdge& o) const noexcept { return !(*this == o); }
    bool operator<(const GraphEdge& o) const noexcept {
        if (m_name != o.m_name) return m_name < o.m_name;
        if (m_dataType != o.m_dataType) return m_dataType < o.m_dataType;
        return m_route < o.m_route;
    }

private:
    std::string m_name;       // 规范化完整边名
    std::string m_from;       // 源节点名（仅连接边有值）
    std::string m_to;         // 目的节点名（仅连接边有值）
    std::string m_dataType;   // 边承载的数据类型标识（可空）
    std::string m_route;      // 业务路由标签（可空）；同起终点不同 condition 视为不同边
    EventBus* m_eventBus = nullptr;  // 该边对应信道的事件总线（可空）
};

}  // namespace graphflow::core

// hash 特化，支持 unordered_map<GraphEdge, ...>（按完整身份 name+dataType+condition）
namespace std {
template<>
struct hash<graphflow::core::GraphEdge> {
    size_t operator()(const graphflow::core::GraphEdge& e) const noexcept {
        size_t h = hash<string>{}(e.name());
        h ^= hash<string>{}(e.dataType()) + 0x9e3779b9 + (h << 6) + (h >> 2);
        h ^= hash<string>{}(e.condition()) + 0x9e3779b9 + (h << 6) + (h >> 2);
        return h;
    }
};
}  // namespace std
