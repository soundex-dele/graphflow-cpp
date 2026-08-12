# graphflow-cpp

GraphFlow 的 C++17 图运行时底座，包含生命周期、事件总线、图节点、图路径、JSON 图构建器和 `GraphEngine`。

此仓库不包含 Agent Engine、Quant Engine、BehaviorTree 或语言 SDK；这些组件通过 Git submodule 依赖本仓库。

```powershell
git clone --recurse-submodules https://github.com/soundex-dele/graphflow-cpp.git
cmake -S . -B build -DGRAPHFLOW_BUILD_EXAMPLES=ON
cmake --build build --config Debug
```
