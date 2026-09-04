#pragma once

#include "Core/Application/RenderFrameQueue.h"
#include "Core/Threading/ITaskExecutor.h"
#include "RHI/FramePacingStatistics.h"
#include "RHI/DeviceCapabilities.h"
#include "RHI/GraphicsApi.h"
#include "RHI/PipelineCreationStatistics.h"
#include "RHI/Profiling/GpuProfiler.h"
#include "Renderer/FrameDiagnostics.h"
#include "Renderer/RendererStatistics.h"
#include "Scene/RenderViewFeedback.h"

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <exception>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <json.hpp>

namespace Prism::Core
{
struct RenderFrameCompletion
{
    std::uint64_t acceptanceId = 0;
    Scene::LogicalFrameId logicalFrameId;
    Scene::SceneGeneration sceneGeneration;
    Scene::RenderSceneDataRevision dataRevision;
    RenderEpoch sceneEpoch;
    RenderEpoch viewEpoch;
    std::uint64_t settingsRevision = 0;
    std::uint64_t captureRequestId = 0;
    bool captureResolved = false;
    std::string captureError;
};

struct RenderViewRuntimeFeedback
{
    Scene::RenderViewId viewId;
    Renderer::RendererStatistics statistics;
    std::shared_ptr<const Scene::RenderViewFeedback>
        visibilityFeedback;
    // Frozen on the execution lane for validation/diagnostics that must not
    // inspect a live RenderGraph from the main lane.
    bool waterOpticsPassScheduled = false;
    std::uint64_t gpuTimingGeneration = 0;
    std::uint32_t gpuTimingFrameSlot = 0;
    std::vector<RHI::GpuProfiler::Timing> gpuTimings;
    RHI::GpuProfiler::TimelineMetadata gpuTimelineMetadata;
    std::optional<Renderer::RenderViewDiagnostics> diagnostics;
};

struct RenderFrameExecutionTimings
{
    double beginFrameCpuMs = 0.0;
    double gameRenderCpuMs = 0.0;
    double sceneRenderCpuMs = 0.0;
    double uiDrawCpuMs = 0.0;
    double submitAndPresentCpuMs = 0.0;
    double captureAndResolveCpuMs = 0.0;
    double totalExecutionCpuMs = 0.0;
};

struct RenderFramePipelineDiagnostics
{
    std::size_t waitingDepthAtAcceptance = 0;
    std::size_t peakWaitingDepth = 0;
    double queueWaitCpuMs = 0.0;
    double inputToPresentCpuMs = 0.0;
};

struct RenderDeviceRuntimeFeedback
{
    RHI::GraphicsApi graphicsApi = RHI::GraphicsApi::Direct3D12;
    RHI::PipelineCreationStatistics pipelineCreation;
    RHI::ResourceRetirementStatistics resourceRetirement;
    RHI::UploadQueueStatistics uploadQueue;
    RHI::DescriptorAllocatorStatistics descriptors;
};

struct RenderFrameFeedback
{
    std::uint64_t acceptanceId = 0;
    Scene::LogicalFrameId logicalFrameId;
    Scene::SceneGeneration sceneGeneration;
    Scene::RenderSceneDataRevision dataRevision;
    RenderEpoch sceneEpoch;
    RenderEpoch viewEpoch;
    std::uint64_t settingsRevision = 0;
    RHI::GraphicsApi graphicsApi = RHI::GraphicsApi::Direct3D12;
    std::uint64_t executionThreadId = 0;
    std::uint64_t rejectedVisibilityFeedbackCount = 0;
    RenderFramePipelineDiagnostics pipeline;
    RenderFrameExecutionTimings timings;
    TaskExecutorStatistics taskStatisticsAtFrameStart;
    TaskExecutorStatistics taskStatisticsAtFrameEnd;
    std::optional<RHI::FramePacingStatistics> framePacingStatistics;
    std::optional<RHI::FrameAdmissionResult> frameAdmission;
    std::optional<RHI::FramePacingState> framePacingState;
    std::optional<RenderDeviceRuntimeFeedback> deviceStatistics;
    std::optional<nlohmann::json> renderGraphReport;
    std::optional<nlohmann::json> gpuTimingCaptureMetadata;
    std::vector<RenderViewRuntimeFeedback> views;
    std::uint32_t frameContextIndex = 0;

    [[nodiscard]] const RenderViewRuntimeFeedback* FindView(
        Scene::RenderViewId viewId) const noexcept;
};

struct RenderTargetFrameResult
{
    std::uint64_t captureRequestId = 0;
    bool captureResolved = false;
    std::string captureError;
    RHI::GraphicsApi graphicsApi = RHI::GraphicsApi::Direct3D12;
    std::uint64_t executionThreadId = 0;
    std::uint64_t presentTimestampNanoseconds = 0;
    RenderFrameExecutionTimings timings;
    TaskExecutorStatistics taskStatisticsAtFrameStart;
    TaskExecutorStatistics taskStatisticsAtFrameEnd;
    std::optional<RHI::FramePacingStatistics> framePacingStatistics;
    std::optional<RHI::FrameAdmissionResult> frameAdmission;
    std::optional<RHI::FramePacingState> framePacingState;
    std::optional<RenderDeviceRuntimeFeedback> deviceStatistics;
    std::optional<nlohmann::json> renderGraphReport;
    std::optional<nlohmann::json> gpuTimingCaptureMetadata;
    std::vector<RenderViewRuntimeFeedback> views;
    std::uint32_t frameContextIndex = 0;
};

enum class RenderExecutionMode : std::uint8_t
{
    Inline,
    Threaded
};

[[nodiscard]] RenderExecutionMode ParseRenderExecutionMode(
    std::string_view value);
[[nodiscard]] std::string_view ToString(RenderExecutionMode mode);

class IRenderExecutionTarget
{
public:
    virtual ~IRenderExecutionTarget() = default;
    virtual void Initialize() = 0;
    // Used once when startup resources were initialized through the inline
    // path before the empty execution service is handed to its resident lane.
    virtual void AdoptExecutionLane() {}
    [[nodiscard]] virtual RenderTargetFrameResult ExecuteFrame(
        const FrameEnvelope& frame) = 0;
    [[nodiscard]] virtual RenderControlAcknowledgement ExecuteControl(
        const RenderControlCommand& command) = 0;
    virtual void Shutdown() noexcept = 0;
};

class RenderExecutionService final
{
public:
    RenderExecutionService(
        RenderExecutionMode mode,
        std::unique_ptr<IRenderExecutionTarget> target,
        std::size_t waitingCapacity =
            RenderFrameQueue::DefaultCapacity);
    ~RenderExecutionService();

    RenderExecutionService(const RenderExecutionService&) = delete;
    RenderExecutionService& operator=(
        const RenderExecutionService&) = delete;

    [[nodiscard]] std::uint64_t SubmitFrame(FrameEnvelope frame);
    [[nodiscard]] RenderControlTicket SubmitControl(
        RenderControlCommand command);
    void StartThreadedExecution();
    void WaitUntilCompleted(std::uint64_t acceptanceId);
    [[nodiscard]] std::vector<RenderFrameCompletion>
        ConsumeFrameCompletions();
    [[nodiscard]] std::optional<RenderFrameFeedback>
        ConsumeLatestFrameFeedback();
    void Shutdown() noexcept;
    void RethrowFailure() const;

    [[nodiscard]] RenderExecutionMode GetMode() const noexcept;
    [[nodiscard]] RenderFrameQueueStatistics
        GetQueueStatistics() const noexcept;

private:
    [[nodiscard]] std::uint64_t SubmitPayload(
        RenderExecutionPayload payload);
    void ExecuteEntry(RenderFrameQueueEntry entry);
    void RunInlineEntry();
    void ThreadMain() noexcept;
    void RecordFailure(std::exception_ptr failure) noexcept;
    void ShutdownTarget() noexcept;
    void PublishFrameFeedbackLocked(
        const RenderFrameCompletion& completion,
        RenderFramePipelineDiagnostics pipeline,
        RenderTargetFrameResult result);

    RenderExecutionMode m_mode;
    RenderFrameQueue m_queue;
    std::unique_ptr<IRenderExecutionTarget> m_target;
    std::thread m_thread;
    mutable std::mutex m_stateMutex;
    std::condition_variable m_completion;
    std::mutex m_lifecycleMutex;
    std::mutex m_inlineMutex;
    std::exception_ptr m_failure;
    std::uint64_t m_completedAcceptanceId = 0;
    std::uint64_t m_lastExecutedLogicalFrameId = 0;
    bool m_initializationComplete = false;
    bool m_initializationAttempted = false;
    bool m_adoptInitializedTarget = false;
    bool m_shutdownStarted = false;
    bool m_shutdownComplete = false;
    bool m_executionStopped = false;
    bool m_targetShutdown = false;
    std::deque<RenderFrameCompletion> m_frameCompletions;
    std::optional<RenderFrameFeedback> m_latestFrameFeedback;
    const std::size_t m_frameCompletionCapacity;
};
} // namespace Prism::Core
