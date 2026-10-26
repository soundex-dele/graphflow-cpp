# graphflow-cpp 设计

版本：v0.1；日期：2026-10-26；读取基线：`4b0c881`。

GraphFlow 是领域无关的 C++17 图运行时基础库，面向工作流、Agent、行为树与媒体等上层系统。本文描述 Core/Engine 的现有边界及 AV Platform 接入要求，不把媒体专用能力纳入通用内核。平台关系见 [总体设计](../../docs/platform-design.md)。

## 1. 职责与非目标

提供节点与边、路径、生命周期、强类型事件、共享黑板、节点工厂、JSON 构图和 GraphEngine 门面。业务领域定义自己的节点、事件和资源，通过通用接口接入。

音视频格式、帧缓冲、GPU 纹理、媒体时钟、编码和播放状态不由 GraphFlow 定义；这些由 SDK 的 `media_graph` 和媒体节点实现。现有 `av_pipeline` 是模拟帧事件示例，不证明真实音视频吞吐、同步或零拷贝。

## 2. 模块与构建

| 模块 | 现有对象/文件 | 职责 |
| --- | --- | --- |
| `core/` | Lifecycle、GraphNode、GraphEdge、GraphPath | 状态转换、节点与拓扑关系 |
| `core/` | Event/EventT、EventBus、EventThreadPool | 类型事件、同步/异步派发 |
| `core/` | Blackboard、NodeRuntimeContext | 同引擎共享状态与运行上下文 |
| `engine/` | NodeConfig、NodeFactory、NodeCreator | 节点配置与实例级注册 |
| `engine/` | GraphBuilder、JsonGraphBuilder、BuildResult | 配置解析、图构建与错误结果 |
| `engine/` | GraphEngine | 生命周期和加载/重载门面 |
| `utils/`、`utoolkit/` | 日志、线程池与基础设施 | 通用实现依赖 |

`graphflow::core` 对应 `graphflow_core` 静态库；`graphflow::engine` 对应 `graphflow_engine` 并依赖 core。通过 `add_subdirectory()` 消费，当前没有独立安装导出包，不假设 `find_package(graphflow-cpp)` 已可用。`utoolkit` 为递归子模块，nlohmann/json 为仓库内依赖。

## 3. 构图与拓扑

JSON 以 nodes 描述稳定 ID、注册 class 与 params，以 paths 描述节点顺序、edge 事件类型和可选 condition。节点 class 必须由当前引擎的工厂注册；同一节点可参与多路径，从而表达扇入、扇出和循环。

外部构图通过 `GraphEngine::loadFromNodes(nodes, connections)` 传入已有节点和 `NodeConnection{from, to, dataType, condition}`，无需外部构建路径或注册工厂。节点名作为唯一非空 ID，节点须为 Created 且无既有连接边；引擎校验所有端点后通过 `GraphBuilder::buildGraph()` 装配为内部生命周期路径，保留孤立节点，按输入节点顺序执行生命周期，不进行拓扑排序。节点控制总线由引擎绑定，业务依赖和 Blackboard 由调用方注入；所有节点与边总线由内部路径共享持有。默认事件边，传入 `EdgeTransport::External` 时仅建立拓扑。

图构建和运行分开，配置失败提供可定位的错误。上层接入必须检查节点类型、必需参数和拓扑，不将“JSON 可解析”当作领域图有效。循环是通用表达能力，上层必须自行约束终止、重入和反馈，媒体图尤其不能允许没有有界缓冲的无限反馈。

GraphEdge 表达 topic、eventBus、dataType 与条件关系；事件类型校验不涵盖图像像素格式或采样率。命名空间化 topic 与节点 ID，避免不同图/分支意外互订。

## 4. 生命周期与重载

```text
Created --load--> Created --init--> Initialized --start--> Running
                                                          |
                                                         stop
                                                          v
                                                       Stopped --start--> Running
Initialized / Running / Stopped --release--> Released
```

以上为调用关系说明，load 构建图但不直接启动；实际状态约束以 Lifecycle/GraphEngine 为准。典型顺序为 load → init → start → stop → release，stop 后允许再次 start，release 是终态。运行中 release 会先停止。

节点按需实现 doInit/doStart/doStop/doRelease；初始化建立订阅与准备资源，启动产生事件。`GraphNode::halt()` 是与整图 stop 正交的幂等软中断，设置 halted 并调用 doHalt；不自动改变 Lifecycle 状态或取消任意外部任务，上层节点负责实际取消。

`reloadFromJson*()` 可在 Initialized/Running/Stopped 使用，候选图构建失败保留旧图。当前重载涉及旧图停止，不等于不中断媒体热切换；上层需要描述间断窗口、资源重建与失败恢复，不能把通用重载包装成无缝切换承诺。

`reloadFromNodes()` 使用相同的提交/回滚流程，支持与 JSON 入口互换；Created 态等价于 load。外部输入校验失败不停止旧图；初始化/启动失败释放候选节点并尝试恢复旧图。成功时旧图被释放，原 Running 状态恢复，其余状态就绪为 Initialized。替换图不能复用旧图节点，已释放的候选实例不能重试。所有成功交给引擎的节点由引擎管理生命周期，调用方不得再独立 init/start/stop/release 或修改连接。

## 5. 事件与线程模型

同步 publish 在调用线程执行处理器；异步 publishAsync 将事件副本投递线程池。相同 topic 映射同一单线程 worker；不同 topic 可能并发，池通常懒创建。不能据此推导整个图全局有序或所有节点线程安全。

`subscribe` 对同 topic 默认保留一个处理器，重复注册被忽略；需要多订阅者时明确使用 `subscribeAll`。回调对象与事件的引用/值生命周期需符合接口要求，尤其禁止捕获离开作用域的局部引用。

`close()` 拒绝后续异步入队，**不阻止同步 publish**；`drain()` 等待已入队任务收拢。关闭流程先停外部事件源，再 close/drain，最后释放节点及订阅。不能在执行自身待等待任务的回调中阻塞 drain；共享线程池的停止必须由共同所有者协调，不能误停其他图。

EventBus 对处理器异常进行捕获记录，并不自动替代领域错误传播。关键节点应显式产生结构化错误或返回失败，不能认为日志出现异常就一定会让场景进入 Failed。

GraphEngine 生命周期/重载操作由调用方在一个控制执行器串行调用，禁止与正在访问拓扑的操作无约束并发。共享 Blackboard 对复合读改写的业务原子性由上层明确协调，不把单个访问接口的同步能力当作事务。

## 6. AV Platform 接入契约

SDK 定义内部适配节点，把通用 GraphNode 生命周期映射到媒体资源准备/启动/停止；对应用仅暴露 SDK 自有 MediaGraph/MediaNode。GraphFlow 不反向依赖 SDK、WebRTC 或 FFmpeg。

| 通用基础 | SDK 需要补充 |
| --- | --- |
| 节点/路径/工厂 | 媒体端口、格式约束与能力查询 |
| EventBus | 控制事件；另建有界 MediaChannel 传帧句柄 |
| 生命周期 | cancel/drain、EOS/Flush、下游就绪顺序 |
| 重载 | generation、旧帧清理、重建间断策略 |
| 线程池 | 实时/播放/预览/离线执行策略，后端线程亲和性 |
| Blackboard | 资源服务引用，不存放逐帧大数组 |

音频回调不经通用 EventBus 的复制/日志/线程池路径传送实时数据；视频大块像素不直接作为异步事件反复复制。通用事件仅携带轻量状态、错误或就绪通知，媒体内容由具有清晰所有权的通道获取。

## 7. 扩展原则与验证

新增通用能力必须至少能用领域无关的最小示例解释；媒体格式、时钟与 GPU 同步留在 SDK。若需要更强取消、排空、错误或隔离语义，先定义通用契约与回归，再由媒体适配消费，避免把隐式行为当保证。

已有 GoogleTest 目标 `graphflow_core_tests`、`graphflow_engine_tests` 及 minimal/multi/agent/av_pipeline 示例，构建入口见 [README](../README.md)。外部节点入口的回归测试位于 `tests/engine/external_graph_test.cpp`，覆盖事件传递、拓扑、非法输入、生命周期、两类构图入口切换和重载失败回滚；这些自动化验证不代表实际媒体设备人工验收。

后续变更重点验证：非法 JSON/节点类/参数、共享节点重复生命周期、条件路由与扇出、同 topic 顺序、跨 topic 并发、重复订阅、处理器异常、close 与入队竞争、drain 后回调结束、释放幂等、重载失败保留旧图。真实媒体背压和时钟测试属于 SDK 仓库，避免用模拟帧通过代替媒体集成验收。
