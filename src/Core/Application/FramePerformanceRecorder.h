#pragma once

#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string_view>

#include <json.hpp>

namespace Prism::Core
{
// Opt-in application observer. Bounded slot identities and one frame of JSON;
// owns no renderer/GPU objects and never resolves or waits for GPU work.
class FramePerformanceRecorder
{
public:
    enum class Metric { Extraction, UiBuild, UiDraw, GameRender, SceneRender, BeginFrame, Present, Count };
    using Clock = std::chrono::steady_clock;
    class Scope
    {
    public:
        Scope(FramePerformanceRecorder* recorder, Metric metric);
        ~Scope();
        Scope(const Scope&) = delete;
        Scope& operator=(const Scope&) = delete;
        void End();
    private:
        FramePerformanceRecorder* m_recorder;
        Metric m_metric;
        Clock::time_point m_start{};
    };

    explicit FramePerformanceRecorder(const std::filesystem::path& path, const nlohmann::json& metadata);
    void BeginFrame(std::uint64_t frameId);
    void CancelFrame();
    void AddCpuMilliseconds(Metric metric, double milliseconds);
    // Must observe the old resolved slot BEFORE associating the current render
    // with that slot. A view's profiler generation is not a logical frame ID.
    void RecordView(std::string_view view, std::uint32_t currentSlot,
        std::uint64_t timingGeneration, std::uint32_t resolvedSlot,
        nlohmann::json viewMetadata, const nlohmann::json& gpuReport);
    void CompleteFrame(nlohmann::json resourceStatistics);
    void Finish(std::uint64_t expectedFrames);

private:
    static constexpr std::size_t MaximumFrameSlots = 16;
    struct ViewState
    {
        std::uint64_t lastGeneration = 0;
        std::array<std::uint64_t, MaximumFrameSlots> slotFrames{};
        std::array<std::uint64_t, MaximumFrameSlots> consumedFrames{};
    };
    void Write(const nlohmann::json& value);
    std::ofstream m_output;
    std::array<ViewState, 2> m_views{};
    std::array<double, static_cast<std::size_t>(Metric::Count)> m_cpu{};
    nlohmann::json m_frameViews;
    nlohmann::json m_gpuRecords;
    Clock::time_point m_frameStart{};
    Clock::time_point m_previousStart{};
    double m_loopIntervalMs = 0;
    std::uint64_t m_frameId = 0;
    std::uint64_t m_completedFrames = 0;
    bool m_open = false;
    bool m_finished = false;
};
} // namespace Prism::Core
