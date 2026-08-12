# graphflow-cpp

GraphFlow 的 C++17 图运行时基础库。它提供生命周期、事件总线、图节点、图路径、黑板、JSON 图构建和 `GraphEngine` 门面，适合作为事件驱动工作流、Agent、量化流程和行为树等上层引擎的底座。

本仓库只包含 C++ Core 与 GraphEngine；Python 绑定、Agent Engine、Quant Engine 和 BehaviorTree 分别位于独立仓库中。

## 核心能力

- 通过 `EventT<T>` 定义强类型事件，支持同步和异步发布/订阅。
- 通过 JSON 声明节点、路径、边类型和条件，并由注册的节点工厂创建实例。
- 同一节点可出现在多条路径中，用于扇入、扇出、循环和多流汇聚。
- `Blackboard` 为同一引擎内的节点提供共享状态。
- `GraphEngine` 统一管理加载、热重载和生命周期。
- `GraphNode::halt()` 提供与整图 `stop()` 相互独立的节点软中断能力。

## 仓库关系

`graphflow-cpp` 是其余 GraphFlow 仓库的原生基础依赖。下游仓库通常将它固定为 `vendor/graphflow-cpp` Git submodule，再通过 `add_subdirectory()` 构建。

本仓库自身还通过 submodule 引入 `utoolkit`：

```text
graphflow-cpp
└── utoolkit (Git submodule)
```

因此克隆和更新时都应包含递归 submodule。

## 环境要求

- Git
- CMake 3.16 或更高版本
- 支持 C++17 的编译器
- 仅在构建测试时需要可被 CMake `find_package(GTest CONFIG)` 找到的 GoogleTest

运行库所需的 nlohmann/json 位于 [`third_party/nlohmann`](third_party/nlohmann/)，日志和线程池等基础设施由 [`utoolkit`](utoolkit/) 提供。

## 快速开始

以下命令适用于 PowerShell：

```powershell
git clone --recurse-submodules https://github.com/soundex-dele/graphflow-cpp.git
Set-Location graphflow-cpp

cmake -S . -B build `
  -DGRAPHFLOW_BUILD_EXAMPLES=ON `
  -DGRAPHFLOW_BUILD_TESTS=OFF
cmake --build build --config Release

.\build\examples\minimal\Release\graphflow_minimal.exe
```

`graphflow_minimal` 从 [`examples/minimal/graph.json`](examples/minimal/graph.json) 加载图，完成一次 `source -> sink` 事件传递。正常输出包含：

```text
minimal example: DATA FLOWED source->sink OK
```

在 Linux 或 macOS 上，单配置生成器通常将程序生成到 `build/examples/minimal/graphflow_minimal`。

## 最小图

完整可运行代码见 [`examples/minimal/main.cpp`](examples/minimal/main.cpp)。基本接入流程如下：

```cpp
#include "engine/graph_engine.h"

graphflow::engine::GraphEngine engine;

// SourceCreator / SinkCreator 实现 NodeCreator，并返回对应 GraphNode。
engine.registerCreator("SourceNode", sourceCreator);
engine.registerCreator("SinkNode", sinkCreator);

if (!engine.loadFromJsonFile("examples/minimal/graph.json") ||
    !engine.init() ||
    !engine.start()) {
    return 1;
}

engine.stop();
engine.release();
```

对应 JSON：

```json
{
  "nodes": [
    { "id": "source", "class": "SourceNode" },
    { "id": "sink", "class": "SinkNode" }
  ],
  "paths": [
    {
      "name": "minimal",
      "path": [
        { "id": "source", "edge": "PayloadEvent" },
        { "id": "sink" }
      ]
    }
  ]
}
```

`class` 必须与 `registerCreator()` 注册的名称一致。路径中每个非末尾元素的 `edge` 描述从当前节点到下一个节点所承载的事件类型，可选 `condition` 用于条件路由；末尾节点不产生出边。节点参数可通过 `params` 传入 `NodeConfig`。

## 生命周期

典型顺序为：

```text
load -> init -> start -> stop -> release
                  ^       |
                  └───────┘  可再次 start
```

- `loadFromJson*()` 首次构建图；只能在 `Created` 状态调用。
- `init()` 用于建立订阅和准备资源。
- `start()` 用于启动节点或发布首个事件。
- `stop()` 停止当前运行，但保留再次 `start()` 的能力。
- `release()` 是终态清理；若引擎仍在运行，会先执行 `stop()`。
- `reloadFromJson*()` 可在 `Initialized`、`Running` 或 `Stopped` 状态事务式替换图；构建失败时保留旧图。

节点的 `start` / `stop` 并非每个节点都必须实现。没有启动或停止动作的节点可以直接使用默认钩子；但引擎级生命周期仍用于保证订阅先于数据发布、支持可逆停止，并在释放前收拢异步任务。派生节点只需按职责覆盖 `doInit()`、`doStart()`、`doStop()` 和 `doRelease()` 中必要的部分。

## 在其他 CMake 项目中使用

当前推荐通过 Git submodule 和 `add_subdirectory()` 引入：

```powershell
git submodule add https://github.com/soundex-dele/graphflow-cpp.git vendor/graphflow-cpp
git submodule update --init --recursive
```

父项目的 `CMakeLists.txt`：

```cmake
set(GRAPHFLOW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(GRAPHFLOW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
add_subdirectory(vendor/graphflow-cpp)

add_executable(my_app main.cpp)
target_link_libraries(my_app PRIVATE graphflow::engine)
```

只使用事件总线、节点、路径或黑板等底层能力时，可链接 `graphflow::core`。本仓库目前没有定义独立的安装或导出包流程，因此不应假设 `find_package(graphflow-cpp)` 可用。

## CMake 选项与目标

| 选项 | 默认值 | 说明 |
| --- | --- | --- |
| `GRAPHFLOW_BUILD_EXAMPLES` | `ON` | 构建仓库内示例 |
| `GRAPHFLOW_BUILD_TESTS` | `OFF` | 构建 GoogleTest 测试 |

| 目标 | 别名 | 说明 |
| --- | --- | --- |
| `graphflow_core` | `graphflow::core` | 生命周期、事件、节点、路径和黑板 |
| `graphflow_engine` | `graphflow::engine` | 节点工厂、JSON 构建器和 `GraphEngine` |

示例目标：

- `graphflow_minimal`：JSON 驱动的最小事件链。
- `graphflow_multi`：扇入、扇出、不同事件类型和循环拓扑。
- `graphflow_agent`：离线模拟的感知、决策、动作闭环。
- `graphflow_av_pipeline`：离线模拟的音视频双流管线与汇聚。

## 测试

先确保 GoogleTest 的 CMake config package 可被当前工具链发现，再执行：

```powershell
cmake -S . -B build-tests `
  -DGRAPHFLOW_BUILD_EXAMPLES=OFF `
  -DGRAPHFLOW_BUILD_TESTS=ON
cmake --build build-tests --config Release
ctest --test-dir build-tests -C Release --output-on-failure
```

测试目标为 `graphflow_core_tests` 和 `graphflow_engine_tests`。

## 目录结构

```text
core/          生命周期、事件总线、图节点、路径、黑板
engine/        节点工厂、图构建器、JSON 构建器、GraphEngine
examples/      minimal、multi、agent、av_pipeline 示例
tests/         Core 与 Engine 的 GoogleTest 测试
third_party/   仓库内第三方依赖
utoolkit/      Git submodule，提供基础工具
utils/         GraphFlow 内部工具适配
```

## 使用边界

- 图上的原生事件是编译期 C++ 类型；新的跨模块事件需要定义公共 `EventT<T>` 类型。
- 生命周期 API 假定由主控制线程串行调用，不应并发执行 `load`、`reload`、`start`、`stop` 或 `release`。
- 本仓库是图运行时底座，不包含大模型调用、交易网关、行为树节点库或语言 SDK。
- `examples/agent` 和 `examples/av_pipeline` 是离线机制演示，不代表完整 Agent Engine 或真实媒体处理实现。

## 相关仓库

- [graphflow-python](https://github.com/soundex-dele/graphflow-python)：仅包含 GraphEngine 的 Python 绑定。
- [agent-engine](https://github.com/soundex-dele/agent-engine)：Agent Engine 及其 C++、Python、Flutter SDK。
- [quant-engine](https://github.com/soundex-dele/quant-engine)：量化工作流引擎。
- [behaviortree](https://github.com/soundex-dele/behaviortree)：GraphFlow 行为树扩展。
