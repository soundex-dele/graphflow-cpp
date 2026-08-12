// engine/graph_builder.h
#pragma once
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>
#include "core/event_bus.h"
#include "core/graph_node.h"
#include "core/graph_path.h"

namespace graphflow::engine {

// 路径节点规格：JSON 路径元素解析为具名结构（取代 pair<string,string>）。
//   id       拓扑构建使用的节点 id，不参与运行时选路。
//   dataType 边承载的事件类型（即 JSON 的 edge 字段、旧 dataType）。
//   condition    业务路由标签，可选，缺失为 ""。
//   该元素描述"本节点到下一节点的出边"，因此仅 id 参与节点表，dataType/condition
//   描述本元素代表的出边；路径最后一个元素没有出边，其 dataType/condition 不产生连接。
struct PathNodeSpec {
    std::string id;
    std::string dataType;
    std::string condition;
};

// 内部装配器：registerNode + build(specs)，为每条边创建独立 EventBus。
//
// 边创建策略（审查 3.2 修复 + 语义路由扩展）：
//   按路径中的相邻位置建边，不再先按节点名折叠邻接表。
//   即 specs[i].id -> specs[i+1].id，dataType = specs[i].dataType，condition = specs[i].condition。
//   同一节点可在一条路径中多次出现，每个位置的出边 dataType/condition 可不同。
//   边完整身份 = (from->to, dataType, condition)：
//     - 完全相同身份的边去重（单次 build 内复用同一 EventBus）。
//     - 同起终点、同 dataType、不同 condition 是不同边，使用各自独立 EventBus，互不串流。
//   跨路径复用为显式策略：当前 = 不跨 build 复用（每次 build 各自创建边总线），
//   如需跨路径共享须显式扩展接口。
class GraphBuilder {
public:
    GraphBuilder();
    ~GraphBuilder();

    bool registerNode(const std::string& nodeName,
                      std::shared_ptr<core::GraphNode> node);

    bool findNode(const std::string& nodeName,
                  std::shared_ptr<core::GraphNode>& node) const {
        auto it = m_nodeMap.find(nodeName);
        if (it != m_nodeMap.end()) {
            node = it->second;
            return true;
        }
        return false;
    }

    GraphBuilder& setEventBus(core::EventBus* eventBus);

    // 构建路径（单数据类型）
    std::shared_ptr<core::GraphPath> build(
        const std::vector<std::string>& nodeNames, const std::string& dataType = "");

    // 构建路径（多数据类型，旧接口）：每一项 (id, dataType) 等价于 condition="" 的 PathNodeSpec。
    //   保留以兼容已有调用方；内部转发到 build(vector<PathNodeSpec>)。
    std::shared_ptr<core::GraphPath> build(
        const std::vector<std::pair<std::string, std::string>>& nodeDataTypes);

    // 构建路径（含 condition）：按相邻位置建边，condition 写入连接边身份与边总线键。
    std::shared_ptr<core::GraphPath> build(const std::vector<PathNodeSpec>& specs);

    // 清空已注册节点与边总线表（供事务化加载/失败回滚使用）。
    void reset();

    // 事务化提交：把 other 的节点表与边总线表整体移动到 this，other 被 reset。
    // 用于 JsonGraphBuilder 在临时 builder 上构建成功后原子提交（边总线 shared_ptr
    // 随之迁移，GraphEdge 裸指针仍有效）。失败时临时 builder 直接析构，持久图不变。
    void absorb(GraphBuilder& other);

private:
    core::EventBus* m_eventBus = nullptr;
    std::unordered_map<std::string, std::shared_ptr<core::GraphNode>> m_nodeMap;
    // 边总线缓存键 = (topic="from->to", dataType, condition)；同起终点不同 condition 各自独立总线。
    std::map<std::tuple<std::string, std::string, std::string>, std::shared_ptr<core::EventBus>> m_nodeEventBusMap;
    mutable std::mutex m_mutex;
};

}  // namespace graphflow::engine
