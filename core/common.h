// core/common.h
#ifndef GRAPHFLOW_COMMON_H
#define GRAPHFLOW_COMMON_H

#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>
#include "core/lifecycle.h"

namespace graphflow::core {

// 节点接口：生命周期由 Lifecycle 基类托管（init/start/stop/release 为模板方法），
// 派生类按需 override doInit/doStart/doStop/doRelease。
class INode : public Lifecycle {
public:
    virtual ~INode() = default;
};

// 节点基类
class NodeBase : public INode {
public:
    explicit NodeBase(const std::string& nodeName) : m_nodeName(nodeName) {}
    const std::string& getName() const { return m_nodeName; }
    virtual ~NodeBase() = default;

protected:
    std::string m_nodeName;
};

}  // namespace graphflow::core

#endif  // GRAPHFLOW_COMMON_H
