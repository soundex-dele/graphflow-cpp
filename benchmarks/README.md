# GraphEngine 音视频性能压测

独立 C++17 程序，通过真实 `GraphEngine::loadFromNodes()`、`GraphNode::publish/publishAsync()` 和边 EventBus 测量合成音视频负载。默认不构建，不加入 CTest，无摄像头、声卡、网络、桌面或额外媒体库依赖。

本次仅编写测试程序，未编译、未运行，没有性能结论或设备验收结果。

## 测量范围

每个 pair 包含音频、视频两条独立链，同一引擎共享异步线程池：

```text
audio source -> stage × N -> sink × fanout
video source -> stage × N -> sink × fanout
```

音频默认 48 kHz、双声道、16-bit PCM，每 10 ms 一帧（1920 bytes）；视频默认 1080p I420、60 fps（3110400 bytes）。`stages=0` 可测纯派发，`fanout` 模拟预览/录制/多接收者分支。`work_iterations` 在每个中间节点加入结果可观测的整数运算，模拟业务 CPU 占用，不代表任何编码器。

载荷模式：

| payload | 行为 | 主要用途 |
| --- | --- | --- |
| metadata | 空载荷，仅时序、序号、完成票据 | 调度/事件派发开销 |
| shared | 预分配并复用不可变合成缓冲区，每次事件复制共享指针 | 媒体引用传递、队列及 fanout 开销 |
| copy | 源端和事件复制都深拷贝实际大小的缓冲区 | 内存分配和拷贝压力 |

shared 模式复用同一流的固定缓冲区，不能据此声称已经达到对应的真实采集内存带宽。没有真实 PCM DSP、编解码、RTP、设备渲染、GPU、A/V 对时或 `EdgeTransport::External` 媒体通道测试；结果用于评估 GraphEngine EventBus 数据路径。

## 构建（后续需要时手动执行）

从 `graphflow-cpp` 目录执行，Windows 示例：

```powershell
cmake -S . -B build-av-bench -DGRAPHFLOW_BUILD_BENCHMARKS=ON -DGRAPHFLOW_BUILD_TESTS=OFF -DGRAPHFLOW_BUILD_EXAMPLES=OFF -DCMAKE_BUILD_TYPE=Release
cmake --build build-av-bench --config Release --target graphflow_av_bench
```

不依赖 GTest 或 Google Benchmark。多配置生成器程序位于 `build-av-bench/benchmarks/Release/graphflow_av_bench.exe`；Linux/macOS 单配置生成器通常位于 `build-av-bench/benchmarks/graphflow_av_bench`。

## 单场景与逐级加压

以下命令会运行压测，留待需要测量时执行：

```powershell
.\build-av-bench\benchmarks\Release\graphflow_av_bench.exe --config benchmarks/av_realtime.json --output av-result.json
```

使用 Python 3.9+ 扫描器（仅标准库），默认只生成配置和计划：

```powershell
python benchmarks/run_av_sweep.py --exe build-av-bench/benchmarks/Release/graphflow_av_bench.exe --output av-plan --scenario realtime
```

显式 `--run` 才启动进程，每个 case 使用独立进程、硬超时和独立日志：

```powershell
python benchmarks/run_av_sweep.py --exe build-av-bench/benchmarks/Release/graphflow_av_bench.exe --output av-results --scenario realtime --levels 1,2,4,8,16,32,64 --workers 1,4,8 --repeat 3 --timeout 120 --run
```

输出目录必须尚不存在，防止误读旧报告。`--scenario` 支持 `realtime`、`metadata`、`shared`、`copy`、`fanout`、`deep`、`work`、`4k`、`all`；`--dispatch sync` 可做同步对照。`--base-config FILE` 覆盖场景默认值，最后由扫描参数覆盖 pairs/workers/dispatch。仅调整持续时间、SLO 等时建议使用只含这些字段的 JSON，避免完整配置把 fanout、copy 等场景特征覆盖掉。

建议依次测：metadata/saturation 调度上限 → shared/saturation 引用传递 → realtime 的路数边界 → fanout/deep/work → copy 与更大分辨率。异步线程池按 topic 哈希固定到单线程 worker，增加 worker 数不保证单链变快；对比应保留节点命名、编译器和构建版本。固定 producer 数后，再独立扫描 producer 数，区分负载发生器瓶颈与引擎瓶颈。

## 负载与计数口径

- `realtime` 按独立绝对时钟产生音频和视频帧，`rate_scale` 同时乘两种帧率，媒体帧大小不变。一次落后多个周期只发布最新周期，遗漏周期记 `source_skipped_frames`；结束时也补记未尝试的计划帧，避免同步慢处理或慢发生器隐藏损失。
- `saturation` 在有容量时尽快发布，发生器对分配给自己的流轮转，不维持音视频帧率比例。满容量记 `saturation_admission_retries`，表示重试次数，不是假定存在的输入帧或丢帧数。
- `max_inflight_per_stream` 是压测发生器的在途帧上限；一个输入帧的全部 fanout 分支完成后释放一个名额。实时模式满容量记 `admission_rejected_frames`。这是测试层的限流，**不是引擎原生背压能力**。fanout 和 copy 仍会扩大内存占用，调参时同时考虑帧大小、路数和在途上限。
- warmup 使用同一图，排空所有已接受帧和回调后清空统计，正式测量不包含预热和构图。
- `graph_latency` 从构造输入载荷前到 sink 开始处理的耗时；`scheduled_latency` 从计划发帧时刻到 sink 的耗时，包含发生器迟到；saturation 模式该起点是本次发送尝试时刻。
- `submit_latency` 包含源端载荷构造及 publish 调用。同步模式包含整条链和全部分支；异步模式主要是源端构造和入队。
- 每个 sink 交付记录一个延迟样本，固定大小对数直方图输出 p50/p95/p99/p99.9（桶上界，16 us 以上桶宽不超过约 6.25%）；均值和最大值直接记录。无样本时返回 0，必须结合 samples 解读。
- `sink_deliveries_per_s` 只统计正式时间窗内的 sink 交付，分母为配置持续时间。排空期间完成的帧只增加 received/延迟统计，不计入窗口吞吐量。fanout 后交付率不是源帧率。
- 正常路径先停止生产、等待在途帧和总线回调全部完成，再 stop/release 图。超过 drain timeout 则标记失败，释放图时仍先关闭/排空总线，以保持回调引用有效。释放阶段可能丢弃后续级联入队，最终 missing 如实报告；超时后才完成也不会把 `drained=false` 改为成功。
- `drain_timeout_s` 限制完整帧等待时间，不是整个进程的硬超时。引擎 drain/release 没有超时接口，独立运行请在外层设置超时；扫描器的 `--timeout` 覆盖构图、预热、测量、排空和退出全部阶段。

程序还记录构图+初始化+启动时间、进程 CPU 时间、以单核 100% 为基准的 CPU 使用率和进程峰值 RSS。CPU 区间包含正式测量及其排空，不含预热、构图和最终 release；RSS 是进程生命周期峰值（包含预热），不是队列独占内存。payload 首尾标记及大小校验只能发现部分数据损坏，不是完整媒体正确性验证。

## 判定与报告

实时模式逐流判定（避免视频吞吐量掩盖音频故障）：

1. 每条流必须有输入，交付数等于 accepted × fanout，无载荷校验错误，且完整排空。
2. `(source_skipped + admission_rejected + missing/fanout) / offered <= max_loss_ratio`，默认 0.1%。
3. `late_sink_deliveries / received_sink_deliveries <= max_late_ratio`，默认 1%；音频默认截止时间 10 ms，视频 16.667 ms，依据 scheduled latency 判定。

饱和模式 pass 仅表示完整性和排空成功，截止时间数据仍输出，不作为通过条件。通过并不代表实际音视频业务能满足同一 SLO。

可执行文件退出码：0 通过；2 SLO/完整性/排空失败；1 配置、构图或运行异常。扫描器完整保留单次 JSON、stdout/stderr、CSV 汇总及 `boundaries.json`。实时扫描完成失败档的重复测量后停止该组更高路数；运行异常或硬超时立即停止该组。扫描器遇到程序异常/硬超时退出 1，预期负载失效退出 0，具体通过情况查看报告。

边界报告只给出所有重复均通过的最高连续采样档和第一个未通过档，不宣称精确最大值。若最大档仍通过，只得到下界；若最小档失败，需更低负载或调整场景；异常/超时必须先诊断，不能直接当作吞吐极限。在两档间缩小步长复测，并延长持续时间，才能进一步定位稳定容量。

记录运行机器、CPU/内存、电源策略、编译器、Release 参数和代码版本。现有 GraphFlow 日志宏即使 Error 级别仍会构造 debug 字符串，报告包含该生产实现开销；本程序未修改引擎日志或优化其热路径。时钟采样、每流统计互斥锁、完成票据和发生器也有开销，需通过 metadata、stages=0、sync/async、producer 数等对照实验解释结果。
