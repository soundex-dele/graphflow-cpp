// examples/av_pipeline/main.cpp
// 音视频管线端到端示例:双流各自经 采集->编码->打包->网络->解码 五阶段(采集后与解码后都是原始帧),
// 汇聚到同步渲染节点做 PTS 配对,体现 GraphFlow 在"长事件链 + 多流汇聚 + 同步"上的表达力。
//
// 纯模拟(零真实媒体依赖,与 minimal/multi/agent 一致):
//   - 帧都是结构化假帧(只带 PTS + 流参数),不产生真实 PCM/YUV/编码流。
//   - 编码/打包/网络/解码阶段做"类型转换"(RawFrame->Encoded->Packet->...->RawFrame),不做真实编解码。
//
// 数据类型区分(每阶段是类型转换器,边 dataType 即数据形态):
//   采集(原始) -> 编码(编码流) -> 打包(包) -> 网络(包) -> 解码(原始) -> 渲染
//   音频: RawAudioFrame -> EncodedAudio -> AudioPacket -> AudioPacket -> RawAudioFrame
//   视频: RawVideoFrame -> EncodedVideo -> VideoPacket -> VideoPacket -> RawVideoFrame
//   采集后和解码后都是原始数据(同类型),用同 dataType 是对的;中间阶段类型不同,边 dataType 区分。
//
// 拓扑(11 节点 2 path,见 examples/av_pipeline/av_pipeline.json):
//   音频路一条 path: acap -> aenc -> apack -> anet -> adec -> render
//   视频路一条 path: vcap -> venc -> vpack -> vnet -> vdec -> render
//   render 跨两条 path 末尾 -> 持 2 条 sub 边(原始音频+原始视频),汇聚成立。
//   每条流是线性链,写在一条 path 里(GraphBuilder 按相邻位置建边,单 path 天然支持线性链)。
//
// 机制要点:
//   - 类型转换分发:StageNode 按 params.stage 查 (输入 dataType -> 输出 dataType) 映射,
//     doInit 按输入边 dataType 订阅对应事件类型,回调里向 pub 边发"输出类型"事件(PTS 透传)。
//     这是 multi 的 dataType 分发的进阶版:不只是同类型接力,而是跨类型转换。
//   - 同步配对:A 路 path 声明先于 V 路。acap.doStart 同步发 N 个原始音频帧,沿 A 路类型转换接力
//     到 render 的 audioBuf 收齐;随后 vcap.doStart 发原始视频帧,每帧到 render 即与已缓存的同 PTS
//     音频配对渲染。顺序配对(两队列 front PTS 相等即出)。无环,无需防环。
//   - 通用 CaptureNode:一个 struct 搞定 acap/vcap,发原始帧。
//   - 通用 StageNode:一个 struct 搞定 8 个中间节点(A/V 各 4 阶段),靠 params.stream+stage 区分类型映射。
//
// 时序正确性:GraphBuilder 按路径相邻位置建边,生命周期 init 先于 start(订阅先于发布)。
//   start 按声明的 path 顺序遍历:A 路 path 先于 V 路。acap.doStart 同步 publish N 帧到 acap->aenc
//   边总线 -> aenc 回调转换 -> ... -> adec 回调向 adec->render 边发原始音频帧 -> render.audioBuf 收齐。
//   全程同步(EventBus::publish 在调用线程直接派发),故轮到 vcap.doStart 时 audioBuf 已满,
//   每个视频帧到达即能与同 PTS 音频配对。无异步、无竞态。
#include <atomic>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <vector>
#include "engine/graph_engine.h"
#include "core/event_bus.h"
#include "core/graph_node.h"

using graphflow::engine::GraphEngine;
using graphflow::core::EventT;
using graphflow::core::EventBus;
using graphflow::core::GraphNode;
using graphflow::engine::NodeConfig;
using graphflow::engine::NodeCreator;

// === 事件类型(纯模拟,每种数据形态一个类型,都带 PTS) ===
// 采集后/解码后的原始帧;编码后的编码流;打包后的包。音频、视频各一套。
struct RawAudioFrame : EventT<RawAudioFrame> {
    size_t pts = 0;
    int samples = 0;
    RawAudioFrame() = default;
    explicit RawAudioFrame(size_t p) : pts(p) {}
};
struct EncodedAudio : EventT<EncodedAudio> {
    size_t pts = 0;
    EncodedAudio() = default;
    explicit EncodedAudio(size_t p) : pts(p) {}
};
struct AudioPacket : EventT<AudioPacket> {
    size_t pts = 0;
    AudioPacket() = default;
    explicit AudioPacket(size_t p) : pts(p) {}
};
struct RawVideoFrame : EventT<RawVideoFrame> {
    size_t pts = 0;
    int width = 0, height = 0;
    RawVideoFrame() = default;
    explicit RawVideoFrame(size_t p) : pts(p) {}
};
struct EncodedVideo : EventT<EncodedVideo> {
    size_t pts = 0;
    EncodedVideo() = default;
    explicit EncodedVideo(size_t p) : pts(p) {}
};
struct VideoPacket : EventT<VideoPacket> {
    size_t pts = 0;
    VideoPacket() = default;
    explicit VideoPacket(size_t p) : pts(p) {}
};

// 执行轨迹打印:统一前缀 "  >> [stage][stream] ...",便于观察事件流转。
static void trace(const std::string& stage, const std::string& stream, const std::string& msg) {
    std::cout << "  >> [" << stage << "][" << stream << "] " << msg << "\n";
}

// === 阶段的"输入 dataType -> 输出 dataType"映射(类型转换器的核心) ===
//   返回 (输入 dataType, 输出 dataType);未知 stage 返回 ("","") 会让节点 doInit 跳过订阅(装配报错)。
//   encode: 原始帧 -> 编码流;pack: 编码流 -> 包;net: 包 -> 包(网络两端同形态,只透传);
//   decode: 包 -> 原始帧(还原成采集时的形态)。
struct StageTypeMapping {
    std::string inDt;
    std::string outDt;
};
static StageTypeMapping stageMapping(const std::string& stream, const std::string& stage) {
    const std::string raw = (stream == "audio") ? "RawAudioFrame" : "RawVideoFrame";
    const std::string enc = (stream == "audio") ? "EncodedAudio" : "EncodedVideo";
    const std::string pkt = (stream == "audio") ? "AudioPacket" : "VideoPacket";
    if (stage == "encode") return {raw, enc};
    if (stage == "pack")   return {enc, pkt};
    if (stage == "net")    return {pkt, pkt};
    if (stage == "decode") return {pkt, raw};
    return {"", ""};
}

// === 采集源节点(通用):按 PTS 0..N-1 顺序发布"原始帧"到自己唯一的 pub 边 ===
// params.stream=audio -> 发 RawAudioFrame;video -> 发 RawVideoFrame。
struct CaptureNode : GraphNode {
    std::string m_stream;
    int m_count = 0;
    int m_samples = 0;
    int m_width = 0, m_height = 0;
    CaptureNode(const std::string& n, EventBus* eb, std::string stream)
        : GraphNode(n, eb), m_stream(std::move(stream)) {}
    bool doInit() override { return true; }
    bool doStart() override {
        for (int i = 0; i < m_count; ++i) {
            const auto& pub = getPubEdges();
            if (pub.empty()) break;
            auto* bus = pub.front().eventBus();
            const std::string& topic = pub.front().name();
            if (m_stream == "audio") {
                RawAudioFrame e(static_cast<size_t>(i));
                e.samples = m_samples;
                trace("capture", m_stream, "emit raw audio pts=" + std::to_string(i) +
                      " samples=" + std::to_string(m_samples));
                bus->publish(topic, e);
            } else {
                RawVideoFrame e(static_cast<size_t>(i));
                e.width = m_width;
                e.height = m_height;
                trace("capture", m_stream, "emit raw video pts=" + std::to_string(i) +
                      " " + std::to_string(m_width) + "x" + std::to_string(m_height));
                bus->publish(topic, e);
            }
        }
        return true;
    }
};

// === 中继阶段节点(通用,类型转换器):按 sub 边输入 dataType 订阅,转换成输出 dataType 发布 ===
// params.stream + params.stage 决定 (输入->输出) 类型映射;PTS 透传,载荷按输出类型重建。
// 一个 struct 搞定 8 个中间节点(A/V 各 4 阶段:encode/pack/net/decode)。
struct StageNode : GraphNode {
    std::string m_stream;
    std::string m_stage;
    StageTypeMapping m_map;   // 输入->输出 dataType
    std::atomic<int> m_received{0};
    StageNode(const std::string& n, EventBus* eb, std::string stream, std::string stage)
        : GraphNode(n, eb), m_stream(std::move(stream)), m_stage(std::move(stage)),
          m_map(stageMapping(m_stream, m_stage)) {}
    bool doInit() override {
        for (const auto& info : getSubEdges()) {
            // 输入边 dataType 应等于 m_map.inDt(JsonGraphBuilder 不强校验字符串匹配,这里靠订阅类型对齐)。
            auto* bus = info.eventBus();
            const std::string& topic = info.name();
            subscribeBy(streamAudio(m_stream), bus, topic);
        }
        return true;
    }
    bool doStart() override { return true; }
    // 按流(audio/video)分派订阅:每个流有 3 种输入类型(raw/enc/pkt),各订对应回调。
    // 用 lambda 捕获 this,回调里调 convertAndPublish 做类型转换。
    void subscribeBy(bool isAudio, EventBus* bus, const std::string& topic) {
        if (isAudio) {
            if (m_map.inDt == "RawAudioFrame")
                bus->subscribe<RawAudioFrame>(topic, [this](RawAudioFrame& e){ onIn(e.pts); });
            else if (m_map.inDt == "EncodedAudio")
                bus->subscribe<EncodedAudio>(topic, [this](EncodedAudio& e){ onIn(e.pts); });
            else if (m_map.inDt == "AudioPacket")
                bus->subscribe<AudioPacket>(topic, [this](AudioPacket& e){ onIn(e.pts); });
        } else {
            if (m_map.inDt == "RawVideoFrame")
                bus->subscribe<RawVideoFrame>(topic, [this](RawVideoFrame& e){ onIn(e.pts); });
            else if (m_map.inDt == "EncodedVideo")
                bus->subscribe<EncodedVideo>(topic, [this](EncodedVideo& e){ onIn(e.pts); });
            else if (m_map.inDt == "VideoPacket")
                bus->subscribe<VideoPacket>(topic, [this](VideoPacket& e){ onIn(e.pts); });
        }
    }
    static bool streamAudio(const std::string& s) { return s == "audio"; }
    // 收帧(只取 pts;载荷形态不重要,因为输出是新类型) -> trace -> 按输出 dataType 发新类型事件。
    void onIn(size_t pts) {
        ++m_received;
        trace(m_stage, m_stream, "convert pts=" + std::to_string(pts) +
              " (" + m_map.inDt + " -> " + m_map.outDt + ")");
        publishBy(pts);
    }
    void publishBy(size_t pts) {
        const auto& pubs = getPubEdges();
        if (pubs.empty()) return;
        auto* bus = pubs.front().eventBus();
        const std::string& topic = pubs.front().name();
        // 不显式指定 publish 模板参数:让转发引用从局部事件 e 推导为左值引用(见 event_bus.h publish 注释)。
        // 流参数(samples/width/height)是采集源的展示性数据,解码还原时不重建——渲染只看 PTS 配对。
        if (m_map.outDt == "RawAudioFrame")      { RawAudioFrame e(pts); bus->publish(topic, e); }
        else if (m_map.outDt == "EncodedAudio") { EncodedAudio e(pts); bus->publish(topic, e); }
        else if (m_map.outDt == "AudioPacket")  { AudioPacket e(pts); bus->publish(topic, e); }
        else if (m_map.outDt == "RawVideoFrame"){ RawVideoFrame e(pts); bus->publish(topic, e); }
        else if (m_map.outDt == "EncodedVideo"){ EncodedVideo e(pts); bus->publish(topic, e); }
        else if (m_map.outDt == "VideoPacket") { VideoPacket e(pts); bus->publish(topic, e); }
    }
};

// === 同步渲染节点:汇聚 A/V 两路原始帧,顺序 PTS 配对后渲染 ===
// 跨两条 path 末尾 -> 持 2 条 sub 边(RawAudioFrame + RawVideoFrame),分别按 dataType 订阅。
// 维护两个 PTS 到达队列;A/V 同 PTS 都到齐(两队列 front PTS 相等)即 trace "render synced" 并记录。
struct SyncRenderNode : GraphNode {
    mutable std::mutex m_mtx;
    std::vector<size_t> m_audioBuf;
    std::vector<size_t> m_videoBuf;
    std::vector<size_t> m_renderedPts;
    std::atomic<int> m_audioRecv{0};
    std::atomic<int> m_videoRecv{0};
    explicit SyncRenderNode(const std::string& n, EventBus* eb) : GraphNode(n, eb) {}
    bool doInit() override {
        for (const auto& info : getSubEdges()) {
            auto* bus = info.eventBus();
            const std::string& topic = info.name();
            if (info.dataType() == "RawAudioFrame") {
                bus->subscribe<RawAudioFrame>(topic, [this](RawAudioFrame& e){ onAudio(e.pts); });
            } else if (info.dataType() == "RawVideoFrame") {
                bus->subscribe<RawVideoFrame>(topic, [this](RawVideoFrame& e){ onVideo(e.pts); });
            }
        }
        return true;
    }
    bool doStart() override { return true; }
    void onAudio(size_t pts) {
        ++m_audioRecv;
        std::lock_guard<std::mutex> lk(m_mtx);
        m_audioBuf.push_back(pts);
        trySync();
    }
    void onVideo(size_t pts) {
        ++m_videoRecv;
        std::lock_guard<std::mutex> lk(m_mtx);
        m_videoBuf.push_back(pts);
        trySync();
    }
    // 顺序配对:两队列 front PTS 相等即配对出队;循环以处理连续配对。
    void trySync() {
        while (!m_audioBuf.empty() && !m_videoBuf.empty()) {
            if (m_audioBuf.front() == m_videoBuf.front()) {
                size_t pts = m_audioBuf.front();
                m_audioBuf.erase(m_audioBuf.begin());
                m_videoBuf.erase(m_videoBuf.begin());
                m_renderedPts.push_back(pts);
                std::cout << "  >> [render][sync] synced pts=" << pts << " -> render\n";
            } else if (m_audioBuf.front() < m_videoBuf.front()) {
                break;   // 音频落后,等视频(本例两边 PTS 一致,不会进此分支)。
            } else {
                break;
            }
        }
    }
    int renderedCount() const {
        std::lock_guard<std::mutex> lk(m_mtx);
        return static_cast<int>(m_renderedPts.size());
    }
    std::vector<size_t> renderedPts() const {
        std::lock_guard<std::mutex> lk(m_mtx);
        return m_renderedPts;
    }
    int audioRecv() const { return m_audioRecv.load(); }
    int videoRecv() const { return m_videoRecv.load(); }
};

// === Creators ===
class CaptureCreator : public NodeCreator {
public:
    std::shared_ptr<GraphNode> createNode(const NodeConfig& cfg) override {
        auto n = std::make_shared<CaptureNode>(cfg.nodeName, cfg.eventBus,
                                               cfg.params.value("stream", std::string("")));
        n->m_count = cfg.params.value("count", 0);
        n->m_samples = cfg.params.value("samples", 0);
        n->m_width = cfg.params.value("width", 0);
        n->m_height = cfg.params.value("height", 0);
        return n;
    }
};
class StageCreator : public NodeCreator {
public:
    std::shared_ptr<GraphNode> createNode(const NodeConfig& cfg) override {
        return std::make_shared<StageNode>(cfg.nodeName, cfg.eventBus,
                                          cfg.params.value("stream", std::string("")),
                                          cfg.params.value("stage", std::string("")));
    }
};
class SyncRenderCreator : public NodeCreator {
public:
    std::shared_ptr<GraphNode> createNode(const NodeConfig& cfg) override {
        return std::make_shared<SyncRenderNode>(cfg.nodeName, cfg.eventBus);
    }
};

// === 工具:按名找节点(沿用 multi/bt 模式) ===
template <typename T>
static T* findNode(GraphEngine& eng, const std::string& name) {
    for (auto& path : eng.paths()) {
        for (auto& nodeBase : path->getNodes()) {
            if (nodeBase->getName() == name) {
                return dynamic_cast<T*>(nodeBase.get());
            }
        }
    }
    return nullptr;
}

// 拓扑来自 examples/av_pipeline/av_pipeline.json(文件即唯一真相源)。
//   ctest 与本程序都从仓库根运行(WORKING_DIRECTORY = CMAKE_SOURCE_DIR),
//   所以路径用相对仓库根的 examples/av_pipeline/av_pipeline.json。
static constexpr const char* kTopoFile = "examples/av_pipeline/av_pipeline.json";

int main(int argc, char** argv) {
    std::string configFile = kTopoFile;
    if (argc > 1) configFile = argv[1];

    std::cout << "====== av_pipeline: 双流 采集->编码->打包->网络->解码->同步渲染 ======\n";
    GraphEngine eng;
    eng.registerCreator("CaptureNode",    std::make_shared<CaptureCreator>());
    eng.registerCreator("StageNode",       std::make_shared<StageCreator>());
    eng.registerCreator("SyncRenderNode",  std::make_shared<SyncRenderCreator>());

    if (!eng.loadFromJsonFile(configFile)) {
        std::cerr << "FAIL: loadFromJsonFile " << configFile << "\n";
        return 1;
    }
    if (!eng.init()) {
        std::cerr << "FAIL: init\n";
        return 1;
    }
    if (!eng.start()) {
        std::cerr << "FAIL: start\n";
        return 1;
    }

    // 在 release() 前验证:release 会清空 eng.paths(),导致无法遍历节点。
    bool ok = false;
    auto* render = findNode<SyncRenderNode>(eng, "render");
    if (render) {
        // 自检(预期 count=3):
        //   - render 配对渲染 3 帧(PTS 0,1,2)
        //   - A/V 原始帧各收到 3 帧(汇聚 + 顺序配对正确)
        auto pts = render->renderedPts();
        bool ptsOk = pts.size() == 3 &&
                     pts[0] == 0 && pts[1] == 1 && pts[2] == 2;
        ok = render->renderedCount() == 3 &&
             render->audioRecv() == 3 &&
             render->videoRecv() == 3 &&
             ptsOk;
    }

    // 抽查中继阶段:encode 与 decode 应各收 3 帧(管线每段都通,类型转换无丢帧)。
    auto* aenc = findNode<StageNode>(eng, "aenc");
    auto* vdec = findNode<StageNode>(eng, "vdec");
    bool stagesOk = aenc && aenc->m_received.load() == 3 &&
                    vdec && vdec->m_received.load() == 3;
    ok = ok && stagesOk;

    eng.stop();
    eng.release();
    std::cout << "\n====== summary ======\n"
              << "av_pipeline (A/V sync merge + fan-out): "
              << (ok ? "PASS" : "FAIL") << "\n"
              << "  render synced " << (render ? render->renderedCount() : -1) << " frames"
              << " (audio recv=" << (render ? render->audioRecv() : -1)
              << ", video recv=" << (render ? render->videoRecv() : -1) << ")\n";
    return ok ? 0 : 1;
}
