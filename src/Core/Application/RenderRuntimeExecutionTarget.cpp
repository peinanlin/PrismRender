#include "Core/Application/RenderRuntimeExecutionTarget.h"

#include "Asset/EnvironmentMapLoader.h"
#include "Asset/GltfLoader.h"
#include "Core/Application/AssetRuntimeCoordinator.h"
#include "Core/Assert.h"
#include "Core/CpuTrace.h"
#include "Core/ProcessDiagnostics.h"
#include "Platform/Window.h"
#include "RHI/IFrameContext.h"
#include "RHI/IGraphicsDevice.h"
#include "RHI/IRenderBackend.h"
#include "RHI/RenderBackendFactory.h"
#include "Renderer/RenderFrameCoordinator.h"
#include "Renderer/RenderGraphDiagnostics.h"
#include "Renderer/SceneRenderer.h"
#include "Renderer/Features/Ocean/WaterBenchmarkReport.h"
#include "Scene/DemoSceneCatalog.h"
#include "Scene/RenderScene.h"
#include "Scene/SceneLoader.h"
#include "Scene/SceneSession.h"

#include <stdexcept>
#include <algorithm>
#include <chrono>
#include <type_traits>
#include <utility>

namespace
{
Prism::RHI::GpuProfiler::SamplingMode ToGpuSamplingMode(
    const Prism::Core::ProfilingLevel level) noexcept
{
    switch (level)
    {
    case Prism::Core::ProfilingLevel::Off:
        return Prism::RHI::GpuProfiler::SamplingMode::Off;
    case Prism::Core::ProfilingLevel::Basic:
        return Prism::RHI::GpuProfiler::SamplingMode::Frame;
    case Prism::Core::ProfilingLevel::Detailed:
    case Prism::Core::ProfilingLevel::Capture:
        return Prism::RHI::GpuProfiler::SamplingMode::Detailed;
    }
    return Prism::RHI::GpuProfiler::SamplingMode::Frame;
}
} // namespace

namespace Prism::Core
{
RenderRuntimeExecutionTarget::RenderRuntimeExecutionTarget(
    Platform::Window& window,
    const RHI::GraphicsApi graphicsApi,
    const bool sceneViewEnabled,
    const TaskExecutorKind taskExecutorKind,
    RHI::FramePacingConfiguration framePacing,
    AssetRuntimeCoordinator& assetRuntimeCoordinator)
    : m_window(window),
      m_assetRuntimeCoordinator(assetRuntimeCoordinator),
      m_graphicsApi(graphicsApi),
      m_framePacingConfiguration(std::move(framePacing)),
      m_sceneViewEnabled(sceneViewEnabled),
      m_taskExecutorKind(taskExecutorKind)
{
    RHI::ValidateFramePacingConfiguration(
        m_framePacingConfiguration);
    m_frameRateLimiter.Configure(
        m_framePacingConfiguration.targetFps);
}

RenderRuntimeExecutionTarget::~RenderRuntimeExecutionTarget()
{
    Shutdown();
}

void RenderRuntimeExecutionTarget::Initialize()
{
    Check(!m_initializationAttempted,
        "Render runtime initialization may only be attempted once.");
    m_initializationAttempted = true;
    m_executionThread = std::this_thread::get_id();

    try
    {
        m_backend = RHI::CreateRenderBackend(
            m_graphicsApi, m_framePacingConfiguration);
        Check(m_backend != nullptr,
            "Render backend factory returned no backend.");
        m_frameCoordinator =
            std::make_unique<Renderer::RenderFrameCoordinator>(
                m_sceneViewEnabled,
                m_taskExecutorKind);
        m_backend->Initialize(m_window);
        m_backendInitialized = true;
        const RHI::FramePacingState& pacing =
            m_backend->GetFramePacingState();
        ProcessDiagnostics::Log(
            "info",
            "renderer.frame_pacing.effective",
            std::string("Effective frame pacing: profile=")
                + std::string(RHI::ToString(pacing.requested.profile))
                + " requested="
                + std::string(RHI::ToString(
                    pacing.requested.presentation))
                + " effective="
                + std::string(RHI::ToString(
                    pacing.effectivePresentation))
                + " native=" + pacing.nativePresentMode
                + " queue="
                + std::to_string(pacing.effectiveMaxQueuedFrames));
    }
    catch (...)
    {
        Shutdown();
        throw;
    }
}

void RenderRuntimeExecutionTarget::AdoptExecutionLane()
{
    Check(m_initializationAttempted && m_backendInitialized
            && m_backend != nullptr && m_frameCoordinator != nullptr
            && !m_shutdown,
        "Only an initialized render runtime can adopt an execution lane.");
    m_executionThread = std::this_thread::get_id();
}

RenderTargetFrameResult RenderRuntimeExecutionTarget::ExecuteFrame(
    const FrameEnvelope& frame)
{
    using ExecutionClock = std::chrono::steady_clock;
    const auto executionStart = ExecutionClock::now();
    Core::CpuTraceSpan executionSpan(
        "RenderFrameExecution",
        "render-lane");
    RenderFrameExecutionTimings timings{};
    const auto measure = [](const ExecutionClock::time_point start)
    {
        return std::chrono::duration<double, std::milli>(
            ExecutionClock::now() - start).count();
    };
    RequireExecutionLane();
    if (!m_renderersInitialized)
    {
        throw std::logic_error(
            "Render runtime cannot execute before renderer initialization.");
    }
    const TaskExecutorStatistics taskStatisticsAtFrameStart =
        m_frameCoordinator->GetTaskExecutorStatistics();
    m_frameCoordinator->SetProfilingMode(
        ToGpuSamplingMode(frame.profilingLevel));
    m_backend->GetFrameContext().SetFramePacingEnabled(
        frame.framePacingEnabled);

    if (m_sceneEpoch && frame.sceneEpoch != m_sceneEpoch)
    {
        throw std::invalid_argument(
            "Render frame scene epoch does not match reliable controls.");
    }
    if (m_viewEpoch && frame.viewEpoch != m_viewEpoch)
    {
        throw std::invalid_argument(
            "Render frame view epoch does not match reliable controls.");
    }
    if (m_settingsRevision != 0
        && frame.settingsRevision != m_settingsRevision)
    {
        throw std::invalid_argument(
            "Render frame settings revision does not match reliable controls.");
    }
    m_sceneEpoch = frame.sceneEpoch;
    m_viewEpoch = frame.viewEpoch;
    m_settingsRevision = frame.settingsRevision;

    PreparedRenderFrame prepared =
        m_frameExecutionState.Prepare(frame);
    Renderer::SceneRenderer& gameRenderer =
        m_frameCoordinator->GetGameRenderer();
    Renderer::SceneRenderer* const sceneRenderer =
        m_frameCoordinator->GetSceneRenderer();
    bool gameActive = false;
    bool sceneActive = false;
    for (const RenderViewFrameState& view : prepared.views)
    {
        Renderer::SceneRenderer* renderer = nullptr;
        if (view.viewId == Scene::GameRenderViewId)
        {
            renderer = &gameRenderer;
            gameActive = view.active;
        }
        else if (view.viewId == Scene::SceneRenderViewId)
        {
            renderer = sceneRenderer;
            sceneActive = view.active && sceneRenderer != nullptr;
        }
        if (renderer == nullptr)
        {
            continue;
        }
        renderer->GetSettings() = view.settings;
        const Scene::RenderView* const frozenView =
            prepared.frame->FindView(view.viewId);
        if (view.active && frozenView != nullptr
            && frozenView->historyInvalidation
                != Scene::RenderViewHistoryInvalidation::None)
        {
            renderer->RequestHistoryReset();
        }
    }

    const auto beginFrameStart = ExecutionClock::now();
    RHI::FrameResult beginResult = BeginRenderFrame();
    if (beginResult == RHI::FrameResult::SwapChainOutOfDate)
    {
        const Scene::RenderView* const gameView =
            prepared.frame->FindView(Scene::GameRenderViewId);
        if (gameView == nullptr)
        {
            m_frameExecutionState.Cancel(prepared);
            throw std::runtime_error(
                "Swap-chain recovery requires the frozen Game view extent.");
        }
        ResizeSwapChain(gameView->width, gameView->height);
        beginResult = BeginRenderFrame();
        if (beginResult != RHI::FrameResult::Ready)
        {
            m_frameExecutionState.Cancel(prepared);
            throw std::runtime_error(
                "Swap chain remained out of date after frame-bound resize.");
        }
    }
    timings.beginFrameCpuMs = measure(beginFrameStart);

    const Scene::LogicalFrameId logicalFrameId =
        prepared.frame->GetLogicalFrameId();
    (void)ProcessAssetRenderWork(logicalFrameId.value);
    m_frameCoordinator->BeginLogicalFrame(
        logicalFrameId.value,
        m_backend->GetFrameContext().GetCurrentFrameIndex(),
        gameActive,
        sceneActive);
    const Renderer::RenderFrameViewPlan plan =
        m_frameCoordinator->BuildViewPlan(sceneActive);
    bool gameRendered = false;
    bool sceneRendered = false;
    for (std::size_t index = 0; index < plan.count; ++index)
    {
        if (plan.order[index] == Renderer::RenderViewKind::Game)
        {
            const auto gameRenderStart = ExecutionClock::now();
            gameRenderer.Render(
                *m_backend,
                prepared.frame,
                Scene::GameRenderViewId);
            timings.gameRenderCpuMs += measure(gameRenderStart);
            gameRendered = true;
        }
        else if (sceneRenderer != nullptr)
        {
            const auto sceneRenderStart = ExecutionClock::now();
            std::shared_ptr<const Scene::RenderViewFeedback>
                visibilityFeedback =
                    m_completedGameVisibilityFeedback;
            if (visibilityFeedback != nullptr
                && !visibilityFeedback->Matches(
                    *prepared.frame,
                    Scene::GameRenderViewId))
            {
                visibilityFeedback.reset();
            }
            sceneRenderer->Render(
                *m_backend,
                prepared.frame,
                Scene::SceneRenderViewId,
                std::move(visibilityFeedback));
            timings.sceneRenderCpuMs += measure(sceneRenderStart);
            sceneRendered = true;
        }
    }

    RecordFrameCapture(prepared, gameRendered, sceneRendered);
    if (prepared.uiDrawPacket != nullptr)
    {
        const auto uiDrawStart = ExecutionClock::now();
        if (!m_uiDraw)
        {
            m_frameExecutionState.Cancel(prepared);
            throw std::logic_error(
                "Render frame contains UI draw data without an execution-lane UI callback.");
        }
        m_uiDraw(*prepared.uiDrawPacket);
        timings.uiDrawCpuMs = measure(uiDrawStart);
    }

    const auto presentStart = ExecutionClock::now();
    const RHI::FrameResult presentResult = EndRenderFrame();
    const std::uint64_t presentTimestampNanoseconds =
        static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                ExecutionClock::now().time_since_epoch()).count());
    m_frameExecutionState.Commit(prepared);
    if (presentResult == RHI::FrameResult::SwapChainOutOfDate)
    {
        const Scene::RenderView* const gameView =
            prepared.frame->FindView(Scene::GameRenderViewId);
        if (gameView != nullptr)
        {
            ResizeSwapChain(gameView->width, gameView->height);
        }
    }
    timings.submitAndPresentCpuMs = measure(presentStart);

    const auto resolveStart = ExecutionClock::now();
    RenderTargetFrameResult result =
        CollectFrameResult(gameRendered, sceneRendered, frame);
    result.graphicsApi = m_graphicsApi;
    result.executionThreadId = static_cast<std::uint64_t>(
        std::hash<std::thread::id>{}(
            std::this_thread::get_id()));
    if (result.executionThreadId == 0)
    {
        result.executionThreadId = 1;
    }
    result.presentTimestampNanoseconds =
        presentTimestampNanoseconds;
    timings.captureAndResolveCpuMs = measure(resolveStart);
    timings.totalExecutionCpuMs = measure(executionStart);
    result.timings = timings;
    result.taskStatisticsAtFrameStart =
        taskStatisticsAtFrameStart;
    result.taskStatisticsAtFrameEnd =
        m_frameCoordinator->GetTaskExecutorStatistics();
    result.frameAdmission = frame.frameAdmission;
    result.framePacingState = m_backend->GetFramePacingState();
    if (const RHI::FramePacingStatistics* const pacing =
            m_backend->GetFrameContext().GetFramePacingStatistics();
        pacing != nullptr)
    {
        result.framePacingStatistics = *pacing;
    }
    // Prime the next display/queue token after Present. This keeps the wait
    // inside the backend-owned lane while allowing render work to consume the
    // current display interval instead of paying a full refresh period after
    // the work has already completed.
    m_prefetchedFrameAdmission =
        m_backend->WaitForFrameAdmission();
    return result;
}

RenderControlAcknowledgement
RenderRuntimeExecutionTarget::ExecuteControl(
    const RenderControlCommand& command)
{
    RequireExecutionLane();
    if (!command.id)
    {
        throw std::invalid_argument(
            "Render runtime control requires a non-zero command ID.");
    }

    bool succeeded = true;
    bool gpuDrained = false;
    bool assetStreamingReportWritten = false;
    bool sceneChanged = false;
    std::optional<RHI::FrameAdmissionResult> frameAdmission;
    std::optional<RHI::FramePacingState> framePacingState;
    std::string framePacingError;
    std::optional<Asset::AssetImportResult> assetImportResult;
    AssetRuntimeRenderWorkResult assetWork{};
    std::visit([this, &succeeded, &gpuDrained, &assetWork,
                   &assetStreamingReportWritten, &sceneChanged,
                   &assetImportResult, &frameAdmission,
                   &framePacingState, &framePacingError](
        const auto& payload)
    {
        using Payload = std::decay_t<decltype(payload)>;
        if constexpr (std::is_same_v<Payload, ResizeRenderViewCommand>)
        {
            m_prefetchedFrameAdmission.reset();
            ResizeSwapChain(payload.width, payload.height);
        }
        else if constexpr (
            std::is_same_v<Payload, ResetRenderHistoryCommand>)
        {
            const std::uint64_t view = payload.viewId.value;
            if (view == Scene::GameRenderViewId.value)
            {
                m_frameExecutionState.InvalidateView(
                    payload.viewId,
                    Scene::RenderViewHistoryInvalidation::CameraCut);
            }
            else if (view == Scene::SceneRenderViewId.value)
            {
                m_frameExecutionState.InvalidateView(
                    payload.viewId,
                    Scene::RenderViewHistoryInvalidation::CameraCut);
            }
            else
            {
                succeeded = false;
            }
        }
        else if constexpr (
            std::is_same_v<Payload, ChangeRenderQualityCommand>)
        {
            if (payload.settingsRevision <= m_settingsRevision)
            {
                succeeded = false;
            }
            else
            {
                m_settingsRevision = payload.settingsRevision;
                m_frameExecutionState.InvalidateView(
                    Scene::GameRenderViewId,
                    Scene::RenderViewHistoryInvalidation::Settings);
                m_frameExecutionState.InvalidateView(
                    Scene::SceneRenderViewId,
                    Scene::RenderViewHistoryInvalidation::Settings);
            }
        }
        else if constexpr (
            std::is_same_v<Payload, SwitchRenderSceneCommand>)
        {
            if (payload.demoScene.has_value())
            {
                Check(m_sceneSession != nullptr,
                    "Demo scene switch requires a configured SceneSession.");
                sceneChanged = ActivateDemoScene(
                    *m_sceneSession,
                    *payload.demoScene,
                    payload.aspectRatio).changed;
            }
            m_sceneEpoch = payload.nextSceneEpoch;
            m_frameExecutionState.InvalidateView(
                Scene::GameRenderViewId,
                Scene::RenderViewHistoryInvalidation::Scene);
            m_frameExecutionState.InvalidateView(
                Scene::SceneRenderViewId,
                Scene::RenderViewHistoryInvalidation::Scene);
        }
        else if constexpr (
            std::is_same_v<Payload, CaptureRenderFrameCommand>)
        {
            succeeded = RequestFrameCapture(
                payload.requestId,
                payload.outputPath);
        }
        else if constexpr (
            std::is_same_v<Payload, AdmitRenderFrameCommand>)
        {
            if (m_prefetchedFrameAdmission.has_value())
            {
                frameAdmission = *m_prefetchedFrameAdmission;
                m_prefetchedFrameAdmission.reset();
            }
            else
            {
                frameAdmission = m_backend->WaitForFrameAdmission();
            }
            frameAdmission->limiterWaitMilliseconds =
                m_frameRateLimiter.Wait();
            frameAdmission->configurationGeneration =
                m_backend->GetFramePacingState()
                    .effectiveGeneration;
            framePacingState = m_backend->GetFramePacingState();
        }
        else if constexpr (
            std::is_same_v<Payload, ChangeFramePacingCommand>)
        {
            m_prefetchedFrameAdmission.reset();
            succeeded = m_backend->ApplyFramePacingConfiguration(
                payload.configuration,
                payload.generation,
                &framePacingError);
            if (succeeded)
            {
                m_framePacingConfiguration = payload.configuration;
                m_frameRateLimiter.Configure(
                    m_framePacingConfiguration.targetFps);
            }
            framePacingState = m_backend->GetFramePacingState();
        }
        else if constexpr (
            std::is_same_v<Payload, DrainRenderExecutionCommand>)
        {
            WaitForGpu();
            gpuDrained = true;
        }
        else if constexpr (
            std::is_same_v<Payload, ProcessAssetRenderWorkCommand>)
        {
            assetWork = ProcessAssetRenderWork(
                payload.logicalFrameId);
            if (assetWork.uploadedCount > 0)
            {
                m_gpuDrainedForShutdown = false;
            }
            gpuDrained = m_gpuDrainedForShutdown;
        }
        else if constexpr (
            std::is_same_v<Payload,
                WriteAssetStreamingReportCommand>)
        {
            std::string error;
            assetStreamingReportWritten =
                m_assetRuntimeCoordinator.WriteStreamingReport(
                    payload.outputPath,
                    m_backend->GetGraphicsDevice(),
                    &error);
            if (!assetStreamingReportWritten)
            {
                throw std::runtime_error(
                    "Asset streaming report failed: " + error);
            }
        }
        else if constexpr (
            std::is_same_v<Payload, ImportRenderAssetCommand>)
        {
            assetImportResult = ImportAndReload(
                payload.sourcePath,
                payload.reimport);
        }
        else if constexpr (
            std::is_same_v<Payload, StopRenderExecutionCommand>)
        {
            m_stopRequested = true;
        }
    }, command.payload);
    m_sceneEpoch = m_sceneEpoch
        ? m_sceneEpoch
        : command.sceneEpoch;
    m_viewEpoch = command.viewEpoch;
    return {
        command.id,
        command.targetFrame,
        command.sceneEpoch,
        command.viewEpoch,
        succeeded,
        gpuDrained,
        assetWork.uploadedCount,
        assetWork.evictedCount,
        assetWork.bindingUpdateCount,
        assetStreamingReportWritten,
        sceneChanged,
        std::move(assetImportResult),
        frameAdmission,
        framePacingState,
        std::move(framePacingError)};
}

void RenderRuntimeExecutionTarget::Shutdown() noexcept
{
    if (m_shutdown)
    {
        return;
    }
    m_shutdown = true;

    if (m_backend != nullptr
        && m_backendInitialized
        && !m_gpuDrainedForShutdown)
    {
        try
        {
            m_backend->GetFrameContext().WaitForGpu();
        }
        catch (...)
        {
        }
    }
    if (m_uiShutdown)
    {
        try
        {
            m_uiShutdown();
        }
        catch (...)
        {
        }
    }
    m_uiDraw = {};
    m_uiRefresh = {};
    m_uiShutdown = {};
    if (m_frameCoordinator != nullptr)
    {
        m_frameCoordinator->Shutdown();
    }
    m_frameCoordinator.reset();
    if (m_sceneSession != nullptr)
    {
        m_sceneSession->ReleaseRuntimeRenderResources();
    }
    // Asset registry resources retain their creating backend context. Release
    // them on the render lane after the GPU/renderers are drained, but before
    // destroying that backend context. ApplicationHost's later Shutdown call
    // is intentionally idempotent.
    m_assetRuntimeCoordinator.Shutdown();
    m_backend.reset();
    m_backendInitialized = false;
}

RHI::IRenderBackend& RenderRuntimeExecutionTarget::GetBackend()
{
    RequireExecutionLane();
    Check(m_backendInitialized && m_backend != nullptr,
        "Render backend is not initialized.");
    return *m_backend;
}

Renderer::RenderFrameCoordinator&
RenderRuntimeExecutionTarget::GetFrameCoordinator()
{
    RequireExecutionLane();
    Check(m_frameCoordinator != nullptr,
        "Render frame coordinator is not initialized.");
    return *m_frameCoordinator;
}

bool RenderRuntimeExecutionTarget::IsOnExecutionLane() const noexcept
{
    return m_initializationAttempted
        && std::this_thread::get_id() == m_executionThread;
}

bool RenderRuntimeExecutionTarget::IsBackendInitialized() const noexcept
{
    return m_backendInitialized;
}

void RenderRuntimeExecutionTarget::ConfigureUiExecutionCallbacks(
    std::function<void(const UI::UiDrawPacket&)> draw,
    std::function<void(
        Renderer::SceneRenderer&,
        Renderer::SceneRenderer&)> refresh,
    std::function<void()> shutdown)
{
    RequireExecutionLane();
    Check(!m_uiDraw && !m_uiRefresh && !m_uiShutdown,
        "Render runtime UI execution callbacks may only be configured once.");
    Check(static_cast<bool>(draw)
            && static_cast<bool>(refresh)
            && static_cast<bool>(shutdown),
        "Render runtime requires complete UI execution callbacks.");
    m_uiDraw = std::move(draw);
    m_uiRefresh = std::move(refresh);
    m_uiShutdown = std::move(shutdown);
}

void RenderRuntimeExecutionTarget::ConfigureSceneSession(
    Scene::SceneSession& session) noexcept
{
    m_sceneSession = &session;
}

void RenderRuntimeExecutionTarget::InitializeRenderers(
    const std::filesystem::path& shaderPath,
    const Scene::RenderScene& scene,
    std::shared_ptr<Asset::Texture> environmentCubemap)
{
    RequireExecutionLane();
    Check(!m_renderersInitialized,
        "Renderers may only be initialized once.");
    m_frameCoordinator->Initialize(
        *m_backend,
        shaderPath,
        scene,
        std::move(environmentCubemap));
    m_renderersInitialized = true;
}

RHI::FrameResult RenderRuntimeExecutionTarget::BeginRenderFrame()
{
    RequireExecutionLane();
    m_gpuDrainedForShutdown = false;
    return m_backend->GetFrameContext().BeginFrame();
}

RHI::FrameResult RenderRuntimeExecutionTarget::EndRenderFrame()
{
    RequireExecutionLane();
    m_gpuDrainedForShutdown = false;
    return m_backend->GetFrameContext().EndFrame();
}

bool RenderRuntimeExecutionTarget::RequestFrameCapture(
    const std::uint64_t requestId,
    const std::filesystem::path& outputPath)
{
    RequireExecutionLane();
    if (requestId == 0 || outputPath.empty()
        || requestId <= m_captureRequestId
        || m_activeCaptureRequestId != 0)
    {
        return false;
    }
    m_captureRequestId = requestId;
    m_activeCaptureRequestId = requestId;
    m_backend->RequestTextureCapture(outputPath);
    return true;
}

RenderTargetFrameResult
RenderRuntimeExecutionTarget::ResolveFrameCapture()
{
    RequireExecutionLane();
    RenderTargetFrameResult result{};
    if (m_activeCaptureRequestId == 0)
    {
        return result;
    }
    result.captureRequestId = m_activeCaptureRequestId;
    result.captureResolved =
        m_backend->ResolveTextureCapture(&result.captureError);
    if (result.captureResolved || !result.captureError.empty())
    {
        m_activeCaptureRequestId = 0;
    }
    return result;
}

RenderTargetFrameResult
RenderRuntimeExecutionTarget::CollectFrameResult(
    const bool gameRendered,
    const bool sceneRendered,
    const FrameEnvelope& frame)
{
    RequireExecutionLane();
    RenderTargetFrameResult result = ResolveFrameCapture();
    result.frameContextIndex =
        m_backend->GetFrameContext().GetCurrentFrameIndex();
    const auto waterOpticsScheduled = [](const auto& renderer)
    {
        return std::ranges::any_of(
            renderer.GetRenderGraph().GetPassInfos(),
            [](const auto& pass)
            {
                return pass.name.starts_with("WaterOptics.");
            });
    };
    if (gameRendered)
    {
        Renderer::SceneRenderer& gameRenderer =
            m_frameCoordinator->GetGameRenderer();
        if (frame.collectGpuTimingReport)
        {
            gameRenderer.ResolveGpuTimings(*m_backend);
            result.gpuTimingCaptureMetadata =
                Renderer::BuildWaterBenchmarkMetadata(
                    gameRenderer.GetSettings().ocean,
                    gameRenderer.GetStatistics().ocean,
                    gameRenderer.GetRenderGraph());
        }
        if (frame.collectRenderGraphReport)
        {
            const RHI::IGraphicsDevice& device =
                m_backend->GetGraphicsDevice();
            const RHI::DescriptorAllocatorStatistics descriptor =
                device.GetDescriptorAllocatorStatistics();
            const RHI::UploadQueueStatistics upload =
                device.GetUploadQueueStatistics();
            const RHI::ResourceRetirementStatistics retirement =
                device.GetResourceRetirementStatistics();
            result.renderGraphReport =
                Renderer::BuildRenderGraphReport(
                    gameRenderer.GetRenderGraph(),
                    m_graphicsApi,
                    &device.GetCapabilities(),
                    &descriptor,
                    &upload,
                    &retirement);
        }
        RenderViewRuntimeFeedback feedback{
            Scene::GameRenderViewId,
            gameRenderer.GetStatistics(),
            gameRenderer.GetGpuVisibilityFeedback(),
            waterOpticsScheduled(gameRenderer)};
        feedback.gpuTimingGeneration =
            gameRenderer.GetGpuTimingGeneration();
        feedback.gpuTimingFrameSlot =
            gameRenderer.GetGpuTimingFrameSlot();
        feedback.gpuTimings = gameRenderer.GetGpuTimings();
        feedback.gpuTimelineMetadata =
            gameRenderer.GetGpuTimelineMetadata();
        if (gameRenderer.IsFrameDiagnosticsEnabled())
        {
            feedback.diagnostics =
                gameRenderer.BuildFrameDiagnostics(m_graphicsApi);
        }
        result.views.push_back(std::move(feedback));
        m_completedGameVisibilityFeedback =
            gameRenderer.GetGpuVisibilityFeedback();
    }
    if (frame.collectPerformanceStatistics)
    {
        const RHI::IGraphicsDevice& device =
            m_backend->GetGraphicsDevice();
        result.deviceStatistics = RenderDeviceRuntimeFeedback{
            m_graphicsApi,
            device.GetPipelineCreationStatistics(),
            device.GetResourceRetirementStatistics(),
            device.GetUploadQueueStatistics(),
            device.GetDescriptorAllocatorStatistics()};
    }
    if (sceneRendered)
    {
        if (Renderer::SceneRenderer* const sceneRenderer =
                m_frameCoordinator->GetSceneRenderer();
            sceneRenderer != nullptr)
        {
            RenderViewRuntimeFeedback feedback{
                Scene::SceneRenderViewId,
                sceneRenderer->GetStatistics(),
                sceneRenderer->GetGpuVisibilityFeedback(),
                waterOpticsScheduled(*sceneRenderer)};
            feedback.gpuTimingGeneration =
                sceneRenderer->GetGpuTimingGeneration();
            feedback.gpuTimingFrameSlot =
                sceneRenderer->GetGpuTimingFrameSlot();
            feedback.gpuTimings = sceneRenderer->GetGpuTimings();
            feedback.gpuTimelineMetadata =
                sceneRenderer->GetGpuTimelineMetadata();
            if (sceneRenderer->IsFrameDiagnosticsEnabled())
            {
                feedback.diagnostics =
                    sceneRenderer->BuildFrameDiagnostics(m_graphicsApi);
            }
            result.views.push_back(std::move(feedback));
        }
    }
    return result;
}

void RenderRuntimeExecutionTarget::ResizeSwapChain(
    const std::uint32_t width,
    const std::uint32_t height)
{
    RequireExecutionLane();
    Check(width > 0 && height > 0,
        "Render runtime resize requires a non-zero extent.");
    m_backend->GetFrameContext().Resize(width, height);
    m_frameCoordinator->RecreateSwapChainResources(*m_backend);
    if (m_uiRefresh)
    {
        Renderer::SceneRenderer* const sceneRenderer =
            m_frameCoordinator->GetSceneRenderer();
        Check(sceneRenderer != nullptr,
            "UI viewport refresh requires the Scene renderer.");
        m_uiRefresh(
            m_frameCoordinator->GetGameRenderer(),
            *sceneRenderer);
    }
    m_frameExecutionState.InvalidateView(
        Scene::GameRenderViewId,
        Scene::RenderViewHistoryInvalidation::Extent);
    m_frameExecutionState.InvalidateView(
        Scene::SceneRenderViewId,
        Scene::RenderViewHistoryInvalidation::Extent);
}

void RenderRuntimeExecutionTarget::WaitForGpu()
{
    RequireExecutionLane();
    m_backend->GetFrameContext().WaitForGpu();
    m_gpuDrainedForShutdown = true;
}

AssetRuntimeRenderWorkResult
RenderRuntimeExecutionTarget::ProcessAssetRenderWork(
    const std::uint64_t logicalFrameId)
{
    RequireExecutionLane();
    return m_assetRuntimeCoordinator.ProcessStreamingRenderWork(
        m_backend->GetGraphicsDevice(),
        logicalFrameId);
}

Asset::AssetRuntimeLoadResult
RenderRuntimeExecutionTarget::LoadRuntimeAssets()
{
    RequireExecutionLane();
    return m_assetRuntimeCoordinator.LoadRuntimeAssets(
        m_backend->GetGraphicsDevice());
}

Asset::AssetImportResult RenderRuntimeExecutionTarget::ImportAndReload(
    const std::filesystem::path& sourcePath,
    const bool reimport)
{
    RequireExecutionLane();
    return m_assetRuntimeCoordinator.ImportAndReload(
        sourcePath,
        reimport,
        m_backend->GetGraphicsDevice(),
        [this]
        {
            WaitForGpu();
        });
}

std::shared_ptr<Asset::Texture>
RenderRuntimeExecutionTarget::LoadEnvironmentCubemap(
    const std::filesystem::path& directory,
    std::string* const outStatus)
{
    RequireExecutionLane();
    Asset::EnvironmentMapLoader loader;
    return loader.LoadCubemapFromDirectory(
        m_backend->GetGraphicsDevice(),
        directory.string(),
        outStatus);
}

Scene::DemoSceneBuildResult
RenderRuntimeExecutionTarget::PopulateDemoScene(
    const Scene::DemoSceneId sceneId,
    Scene::RenderScene& scene,
    const float aspectRatio)
{
    RequireExecutionLane();
    return Scene::DemoSceneCatalog::Populate(
        sceneId,
        m_assetRuntimeCoordinator.GetRegistry(),
        m_backend->GetGraphicsDevice(),
        scene,
        aspectRatio);
}

bool RenderRuntimeExecutionTarget::LoadStartupSceneFromGltf(
    const std::filesystem::path& path,
    Scene::RenderScene& scene,
    std::string* const outError)
{
    RequireExecutionLane();
    Scene::SceneLoader loader;
    return loader.LoadFromGltf(
        path.string(),
        m_backend->GetGraphicsDevice(),
        m_assetRuntimeCoordinator.GetRegistry(),
        scene,
        outError,
        false,
        {2.8f, 0.0f, 1.5f});
}

Scene::SceneSessionDemoActivationResult
RenderRuntimeExecutionTarget::ActivateDemoScene(
    Scene::SceneSession& session,
    const Scene::DemoSceneId sceneId,
    const float aspectRatio)
{
    RequireExecutionLane();
    WaitForGpu();
    Scene::SceneSessionDemoActivationResult result =
        session.ActivateDemoScene(
            sceneId,
            m_backend->GetGraphicsDevice(),
            aspectRatio);
    if (result.changed)
    {
        m_frameCoordinator->NotifySceneChanged(
            session.GetRenderScene());
    }
    return result;
}

void RenderRuntimeExecutionTarget::RecordFrameCapture(
    const PreparedRenderFrame& prepared,
    const bool gameRendered,
    const bool sceneRendered)
{
    RequireExecutionLane();
    if (m_activeCaptureRequestId == 0
        || !prepared.captureViewId.has_value())
    {
        return;
    }

    Renderer::SceneRenderer* renderer = nullptr;
    bool rendered = false;
    if (*prepared.captureViewId == Scene::GameRenderViewId)
    {
        renderer = &m_frameCoordinator->GetGameRenderer();
        rendered = gameRendered;
    }
    else if (*prepared.captureViewId == Scene::SceneRenderViewId)
    {
        renderer = m_frameCoordinator->GetSceneRenderer();
        rendered = sceneRendered;
    }
    Check(renderer != nullptr && rendered,
        "Capture view was not rendered by the accepted frame.");

    const std::shared_ptr<RHI::ITexture>& texture =
        renderer->GetFinalOutputTexture();
    if (texture != nullptr)
    {
        m_backend->RecordTextureCapture(texture.get());
    }
    else
    {
        Check(!m_sceneViewEnabled
                && m_graphicsApi == RHI::GraphicsApi::Vulkan
                && *prepared.captureViewId == Scene::GameRenderViewId,
            "Capture view did not publish a final output texture.");
        // The no-Editor Vulkan backend captures the swap chain in EndFrame.
    }
}

void RenderRuntimeExecutionTarget::RequireExecutionLane() const
{
    Check(IsOnExecutionLane(),
        "Render runtime access crossed execution-lane affinity.");
    Check(!m_shutdown,
        "Render runtime access occurred after shutdown started.");
}
} // namespace Prism::Core
