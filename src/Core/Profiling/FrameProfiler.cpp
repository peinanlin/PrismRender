#include "Core/Profiling/FrameProfiler.h"

#include <cmath>
#include <functional>
#include <stdexcept>
#include <string>
#include <thread>

namespace Prism::Core
{
std::string_view ToString(const ProfilingLevel level)
{
    switch (level)
    {
    case ProfilingLevel::Off: return "off";
    case ProfilingLevel::Basic: return "basic";
    case ProfilingLevel::Detailed: return "detailed";
    case ProfilingLevel::Capture: return "capture";
    }
    return "unknown";
}

ProfilingLevel ParseProfilingLevel(
    const std::string_view value)
{
    if (value == "off") return ProfilingLevel::Off;
    if (value == "basic") return ProfilingLevel::Basic;
    if (value == "detailed") return ProfilingLevel::Detailed;
    if (value == "capture") return ProfilingLevel::Capture;
    throw std::invalid_argument(
        "Profiling level must be off, basic, detailed, or capture.");
}

std::string_view ToString(const FrameCpuLane lane)
{
    switch (lane)
    {
    case FrameCpuLane::Main: return "main";
    case FrameCpuLane::Render: return "render";
    case FrameCpuLane::Worker: return "worker";
    case FrameCpuLane::Count: break;
    }
    return "unknown";
}

std::string_view ToString(const FrameCpuPhase phase)
{
    switch (phase)
    {
    case FrameCpuPhase::EventsAndInput: return "events-and-input";
    case FrameCpuPhase::UiBuild: return "ui-build";
    case FrameCpuPhase::BeginFrame: return "begin-frame";
    case FrameCpuPhase::ScenePublication: return "scene-publication";
    case FrameCpuPhase::GameRender: return "game-render";
    case FrameCpuPhase::SceneRender: return "scene-render";
    case FrameCpuPhase::RenderGraphBuild: return "render-graph-build";
    case FrameCpuPhase::RenderGraphExecute: return "render-graph-execute";
    case FrameCpuPhase::UiDraw: return "ui-draw";
    case FrameCpuPhase::SubmitAndPresent: return "submit-and-present";
    case FrameCpuPhase::CaptureAndResolve: return "capture-and-resolve";
    case FrameCpuPhase::Count: break;
    }
    return "unknown";
}

std::string_view ToString(const FrameWaitPhase phase)
{
    switch (phase)
    {
    case FrameWaitPhase::DisplayAdmission: return "display-admission";
    case FrameWaitPhase::FrameLimiter: return "frame-limiter";
    case FrameWaitPhase::FrameFence: return "frame-fence";
    case FrameWaitPhase::Reclaim: return "reclaim";
    case FrameWaitPhase::Acquire: return "acquire";
    case FrameWaitPhase::ImageFence: return "image-fence";
    case FrameWaitPhase::Upload: return "upload";
    case FrameWaitPhase::Prepare: return "prepare";
    case FrameWaitPhase::Submit: return "submit";
    case FrameWaitPhase::NativePresent: return "native-present";
    case FrameWaitPhase::Signal: return "signal";
    case FrameWaitPhase::QueueBackpressure: return "queue-backpressure";
    case FrameWaitPhase::Count: break;
    }
    return "unknown";
}

std::string_view ToString(const ProfiledView view)
{
    switch (view)
    {
    case ProfiledView::Game: return "game";
    case ProfiledView::Scene: return "scene";
    case ProfiledView::Count: break;
    }
    return "unknown";
}

std::string_view ToString(const SceneViewRefreshReason reason)
{
    switch (reason)
    {
    case SceneViewRefreshReason::Unavailable: return "unavailable";
    case SceneViewRefreshReason::CatalogDefault: return "catalog-default";
    case SceneViewRefreshReason::Live: return "live";
    case SceneViewRefreshReason::Interaction: return "interaction";
    case SceneViewRefreshReason::RateLimited: return "rate-limited";
    case SceneViewRefreshReason::Paused: return "paused";
    case SceneViewRefreshReason::Capture: return "capture";
    case SceneViewRefreshReason::Hidden: return "hidden";
    }
    return "unknown";
}

std::string_view ToString(const SceneViewRefreshPolicy policy)
{
    switch (policy)
    {
    case SceneViewRefreshPolicy::CatalogDefault:
        return "catalog-default";
    case SceneViewRefreshPolicy::Live: return "live";
    case SceneViewRefreshPolicy::OnInteraction:
        return "on-interaction";
    case SceneViewRefreshPolicy::ThirtyHertz: return "30hz";
    case SceneViewRefreshPolicy::Paused: return "paused";
    }
    return "unknown";
}

SceneViewRefreshPolicy ParseSceneViewRefreshPolicy(
    const std::string_view value)
{
    if (value == "catalog-default")
    {
        return SceneViewRefreshPolicy::CatalogDefault;
    }
    if (value == "live") return SceneViewRefreshPolicy::Live;
    if (value == "on-interaction")
    {
        return SceneViewRefreshPolicy::OnInteraction;
    }
    if (value == "30hz") return SceneViewRefreshPolicy::ThirtyHertz;
    if (value == "paused") return SceneViewRefreshPolicy::Paused;
    throw std::invalid_argument(
        "Scene View refresh policy must be catalog-default, live, "
        "on-interaction, 30hz, or paused.");
}

std::optional<FrameProfiler::ResolvedGpuSample>
FrameProfiler::AssociateViewGpuTiming(
    const ProfiledView view,
    const std::uint64_t currentFrameId,
    const std::uint32_t currentSlot,
    const std::uint64_t timingGeneration,
    const std::uint32_t resolvedSlot,
    const double totalMilliseconds)
{
    const std::size_t viewIndex = static_cast<std::size_t>(view);
    if (viewIndex >= m_viewGpuStates.size()
        || currentFrameId == 0
        || currentSlot >= MaximumGpuFrameSlots
        || resolvedSlot >= MaximumGpuFrameSlots
        || !std::isfinite(totalMilliseconds)
        || totalMilliseconds < 0.0)
    {
        throw std::invalid_argument(
            "A view GPU timing association is invalid.");
    }

    std::scoped_lock lock(m_mutex);
    if (m_shutdown)
    {
        throw std::logic_error(
            "A GPU timing was associated after profiler shutdown.");
    }
    ViewGpuState& state = m_viewGpuStates[viewIndex];
    if (timingGeneration < state.lastGeneration)
    {
        throw std::logic_error(
            "A view GPU timing generation moved backwards.");
    }

    std::optional<ResolvedGpuSample> result;
    if (timingGeneration > state.lastGeneration)
    {
        const std::uint64_t sourceFrame =
            state.slotFrames[resolvedSlot];
        if (sourceFrame != 0
            && sourceFrame < currentFrameId
            && state.consumedFrames[resolvedSlot]
                != sourceFrame)
        {
            result = ResolvedGpuSample{
                totalMilliseconds,
                timingGeneration,
                sourceFrame};
            state.consumedFrames[resolvedSlot] = sourceFrame;
        }
        state.lastGeneration = timingGeneration;
    }
    state.slotFrames[currentSlot] = currentFrameId;
    return result;
}

void FrameProfiler::ValidateDuration(
    const FrameProfileDuration& duration,
    const char* const name)
{
    if (!duration.available)
    {
        if (duration.milliseconds != 0.0)
        {
            throw std::invalid_argument(
                std::string(name)
                + " is unavailable but contains a duration.");
        }
        return;
    }
    if (!std::isfinite(duration.milliseconds)
        || duration.milliseconds < 0.0)
    {
        throw std::invalid_argument(
            std::string(name)
            + " must contain a finite non-negative duration.");
    }
}

void FrameProfiler::ValidateSnapshot(
    const FrameProfilerSnapshot& snapshot) const
{
    if (snapshot.frameId == 0)
    {
        throw std::invalid_argument(
            "A completed frame profile requires a non-zero frame ID.");
    }
    if (snapshot.frameId <= m_latestFrameId)
    {
        throw std::logic_error(
            "A frame profile was completed more than once or out of order.");
    }
    ValidateDuration(snapshot.editorLoop, "Editor loop");
    ValidateDuration(snapshot.gpuFrame, "GPU frame");
    ValidateDuration(snapshot.profilerOverhead, "Profiler overhead");
    for (std::size_t laneIndex = 0;
         laneIndex < snapshot.cpuLanes.size();
         ++laneIndex)
    {
        const FrameCpuLaneProfile& lane = snapshot.cpuLanes[laneIndex];
        ValidateDuration(lane.active, "CPU lane active time");
        ValidateDuration(lane.waiting, "CPU lane wait time");
        if ((lane.active.available || lane.waiting.available)
            && lane.threadId == 0
            && laneIndex != static_cast<std::size_t>(
                FrameCpuLane::Worker))
        {
            throw std::invalid_argument(
                "An available CPU lane requires a thread ID.");
        }
    }
    for (const FrameProfileDuration& phase : snapshot.cpuPhases)
    {
        ValidateDuration(phase, "CPU phase");
    }
    for (const FrameProfileDuration& wait : snapshot.waits)
    {
        ValidateDuration(wait, "Wait phase");
    }
    ValidateDuration(snapshot.workers.queued, "Worker queue time");
    ValidateDuration(snapshot.workers.executing, "Worker execution time");
    ValidateDuration(snapshot.workers.joining, "Worker join time");
    for (std::size_t index = 0; index < snapshot.views.size(); ++index)
    {
        const FrameViewProfile& view = snapshot.views[index];
        ValidateDuration(view.cpuRender, "View CPU render time");
        ValidateDuration(view.gpuTotal, "View GPU total time");
        if (!view.gpuTotal.available)
        {
            if (view.gpuGeneration != 0
                || view.gpuResolvedFrameId != 0)
            {
                throw std::invalid_argument(
                    "An unavailable view GPU sample contains an identity.");
            }
            continue;
        }
        if (view.gpuGeneration == 0
            || view.gpuResolvedFrameId == 0
            || view.gpuResolvedFrameId > snapshot.frameId)
        {
            throw std::invalid_argument(
                "A view GPU sample has an invalid resolved identity.");
        }
        if (view.gpuGeneration
            <= m_latestGpuGenerations[index])
        {
            throw std::logic_error(
                "A view GPU timing generation moved backwards or repeated.");
        }
    }
}

void FrameProfiler::SubmitCompleted(
    FrameProfilerSnapshot snapshot)
{
    std::scoped_lock lock(m_mutex);
    if (m_shutdown)
    {
        throw std::logic_error(
            "A completed frame was submitted after profiler shutdown.");
    }
    ValidateSnapshot(snapshot);
    snapshot.completed = true;
    snapshot.activeViewMask = 0;
    for (std::size_t index = 0; index < snapshot.views.size(); ++index)
    {
        const FrameViewProfile& view = snapshot.views[index];
        if (view.active)
        {
            snapshot.activeViewMask |=
                static_cast<std::uint8_t>(1u << index);
        }
        if (view.gpuTotal.available)
        {
            m_latestGpuGenerations[index] =
                view.gpuGeneration;
        }
    }

    std::size_t destination = 0;
    if (m_size < MaximumHistoryFrames)
    {
        destination = (m_oldest + m_size)
            % MaximumHistoryFrames;
        ++m_size;
    }
    else
    {
        destination = m_oldest;
        m_oldest = (m_oldest + 1)
            % MaximumHistoryFrames;
    }
    m_history[destination] = std::move(snapshot);
    m_latestFrameId = m_history[destination].frameId;
}

void FrameProfiler::SetRequestedLevel(
    const ProfilingLevel level)
{
    std::scoped_lock lock(m_mutex);
    if (m_shutdown)
    {
        throw std::logic_error(
            "Profiling level changed after profiler shutdown.");
    }
    m_requestedLevel = level;
}

ProfilingLevel FrameProfiler::GetRequestedLevel() const
{
    std::scoped_lock lock(m_mutex);
    return m_requestedLevel;
}

std::optional<FrameProfilerSnapshot>
FrameProfiler::GetLatest() const
{
    std::scoped_lock lock(m_mutex);
    if (m_size == 0)
    {
        return std::nullopt;
    }
    const std::size_t latest =
        (m_oldest + m_size - 1)
        % MaximumHistoryFrames;
    return m_history[latest];
}

std::vector<FrameProfilerSnapshot>
FrameProfiler::GetHistory() const
{
    std::scoped_lock lock(m_mutex);
    std::vector<FrameProfilerSnapshot> result;
    result.reserve(m_size);
    for (std::size_t index = 0; index < m_size; ++index)
    {
        result.push_back(
            m_history[(m_oldest + index)
                % MaximumHistoryFrames]);
    }
    return result;
}

std::size_t FrameProfiler::GetHistorySize() const
{
    std::scoped_lock lock(m_mutex);
    return m_size;
}

bool FrameProfiler::IsShutdown() const
{
    std::scoped_lock lock(m_mutex);
    return m_shutdown;
}

void FrameProfiler::Shutdown()
{
    std::scoped_lock lock(m_mutex);
    m_shutdown = true;
}

std::uint64_t FrameProfiler::CurrentThreadId()
{
    const std::uint64_t value = static_cast<std::uint64_t>(
        std::hash<std::thread::id>{}(
            std::this_thread::get_id()));
    return value == 0 ? 1 : value;
}
} // namespace Prism::Core
