#include "Core/Application/FramePerformanceRecorder.h"

#include <cmath>
#include <stdexcept>
#include <string>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <psapi.h>
#else
#include <sys/resource.h>
#endif

namespace Prism::Core
{
namespace
{
using Json = nlohmann::json;
constexpr std::array<const char*, 7> MetricNames{
    "extractionMs", "uiBuildMs", "uiDrawMs", "gameRenderMs", "sceneRenderMs", "beginFrameMs", "presentMs"};

Json ReadProcessMemory()
{
#if defined(_WIN32)
    PROCESS_MEMORY_COUNTERS_EX memory{};
    if (!K32GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&memory), sizeof(memory)))
        throw std::runtime_error("Performance recorder could not read process memory.");
    return {{"source", "GetProcessMemoryInfo"}, {"workingSetBytes", memory.WorkingSetSize},
        {"peakWorkingSetBytes", memory.PeakWorkingSetSize}, {"privateCommitBytes", memory.PrivateUsage},
        {"peakCommitBytes", memory.PeakPagefileUsage}};
#else
    rusage usage{};
    if (getrusage(RUSAGE_SELF, &usage) != 0)
        throw std::runtime_error("Performance recorder could not read process memory.");
    // Linux peak RSS is KiB. Unsupported current/private values stay null,
    // never zero (which could be mistaken for a valid measurement).
    return {{"source", "getrusage-linux"}, {"workingSetBytes", nullptr},
        {"peakWorkingSetBytes", static_cast<std::uint64_t>(usage.ru_maxrss) * 1024u},
        {"privateCommitBytes", nullptr}, {"peakCommitBytes", nullptr}};
#endif
}
} // namespace

FramePerformanceRecorder::Scope::Scope(FramePerformanceRecorder* recorder, Metric metric)
    : m_recorder(recorder), m_metric(metric)
{
    if (recorder) m_start = Clock::now();
}
FramePerformanceRecorder::Scope::~Scope() { End(); }
void FramePerformanceRecorder::Scope::End()
{
    if (!m_recorder) return;
    // Scopes are owned by the application and end before Complete/CancelFrame.
    m_recorder->AddCpuMilliseconds(m_metric, std::chrono::duration<double, std::milli>(Clock::now() - m_start).count());
    m_recorder = nullptr;
}

FramePerformanceRecorder::FramePerformanceRecorder(const std::filesystem::path& path, const Json& metadata)
{
    if (path.empty() || std::filesystem::exists(path))
        throw std::invalid_argument("Performance output must be a new file.");
    if (!path.parent_path().empty()) std::filesystem::create_directories(path.parent_path());
    m_output.open(path, std::ios::binary | std::ios::out);
    if (!m_output) throw std::runtime_error("Could not open performance output.");
    Write({{"type", "header"}, {"format", "PrismFramePerformance"}, {"version", 1}, {"metadata", metadata},
        {"gpuPolicy", "read-at-normal-frame-slot-reuse-no-extra-wait"},
        {"memoryPolicy", "process-cpu-memory-not-vram; lifetime-peaks-include-startup"},
        {"psoPolicy", "successful-IGraphicsDevice-graphics-and-compute-creations; excludes-external-ImGui-backend"},
        {"cpuPolicy", "elapsed-wall-ms-not-thread-cpu; frame-excludes-final-memory-sample-and-json-write; loopInterval-includes-observer-cost"}});
}

void FramePerformanceRecorder::Write(const Json& value)
{
    m_output << value.dump() << '\n';
    if (!m_output) throw std::runtime_error("Failed to write performance output.");
}

void FramePerformanceRecorder::BeginFrame(std::uint64_t frameId)
{
    if (m_finished || m_open || frameId != m_completedFrames + 1)
        throw std::logic_error("Performance frame must be consecutive and not already open.");
    m_frameStart = Clock::now();
    m_loopIntervalMs = m_previousStart == Clock::time_point{} ? 0.0
        : std::chrono::duration<double, std::milli>(m_frameStart - m_previousStart).count();
    m_previousStart = m_frameStart;
    m_frameId = frameId;
    m_cpu.fill(0);
    m_frameViews = Json::object();
    m_gpuRecords = Json::array();
    m_open = true;
}

void FramePerformanceRecorder::CancelFrame()
{
    if (!m_open || !m_frameViews.empty()) throw std::logic_error("Cannot cancel a recorded performance frame.");
    m_open = false;
}

void FramePerformanceRecorder::AddCpuMilliseconds(Metric metric, double milliseconds)
{
    const auto index = static_cast<std::size_t>(metric);
    if (!m_open || index >= m_cpu.size() || !std::isfinite(milliseconds) || milliseconds < 0)
        throw std::logic_error("Invalid performance CPU sample.");
    m_cpu[index] += milliseconds;
}

void FramePerformanceRecorder::RecordView(std::string_view view, std::uint32_t currentSlot,
    std::uint64_t timingGeneration, std::uint32_t resolvedSlot, Json viewMetadata, const Json& gpuReport)
{
    const std::string key(view);
    if (!m_open || (view != "game" && view != "scene") || m_frameViews.contains(key)
        || currentSlot >= MaximumFrameSlots || resolvedSlot >= MaximumFrameSlots)
        throw std::logic_error("Invalid or duplicate performance view/slot.");
    auto& state = m_views[view == "game" ? 0 : 1];
    if (timingGeneration < state.lastGeneration)
        throw std::logic_error("Performance GPU generation moved backwards.");
    if (timingGeneration > state.lastGeneration)
    {
        const auto sourceFrame = state.slotFrames[resolvedSlot];
        if (sourceFrame == 0 || sourceFrame >= m_frameId || state.consumedFrames[resolvedSlot] == sourceFrame)
            throw std::logic_error("Performance GPU sample has no unique retained source frame.");
        if (gpuReport.at("format") != "PrismGpuTimingReport" || gpuReport.at("passes").empty())
            throw std::logic_error("Performance GPU report is missing timings.");
        m_gpuRecords.push_back({{"type", "gpu"}, {"view", key}, {"frameId", sourceFrame},
            {"observedFrameId", m_frameId}, {"slot", resolvedSlot}, {"generation", timingGeneration}, {"report", gpuReport}});
        state.consumedFrames[resolvedSlot] = sourceFrame;
        state.lastGeneration = timingGeneration;
    }
    state.slotFrames[currentSlot] = m_frameId;
    viewMetadata["slot"] = currentSlot;
    m_frameViews[key] = std::move(viewMetadata);
}

void FramePerformanceRecorder::CompleteFrame(Json resourceStatistics)
{
    if (!m_open || !m_frameViews.contains("game")) throw std::logic_error("Performance frame lacks Game view.");
    Json cpu{{"frameMs", std::chrono::duration<double, std::milli>(Clock::now() - m_frameStart).count()},
        {"loopIntervalMs", m_loopIntervalMs}};
    for (std::size_t i = 0; i < m_cpu.size(); ++i) cpu[MetricNames[i]] = m_cpu[i];
    Write({{"type", "cpu"}, {"frameId", m_frameId}, {"cpu", cpu}, {"views", m_frameViews},
        {"memory", ReadProcessMemory()}, {"resources", std::move(resourceStatistics)}});
    for (const auto& record : m_gpuRecords) Write(record);
    m_open = false;
    ++m_completedFrames;
}

void FramePerformanceRecorder::Finish(std::uint64_t expectedFrames)
{
    if (m_finished || m_open || expectedFrames == 0 || m_completedFrames != expectedFrames)
        throw std::logic_error("Performance run ended before the requested frame count.");
    Write({{"type", "footer"}, {"status", "complete"}, {"completedFrames", m_completedFrames}});
    m_output.flush();
    if (!m_output) throw std::runtime_error("Could not flush performance output.");
    m_finished = true;
}
} // namespace Prism::Core
