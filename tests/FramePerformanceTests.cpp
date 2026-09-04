#include "Core/Application/FramePerformanceRecorder.h"
#include "Core/Application/FramePacingAutomation.h"
#include "RHI/PipelineCreationStatistics.h"
#include "RHI/FramePacingStatistics.h"
#include "RHI/FramePacing.h"
#include "Core/Application/PerformanceActiveViews.h"
#include "Core/Application/PerformanceWindowPosition.h"
#include "Core/Application/PerformanceWindowFocus.h"
#include "Core/Application/FrameRateLimiter.h"

#include <cmath>
#include <array>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <thread>
#include <vector>

namespace
{
using Recorder = Prism::Core::FramePerformanceRecorder;
using Json = nlohmann::json;
void Expect(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
template<class Callback> void Reject(Callback callback)
{
    bool rejected = false;
    try { callback(); } catch (const std::exception&) { rejected = true; }
    Expect(rejected, "Invalid performance sample was accepted.");
}
Json Report()
{
    return {{"format", "PrismGpuTimingReport"}, {"passes", {{{"name", "Renderer"}, {"gpuMilliseconds", 2.0}}}}};
}
void TestRecorder(const std::filesystem::path& root)
{
    const auto path = root / "samples.jsonl";
    Recorder recorder(path, {{"test", true}});
    Reject([&] { Recorder duplicate(path, {}); });
    Reject([&] { recorder.BeginFrame(2); });
    recorder.BeginFrame(1);
    Reject([&] { recorder.BeginFrame(1); });
    Reject([&] { recorder.CompleteFrame({}); });
    Reject([&] { recorder.AddCpuMilliseconds(Recorder::Metric::Extraction, -1); });
    Reject([&] { recorder.AddCpuMilliseconds(Recorder::Metric::Count, 1); });
    Reject([&] { recorder.AddCpuMilliseconds(Recorder::Metric::UiBuild, std::numeric_limits<double>::quiet_NaN()); });
    Reject([&] { recorder.RecordView("game", 0, 1, 0, {}, Report()); });
    Reject([&] { recorder.RecordView("other", 0, 0, 0, {}, {}); });
    Reject([&] { recorder.RecordView("game", 16, 0, 0, {}, {}); });
    recorder.CancelFrame();
    Reject([&] { recorder.CancelFrame(); });
    recorder.BeginFrame(1);
    recorder.AddCpuMilliseconds(Recorder::Metric::Extraction, 1.0);
    recorder.AddCpuMilliseconds(Recorder::Metric::Extraction, 2.0);
    recorder.RecordView("game", 0, 0, 0, {}, {});
    recorder.RecordView("scene", 0, 0, 0, {}, {});
    Reject([&] { recorder.RecordView("game", 0, 0, 0, {}, {}); });
    Reject([&] { recorder.CancelFrame(); });
    recorder.CompleteFrame({});
    Reject([&] { recorder.Finish(3); });
    for (std::uint64_t frame = 2; frame <= 9; ++frame)
    {
        recorder.BeginFrame(frame);
        Recorder::Scope disabled(nullptr, Recorder::Metric::UiBuild);
        Recorder::Scope scope(&recorder, Recorder::Metric::UiBuild);
        scope.End(); scope.End();
        const auto slot = static_cast<std::uint32_t>((frame - 1) % 2);
        recorder.RecordView("game", slot, frame > 2 ? frame - 2 : 0, slot, {}, Report());
        // Sparse Scene refresh proves generation 2 maps to frame 3, not frame 2.
        if (frame == 3 || frame == 7)
            recorder.RecordView("scene", 0, frame == 3 ? 1 : 2, 0, {}, Report());
        recorder.CompleteFrame({});
    }
    recorder.Finish(9);
    Reject([&] { recorder.BeginFrame(10); });
    Reject([&] { recorder.Finish(9); });
    std::ifstream input(path);
    std::vector<Json> gpu;
    std::size_t cpu = 0;
    for (std::string line; std::getline(input, line);)
    {
        const auto row = Json::parse(line);
        if (row.at("type") == "cpu")
        {
            ++cpu;
            if (cpu == 1) Expect(row.at("cpu").at("extractionMs") == 3, "Extraction did not accumulate.");
            Expect(row.at("memory").at("peakWorkingSetBytes") > 0, "Process memory was not sampled.");
        }
        if (row.at("type") == "gpu") gpu.push_back(row);
    }
    Expect(cpu == 9 && gpu.size() == 9, "Dropped or duplicated CPU/GPU samples.");
    Expect(gpu.at(1).at("view") == "scene" && gpu.at(1).at("frameId") == 1, "First Scene mapping incorrect.");
    bool foundSparse = false;
    for (const auto& row : gpu) if (row.at("view") == "scene" && row.at("generation") == 2)
        foundSparse = row.at("frameId") == 3 && row.at("observedFrameId") == 7;
    Expect(foundSparse, "Profiler generation was mistaken for a logical frame ID.");

    Recorder stale(root / "stale.jsonl", {});
    stale.BeginFrame(1); stale.RecordView("game", 0, 0, 0, {}, {}); stale.CompleteFrame({});
    stale.BeginFrame(2); stale.RecordView("game", 1, 1, 0, {}, Report()); stale.CompleteFrame({});
    stale.BeginFrame(3);
    Reject([&] { stale.RecordView("game", 2, 0, 0, {}, Report()); });
    Reject([&] { stale.RecordView("game", 2, 2, 0, {}, Report()); });
    stale.RecordView("game", 2, 1, 0, {}, Report()); // persistent old result: no duplicate GPU record
    stale.CompleteFrame({}); stale.Finish(3);
}
void TestCounters()
{
    Prism::RHI::PipelineCreationCounters counters;
    std::vector<std::thread> workers;
    for (int worker = 0; worker < 4; ++worker)
        workers.emplace_back([&] { for (int i = 0; i < 1000; ++i) { counters.RecordGraphics(); counters.RecordCompute(); } });
    for (auto& worker : workers) worker.join();
    const auto result = counters.Read();
    Expect(result.graphics == 4000 && result.compute == 4000, "Concurrent creation counters lost increments.");
}
void TestPacing()
{
    Prism::Core::FrameRateLimiter limiter;
    for (const std::uint32_t targetFps : std::array{60u, 120u, 144u})
    {
        limiter.Configure(targetFps);
        Expect(limiter.Wait() == 0.0,
            "The first limiter admission unexpectedly waited.");
        const auto limiterStart = std::chrono::steady_clock::now();
        const double limiterWait = limiter.Wait();
        const double elapsedMilliseconds =
            std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - limiterStart).count();
        const double targetMilliseconds = 1000.0 / targetFps;
        Expect(std::isfinite(limiterWait)
                && limiterWait >= targetMilliseconds * 0.70
                && elapsedMilliseconds >= targetMilliseconds * 0.70
                && elapsedMilliseconds < 100.0,
            "The 60/120/144 Hz limiter cadence was invalid.");
    }
    limiter.Configure(1000u);
    Expect(limiter.Wait() == 0.0,
        "The hitch-reset limiter did not start cleanly.");
    std::this_thread::sleep_for(std::chrono::milliseconds(6));
    Expect(limiter.Wait() == 0.0,
        "The target frame limiter tried to catch up after a hitch.");
    limiter.Configure(std::nullopt);
    Expect(limiter.Wait() == 0.0,
        "An uncapped limiter introduced a wait.");

    using Prism::RHI::FramePacingProfile;
    using Prism::RHI::PresentationIntent;
    const auto interactive =
        Prism::RHI::ParseFramePacingConfiguration("");
    Expect(interactive.profile == FramePacingProfile::InteractiveSmooth
            && interactive.presentation == PresentationIntent::Synchronized
            && !interactive.targetFps.has_value()
            && interactive.maxQueuedFrames == 2,
        "Default frame-pacing profile changed.");
    const auto lowLatency =
        Prism::RHI::ParseFramePacingConfiguration("low-latency");
    Expect(lowLatency.presentation
            == PresentationIntent::LowLatencySynchronized
            && lowLatency.maxQueuedFrames == 1,
        "Low-latency preset changed.");
    const auto benchmark =
        Prism::RHI::ParseFramePacingConfiguration("benchmark");
    Expect(benchmark.presentation == PresentationIntent::Immediate
            && !benchmark.targetFps.has_value()
            && benchmark.maxQueuedFrames == 2,
        "Benchmark preset changed.");
    const auto custom = Prism::RHI::ParseFramePacingConfiguration(
        "custom", "synchronized", "120", "3");
    Expect(custom.targetFps == 120u
            && custom.maxQueuedFrames == 3
            && Prism::RHI::ToString(custom.profile) == "custom"
            && Prism::RHI::ToString(custom.presentation)
                == "synchronized",
        "Custom frame-pacing configuration changed.");
    const auto customUncapped =
        Prism::RHI::ParseFramePacingConfiguration(
            "custom", "immediate", "uncapped", "1");
    Expect(!customUncapped.targetFps.has_value(),
        "Explicit uncapped target was not preserved.");
    Reject([] { (void)Prism::RHI::ParseFramePacingConfiguration("vsync"); });
    Reject([] { (void)Prism::RHI::ParseFramePacingConfiguration(
        "benchmark", "immediate", "", "2"); });
    Reject([] { (void)Prism::RHI::ParseFramePacingConfiguration(
        "custom", "immediate", "0", "1"); });
    Reject([] { (void)Prism::RHI::ParseFramePacingConfiguration(
        "custom", "immediate", "60junk", "1"); });
    Reject([] { (void)Prism::RHI::ParseFramePacingConfiguration(
        "custom", "immediate", "60", "9"); });
    Prism::RHI::FramePacingState state{};
    state.swapchainImageCount = 3;
    state.effectiveMaxQueuedFrames = 1;
    state.frameResourceSlotCount = 2;
    Expect(state.swapchainImageCount == 3
            && state.effectiveMaxQueuedFrames == 1
            && state.frameResourceSlotCount == 2,
        "Independent frame-pacing counts aliased one another.");

    using Prism::Core::ParsePerformanceGameOnly;
    Expect(!ParsePerformanceGameOnly("", false),
        "Ordinary active views changed.");
    Expect(ParsePerformanceGameOnly("game", true),
        "Game-only performance policy rejected.");
    Reject([] { ParsePerformanceGameOnly("game", false); });
    Reject([] { ParsePerformanceGameOnly("default", true); });
    Reject([] { ParsePerformanceGameOnly("scene", true); });
    using Prism::Core::ParsePerformanceWindowUnfocused;
    Expect(!ParsePerformanceWindowUnfocused("", false, false), "Ordinary focus default changed.");
    Expect(!ParsePerformanceWindowUnfocused("", false, true), "Headless focus default changed.");
    Expect(!ParsePerformanceWindowUnfocused("default", true, false), "Explicit focus default changed.");
    Expect(ParsePerformanceWindowUnfocused("unfocused", true, false), "Unfocused test policy rejected.");
    using Prism::Core::ParsePerformanceWindowFocusPolicy;
    using Prism::Core::PerformanceWindowFocusPolicy;
    Expect(ParsePerformanceWindowFocusPolicy("focused", true, false) == PerformanceWindowFocusPolicy::Focused,
        "Focused test policy rejected.");
    Expect(ParsePerformanceWindowFocusPolicy("unfocused", true, false) == PerformanceWindowFocusPolicy::Unfocused,
        "Unfocused policy identity changed.");
    Reject([] { ParsePerformanceWindowUnfocused("unfocused", false, false); });
    Reject([] { ParsePerformanceWindowFocusPolicy("focused", false, false); });
    Reject([] { ParsePerformanceWindowUnfocused("default", false, false); });
    Reject([] { ParsePerformanceWindowUnfocused("unfocused", true, true); });
    Reject([] { ParsePerformanceWindowFocusPolicy("focused", true, true); });
    Reject([] { ParsePerformanceWindowUnfocused("false", true, false); });
    Reject([] { ParsePerformanceWindowUnfocused(" unfocused", true, false); });
    using Prism::Core::ParsePerformanceWindowPosition;
    Expect(!ParsePerformanceWindowPosition("", ""), "Unconfigured window placement changed.");
    Expect(ParsePerformanceWindowPosition("-100", "100")->first == -100, "Negative desktop position rejected.");
    Expect(*ParsePerformanceWindowPosition("100", "100") == std::pair{100, 100}, "Requested window position changed.");
    Reject([] { ParsePerformanceWindowPosition("1", ""); });
    Reject([] { ParsePerformanceWindowPosition("1junk", "2"); });
    Reject([] { ParsePerformanceWindowPosition("32768", "2"); });
    Reject([] { ParsePerformanceWindowPosition("1.5", "2"); });
    using Stats = Prism::RHI::FramePacingStatistics;
    Stats stats;
    Expect(stats.presentMode == -1 && stats.generation == 0, "Unexpected pacing defaults.");
    stats.Reset(1);
    { Prism::RHI::FramePacingScope disabled(nullptr, Stats::Phase::FrameFence); }
    Expect(stats.milliseconds[0] == 0, "Disabled observer changed statistics.");
    Prism::RHI::FramePacingScope scope(&stats, Stats::Phase::FrameFence);
    scope.End();
    const auto elapsed = stats.milliseconds[0];
    scope.End();
    Expect(std::isfinite(elapsed) && elapsed >= 0 && stats.milliseconds[0] == elapsed, "Scope ended twice.");
    stats.endCompleted = true;
    stats.presentResult = 123;
    stats.Reset(0);
    Expect(stats.generation == 2 && stats.slot == 0 && !stats.endCompleted && stats.presentResult == 0 &&
        stats.milliseconds[0] == 0 && stats.presentMode == -1, "Stale pacing frame data survived reset.");
}

void TestFramePacingAutomation()
{
    using Prism::Core::ParseFramePacingAutomationSequence;
    using Prism::Core::ParseWindowResizeAutomationSequence;
    using Prism::RHI::FramePacingProfile;

    Expect(ParseFramePacingAutomationSequence("").empty(),
        "Empty frame-pacing automation changed runtime behavior.");
    const auto pacing = ParseFramePacingAutomationSequence(
        "20:low-latency,40:benchmark,60:interactive-smooth");
    Expect(pacing.size() == 3
            && pacing[0].frame == 20
            && pacing[0].profile == FramePacingProfile::LowLatency
            && pacing[1].profile == FramePacingProfile::Benchmark
            && pacing[2].profile
                == FramePacingProfile::InteractiveSmooth,
        "Valid frame-pacing automation was parsed incorrectly.");
    Reject([] { (void)ParseFramePacingAutomationSequence(
        "1:benchmark"); });
    Reject([] { (void)ParseFramePacingAutomationSequence(
        "20:benchmark,20:low-latency"); });
    Reject([] { (void)ParseFramePacingAutomationSequence(
        "20:custom"); });
    Reject([] { (void)ParseFramePacingAutomationSequence(
        "20:benchmark,"); });
    Reject([] { (void)ParseFramePacingAutomationSequence(
        "20benchmark"); });

    Expect(ParseWindowResizeAutomationSequence("").empty(),
        "Empty resize automation changed runtime behavior.");
    const auto resizes = ParseWindowResizeAutomationSequence(
        "30:1024x720,50:1280x800");
    Expect(resizes.size() == 2
            && resizes[0].frame == 30
            && resizes[0].width == 1024
            && resizes[0].height == 720
            && resizes[1].frame == 50
            && resizes[1].width == 1280
            && resizes[1].height == 800,
        "Valid resize automation was parsed incorrectly.");
    Reject([] { (void)ParseWindowResizeAutomationSequence(
        "1:1024x720"); });
    Reject([] { (void)ParseWindowResizeAutomationSequence(
        "30:1024x720,20:1280x800"); });
    Reject([] { (void)ParseWindowResizeAutomationSequence(
        "30:63x720"); });
    Reject([] { (void)ParseWindowResizeAutomationSequence(
        "30:1024x8193"); });
    Reject([] { (void)ParseWindowResizeAutomationSequence(
        "30:1024X720"); });
    Reject([] { (void)ParseWindowResizeAutomationSequence(
        "30:1024x720,"); });
}
} // namespace
int main()
{
    try
    {
        const auto root = std::filesystem::current_path() / "frame-performance-tests"
            / std::to_string(Recorder::Clock::now().time_since_epoch().count());
        std::filesystem::create_directories(root);
        TestRecorder(root); TestCounters(); TestPacing();
        TestFramePacingAutomation();
        std::cout << "Frame performance tests passed.\n";
        return 0;
    }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
