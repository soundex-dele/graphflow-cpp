// Synthetic AV transport/scheduling benchmark; no devices, codecs or renderer.
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <exception>
#include <fstream>
#include <iostream>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>
#ifdef _WIN32
#include <windows.h>
#include <psapi.h>
#else
#include <sys/resource.h>
#endif
#include "engine/graph_engine.h"

namespace {
using Clock = std::chrono::steady_clock;
using Time = Clock::time_point;
using Json = nlohmann::json;
using graphflow::core::GraphNode;
using graphflow::core::EventBus;
using graphflow::engine::GraphEngine;
using graphflow::engine::NodeConnection;

double seconds(Time a, Time b) { return std::chrono::duration<double>(b - a).count(); }
Time after(Time t, double s) {
    return t + std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(s));
}

struct Config {
    int pairs = 1, workers = 4, producers = 2, stages = 4, fanout = 1;
    int width = 1920, height = 1080, sampleRate = 48000, channels = 2;
    int maxInflight = 64, workIterations = 0;
    double audioMs = 10, videoFps = 60, rateScale = 1;
    double duration = 10, warmup = 2, drainTimeout = 10;
    double audioDeadlineMs = 10, videoDeadlineMs = 16.667;
    double maxLossRatio = 0.001, maxLateRatio = 0.01;
    std::string dispatch = "async", payload = "shared", mode = "realtime";

    Json json() const {
        return {{"pairs", pairs}, {"workers", workers}, {"producers", producers},
            {"stages", stages}, {"fanout", fanout}, {"width", width}, {"height", height},
            {"sample_rate", sampleRate}, {"channels", channels}, {"audio_ms", audioMs},
            {"video_fps", videoFps}, {"rate_scale", rateScale}, {"duration_s", duration},
            {"warmup_s", warmup}, {"drain_timeout_s", drainTimeout},
            {"max_inflight_per_stream", maxInflight}, {"work_iterations", workIterations},
            {"audio_deadline_ms", audioDeadlineMs}, {"video_deadline_ms", videoDeadlineMs},
            {"max_loss_ratio", maxLossRatio}, {"max_late_ratio", maxLateRatio},
            {"dispatch", dispatch}, {"payload", payload}, {"mode", mode}};
    }
    static Config read(const Json& j) {
        Config c;
        const Json defaults = c.json();
        if (!j.is_object()) throw std::invalid_argument("config must be a JSON object");
        for (auto it = j.begin(); it != j.end(); ++it)
            if (!defaults.contains(it.key())) throw std::invalid_argument("unknown option: " + it.key());
        auto integer = [&](const char* k, int& v, int lo, int hi) {
            if (j.contains(k)) {
                if (!j.at(k).is_number_integer()) throw std::invalid_argument(std::string(k) + " must be an integer");
                const auto n = j.at(k).get<int64_t>();
                if (n < lo || n > hi) throw std::invalid_argument(std::string(k) + " out of range");
                v = static_cast<int>(n);
            }
        };
        auto number = [&](const char* k, double& v, double lo, double hi) {
            v = j.value(k, v);
            if (!std::isfinite(v) || v < lo || v > hi)
                throw std::invalid_argument(std::string(k) + " out of range");
        };
        integer("pairs", c.pairs, 1, 256); integer("workers", c.workers, 1, 256);
        integer("producers", c.producers, 1, 256); integer("stages", c.stages, 0, 64);
        integer("fanout", c.fanout, 1, 64); integer("width", c.width, 2, 7680);
        integer("height", c.height, 2, 4320); integer("sample_rate", c.sampleRate, 8000, 192000);
        integer("channels", c.channels, 1, 8);
        integer("max_inflight_per_stream", c.maxInflight, 1, 65536);
        integer("work_iterations", c.workIterations, 0, 10000000);
        number("audio_ms", c.audioMs, 1, 100); number("video_fps", c.videoFps, 1, 1000);
        number("rate_scale", c.rateScale, 0.01, 10000);
        number("duration_s", c.duration, 0.1, 3600); number("warmup_s", c.warmup, 0, 600);
        number("drain_timeout_s", c.drainTimeout, 0.1, 600);
        number("audio_deadline_ms", c.audioDeadlineMs, 0.001, 60000);
        number("video_deadline_ms", c.videoDeadlineMs, 0.001, 60000);
        number("max_loss_ratio", c.maxLossRatio, 0, 1);
        number("max_late_ratio", c.maxLateRatio, 0, 1);
        c.dispatch = j.value("dispatch", c.dispatch); c.payload = j.value("payload", c.payload);
        c.mode = j.value("mode", c.mode);
        if ((c.dispatch != "sync" && c.dispatch != "async") ||
            (c.payload != "metadata" && c.payload != "shared" && c.payload != "copy") ||
            (c.mode != "realtime" && c.mode != "saturation"))
            throw std::invalid_argument("invalid dispatch, payload or mode");
        if (c.width % 2 || c.height % 2) throw std::invalid_argument("I420 dimensions must be even");
        return c;
    }
};

// Fixed memory, 16 subdivisions per power of two; quantiles are bucket upper
// bounds (<= 6.25% bucket width above 16 us), not retained sample vectors.
struct Histogram {
    std::array<uint64_t, 1024> buckets{};
    uint64_t count = 0;
    double sumUs = 0, maxUs = 0;
    void add(double us) {
        int exp = 0;
        const double fraction = std::frexp(std::max(1.0, us), &exp);
        const int sub = std::min(15, static_cast<int>((fraction * 2 - 1) * 16));
        const int index = std::clamp((exp - 1) * 16 + sub, 0, 1023);
        ++buckets[static_cast<size_t>(index)]; ++count;
        sumUs += us; maxUs = std::max(maxUs, us);
    }
    double percentile(double q) const {
        if (!count) return 0;
        const auto target = static_cast<uint64_t>(std::ceil(q * static_cast<double>(count)));
        uint64_t total = 0;
        for (size_t i = 0; i < buckets.size(); ++i) {
            total += buckets[i];
            if (total >= target)
                return std::min(maxUs, std::ldexp(1.0 + (i % 16 + 1) / 16.0, static_cast<int>(i / 16)));
        }
        return maxUs;
    }
    Json json() const {
        return {{"samples", count}, {"mean_us", count ? sumUs / count : 0},
            {"p50_us", percentile(.5)}, {"p95_us", percentile(.95)},
            {"p99_us", percentile(.99)}, {"p999_us", percentile(.999)}, {"max_us", maxUs}};
    }
};

struct Stream {
    bool audio = false;
    double rate = 0, deadlineUs = 0;
    size_t bytes = 0;
    std::shared_ptr<const std::vector<uint8_t>> buffer;
    std::atomic<uint64_t> inflight{0};
    // Source counters have one writer; read only after all producers join.
    uint64_t accepted = 0, skipped = 0, rejected = 0, retries = 0, peakInflight = 0;
    uint64_t next = 0;
    Histogram submit;
    std::mutex mutex;
    uint64_t received = 0, inWindow = 0, late = 0, corrupt = 0, checksum = 0;
    Histogram graphLatency, scheduledLatency;
    Time end;
    void reset() {
        accepted = skipped = rejected = retries = peakInflight = next = 0;
        received = inWindow = late = corrupt = checksum = 0;
        submit = {}; graphLatency = {}; scheduledLatency = {};
    }
};
struct Ticket {
    explicit Ticket(int n) : remaining(n) {}
    std::atomic<int> remaining;
};
struct MediaFrame : graphflow::core::EventT<MediaFrame> {
    Time scheduled, emitted;
    uint64_t sequence = 0, checksum = 0;
    std::shared_ptr<Ticket> ticket;
    std::shared_ptr<const std::vector<uint8_t>> shared;
    std::vector<uint8_t> owned;  // copy mode: actual deep copy at every event copy
};

class MediaNode final : public GraphNode {
public:
    MediaNode(const std::string& name, const Config& c, Stream& s, bool sink = false)
        : GraphNode(name, nullptr), config_(c), stream_(s), sink_(sink) {}
    void send(const MediaFrame& f) {
        if (config_.dispatch == "async") publishAsync(f);
        else publish(f);
    }
protected:
    bool doInit() override {
        subscribe<MediaFrame>([this](MediaFrame& f) {
            if (!sink_) {
                uint64_t value = f.checksum ^ (f.sequence + 1);
                for (int i = 0; i < config_.workIterations; ++i)
                    value = value * 6364136223846793005ULL + 1442695040888963407ULL;
                f.checksum = value;
                send(f);
                return;
            }
            const auto now = Clock::now();
            const auto& data = f.shared ? *f.shared : f.owned;
            const bool valid = data.size() == stream_.bytes &&
                (data.empty() || (data.front() == 0x5a && data.back() == 0x5a));
            {
                std::lock_guard<std::mutex> lock(stream_.mutex);
                ++stream_.received;
                stream_.inWindow += now < stream_.end;
                stream_.corrupt += !valid;
                const double scheduledUs = seconds(f.scheduled, now) * 1e6;
                stream_.late += scheduledUs > stream_.deadlineUs;
                stream_.graphLatency.add(seconds(f.emitted, now) * 1e6);
                stream_.scheduledLatency.add(scheduledUs);
                stream_.checksum ^= f.checksum;
            }
            if (f.ticket->remaining.fetch_sub(1) == 1) stream_.inflight.fetch_sub(1);
        });
        return true;
    }
private:
    const Config& config_;
    Stream& stream_;
    bool sink_;
};

double cpuSeconds() {
#ifdef _WIN32
    FILETIME created{}, exited{}, kernel{}, user{};
    if (!GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user)) return -1;
    ULARGE_INTEGER k{}, u{};
    k.LowPart = kernel.dwLowDateTime; k.HighPart = kernel.dwHighDateTime;
    u.LowPart = user.dwLowDateTime; u.HighPart = user.dwHighDateTime;
    return static_cast<double>(k.QuadPart + u.QuadPart) * 1e-7;
#else
    rusage r{};
    if (getrusage(RUSAGE_SELF, &r) != 0) return -1;
    return r.ru_utime.tv_sec + r.ru_utime.tv_usec * 1e-6 + r.ru_stime.tv_sec + r.ru_stime.tv_usec * 1e-6;
#endif
}
uint64_t peakRssBytes() {
#ifdef _WIN32
    PROCESS_MEMORY_COUNTERS r{};
    if (!GetProcessMemoryInfo(GetCurrentProcess(), &r, sizeof(r))) return 0;
    return static_cast<uint64_t>(r.PeakWorkingSetSize);
#else
    rusage r{};
    if (getrusage(RUSAGE_SELF, &r) != 0) return 0;
#ifdef __APPLE__
    return static_cast<uint64_t>(r.ru_maxrss);
#else
    return static_cast<uint64_t>(r.ru_maxrss) * 1024;
#endif
#endif
}

class Benchmark {
public:
    explicit Benchmark(Config config) : config_(std::move(config)), engine_(config_.workers) {
        const auto begin = Clock::now();
        std::vector<NodeConnection> connections;
        for (int id = 0; id < config_.pairs * 2; ++id) {
            auto stream = std::make_unique<Stream>();
            stream->audio = id % 2 == 0;
            stream->rate = (stream->audio ? 1000.0 / config_.audioMs : config_.videoFps) * config_.rateScale;
            stream->deadlineUs = (stream->audio ? config_.audioDeadlineMs : config_.videoDeadlineMs) * 1000;
            stream->bytes = config_.payload == "metadata" ? 0 : stream->audio
                ? static_cast<size_t>(std::llround(config_.sampleRate * config_.audioMs / 1000)) * config_.channels * 2
                : static_cast<size_t>(config_.width) * config_.height * 3 / 2;
            stream->buffer = std::make_shared<const std::vector<uint8_t>>(stream->bytes, 0x5a);
            const std::string prefix = "stream" + std::to_string(id);
            auto source = std::make_shared<MediaNode>(prefix + "_source", config_, *stream);
            sources_.push_back(source); nodes_.push_back(source);
            std::string previous = source->getName();
            for (int stage = 0; stage < config_.stages; ++stage) {
                auto node = std::make_shared<MediaNode>(prefix + "_stage" + std::to_string(stage), config_, *stream);
                connections.push_back({previous, node->getName(), "MediaFrame", ""});
                previous = node->getName(); nodes_.push_back(node);
            }
            for (int branch = 0; branch < config_.fanout; ++branch) {
                auto sink = std::make_shared<MediaNode>(prefix + "_sink" + std::to_string(branch), config_, *stream, true);
                connections.push_back({previous, sink->getName(), "MediaFrame", ""});
                nodes_.push_back(sink);
            }
            streams_.push_back(std::move(stream));
        }
        if (!engine_.loadFromNodes(nodes_, connections) || !engine_.init() || !engine_.start())
            throw std::runtime_error("GraphEngine load/init/start failed");
        buildMs_ = seconds(begin, Clock::now()) * 1000;
    }

    Json run() {
        if (config_.warmup > 0) {
            const auto warm = phase(config_.warmup);
            if (!warm.drained) throw std::runtime_error("warmup did not drain; reduce load or increase drain_timeout_s");
        }
        for (auto& s : streams_) s->reset();
        const auto measured = phase(config_.duration);
        // Producers have joined. Closing all edge buses before draining prevents
        // new downstream work if the measurement timed out. Counts stay truthful.
        engine_.stop();
        engine_.release();
        Json rows = Json::array();
        bool pass = measured.drained;
        uint64_t accepted = 0, received = 0, inWindow = 0;
        for (size_t id = 0; id < streams_.size(); ++id) {
            const auto& s = *streams_[id];
            const uint64_t expected = s.accepted * config_.fanout;
            const uint64_t missing = expected > s.received ? expected - s.received : 0;
            const uint64_t offered = s.accepted + s.skipped + s.rejected;
            const double loss = offered ? (static_cast<double>(s.skipped + s.rejected) +
                static_cast<double>(missing) / config_.fanout) / offered : 1;
            const double late = s.received ? static_cast<double>(s.late) / s.received : 1;
            const bool integrity = s.accepted > 0 && s.received == expected && s.corrupt == 0;
            const bool slo = integrity && loss <= config_.maxLossRatio && late <= config_.maxLateRatio;
            pass = pass && (config_.mode == "realtime" ? slo : integrity);
            rows.push_back({{"stream", id}, {"media", s.audio ? "audio" : "video"},
                {"payload_bytes", s.bytes}, {"offered_frames", offered}, {"accepted_frames", s.accepted},
                {"source_skipped_frames", s.skipped}, {"admission_rejected_frames", s.rejected},
                {"saturation_admission_retries", s.retries}, {"peak_inflight_frames", s.peakInflight},
                {"expected_sink_deliveries", expected}, {"received_sink_deliveries", s.received},
                {"missing_sink_deliveries", missing}, {"corrupt_sink_deliveries", s.corrupt},
                {"late_sink_deliveries", s.late}, {"loss_ratio", loss}, {"late_ratio", late},
                {"sink_deliveries_in_window", s.inWindow},
                {"sink_deliveries_per_s", s.inWindow / config_.duration},
                {"graph_latency", s.graphLatency.json()}, {"scheduled_latency", s.scheduledLatency.json()},
                {"submit_latency", s.submit.json()}, {"checksum", s.checksum}, {"slo_pass", slo}});
            accepted += s.accepted; received += s.received; inWindow += s.inWindow;
        }
        return {{"schema_version", 1}, {"config", config_.json()},
            {"build_mode",
#ifdef NDEBUG
            "release"
#else
            "debug"
#endif
            }, {"hardware_concurrency", std::thread::hardware_concurrency()},
            {"graph_build_init_start_ms", buildMs_}, {"nodes", nodes_.size()},
            {"actual_producers", std::min(config_.producers, config_.pairs * 2)},
            {"source_join_elapsed_s", measured.sourceElapsed}, {"drain_wait_s", measured.drainSeconds},
            {"drained", measured.drained}, {"cpu_seconds_including_drain", measured.cpu},
            {"cpu_percent_one_core_including_drain", measured.cpu < 0 ? -1 :
                100 * measured.cpu / (measured.sourceElapsed + measured.drainSeconds)},
            {"process_peak_rss_bytes", peakRssBytes()}, {"accepted_frames", accepted},
            {"received_sink_deliveries", received}, {"sink_deliveries_in_window", inWindow},
            {"sink_deliveries_per_s", inWindow / config_.duration},
            {"pass", pass}, {"streams", rows}};
    }
private:
    struct Phase { bool drained; double sourceElapsed, drainSeconds, cpu; };
    Phase phase(double duration) {
        const int count = std::min(config_.producers, config_.pairs * 2);
        std::atomic<int> ready{0};
        std::atomic<bool> go{false}, cancel{false};
        std::exception_ptr error;
        std::mutex errorMutex;
        Time start, end;
        std::vector<std::thread> threads;
        try {
            for (int worker = 0; worker < count; ++worker) {
                threads.emplace_back([&, worker] {
                    ready.fetch_add(1);
                    while (!go.load()) std::this_thread::yield();
                    try {
                        while (!cancel.load() && Clock::now() < end) {
                            Time wake = end;
                            bool didWork = false;
                            for (size_t id = worker; id < streams_.size(); id += count) {
                                auto& s = *streams_[id];
                                auto now = Clock::now();
                                if (now >= end) break;
                                Time due = now;
                                if (config_.mode == "realtime") {
                                    due = after(start, static_cast<double>(s.next) / s.rate);
                                    if (due > now) { wake = std::min(wake, due); continue; }
                                    const auto slot = static_cast<uint64_t>(seconds(start, now) * s.rate);
                                    if (slot > s.next) { s.skipped += slot - s.next; s.next = slot; }
                                    due = after(start, static_cast<double>(s.next) / s.rate);
                                    ++s.next;
                                }
                                if (s.inflight.load() >= static_cast<uint64_t>(config_.maxInflight)) {
                                    if (config_.mode == "realtime") ++s.rejected;
                                    else ++s.retries;
                                    wake = std::min(wake, after(now, .0001));
                                    continue;
                                }
                                MediaFrame frame;
                                frame.scheduled = due; frame.emitted = Clock::now();
                                frame.sequence = s.accepted;
                                frame.ticket = std::make_shared<Ticket>(config_.fanout);
                                if (config_.payload == "copy") frame.owned = *s.buffer;
                                else frame.shared = s.buffer;
                                const auto outstanding = s.inflight.fetch_add(1) + 1;
                                s.peakInflight = std::max(s.peakInflight, outstanding);
                                ++s.accepted;
                                sources_[id]->send(frame);
                                s.submit.add(seconds(frame.emitted, Clock::now()) * 1e6);
                                didWork = true;
                            }
                            if (!didWork) {
                                if (config_.mode == "realtime") std::this_thread::sleep_until(wake);
                                else std::this_thread::yield();
                            }
                        }
                        if (config_.mode == "realtime") {
                            for (size_t id = worker; id < streams_.size(); id += count) {
                                auto& s = *streams_[id];
                                const auto total = static_cast<uint64_t>(std::ceil(duration * s.rate));
                                if (total > s.next) s.skipped += total - s.next;
                            }
                        }
                    } catch (...) {
                        std::lock_guard<std::mutex> lock(errorMutex);
                        if (!error) error = std::current_exception();
                        cancel.store(true);
                    }
                });
            }
        } catch (...) {
            cancel.store(true); go.store(true);
            for (auto& t : threads) t.join();
            throw;
        }
        while (ready.load() != count) std::this_thread::yield();
        start = Clock::now(); end = after(start, duration);
        for (auto& s : streams_) s->end = end;
        const double cpuStart = cpuSeconds();
        go.store(true);
        for (auto& t : threads) t.join();
        const auto joined = Clock::now();
        const auto timeout = after(joined, config_.drainTimeout);
        auto complete = [&] {
            return std::all_of(streams_.begin(), streams_.end(), [](const auto& s) { return s->inflight.load() == 0; });
        };
        while (!complete() && Clock::now() < timeout)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        const bool drained = complete();
        if (drained) {
            // Last sink can decrement inflight before returning from its callback.
            // Wait for callback epilogues before resetting warmup statistics.
            for (const auto& path : engine_.paths())
                for (const auto& bus : path->eventBusesSnapshot()) bus->drain();
        }
        if (error) std::rethrow_exception(error);
        const double cpuEnd = cpuSeconds();
        return {drained, seconds(start, joined), seconds(joined, Clock::now()),
            cpuStart < 0 || cpuEnd < 0 ? -1 : cpuEnd - cpuStart};
    }
    // Engine destroyed first, while callback dependencies are still alive.
    Config config_;
    std::vector<std::unique_ptr<Stream>> streams_;
    std::vector<std::shared_ptr<GraphNode>> nodes_;
    std::vector<std::shared_ptr<MediaNode>> sources_;
    double buildMs_ = 0;
    GraphEngine engine_;
};
}  // namespace

int main(int argc, char** argv) {
    try {
        std::string configPath, outputPath;
        for (int i = 1; i < argc; ++i) {
            const std::string arg = argv[i];
            if (arg == "--help") {
                std::cout << "graphflow_av_bench [--config case.json] [--output result.json]\n"
                             "Exit: 0 pass; 2 SLO/integrity/drain failure; 1 configuration/runtime error.\n";
                return 0;
            }
            if ((arg != "--config" && arg != "--output") || i + 1 >= argc)
                throw std::invalid_argument("expected --config FILE or --output FILE");
            (arg == "--config" ? configPath : outputPath) = argv[++i];
        }
        Json input = Json::object();
        if (!configPath.empty()) {
            std::ifstream file(configPath);
            if (!file) throw std::runtime_error("cannot open config: " + configPath);
            file >> input;
        }
        const Config config = Config::read(input);
        utoolkit::logging::Logger::GetInstance().Initialize("av_bench", "%v", utoolkit::logging::LogLevel::Error);
        const Json result = Benchmark(config).run();
        if (!outputPath.empty()) {
            std::ofstream out(outputPath);
            if (!out) throw std::runtime_error("cannot open output: " + outputPath);
            out << result.dump(2) << '\n';
            out.flush();
            if (!out) throw std::runtime_error("cannot write output: " + outputPath);
        }
        std::cout << result.dump(2) << '\n';
        return result.at("pass").get<bool>() ? 0 : 2;
    } catch (const std::exception& e) {
        std::cerr << "av_bench: " << e.what() << '\n';
        return 1;
    }
}
