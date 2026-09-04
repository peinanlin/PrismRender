#include "Core/Application/FramePerformanceSampling.h"
#include "Core/Application/FramePerformanceRecorder.h"
#include "Core/Application/RenderExecutionService.h"
#include "Renderer/GpuTimingReport.h"
#include "Renderer/RenderSettings.h"
#include "Platform/WindowDiagnostics.h"

#include <stdexcept>

namespace Prism::Core
{
void RecordPerformanceView(
    FramePerformanceRecorder& recorder,
    const RenderViewRuntimeFeedback& feedback,
    const Renderer::RenderSettings& settings,
    const RHI::GraphicsApi graphicsApi,
    const std::uint32_t frameContextIndex,
    const std::uint32_t width,
    const std::uint32_t height,
    const std::string_view view,
    const double simulationTime,
    const std::uint64_t sceneGeneration)
{
    recorder.RecordView(view, frameContextIndex,
        feedback.gpuTimingGeneration, feedback.gpuTimingFrameSlot,
        {{"width", width}, {"height", height},
            {"simulationTimeSeconds", simulationTime}, {"sceneGeneration", sceneGeneration},
            {"taaEnabled", settings.temporalAntiAliasingEnabled}, {"shadowsEnabled", settings.shadowsEnabled},
            {"fluidEnabled", settings.fluid.enabled}, {"cachedPsoCount", feedback.statistics.cachedPsoCount}},
        Renderer::BuildGpuTimingReport(feedback.gpuTimings, graphicsApi,
            feedback.gpuTimelineMetadata));
}

void CompletePerformanceFrame(
    FramePerformanceRecorder& recorder,
    const RenderFrameFeedback& feedback,
    const Platform::Window& window)
{
    if (!feedback.deviceStatistics.has_value())
    {
        throw std::logic_error(
            "Performance frame is missing execution-lane device statistics.");
    }
    const RenderDeviceRuntimeFeedback& device =
        *feedback.deviceStatistics;
    const auto& pso = device.pipelineCreation;
    const auto& retirement = device.resourceRetirement;
    const auto& upload = device.uploadQueue;
    const auto& descriptors = device.descriptors;
    nlohmann::json resources = {{"pso", {{"graphicsCreated", pso.graphics}, {"computeCreated", pso.compute}}},
        {"retirement", {{"retired", retirement.totalRetiredObjectCount}, {"reclaimed", retirement.totalReclaimedObjectCount},
            {"pending", retirement.pendingObjectCount}, {"highWatermark", retirement.pendingObjectHighWatermark}}},
        {"upload", {{"stagingCapacityBytes", upload.stagingCapacityBytes}, {"stagingHighWatermarkBytes", upload.stagingHighWatermarkBytes},
            {"pendingBytes", upload.pendingBytes}, {"outstandingBatchCount", upload.outstandingBatchCount}}},
        {"descriptors", {{"allocatedSets", descriptors.allocatedSetCount}, {"pendingReleaseCount", descriptors.pendingReleaseCount},
            {"resourceHighWatermark", descriptors.resourceDescriptorHighWatermark}, {"samplerHighWatermark", descriptors.samplerDescriptorHighWatermark}}}};
    if (feedback.framePacingStatistics.has_value())
    {
        const RHI::FramePacingStatistics* const pacing =
            &*feedback.framePacingStatistics;
        static constexpr const char* names[] = {"frameFenceMs", "reclaimMs", "acquireMs", "imageFenceMs",
            "uploadMs", "prepareMs", "submitMs", "nativePresentMs", "signalMs"};
        static_assert(std::size(names) == static_cast<std::size_t>(RHI::FramePacingStatistics::Phase::Count));
        nlohmann::json times = nlohmann::json::object();
        for (std::size_t i = 0; i < std::size(names); ++i) times[names[i]] = pacing->milliseconds[i];
        resources["pacing"] = {{"generation", pacing->generation}, {"slot", pacing->slot},
            {"image", pacing->image}, {"imageCount", pacing->imageCount}, {"times", std::move(times)},
            {"api", device.graphicsApi == RHI::GraphicsApi::Direct3D12 ? "d3d12" : "vulkan"},
            {"presentMode", pacing->presentMode}, {"syncInterval", pacing->syncInterval}, {"presentFlags", pacing->presentFlags},
            {"presentResult", pacing->presentResult}, {"acquireResult", pacing->acquireResult}, {"waitResult", pacing->waitResult},
            {"frameFenceWaitCalled", pacing->frameFenceWaitCalled}, {"imageFenceWaitCalled", pacing->imageFenceWaitCalled},
            {"fenceTarget", pacing->fenceTarget}, {"fenceCompletedBefore", pacing->fenceCompletedBefore},
            {"beginCompleted", pacing->beginCompleted}, {"endCompleted", pacing->endCompleted}};
    }
    if (feedback.frameAdmission.has_value())
    {
        const RHI::FrameAdmissionResult& admission =
            *feedback.frameAdmission;
        resources["frameAdmission"] = {
            {"displayWaitMs", admission.displayWaitMilliseconds},
            {"limiterWaitMs", admission.limiterWaitMilliseconds},
            {"queueWaitMs", admission.queueWaitMilliseconds},
            {"submittedFrames", admission.submittedFrames},
            {"outstandingFrames", admission.outstandingFrames},
            {"configurationGeneration",
                admission.configurationGeneration}};
    }
    if (feedback.framePacingState.has_value())
    {
        const RHI::FramePacingState& state =
            *feedback.framePacingState;
        resources["framePacingState"] = {
            {"profile", RHI::ToString(state.requested.profile)},
            {"requestedPresentation",
                RHI::ToString(state.requested.presentation)},
            {"effectivePresentation",
                RHI::ToString(state.effectivePresentation)},
            {"targetFps", state.requested.targetFps.has_value()
                ? nlohmann::json(*state.requested.targetFps)
                : nlohmann::json(nullptr)},
            {"configuredMaxQueuedFrames",
                state.requested.maxQueuedFrames},
            {"effectiveMaxQueuedFrames",
                state.effectiveMaxQueuedFrames},
            {"swapchainImageCount", state.swapchainImageCount},
            {"frameResourceSlotCount", state.frameResourceSlotCount},
            {"syncInterval", state.syncInterval},
            {"presentFlags", state.presentFlags},
            {"nativePresentMode", state.nativePresentMode},
            {"admissionSource", RHI::ToString(state.admissionSource)},
            {"tearingSupported", state.tearingSupported},
            {"tearingEnabled", state.tearingEnabled},
            {"requestedGeneration", state.requestedGeneration},
            {"effectiveGeneration", state.effectiveGeneration},
            {"transitionPending", state.transitionPending},
            {"fallbackReason", state.fallbackReason}};
    }
    const auto observationStart = FramePerformanceRecorder::Clock::now();
    const auto state = Platform::ReadWindowDiagnostics(window);
    resources["window"] = {{"x", state.x}, {"y", state.y}, {"width", state.width}, {"height", state.height},
        {"framebufferWidth", state.framebufferWidth}, {"framebufferHeight", state.framebufferHeight},
        {"scaleX", state.scaleX}, {"scaleY", state.scaleY}, {"visible", state.visible}, {"focused", state.focused},
        {"minimized", state.minimized}, {"displayAvailable", state.displayAvailable}, {"displayName", state.displayName},
        {"monitorX", state.monitorX}, {"monitorY", state.monitorY}, {"monitorWidth", state.monitorWidth},
        {"monitorHeight", state.monitorHeight}, {"refreshHz", state.refreshHz}};
    resources["windowObservationMs"] = std::chrono::duration<double, std::milli>(
        FramePerformanceRecorder::Clock::now() - observationStart).count();
    recorder.CompleteFrame(std::move(resources));
}
} // namespace Prism::Core
