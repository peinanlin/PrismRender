#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <string>

namespace Prism::Core
{
enum class ProfilingLevel : std::uint8_t
{
    Off,
    Basic,
    Detailed,
    Capture
};

enum class FrameCpuLane : std::uint8_t
{
    Main,
    Render,
    Worker,
    Count
};

enum class FrameCpuPhase : std::uint8_t
{
    EventsAndInput,
    UiBuild,
    BeginFrame,
    ScenePublication,
    GameRender,
    SceneRender,
    RenderGraphBuild,
    RenderGraphExecute,
    UiDraw,
    SubmitAndPresent,
    CaptureAndResolve,
    Count
};

enum class FrameWaitPhase : std::uint8_t
{
    DisplayAdmission,
    FrameLimiter,
    FrameFence,
    Reclaim,
    Acquire,
    ImageFence,
    Upload,
    Prepare,
    Submit,
    NativePresent,
    Signal,
    QueueBackpressure,
    Count
};

enum class ProfiledView : std::uint8_t
{
    Game,
    Scene,
    Count
};

enum class SceneViewRefreshReason : std::uint8_t
{
    Unavailable,
    CatalogDefault,
    Live,
    Interaction,
    RateLimited,
    Paused,
    Capture,
    Hidden
};

enum class SceneViewRefreshPolicy : std::uint8_t
{
    CatalogDefault,
    Live,
    OnInteraction,
    ThirtyHertz,
    Paused
};

[[nodiscard]] std::string_view ToString(ProfilingLevel level);
[[nodiscard]] ProfilingLevel ParseProfilingLevel(
    std::string_view value);
[[nodiscard]] std::string_view ToString(FrameCpuLane lane);
[[nodiscard]] std::string_view ToString(FrameCpuPhase phase);
[[nodiscard]] std::string_view ToString(FrameWaitPhase phase);
[[nodiscard]] std::string_view ToString(ProfiledView view);
[[nodiscard]] std::string_view ToString(SceneViewRefreshReason reason);
[[nodiscard]] std::string_view ToString(SceneViewRefreshPolicy policy);
[[nodiscard]] SceneViewRefreshPolicy ParseSceneViewRefreshPolicy(
    std::string_view value);

struct FrameProfileDuration
{
    double milliseconds = 0.0;
    bool available = false;
};

struct FrameCpuLaneProfile
{
    FrameProfileDuration active;
    FrameProfileDuration waiting;
    std::uint64_t threadId = 0;
};

struct FrameViewProfile
{
    FrameProfileDuration cpuRender;
    FrameProfileDuration gpuTotal;
    std::uint64_t gpuGeneration = 0;
    std::uint64_t gpuResolvedFrameId = 0;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    SceneViewRefreshReason refreshReason =
        SceneViewRefreshReason::Unavailable;
    SceneViewRefreshPolicy refreshPolicy =
        SceneViewRefreshPolicy::CatalogDefault;
    bool active = false;
    bool rendered = false;
};

struct FrameWorkerProfile
{
    FrameProfileDuration queued;
    FrameProfileDuration executing;
    FrameProfileDuration joining;
    std::uint32_t activeWorkers = 0;
    std::uint32_t peakActiveWorkers = 0;
    std::uint32_t queuedTasks = 0;
    std::uint32_t peakQueuedTasks = 0;
    bool available = false;
};

struct FramePipelineProfile
{
    std::uint64_t acceptanceId = 0;
    std::uint64_t logicalFrameId = 0;
    std::uint64_t sceneEpoch = 0;
    std::uint64_t viewEpoch = 0;
    std::uint64_t settingsRevision = 0;
    std::uint32_t waitingDepthAtAcceptance = 0;
    std::uint32_t peakWaitingDepth = 0;
    FrameProfileDuration queueWait;
    FrameProfileDuration completionWait;
    FrameProfileDuration mainPreparationOverlap;
    FrameProfileDuration inputToPresent;
};

struct FramePacingProfileSnapshot
{
    std::string profile;
    std::string requestedPresentation;
    std::string effectivePresentation;
    std::string nativePresentMode;
    std::string admissionSource;
    std::string fallbackReason;
    std::uint32_t targetFps = 0;
    std::uint32_t configuredMaxQueuedFrames = 0;
    std::uint32_t effectiveMaxQueuedFrames = 0;
    std::uint32_t swapchainImageCount = 0;
    std::uint32_t frameResourceSlotCount = 0;
    std::uint32_t syncInterval = 0;
    std::uint32_t presentFlags = 0;
    std::uint64_t requestedGeneration = 0;
    std::uint64_t effectiveGeneration = 0;
    bool targetFpsEnabled = false;
    bool tearingSupported = false;
    bool tearingEnabled = false;
    bool transitionPending = false;
    bool available = false;
};

struct FrameProfilerSnapshot
{
    static constexpr std::size_t CpuLaneCount =
        static_cast<std::size_t>(FrameCpuLane::Count);
    static constexpr std::size_t CpuPhaseCount =
        static_cast<std::size_t>(FrameCpuPhase::Count);
    static constexpr std::size_t WaitPhaseCount =
        static_cast<std::size_t>(FrameWaitPhase::Count);
    static constexpr std::size_t ViewCount =
        static_cast<std::size_t>(ProfiledView::Count);

    std::uint64_t frameId = 0;
    ProfilingLevel requestedLevel = ProfilingLevel::Basic;
    ProfilingLevel actualLevel = ProfilingLevel::Basic;
    FrameProfileDuration editorLoop;
    std::array<FrameCpuLaneProfile, CpuLaneCount> cpuLanes{};
    std::array<FrameProfileDuration, CpuPhaseCount> cpuPhases{};
    std::array<FrameProfileDuration, WaitPhaseCount> waits{};
    std::array<FrameViewProfile, ViewCount> views{};
    FrameProfileDuration gpuFrame;
    FrameWorkerProfile workers;
    FramePipelineProfile pipeline;
    FramePacingProfileSnapshot framePacing;
    FrameProfileDuration profilerOverhead;
    std::uint8_t activeViewMask = 0;
    bool completed = false;
};
} // namespace Prism::Core
