// engine/build_result.h
#pragma once
#include <memory>
#include <string>
#include <vector>
#include "core/graph_path.h"

namespace graphflow::engine {

// 构建错误：消息 + JSON Pointer 定位（如 /paths/0/path/1/id）。
struct BuildError {
    std::string message;
    std::string jsonPointer;

    BuildError() = default;
    BuildError(std::string msg, std::string ptr)
        : message(std::move(msg)), jsonPointer(std::move(ptr)) {}
};

// 构建结果：成功时 paths 为非空（或合法空图），errors 为空；失败时 paths 为空、errors 非空。
// 用 ok() 判定成功，取代旧 "paths 是否为空" 判失败（避免合法空图歧义）。
struct BuildResult {
    std::vector<std::shared_ptr<core::GraphPath>> paths;
    std::vector<BuildError> errors;

    bool ok() const noexcept { return errors.empty(); }
};

}  // namespace graphflow::engine
