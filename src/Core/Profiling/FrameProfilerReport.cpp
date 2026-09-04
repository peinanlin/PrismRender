#include "Core/Profiling/FrameProfilerReport.h"

#include <stdexcept>

namespace Prism::Core
{
namespace
{
nlohmann::json SerializeDuration(
    const FrameProfileDuration& duration)
{
    return duration.available
        ? nlohmann::json(duration.milliseconds)
        : nlohmann::json(nullptr);
}

nlohmann::json SerializeLane(
    const FrameCpuLaneProfile& lane)
{
    return {
        {"activeMs", SerializeDuration(lane.active)},
        {"waitMs", SerializeDuration(lane.waiting)},
        {"threadId", lane.threadId == 0
            ? nlohmann::json(nullptr)
            : nlohmann::json(lane.threadId)}};
}

nlohmann::json SerializeView(
    const FrameViewProfile& view)
{
    return {
        {"active", view.active},
        {"rendered", view.rendered},
        {"width", view.width},
        {"height", view.height},
        {"refreshPolicy", ToString(view.refreshPolicy)},
        {"refreshReason", ToString(view.refreshReason)},
        {"cpuRenderMs", SerializeDuration(view.cpuRender)},
        {"gpuTotalMs", SerializeDuration(view.gpuTotal)},
        {"gpuGeneration", view.gpuGeneration == 0
            ? nlohmann::json(nullptr)
            : nlohmann::json(view.gpuGeneration)},
        {"gpuResolvedFrameId", view.gpuResolvedFrameId == 0
            ? nlohmann::json(nullptr)
            : nlohmann::json(view.gpuResolvedFrameId)}};
}
} // namespace

FrameProfilerReport::FrameProfilerReport(
    const std::filesystem::path& path,
    const nlohmann::json& metadata)
{
    if (path.empty())
    {
        throw std::invalid_argument(
            "Frame profiler report path must not be empty.");
    }
    if (path.has_parent_path())
    {
        std::filesystem::create_directories(path.parent_path());
    }
    m_output.open(path, std::ios::out | std::ios::trunc);
    if (!m_output)
    {
        throw std::runtime_error(
            "Could not create frame profiler report.");
    }
    Write({
        {"type", "header"},
        {"format", "PrismFrameProfiler"},
        {"version", 1},
        {"metadata", metadata},
        {"exporterTiming", "outside-editor-loop"}});
}

void FrameProfilerReport::Write(const nlohmann::json& value)
{
    m_output << value.dump() << '\n';
    if (!m_output)
    {
        throw std::runtime_error(
            "Could not write frame profiler report.");
    }
}

void FrameProfilerReport::WriteCompleted(
    const FrameProfilerSnapshot& snapshot)
{
    if (m_finished)
    {
        throw std::logic_error(
            "Frame profiler report was written after finish.");
    }
    if (!snapshot.completed
        || snapshot.frameId != m_completedFrameCount + 1)
    {
        throw std::logic_error(
            "Frame profiler report requires ordered completed snapshots.");
    }

    nlohmann::json phases = nlohmann::json::object();
    for (std::size_t index = 0; index < snapshot.cpuPhases.size(); ++index)
    {
        phases[ToString(static_cast<FrameCpuPhase>(index))] =
            SerializeDuration(snapshot.cpuPhases[index]);
    }
    nlohmann::json waits = nlohmann::json::object();
    for (std::size_t index = 0; index < snapshot.waits.size(); ++index)
    {
        waits[ToString(static_cast<FrameWaitPhase>(index))] =
            SerializeDuration(snapshot.waits[index]);
    }
    Write({
        {"type", "frame"},
        {"frameId", snapshot.frameId},
        {"requestedLevel", ToString(snapshot.requestedLevel)},
        {"actualLevel", ToString(snapshot.actualLevel)},
        {"activeViewMask", snapshot.activeViewMask},
        {"editorLoopMs", SerializeDuration(snapshot.editorLoop)},
        {"gpuFrameMs", SerializeDuration(snapshot.gpuFrame)},
        {"profilerOverheadMs", SerializeDuration(
            snapshot.profilerOverhead)},
        {"lanes", {
            {"main", SerializeLane(snapshot.cpuLanes[
                static_cast<std::size_t>(FrameCpuLane::Main)])},
            {"render", SerializeLane(snapshot.cpuLanes[
                static_cast<std::size_t>(FrameCpuLane::Render)])},
            {"worker", SerializeLane(snapshot.cpuLanes[
                static_cast<std::size_t>(FrameCpuLane::Worker)])}}},
        {"workers", {
            {"available", snapshot.workers.available},
            {"queuedMs", SerializeDuration(snapshot.workers.queued)},
            {"executingMs", SerializeDuration(
                snapshot.workers.executing)},
            {"joiningMs", SerializeDuration(snapshot.workers.joining)},
            {"activeWorkers", snapshot.workers.activeWorkers},
            {"peakActiveWorkers", snapshot.workers.peakActiveWorkers},
            {"queuedTasks", snapshot.workers.queuedTasks},
            {"peakQueuedTasks", snapshot.workers.peakQueuedTasks}}},
        {"pipeline", {
            {"acceptanceId", snapshot.pipeline.acceptanceId},
            {"logicalFrameId", snapshot.pipeline.logicalFrameId},
            {"sceneEpoch", snapshot.pipeline.sceneEpoch},
            {"viewEpoch", snapshot.pipeline.viewEpoch},
            {"settingsRevision", snapshot.pipeline.settingsRevision},
            {"waitingDepthAtAcceptance",
                snapshot.pipeline.waitingDepthAtAcceptance},
            {"peakWaitingDepth", snapshot.pipeline.peakWaitingDepth},
            {"queueWaitMs", SerializeDuration(
                snapshot.pipeline.queueWait)},
            {"completionWaitMs", SerializeDuration(
                snapshot.pipeline.completionWait)},
            {"mainPreparationOverlapMs", SerializeDuration(
                snapshot.pipeline.mainPreparationOverlap)},
            {"inputToPresentMs", SerializeDuration(
                snapshot.pipeline.inputToPresent)}}},
        {"framePacing", {
            {"available", snapshot.framePacing.available},
            {"profile", snapshot.framePacing.profile},
            {"requestedPresentation",
                snapshot.framePacing.requestedPresentation},
            {"effectivePresentation",
                snapshot.framePacing.effectivePresentation},
            {"nativePresentMode",
                snapshot.framePacing.nativePresentMode},
            {"admissionSource", snapshot.framePacing.admissionSource},
            {"fallbackReason", snapshot.framePacing.fallbackReason},
            {"targetFps", snapshot.framePacing.targetFpsEnabled
                ? nlohmann::json(snapshot.framePacing.targetFps)
                : nlohmann::json(nullptr)},
            {"configuredMaxQueuedFrames",
                snapshot.framePacing.configuredMaxQueuedFrames},
            {"effectiveMaxQueuedFrames",
                snapshot.framePacing.effectiveMaxQueuedFrames},
            {"swapchainImageCount",
                snapshot.framePacing.swapchainImageCount},
            {"frameResourceSlotCount",
                snapshot.framePacing.frameResourceSlotCount},
            {"syncInterval", snapshot.framePacing.syncInterval},
            {"presentFlags", snapshot.framePacing.presentFlags},
            {"requestedGeneration",
                snapshot.framePacing.requestedGeneration},
            {"effectiveGeneration",
                snapshot.framePacing.effectiveGeneration},
            {"tearingSupported", snapshot.framePacing.tearingSupported},
            {"tearingEnabled", snapshot.framePacing.tearingEnabled},
            {"transitionPending", snapshot.framePacing.transitionPending}}},
        {"phases", std::move(phases)},
        {"waits", std::move(waits)},
        {"views", {
            {"game", SerializeView(snapshot.views[
                static_cast<std::size_t>(ProfiledView::Game)])},
            {"scene", SerializeView(snapshot.views[
                static_cast<std::size_t>(ProfiledView::Scene)])}}}});
    ++m_completedFrameCount;
}

void FrameProfilerReport::Finish(
    const std::uint64_t expectedFrameCount)
{
    if (m_finished
        || expectedFrameCount == 0
        || m_completedFrameCount != expectedFrameCount)
    {
        throw std::logic_error(
            "Frame profiler report ended before the requested frame count.");
    }
    Write({
        {"type", "footer"},
        {"status", "complete"},
        {"completedFrames", m_completedFrameCount}});
    m_output.flush();
    if (!m_output)
    {
        throw std::runtime_error(
            "Could not flush frame profiler report.");
    }
    m_finished = true;
}
} // namespace Prism::Core
