#include "Core/ApplicationHost.h"
#include "Core/Application/FrameCaptureSequence.h"
#include "Core/Application/FramePacingAutomation.h"
#include "Core/Application/FramePerformanceRecorder.h"
#include "Core/Application/FramePerformanceSampling.h"
#include "Core/Application/PerformanceActiveViews.h"
#include "Core/Application/PerformanceWindowPosition.h"
#include "Core/Application/PerformanceWindowFocus.h"
#include "Core/Application/AssetRuntimeCoordinator.h"
#include "Core/Application/CaptureAutomationController.h"
#include "Core/Application/WaterValidationSequence.h"
#include "Core/Application/RenderRuntimeExecutionTarget.h"

#include "Asset/AssetRegistry.h"
#include "Asset/Mesh.h"
#include "Core/Assert.h"
#include "Core/CpuTrace.h"
#include "Core/Environment.h"
#include "Core/Threading/TaskExecutorFactory.h"
#include "Core/FrameTimer.h"
#include "Core/Profiling/FrameProfiler.h"
#include "Core/Profiling/FrameProfilerReport.h"
#include "Core/Profiling/ProfilingLevelSchedule.h"
#include "Core/Profiling/SceneViewRefreshController.h"
#include "Core/ProcessDiagnostics.h"
#include "Engine/CommandSystem.h"
#include "Platform/Window.h"
#include "RHI/IRenderBackend.h"
#include "RHI/FramePacing.h"
#include "RHI/IGraphicsDevice.h"
#include "RHI/IFrameContext.h"
#include "RHI/RenderBackendFactory.h"
#include "RHI/RayTracingValidation.h"
#include "RHI/GraphicsApi.h"
#include "RHI/TransientResources.h"
#include "Renderer/RendererStatistics.h"
#include "Renderer/DemoSceneSettings.h"
#include "Renderer/GpuTimingReport.h"
#include "Renderer/FrameDiagnostics.h"
#include "Renderer/Features/Ocean/WaterBenchmarkReport.h"
#include "Renderer/PerformanceIdentity.h"
#include "Renderer/SceneRenderer.h"
#include "Renderer/SceneRendererSharedResources.h"
#include "Renderer/RenderCapture.h"
#include "Renderer/RenderFrameCoordinator.h"
#include "Renderer/RenderGraphDiagnostics.h"
#include "Renderer/RenderHistorySettingsTracker.h"
#include "Scene/CameraController.h"
#include "Scene/AssetStreamingSceneBridge.h"
#include "Scene/DemoSceneCatalog.h"
#include "Scene/RenderScene.h"
#include "Scene/RenderSceneExtractionStatistics.h"
#include "Scene/RenderSceneMailbox.h"
#include "Scene/SceneSession.h"
#include "Scene/SceneFraming.h"
#include "Scene/SceneSerializer.h"
#include "Scene/Transform.h"
#include "Scene/WorldRenderSceneBridge.h"
#include "Scene/WorldRenderSnapshot.h"
#if defined(PRISM_RENDER_HAS_EDITOR)
#include "UI/EditorCoordinator.h"
#include "UI/UiDrawPacket.h"
#endif

#include <DirectXMath.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <utility>
#include <vector>

namespace Prism::Core
{
using namespace DirectX;

namespace
{
#if defined(PRISM_RENDER_HAS_TINYGLTF)
constexpr bool GltfImportCompiledIn = true;
#else
constexpr bool GltfImportCompiledIn = false;
#endif

// A long render or swap-chain wait must not turn one navigation update into a
// large camera jump. Keep this scoped to the WaveWorks presentation Lab; the
// navigation behavior of every other scene remains unchanged.
constexpr float WaveWorksMaximumNavigationDeltaSeconds = 1.0f / 30.0f;

using FrameProfileClock = std::chrono::steady_clock;

template <typename Enum>
constexpr std::size_t ProfileIndex(const Enum value) noexcept
{
    return static_cast<std::size_t>(value);
}

void AddProfileElapsed(
    FrameProfileDuration& duration,
    const FrameProfileClock::time_point start)
{
    duration.milliseconds +=
        std::chrono::duration<double, std::milli>(
            FrameProfileClock::now() - start).count();
    duration.available = true;
}

void AddProfileMilliseconds(
    FrameProfileDuration& duration,
    const double milliseconds)
{
    duration.milliseconds += milliseconds;
    duration.available = true;
}

void RecordFramePacing(
    FrameProfilerSnapshot& snapshot,
    const RHI::FramePacingStatistics& pacing)
{
    using RhiPhase = RHI::FramePacingStatistics::Phase;
    constexpr std::array phaseMapping{
        std::pair{RhiPhase::FrameFence, FrameWaitPhase::FrameFence},
        std::pair{RhiPhase::Reclaim, FrameWaitPhase::Reclaim},
        std::pair{RhiPhase::Acquire, FrameWaitPhase::Acquire},
        std::pair{RhiPhase::ImageFence, FrameWaitPhase::ImageFence},
        std::pair{RhiPhase::Upload, FrameWaitPhase::Upload},
        std::pair{RhiPhase::Prepare, FrameWaitPhase::Prepare},
        std::pair{RhiPhase::Submit, FrameWaitPhase::Submit},
        std::pair{RhiPhase::Present, FrameWaitPhase::NativePresent},
        std::pair{RhiPhase::Signal, FrameWaitPhase::Signal}};
    for (const auto [source, destination] : phaseMapping)
    {
        snapshot.waits[ProfileIndex(destination)] = {
            pacing.milliseconds[ProfileIndex(source)], true};
    }
}

double GetBlockingWaitMilliseconds(
    const FrameProfilerSnapshot& snapshot)
{
    double result = 0.0;
    for (const FrameWaitPhase phase : {
             FrameWaitPhase::DisplayAdmission,
             FrameWaitPhase::FrameLimiter,
             FrameWaitPhase::FrameFence,
             FrameWaitPhase::Acquire,
             FrameWaitPhase::ImageFence,
             FrameWaitPhase::NativePresent,
             FrameWaitPhase::QueueBackpressure})
    {
        const FrameProfileDuration& wait =
            snapshot.waits[ProfileIndex(phase)];
        if (wait.available)
        {
            result += wait.milliseconds;
        }
    }
    return result;
}

double GetAdmissionWaitMilliseconds(
    const FrameProfilerSnapshot& snapshot)
{
    double result = 0.0;
    for (const FrameWaitPhase phase : {
             FrameWaitPhase::DisplayAdmission,
             FrameWaitPhase::FrameLimiter,
             FrameWaitPhase::QueueBackpressure})
    {
        const FrameProfileDuration& wait =
            snapshot.waits[ProfileIndex(phase)];
        if (wait.available)
        {
            result += wait.milliseconds;
        }
    }
    return result;
}

double GetFrameExecutionWaitMilliseconds(
    const FrameProfilerSnapshot& snapshot)
{
    double result = 0.0;
    for (const FrameWaitPhase phase : {
             FrameWaitPhase::FrameFence,
             FrameWaitPhase::Acquire,
             FrameWaitPhase::ImageFence,
             FrameWaitPhase::NativePresent})
    {
        const FrameProfileDuration& wait =
            snapshot.waits[ProfileIndex(phase)];
        if (wait.available)
        {
            result += wait.milliseconds;
        }
    }
    return result;
}

std::uint64_t SaturatingDelta(
    const std::uint64_t after,
    const std::uint64_t before) noexcept
{
    return after >= before ? after - before : 0;
}

double NanosecondsToMilliseconds(
    const std::uint64_t nanoseconds) noexcept
{
    return static_cast<double>(nanoseconds) / 1'000'000.0;
}

void AssociateViewGpuTiming(
    FrameProfiler& profiler,
    FrameProfilerSnapshot& snapshot,
    const ProfiledView view,
    const RenderViewRuntimeFeedback& feedback,
    const std::uint64_t currentFrameId,
    const std::uint32_t currentSlot)
{
    const auto resolved = profiler.AssociateViewGpuTiming(
        view,
        currentFrameId,
        currentSlot,
        feedback.gpuTimingGeneration,
        feedback.gpuTimingFrameSlot,
        feedback.statistics.totalRendererGpuMs);
    if (!resolved.has_value())
    {
        return;
    }
    FrameViewProfile& profile =
        snapshot.views[ProfileIndex(view)];
    profile.gpuTotal = {resolved->totalMilliseconds, true};
    profile.gpuGeneration = resolved->generation;
    profile.gpuResolvedFrameId = resolved->frameId;
}

float ClampNavigationDeltaSeconds(
    const Scene::DemoSceneId sceneId,
    const float deltaSeconds) noexcept
{
    if (sceneId != Scene::DemoSceneId::WaveWorksLab
        && sceneId != Scene::DemoSceneId::HpWaterOceanLab)
    {
        return deltaSeconds;
    }
    return std::clamp(
        deltaSeconds,
        0.0f,
        WaveWorksMaximumNavigationDeltaSeconds);
}

void ValidateInitializationFailureStage()
{
    const std::string stage = ReadEnvironmentVariableValue(
        "PRISM_RENDER_TEST_INITIALIZATION_FAILURE_STAGE");
    if (stage.empty())
    {
        return;
    }
    constexpr std::array<std::string_view, 5> ValidStages{
        "services", "backend", "content", "renderers", "editor"};
    if (std::ranges::find(ValidStages, stage) == ValidStages.end())
    {
        throw std::invalid_argument(
            "PRISM_RENDER_TEST_INITIALIZATION_FAILURE_STAGE must be "
            "services, backend, content, renderers, editor or unset.");
    }
}

void InjectInitializationFailure(const std::string_view stage)
{
    if (ReadEnvironmentVariableValue(
            "PRISM_RENDER_TEST_INITIALIZATION_FAILURE_STAGE") == stage)
    {
        throw std::runtime_error(
            "Injected application initialization failure at stage: "
            + std::string(stage));
    }
}

} // namespace

ApplicationHost::ApplicationHost(
    const RHI::GraphicsApi graphicsApi,
    const std::optional<Scene::DemoSceneId> startupDemoScene)
    : m_graphicsApi(graphicsApi),
      m_startupDemoScene(startupDemoScene)
{
}

ApplicationHost::~ApplicationHost()
{
    Shutdown();
}

void ApplicationHost::Initialize()
{
    if (m_lifecycleState == LifecycleState::Initialized)
    {
        return;
    }
    Check(m_lifecycleState == LifecycleState::Uninitialized,
        "Application initialization cannot overlap shutdown or another "
        "initialization attempt.");
    ValidateInitializationFailureStage();
    m_lifecycleState = LifecycleState::Initializing;
    m_gpuDrainedForShutdown = false;

    try
    {

    const std::uint32_t windowWidth =
        Renderer::ReadRenderWindowDimension("PRISM_RENDER_WIDTH", 1600);
    const std::uint32_t windowHeight =
        Renderer::ReadRenderWindowDimension("PRISM_RENDER_HEIGHT", 900);
    const bool performanceEnabled =
        !ReadEnvironmentVariableValue(
             "PRISM_RENDER_FRAME_PERFORMANCE_PATH")
             .empty();
    const bool frameProfilerReportingEnabled =
        !ReadEnvironmentVariableValue(
             "PRISM_RENDER_FRAME_PROFILER_PATH")
             .empty();
    const std::string performanceWorkload =
        ReadEnvironmentVariableValue(
            "PRISM_RENDER_PERFORMANCE_WORKLOAD");
    if (!performanceWorkload.empty()
        && performanceWorkload != "empty")
    {
        throw std::invalid_argument(
            "PRISM_RENDER_PERFORMANCE_WORKLOAD must be empty or unset.");
    }
    const bool emptyPerformanceWorkload =
        performanceWorkload == "empty";
    if (emptyPerformanceWorkload
        && (!Renderer::IsDeterministicRenderCaptureEnabled()
            || (!performanceEnabled
                && !frameProfilerReportingEnabled)))
    {
        throw std::invalid_argument(
            "Empty performance workload requires deterministic input "
            "and an explicit performance/profiler report.");
    }
    m_performanceGameOnly = ParsePerformanceGameOnly(
        ReadEnvironmentVariableValue(
            "PRISM_RENDER_PERFORMANCE_ACTIVE_VIEWS"),
        performanceEnabled || frameProfilerReportingEnabled);
    const auto performanceWindowFocus = ParsePerformanceWindowFocusPolicy(
        ReadEnvironmentVariableValue("PRISM_RENDER_PERFORMANCE_WINDOW_FOCUS"),
        performanceEnabled || frameProfilerReportingEnabled,
        IsEnvironmentVariableEnabled("PRISM_RENDER_HEADLESS"));
    const bool performanceStandalone = IsEnvironmentVariableEnabled(
        "PRISM_RENDER_PERFORMANCE_STANDALONE");
    if (performanceStandalone
        && (!Renderer::IsDeterministicRenderCaptureEnabled()
            || !frameProfilerReportingEnabled))
    {
        throw std::invalid_argument(
            "Performance standalone mode requires deterministic input "
            "and an explicit frame-profiler report.");
    }
#if defined(PRISM_RENDER_HAS_EDITOR)
    if (!performanceStandalone)
    {
        m_editorCoordinator = UI::EditorCoordinator::Create(
            m_graphicsApi);
    }
    m_editorEnabled = m_editorCoordinator != nullptr;
#endif
    m_window = std::make_unique<Platform::Window>(
        m_editorEnabled
            ? "PrismRender Editor"
            : "PrismRender Vulkan RHI",
        windowWidth,
        windowHeight,
        performanceWindowFocus == PerformanceWindowFocusPolicy::Focused
            ? Platform::WindowFocusPolicy::Focused
            : performanceWindowFocus == PerformanceWindowFocusPolicy::Unfocused
                ? Platform::WindowFocusPolicy::Unfocused
                : Platform::WindowFocusPolicy::Default);
    const TaskExecutorKind renderTaskExecutorKind =
        ParseTaskExecutorKind(ReadEnvironmentVariableValue(
            "PRISM_RENDER_TASK_EXECUTOR"));
    const RenderExecutionMode requestedRenderExecutionMode =
        ParseRenderExecutionMode(ReadEnvironmentVariableValue(
            "PRISM_RENDER_EXECUTION_MODE"));
    m_threadedRenderExecutionRequested =
        requestedRenderExecutionMode
        == RenderExecutionMode::Threaded;
    const RHI::FramePacingConfiguration framePacingConfiguration =
        RHI::ParseFramePacingConfiguration(
            ReadEnvironmentVariableValue(
                "PRISM_RENDER_FRAME_PACING_PROFILE"),
            ReadEnvironmentVariableValue(
                "PRISM_RENDER_PRESENTATION_INTENT"),
            ReadEnvironmentVariableValue(
                "PRISM_RENDER_TARGET_FPS"),
            ReadEnvironmentVariableValue(
                "PRISM_RENDER_MAX_QUEUED_FRAMES"));
    ProcessDiagnostics::Log(
        "info",
        "renderer.frame_pacing.requested",
        std::string("Requested frame-pacing profile: ")
            + std::string(RHI::ToString(
                framePacingConfiguration.profile)));
    ProcessDiagnostics::Log(
        "info",
        "renderer.task_executor",
        std::string("Render task executor: ")
            + std::string(ToString(renderTaskExecutorKind)));
    ProcessDiagnostics::Log(
        "info",
        "renderer.execution_mode.requested",
        std::string("Requested render execution mode: ")
            + std::string(ToString(requestedRenderExecutionMode)));
    m_frameTimer = std::make_unique<FrameTimer>();
    m_frameProfiler = std::make_unique<FrameProfiler>();
    m_sceneViewRefreshController =
        std::make_unique<SceneViewRefreshController>();
    const std::string configuredSceneViewPolicy =
        ReadEnvironmentVariableValue(
            "PRISM_RENDER_SCENE_VIEW_REFRESH_POLICY");
    if (!configuredSceneViewPolicy.empty())
    {
        m_sceneViewRefreshController->SetPolicy(
            ParseSceneViewRefreshPolicy(
                configuredSceneViewPolicy));
    }
    m_editorScenePath = std::filesystem::path(PRISM_RENDER_PROJECT_DIR) / "assets/scenes/EditorWorld.prismworld.json";
    const std::string configuredManifest =
        Renderer::ReadRenderEnvironmentVariable(
            "PRISM_RENDER_ASSET_MANIFEST_PATH");
    const std::filesystem::path assetManifestPath = configuredManifest.empty()
        ? std::filesystem::path(PRISM_RENDER_PROJECT_DIR)
            / "automation/assets/AssetManifest.json"
        : std::filesystem::path(configuredManifest);
    m_assetRuntimeCoordinator =
        std::make_unique<AssetRuntimeCoordinator>(
            PRISM_RENDER_PROJECT_DIR,
            assetManifestPath);
    m_sceneSession = std::make_unique<Scene::SceneSession>(
        PRISM_RENDER_PROJECT_DIR,
        m_assetRuntimeCoordinator->GetRegistry());
    m_scene = &m_sceneSession->GetRenderScene();
    m_renderSceneMailbox = &m_sceneSession->GetMailbox();
    m_commandProcessor = &m_sceneSession->GetCommandProcessor();
    m_cameraController =
        &m_sceneSession->GetSceneCameraController();
    m_gameCameraController =
        &m_sceneSession->GetGameCameraController();
    InjectInitializationFailure("services");

    {
        CpuTraceSpan span(
            "GraphicsDeviceInitialize",
            "initialization");
        auto renderTarget =
            std::make_unique<RenderRuntimeExecutionTarget>(
                *m_window,
                m_graphicsApi,
                m_editorEnabled,
                renderTaskExecutorKind,
                framePacingConfiguration,
                *m_assetRuntimeCoordinator);
        RenderRuntimeExecutionTarget* const target =
            renderTarget.get();
        m_renderExecutionService =
            std::make_unique<RenderExecutionService>(
                RenderExecutionMode::Inline,
                std::move(renderTarget),
                framePacingConfiguration.maxQueuedFrames);
        m_renderExecutionTarget = target;
        target->ConfigureSceneSession(*m_sceneSession);
        m_backend = &target->GetBackend();
        m_renderFrameCoordinator =
            &target->GetFrameCoordinator();
        m_sceneRenderer =
            &m_renderFrameCoordinator->GetGameRenderer();
        m_sceneViewRenderer =
            m_renderFrameCoordinator->GetSceneRenderer();
        m_adapterName = m_backend->GetAdapterName();
        m_graphicsApiName = std::string(RHI::ToString(
            m_backend->GetFrameContext().GetGraphicsApi()));
    }
    m_backendInitialized = true;
    InjectInitializationFailure("backend");
    const std::filesystem::path shaderPath = std::filesystem::path(PRISM_RENDER_SHADER_DIR) / "Mesh.hlsl";
    const std::filesystem::path performanceIdentityPath =
        Renderer::ReadRenderEnvironmentVariable(
            "PRISM_RENDER_PERFORMANCE_IDENTITY_PATH");
    if (!performanceIdentityPath.empty())
    {
        CpuTraceSpan span(
            "PerformanceIdentityCapture",
            "diagnostics");
        std::string identityError;
        if (!Renderer::WriteRuntimePerformanceIdentity(
                performanceIdentityPath,
                Renderer::CaptureRuntimePerformanceIdentity(
                    m_backend->GetFrameContext().GetGraphicsApi(),
                    m_backend->GetAdapterInfo(),
                    shaderPath.parent_path()),
                &identityError))
        {
            throw std::runtime_error(
                "Performance identity report failed: "
                + identityError);
        }
    }
    if (IsEnvironmentVariableEnabled(
            "PRISM_RENDER_VALIDATE_TRANSIENT_BUFFERS"))
    {
        std::string validationError;
        if (!RHI::RunTransientBufferPoolValidation(
                m_backend->GetGraphicsDevice(),
                &validationError))
        {
            throw std::runtime_error(
                "Transient buffer validation failed: "
                + validationError);
        }
    }
    const std::string rayTracingReportPath =
        Renderer::ReadRenderEnvironmentVariable(
            "PRISM_RENDER_RAY_TRACING_REPORT_PATH");
    if (!rayTracingReportPath.empty())
    {
        std::string validationError;
        if (!RHI::WriteRayTracingValidationReport(
                rayTracingReportPath,
                m_backend->GetGraphicsDevice(),
                &validationError))
        {
            throw std::runtime_error(
                "Ray-tracing validation failed: "
                + validationError);
        }
    }
    m_captureAutomationController =
        std::make_unique<CaptureAutomationController>();
    m_captureAutomationController->Initialize(
        m_sceneSession->GetIdentity().activeDemoScene,
        Renderer::IsDeterministicRenderCaptureEnabled(),
        IsEnvironmentVariableEnabled(
            "PRISM_RENDER_ASSET_STREAMING"));
    if (m_captureAutomationController->IsDiagnosticsEnabled())
    {
        m_renderFrameCoordinator->SetFrameDiagnosticsEnabled(true);
    }
    const std::string configuredProfilingLevel =
        ReadEnvironmentVariableValue(
            "PRISM_RENDER_PROFILING_LEVEL");
    ProfilingLevel startupProfilingLevel =
        configuredProfilingLevel.empty()
            ? ProfilingLevel::Basic
            : ParseProfilingLevel(configuredProfilingLevel);
    if (performanceEnabled
        || m_captureAutomationController
               ->RequiresCaptureProfiling())
    {
        startupProfilingLevel = ProfilingLevel::Capture;
    }
    m_frameProfiler->SetRequestedLevel(
        startupProfilingLevel);
    m_gltfImportEnabled = GltfImportCompiledIn;

    CpuTraceSpan environmentSpan(
        "EnvironmentMapLoad",
        "asset");
    const std::filesystem::path environmentDirectory = std::filesystem::path(PRISM_RENDER_PROJECT_DIR) / "assets/environment/default";
    std::string environmentStatusMessage;
    m_environmentCubemap =
        m_renderExecutionTarget->LoadEnvironmentCubemap(
            environmentDirectory,
            &environmentStatusMessage);
    if (m_environmentCubemap != nullptr)
    {
        m_environmentMapStatus = environmentStatusMessage;
    }
    else
    {
        m_environmentMapStatus = environmentStatusMessage.empty()
                                     ? "Environment cubemap not found. Using built-in fallback cubemap."
                                     : environmentStatusMessage + " Using built-in fallback cubemap.";
    }
    environmentSpan.End();

    const Scene::DemoSceneId legacySceneFallback =
        IsEnvironmentVariableEnabled(
            "PRISM_RENDER_SHOWCASE_SCENE")
        ? Scene::DemoSceneId::Showcase
        : Scene::DemoSceneId::EditorPreview;
    const Scene::DemoSceneId activeDemoScene =
        m_startupDemoScene.value_or(
        Scene::DemoSceneCatalog::Parse(
            Renderer::ReadRenderEnvironmentVariable(
                "PRISM_RENDER_DEMO_SCENE"),
            legacySceneFallback));
    m_sceneSession->SetActiveDemoScene(activeDemoScene);
    const bool featureDemoScene =
        Scene::DemoSceneCatalog::IsFeatureDemo(
            activeDemoScene);
    const float cameraAspectRatio =
        static_cast<float>(m_window->GetWidth())
        / static_cast<float>(m_window->GetHeight());
    Scene::DemoSceneBuildResult demoSceneResult{};
    if (!emptyPerformanceWorkload)
    {
        demoSceneResult = m_renderExecutionTarget->PopulateDemoScene(
            activeDemoScene,
            *m_scene,
            cameraAspectRatio);
        Renderer::ApplyDemoSceneSettings(
            activeDemoScene,
            m_sceneRenderer->GetSettings());
        m_sceneSession->ConfigureViewportNavigation(
            activeDemoScene);
    }
    else
    {
        m_scene->ClearRenderObjects();
        m_scene->SetActivePointLightCount(0);
        m_scene->SetActiveSpotLightCount(0);
        m_scene->GetDirectionalLight().intensity = 0.0f;
        m_scene->GetCamera().SetAspectRatio(cameraAspectRatio);
        m_scene->GetGameCamera().SetAspectRatio(cameraAspectRatio);
        m_sceneRenderer->GetSettings().editorGridEnabled = false;
    }
    if (m_editorEnabled)
    {
        if (!emptyPerformanceWorkload)
        {
            Renderer::ApplyDemoSceneSettings(
                activeDemoScene,
                m_sceneViewRenderer->GetSettings());
        }
        else
        {
            m_sceneViewRenderer->GetSettings() =
                m_sceneRenderer->GetSettings();
        }
        m_sceneViewRenderer->GetSettings().gameCameraEnabled = false;
    }
    std::string sceneLoadError;
    const std::filesystem::path startupScenePath = std::filesystem::path(PRISM_RENDER_PROJECT_DIR) / "assets/scenes/StartupScene.gltf";
    CpuTraceSpan startupSceneSpan(
        "StartupSceneLoad",
        "asset");
    if (emptyPerformanceWorkload)
    {
        m_sceneSession->SetSourceLabel(
            "Performance Empty Workload (outside DemoSceneCatalog)");
        m_sceneSession->SetLoadMessage(
            "No DemoSceneCatalog factory, glTF scene, object, or light "
            "was added for this opt-in performance workload.");
        m_sceneSession->SetUsingFallbackScene(false);
    }
    else if (featureDemoScene)
    {
        m_sceneSession->SetSourceLabel("Built-in "
            + std::string(
                Scene::DemoSceneCatalog::GetDescription(
                    activeDemoScene).displayName));
        m_sceneSession->SetLoadMessage(demoSceneResult.summary);
        m_sceneSession->SetUsingFallbackScene(false);
    }
    else if (IsEnvironmentVariableEnabled(
            "PRISM_RENDER_ASSET_STREAMING"))
    {
        m_sceneSession->SetSourceLabel(
            "Built-in preview awaiting streamed Scene");
        m_sceneSession->SetLoadMessage(
            "Startup glTF synchronous loading was skipped because Asset Streaming is enabled.");
        m_sceneSession->SetUsingFallbackScene(true);
    }
    else if (!m_renderExecutionTarget->LoadStartupSceneFromGltf(
            startupScenePath,
            *m_scene,
            &sceneLoadError))
    {
        m_sceneSession->SetSourceLabel(
            "Built-in Editor Preview Scene");
        m_sceneSession->SetLoadMessage(sceneLoadError.empty()
                                 ? "StartupScene.gltf was not loaded. PrismRender is using the built-in editor preview scene."
                                 : "StartupScene.gltf load failed, editor preview scene is active. Reason: " + sceneLoadError);
        m_sceneSession->SetUsingFallbackScene(true);
    }
    else
    {
        m_sceneSession->SetSourceLabel(
            "Editor Preview World + " + startupScenePath.string());
        m_sceneSession->SetLoadMessage(
            "glTF startup asset loaded into the editor preview world.");
        m_sceneSession->SetUsingFallbackScene(false);
    }
    startupSceneSpan.End();

    char* roundTripPath = nullptr;
    std::size_t roundTripPathLength = 0;
    if (_dupenv_s(&roundTripPath, &roundTripPathLength, "PRISM_RENDER_SCENE_ROUNDTRIP_PATH") == 0
        && roundTripPath != nullptr
        && roundTripPath[0] != '\0')
    {
        std::string roundTripMessage;
        const std::filesystem::path validationPath(roundTripPath);
        const bool saved = Scene::SceneSerializer::Save(
            validationPath,
            *m_scene,
            m_assetRuntimeCoordinator->GetRegistry(),
            &roundTripMessage);
        const bool loaded = saved && Scene::SceneSerializer::Load(
            validationPath,
            *m_scene,
            m_assetRuntimeCoordinator->GetRegistry(),
            &roundTripMessage);
        m_sceneSession->SetLoadMessage(
            loaded
                ? "Scene serialization round-trip succeeded: "
                    + validationPath.string()
                : roundTripMessage);
    }
    std::free(roundTripPath);

    const std::string worldSnapshotPath =
        Renderer::ReadRenderEnvironmentVariable("PRISM_RENDER_WORLD_PATH");
    m_demoSceneSwitchingEnabled =
        !emptyPerformanceWorkload
        && worldSnapshotPath.empty()
        && !IsEnvironmentVariableEnabled(
            "PRISM_RENDER_ASSET_STREAMING");
    if (!worldSnapshotPath.empty())
    {
        const Asset::AssetRuntimeLoadResult assetLoad =
            m_renderExecutionTarget->LoadRuntimeAssets();
        Scene::WorldRenderSnapshotResult worldResult{};
        if (assetLoad.success)
        {
            worldResult = Scene::LoadWorldRenderSnapshot(
                worldSnapshotPath,
                Renderer::ReadRenderEnvironmentVariable("PRISM_RENDER_EXPECTED_WORLD_HASH"),
                *m_commandProcessor,
                m_assetRuntimeCoordinator->GetRegistry(),
                *m_scene);
            worldResult.expectedAssetManifestHash =
                Renderer::ReadRenderEnvironmentVariable(
                    "PRISM_RENDER_EXPECTED_ASSET_MANIFEST_HASH");
            worldResult.renderedAssetManifestHash = assetLoad.manifestHash;
            worldResult.loadedAssetSourceCount = assetLoad.sourceCount;
            worldResult.assetCacheHitCount = assetLoad.cacheHitCount;
            worldResult.assetCacheMissCount = assetLoad.cacheMissCount;
            worldResult.assetCachedBytes = assetLoad.cachedBytes;
            worldResult.cookedAssetCount =
                assetLoad.cookedAssetCount;
            worldResult.cookedAssetBytes =
                assetLoad.cookedBytes;
            worldResult.assetDiagnostics = assetLoad.diagnostics;
            if (worldResult.success
                && !worldResult.expectedAssetManifestHash.empty()
                && worldResult.expectedAssetManifestHash
                    != worldResult.renderedAssetManifestHash)
            {
                worldResult.success = false;
                worldResult.errorCode = "asset_manifest_hash_mismatch";
                worldResult.errorMessage =
                    "The renderer loaded a different Asset Manifest than the Harness.";
            }
        }
        else
        {
            worldResult.snapshotPath =
                std::filesystem::absolute(worldSnapshotPath).lexically_normal();
            worldResult.expectedWorldHash =
                Renderer::ReadRenderEnvironmentVariable(
                    "PRISM_RENDER_EXPECTED_WORLD_HASH");
            worldResult.expectedAssetManifestHash =
                Renderer::ReadRenderEnvironmentVariable(
                    "PRISM_RENDER_EXPECTED_ASSET_MANIFEST_HASH");
            worldResult.renderedAssetManifestHash = assetLoad.manifestHash;
            worldResult.assetDiagnostics = assetLoad.diagnostics;
            worldResult.errorCode = assetLoad.errorCode;
            worldResult.errorMessage = assetLoad.errorMessage;
        }
        std::string receiptError;
        if (!Scene::WriteWorldRenderReceipt(
                Renderer::ReadRenderEnvironmentVariable("PRISM_RENDER_WORLD_REPORT_PATH"),
                worldResult,
                RHI::ToString(m_backend->GetFrameContext().GetGraphicsApi()),
                &receiptError))
        {
            throw std::runtime_error("Rendered-world receipt failed: " + receiptError);
        }
        if (!worldResult.success)
        {
            throw std::runtime_error(
                "Automated World load failed [" + worldResult.errorCode + "]: "
                + worldResult.errorMessage);
        }
        m_sceneSession->SetSourceLabel(
            "Harness Snapshot: "
            + worldResult.snapshotPath.string());
        m_sceneSession->SetLoadMessage(
            "Engine Snapshot loaded with state hash "
            + worldResult.renderedWorldHash + '.');
        m_sceneSession->SetUsingFallbackScene(false);
    }
    else
    {
        if (IsEnvironmentVariableEnabled(
                "PRISM_RENDER_ASSET_STREAMING"))
        {
            Asset::AssetStreamingConfiguration
                streamingConfiguration{};
            const std::string configuredBudget =
                ReadEnvironmentVariableValue(
                    "PRISM_RENDER_ASSET_STREAMING_BUDGET_MB");
            if (!configuredBudget.empty())
            {
                streamingConfiguration.residentBudgetBytes =
                    std::stoull(configuredBudget)
                    * 1024ull * 1024ull;
            }
            std::string streamingError;
            if (!m_assetRuntimeCoordinator->EnableStreaming(
                    streamingConfiguration,
                    &streamingError))
            {
                m_sceneSession->SetLoadMessage(
                    "Asset Streaming fallback: "
                    + streamingError);
            }
            else
            {
                for (const auto& entry :
                     m_assetRuntimeCoordinator->GetStreamingEntries())
                {
                    if (entry.type == Asset::AssetType::Scene
                        && m_captureAutomationController
                               ->GetStreamingSceneAssetId()
                               .empty())
                    {
                        m_captureAutomationController
                            ->SetStreamingSceneAssetId(
                                entry.assetId);
                        if (!m_assetRuntimeCoordinator->RequestStreamingAsset(
                                entry.assetId,
                                100,
                                false,
                                &streamingError))
                        {
                            m_sceneSession->SetLoadMessage(
                                "Asset Streaming request failed: "
                                + streamingError);
                        }
                    }
                }
                if (m_sceneSession->GetIdentity()
                        .loadMessage.empty())
                {
                    m_sceneSession->SetLoadMessage(
                        "Asynchronous Asset Streaming enabled.");
                }
            }
        }
        else
        {
            const Asset::AssetRuntimeLoadResult assetLoad =
                m_renderExecutionTarget->LoadRuntimeAssets();
            if (!assetLoad.success)
            {
                m_sceneSession->SetLoadMessage(
                    "Asset Manifest load failed: "
                    + assetLoad.errorMessage);
            }
        }
        m_sceneSession->InitializeWorldFromRenderScene();
    }

    std::string assetDatabaseError;
    if (!m_assetRuntimeCoordinator->InitializeImportService(
            m_editorEnabled,
            &assetDatabaseError))
    {
        const std::string prefix =
            m_sceneSession->GetIdentity().loadMessage.empty()
            ? std::string{}
            : m_sceneSession->GetIdentity().loadMessage + " ";
        m_sceneSession->SetLoadMessage(prefix
            + "Editor Asset Database unavailable: "
            + assetDatabaseError);
    }
    InjectInitializationFailure("content");

    {
        CpuTraceSpan span(
            "RendererPipelineInitialize",
            "initialization");
        m_renderExecutionTarget->InitializeRenderers(
            shaderPath,
            *m_scene,
            m_environmentCubemap);
        m_environmentCubemap.reset();
        m_gameRenderSettings = m_sceneRenderer->GetSettings();
        m_gameOutputWidth =
            m_sceneRenderer->GetFinalOutputWidth();
        m_gameOutputHeight =
            m_sceneRenderer->GetFinalOutputHeight();
        if (m_editorEnabled)
        {
            m_sceneRenderSettings =
                m_sceneViewRenderer->GetSettings();
            m_sceneOutputWidth =
                m_sceneViewRenderer->GetFinalOutputWidth();
            m_sceneOutputHeight =
                m_sceneViewRenderer->GetFinalOutputHeight();
        }
        m_gameHistorySettingsTracker.Reset();
        m_sceneHistorySettingsTracker.Reset();
    }
    InjectInitializationFailure("renderers");

#if defined(PRISM_RENDER_HAS_EDITOR)
    if (m_editorEnabled)
    {
        CpuTraceSpan span(
            "EditorUiInitialize",
            "initialization");
        m_editorCoordinator->Initialize(
            *m_window,
            *m_backend,
            *m_sceneRenderer,
            *m_sceneViewRenderer,
            m_performanceGameOnly);
        m_renderExecutionTarget->ConfigureUiExecutionCallbacks(
            [this](const UI::UiDrawPacket& packet)
            {
                m_editorCoordinator->Render(packet);
            },
            [this](
                Renderer::SceneRenderer& gameRenderer,
                Renderer::SceneRenderer& sceneRenderer)
            {
                m_editorCoordinator->RefreshViewportTextures(
                    gameRenderer,
                    sceneRenderer,
                    m_performanceGameOnly);
            },
            [this]
            {
                m_editorCoordinator->Shutdown();
            });
    }
#endif
    InjectInitializationFailure("editor");
    m_runtimePerformanceIdentity =
        Renderer::SerializeRuntimePerformanceIdentity(
            Renderer::CaptureRuntimePerformanceIdentity(
                m_graphicsApi,
                m_backend->GetAdapterInfo(),
                std::filesystem::path(PRISM_RENDER_SHADER_DIR)));
    if (m_threadedRenderExecutionRequested)
    {
        m_renderExecutionService->StartThreadedExecution();
        ProcessDiagnostics::Log(
            "info",
            "renderer.execution_mode.threaded",
            "Render runtime ownership handed to the resident execution lane.");
    }
    // Startup is the final caller-lane access to these non-owning aliases.
    m_sceneViewRenderer = nullptr;
    m_sceneRenderer = nullptr;
    m_renderFrameCoordinator = nullptr;
    m_backend = nullptr;
    m_renderExecutionTarget = nullptr;
    m_lifecycleState = LifecycleState::Initialized;
    }
    catch (...)
    {
        Shutdown();
        throw;
    }
}

int ApplicationHost::Run()
{
    Check(m_lifecycleState == LifecycleState::Initialized,
        "Application must be initialized before Run().");

    const std::uint64_t maximumFrameCount =
        m_captureAutomationController->GetMaximumFrameCount();
    m_completedFrameCount = 0;
    m_nextRenderControlCommandId = 1;
    m_currentSceneEpoch = RenderEpoch{1};
    m_currentViewEpoch = RenderEpoch{1};
    std::uint64_t& completedFrameCount = m_completedFrameCount;

    const std::filesystem::path frameProfilerReportPath =
        ReadEnvironmentVariableValue(
            "PRISM_RENDER_FRAME_PROFILER_PATH");
    const std::string framePacingAutomationText =
        ReadEnvironmentVariableValue(
            "PRISM_RENDER_FRAME_PACING_SEQUENCE");
    const std::string windowResizeAutomationText =
        ReadEnvironmentVariableValue(
            "PRISM_RENDER_RESIZE_SEQUENCE");
    const std::vector<ScheduledFramePacingChange>
        framePacingAutomation =
            ParseFramePacingAutomationSequence(
                framePacingAutomationText);
    const std::vector<ScheduledWindowResize>
        windowResizeAutomation =
            ParseWindowResizeAutomationSequence(
                windowResizeAutomationText);
    const std::string profilingSequenceText =
        ReadEnvironmentVariableValue(
            "PRISM_RENDER_PROFILING_LEVEL_SEQUENCE");
    std::optional<ProfilingLevelSchedule> profilingSchedule;
    if (!profilingSequenceText.empty())
    {
        if (frameProfilerReportPath.empty())
        {
            throw std::invalid_argument(
                "Profiling level sequence requires "
                "PRISM_RENDER_FRAME_PROFILER_PATH.");
        }
        profilingSchedule =
            ProfilingLevelSchedule::Parse(
                profilingSequenceText);
    }
    if (!frameProfilerReportPath.empty())
    {
        if (maximumFrameCount == 0
            || !Renderer::IsDeterministicRenderCaptureEnabled())
        {
            throw std::invalid_argument(
                "Frame profiler reporting requires deterministic input "
                "and a finite frame count.");
        }
        if (profilingSchedule.has_value()
            && profilingSchedule->GetTotalFrameCount()
                != maximumFrameCount)
        {
            throw std::invalid_argument(
                "Profiling level sequence frame count must equal "
                "PRISM_RENDER_MAX_FRAMES.");
        }
    }

    std::unique_ptr<FramePerformanceRecorder> performance;
    std::optional<std::pair<int, int>> performancePosition;
    const auto performancePath = ReadEnvironmentVariableValue("PRISM_RENDER_FRAME_PERFORMANCE_PATH");
    if (!framePacingAutomation.empty()
        || !windowResizeAutomation.empty())
    {
        if (maximumFrameCount == 0
            || !Renderer::IsDeterministicRenderCaptureEnabled()
            || (performancePath.empty()
                && frameProfilerReportPath.empty()))
        {
            throw std::invalid_argument(
                "Frame-pacing and resize automation require deterministic "
                "input, a finite frame count, and a performance report.");
        }
        const auto validateLastFrame =
            [maximumFrameCount](const std::uint64_t frame)
            {
                if (frame > maximumFrameCount)
                {
                    throw std::invalid_argument(
                        "Automation sequence frame exceeds "
                        "PRISM_RENDER_MAX_FRAMES.");
                }
            };
        if (!framePacingAutomation.empty())
        {
            validateLastFrame(framePacingAutomation.back().frame);
        }
        if (!windowResizeAutomation.empty())
        {
            validateLastFrame(windowResizeAutomation.back().frame);
        }
    }
    if (!performancePath.empty())
    {
        if (maximumFrameCount == 0 || !Renderer::IsDeterministicRenderCaptureEnabled())
            throw std::invalid_argument("Performance sampling requires deterministic input and a finite frame count.");
        for (const char* incompatible : {"PRISM_RENDER_CAPTURE_PATH", "PRISM_RENDER_CAPTURE_SEQUENCE_PATH",
                 "PRISM_RENDER_GPU_TIMING_REPORT_PATH", "PRISM_RENDER_FRAME_DIAGNOSTICS_PATH",
                 "PRISM_RENDER_CAPTURE_DIAGNOSTICS_PATH", "PRISM_RENDER_CPU_TRACE_PATH",
                 "PRISM_RENDER_WATER_VALIDATION_SEQUENCE", "PRISM_RENDER_RDG_REPORT_PATH"})
            if (!ReadEnvironmentVariableValue(incompatible).empty())
                throw std::invalid_argument(std::string("Performance sampling conflicts with ") + incompatible);
        performancePosition = ParsePerformanceWindowPosition(
            ReadEnvironmentVariableValue("PRISM_RENDER_PERFORMANCE_WINDOW_X"),
            ReadEnvironmentVariableValue("PRISM_RENDER_PERFORMANCE_WINDOW_Y"));
        if (m_performanceGameOnly
            && Renderer::ReadRenderEnvironmentVariable(
                   "PRISM_RENDER_CAPTURE_VIEW")
                   == "scene")
            throw std::invalid_argument(
                "Game-only performance sampling cannot capture the Scene view.");
        const char* windowFocusPolicy = m_window->GetFocusPolicy() == Platform::WindowFocusPolicy::Focused
            ? "focused"
            : m_window->GetFocusPolicy() == Platform::WindowFocusPolicy::Unfocused ? "unfocused" : "default";
        performance = std::make_unique<FramePerformanceRecorder>(performancePath,
            nlohmann::json{{"scene", m_sceneSession->GetIdentity().sourceLabel}, {"editorEnabled", m_editorEnabled},
                {"maximumFrames", maximumFrameCount}, {"deterministic", true}, {"pacingVersion", 1},
                {"activeViewPolicy", m_performanceGameOnly ? "game" : "default"},
                {"windowFocusPolicy", windowFocusPolicy}});
    }
    using PerfMetric = FramePerformanceRecorder::Metric;
    if (performance != nullptr
        && profilingSchedule.has_value())
    {
        throw std::invalid_argument(
            "Profiling level sequence conflicts with legacy "
            "frame performance sampling, which requires capture level.");
    }
    std::unique_ptr<FrameProfilerReport> frameProfilerReport;
    if (!frameProfilerReportPath.empty())
    {
        frameProfilerReport =
            std::make_unique<FrameProfilerReport>(
                frameProfilerReportPath,
                nlohmann::json{
                    {"scene", m_sceneSession->GetIdentity().sourceLabel},
                    {"editorEnabled", m_editorEnabled},
                    {"maximumFrames", maximumFrameCount},
                    {"deterministic", true},
                    {"width", m_gameOutputWidth},
                    {"height", m_gameOutputHeight},
                    {"backend", m_graphicsApiName},
                    {"validationRequested",
                        IsEnvironmentVariableEnabled(
                            "PRISM_RENDER_GPU_VALIDATION")},
                    {"headless", IsEnvironmentVariableEnabled(
                        "PRISM_RENDER_HEADLESS")},
                    {"windowFocusPolicy",
                        m_window->GetFocusPolicy()
                                == Platform::WindowFocusPolicy::Focused
                            ? "focused"
                            : m_window->GetFocusPolicy()
                                    == Platform::WindowFocusPolicy::Unfocused
                                ? "unfocused"
                                : "default"},
                    {"activeViewPolicy",
                        m_performanceGameOnly ? "game" : "default"},
                    {"sceneViewRefreshPolicy",
                        ToString(m_sceneViewRefreshController
                            ->GetPolicy())},
                    {"workload",
                        ReadEnvironmentVariableValue(
                            "PRISM_RENDER_PERFORMANCE_WORKLOAD")
                            .empty()
                            ? "preview"
                            : ReadEnvironmentVariableValue(
                                "PRISM_RENDER_PERFORMANCE_WORKLOAD")},
                    {"identity", m_runtimePerformanceIdentity},
                    {"profilingSequence",
                        profilingSequenceText.empty()
                            ? nlohmann::json(nullptr)
                            : nlohmann::json(
                                profilingSequenceText)}});
    }
    const std::filesystem::path scenePublicationStatisticsPath =
        ReadEnvironmentVariableValue(
            "PRISM_RENDER_SCENE_PUBLICATION_STATS_PATH");
    Scene::RenderSceneExtractionStatisticsReport
        scenePublicationStatistics(
            scenePublicationStatisticsPath);
    if (scenePublicationStatistics.IsEnabled()
        && (maximumFrameCount == 0
            || !Renderer::IsDeterministicRenderCaptureEnabled()))
    {
        throw std::invalid_argument(
            "Scene publication statistics require deterministic input "
            "and a finite frame count.");
    }
    bool sequenceStreamingActivationAllowed =
        !m_captureAutomationController->HasSequenceAction(
            FrameCaptureActionType::ActivateStreamingScene);
    std::optional<RenderFrameFeedback> latestRenderFeedback;
    std::uint64_t& nextRenderControlCommandId =
        m_nextRenderControlCommandId;
    std::uint64_t publishedSettingsRevision = 0;
    RenderEpoch& currentSceneEpoch = m_currentSceneEpoch;
    RenderEpoch& currentViewEpoch = m_currentViewEpoch;
    std::optional<ResizeRenderViewCommand> pendingResize;
    std::optional<std::function<bool()>> pendingFrameCompletion;
    double pendingPreparationOverlapMilliseconds = 0.0;
    std::optional<FrameProfileClock::time_point> pendingProducerStart;
    std::optional<double> pendingFrameIntervalMilliseconds;
    std::optional<RHI::FramePacingConfiguration>
        pendingFramePacingConfiguration;
    std::uint64_t nextFramePacingGeneration = 2;
    std::size_t nextFramePacingAutomation = 0;
    std::size_t nextWindowResizeAutomation = 0;
    Check(m_frameProfiler != nullptr,
        "Frame profiler was not initialized.");
    const auto pollPlatformEvents = [this]
    {
#if defined(PRISM_RENDER_HAS_EDITOR)
        if (m_editorEnabled)
        {
            m_editorCoordinator->PollPlatformEvents(*m_window);
            return;
        }
#endif
        m_window->PollEvents();
    };

    while (!m_window->ShouldClose())
    {
        if (performance != nullptr
            && pendingFrameCompletion.has_value())
        {
            pendingPreparationOverlapMilliseconds = 0.0;
            const bool shouldExit = (*pendingFrameCompletion)();
            pendingFrameCompletion.reset();
            pendingProducerStart.reset();
            pendingFrameIntervalMilliseconds.reset();
            if (shouldExit)
            {
                break;
            }
        }
        const std::uint64_t logicalFrameId =
            m_captureAutomationController
                ->GetAcceptedFrameCount() + 1u;
        const auto profileLoopStart = FrameProfileClock::now();
        if (nextFramePacingAutomation
                < framePacingAutomation.size()
            && framePacingAutomation[
                   nextFramePacingAutomation].frame
                == logicalFrameId)
        {
            pendingFramePacingConfiguration =
                RHI::MakeFramePacingConfiguration(
                    framePacingAutomation[
                        nextFramePacingAutomation].profile);
            ++nextFramePacingAutomation;
        }
        if (nextWindowResizeAutomation
                < windowResizeAutomation.size()
            && windowResizeAutomation[
                   nextWindowResizeAutomation].frame
                == logicalFrameId)
        {
            const ScheduledWindowResize& resize =
                windowResizeAutomation[
                    nextWindowResizeAutomation];
            m_window->SetSize(resize.width, resize.height);
            ++nextWindowResizeAutomation;
        }
        if (pendingFramePacingConfiguration.has_value())
        {
            RenderControlAcknowledgement pacingAcknowledgement =
                m_renderExecutionService->SubmitControl({
                    RenderControlCommandId{
                        nextRenderControlCommandId++},
                    Scene::LogicalFrameId{logicalFrameId},
                    currentSceneEpoch,
                    currentViewEpoch,
                    RenderControlBoundary::BeforeFrame,
                    ChangeFramePacingCommand{
                        *pendingFramePacingConfiguration,
                        nextFramePacingGeneration}}).Wait();
            if (!pacingAcknowledgement.succeeded)
            {
                ProcessDiagnostics::Log(
                    "error",
                    "renderer.frame_pacing.transition_failed",
                    pacingAcknowledgement.framePacingError);
            }
            else
            {
                ++nextFramePacingGeneration;
            }
            pendingFramePacingConfiguration.reset();
        }
        RenderControlAcknowledgement admissionAcknowledgement =
            m_renderExecutionService->SubmitControl({
                RenderControlCommandId{
                    nextRenderControlCommandId++},
                Scene::LogicalFrameId{logicalFrameId},
                currentSceneEpoch,
                currentViewEpoch,
                RenderControlBoundary::BeforeFrame,
                AdmitRenderFrameCommand{}}).Wait();
        Check(admissionAcknowledgement.succeeded
                && admissionAcknowledgement.frameAdmission.has_value()
                && admissionAcknowledgement.framePacingState.has_value(),
            "Render lane rejected frame admission.");
        const RHI::FrameAdmissionResult frameAdmission =
            *admissionAcknowledgement.frameAdmission;
        const RHI::FramePacingState framePacingState =
            *admissionAcknowledgement.framePacingState;
        const std::uint64_t producerInputTimestampNanoseconds =
            static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    profileLoopStart.time_since_epoch()).count());
        FrameProfilerSnapshot frameProfile{};
        frameProfile.frameId = logicalFrameId;
        frameProfile.framePacing.available = true;
        frameProfile.framePacing.profile = RHI::ToString(
            framePacingState.requested.profile);
        frameProfile.framePacing.requestedPresentation = RHI::ToString(
            framePacingState.requested.presentation);
        frameProfile.framePacing.effectivePresentation = RHI::ToString(
            framePacingState.effectivePresentation);
        frameProfile.framePacing.nativePresentMode =
            framePacingState.nativePresentMode;
        frameProfile.framePacing.admissionSource = RHI::ToString(
            framePacingState.admissionSource);
        frameProfile.framePacing.fallbackReason =
            framePacingState.fallbackReason;
        frameProfile.framePacing.targetFpsEnabled =
            framePacingState.requested.targetFps.has_value();
        frameProfile.framePacing.targetFps =
            framePacingState.requested.targetFps.value_or(0);
        frameProfile.framePacing.configuredMaxQueuedFrames =
            framePacingState.requested.maxQueuedFrames;
        frameProfile.framePacing.effectiveMaxQueuedFrames =
            framePacingState.effectiveMaxQueuedFrames;
        frameProfile.framePacing.swapchainImageCount =
            framePacingState.swapchainImageCount;
        frameProfile.framePacing.frameResourceSlotCount =
            framePacingState.frameResourceSlotCount;
        frameProfile.framePacing.syncInterval =
            framePacingState.syncInterval;
        frameProfile.framePacing.presentFlags =
            framePacingState.presentFlags;
        frameProfile.framePacing.requestedGeneration =
            framePacingState.requestedGeneration;
        frameProfile.framePacing.effectiveGeneration =
            framePacingState.effectiveGeneration;
        frameProfile.framePacing.tearingSupported =
            framePacingState.tearingSupported;
        frameProfile.framePacing.tearingEnabled =
            framePacingState.tearingEnabled;
        frameProfile.framePacing.transitionPending =
            framePacingState.transitionPending;
        frameProfile.waits[ProfileIndex(
            FrameWaitPhase::DisplayAdmission)] = {
                frameAdmission.displayWaitMilliseconds, true};
        frameProfile.waits[ProfileIndex(
            FrameWaitPhase::FrameLimiter)] = {
                frameAdmission.limiterWaitMilliseconds, true};
        frameProfile.waits[ProfileIndex(
            FrameWaitPhase::QueueBackpressure)] = {
                frameAdmission.queueWaitMilliseconds, true};
#if defined(PRISM_RENDER_HAS_EDITOR)
        std::shared_ptr<const UI::UiDrawPacket> uiDrawPacket;
#endif
        if (profilingSchedule.has_value())
        {
            m_frameProfiler->SetRequestedLevel(
                profilingSchedule->GetLevel(
                    frameProfile.frameId));
        }
        frameProfile.requestedLevel =
            m_frameProfiler->GetRequestedLevel();
        frameProfile.actualLevel = frameProfile.requestedLevel;
        FrameCpuLaneProfile& mainLane =
            frameProfile.cpuLanes[
                ProfileIndex(FrameCpuLane::Main)];
        mainLane.threadId = FrameProfiler::CurrentThreadId();
        FrameCpuLaneProfile& renderLane =
            frameProfile.cpuLanes[
                ProfileIndex(FrameCpuLane::Render)];
        // Inline begins on Main; completed feedback replaces this with the
        // resident render-lane identity in threaded mode.
        renderLane.threadId = mainLane.threadId;
        const auto eventsAndInputStart = FrameProfileClock::now();
        if (performance) performance->BeginFrame(logicalFrameId);
        CpuTraceSpan frameSpan("Frame", "frame");
        CpuTraceSpan updateSpan("FrameUpdate", "frame");
        pollPlatformEvents();
        if (m_assetRuntimeCoordinator->IsStreamingEnabled())
        {
            const AssetRuntimeBindingApplyResult applied =
                m_assetRuntimeCoordinator
                    ->ApplyCompletedStreamingBindings();
            if (applied.bindingUpdateCount != 0)
            {
                (void)m_sceneSession
                    ->SynchronizeRuntimeAssetBindingChanges();
            }
        }
        // Apply this one-shot test input after initial platform events. Do not
        // force placement every frame or hide later external window changes.
        if (performancePosition)
        {
            m_window->SetPosition(performancePosition->first, performancePosition->second);
            performancePosition.reset();
        }
        WaterValidationSequence& waterValidation =
            m_captureAutomationController->GetWaterValidationSequence();
        if (waterValidation.IsEnabled())
        {
            const RenderViewRuntimeFeedback* const gameFeedback =
                latestRenderFeedback.has_value()
                ? latestRenderFeedback->FindView(
                    Scene::GameRenderViewId)
                : nullptr;
            m_sceneViewRequiresRefresh = waterValidation.Update(
                logicalFrameId - 1u,
                m_gameRenderSettings,
                gameFeedback != nullptr
                    && gameFeedback->waterOpticsPassScheduled,
                gameFeedback != nullptr
                    && gameFeedback->statistics.ocean
                        .waterCoverageAvailable,
                *m_scene,
                *m_window,
                [this](const Scene::DemoSceneId sceneId)
                {
                    ActivateDemoScene(sceneId);
                },
                [this](
                    const Scene::RenderViewId viewId,
                    const Scene::RenderViewHistoryInvalidation reason)
                {
                    m_sceneSession->NotifyDynamicViewHistory(
                        viewId,
                        reason);
                });
        }
        m_frameTimer->Tick();
        UpdateFluidInput();

        std::uint32_t resizedWidth = 0;
        std::uint32_t resizedHeight = 0;
        if (m_window->ConsumeResize(resizedWidth, resizedHeight))
        {
            ++currentViewEpoch.value;
            pendingResize = ResizeRenderViewCommand{
                Scene::GameRenderViewId,
                resizedWidth,
                resizedHeight};
            m_gameOutputWidth = resizedWidth;
            m_gameOutputHeight = resizedHeight;
            if (m_editorEnabled)
            {
                m_sceneOutputWidth = resizedWidth;
                m_sceneOutputHeight = resizedHeight;
            }
            const float resizedAspectRatio =
                static_cast<float>(resizedWidth)
                / static_cast<float>(resizedHeight);
            m_scene->GetCamera().SetAspectRatio(resizedAspectRatio);
            m_scene->GetGameCamera().SetAspectRatio(
                resizedAspectRatio);
            m_sceneViewRequiresRefresh = true;
            const RenderControlAcknowledgement resize =
                m_renderExecutionService->SubmitControl({
                    RenderControlCommandId{
                        nextRenderControlCommandId++},
                    Scene::LogicalFrameId{
                        logicalFrameId},
                    currentSceneEpoch,
                    currentViewEpoch,
                    RenderControlBoundary::BeforeFrame,
                    *pendingResize}).Wait();
            Check(resize.succeeded,
                "Render lane rejected a swap-chain resize.");
            pendingResize.reset();
        }
        AddProfileElapsed(
            frameProfile.cpuPhases[
                ProfileIndex(FrameCpuPhase::EventsAndInput)],
            eventsAndInputStart);

#if defined(PRISM_RENDER_HAS_EDITOR)
        if (m_editorEnabled)
        {
            const auto uiBuildStart = FrameProfileClock::now();
            FramePerformanceRecorder::Scope uiScope(
                performance.get(),
                PerfMetric::UiBuild);
            UI::EditorCoordinatorFrameInput editorInput(
                *m_sceneSession,
                m_assetRuntimeCoordinator->GetRegistry(),
                m_gameRenderSettings,
                m_gameOutputWidth,
                m_gameOutputHeight,
                m_sceneOutputWidth,
                m_sceneOutputHeight,
                *m_frameProfiler,
                *m_sceneViewRefreshController,
                *m_window);
            if (latestRenderFeedback.has_value())
            {
                if (const RenderViewRuntimeFeedback* const gameFeedback =
                        latestRenderFeedback->FindView(
                            Scene::GameRenderViewId);
                    gameFeedback != nullptr)
                {
                    editorInput.completedGameStatistics =
                        &gameFeedback->statistics;
                    editorInput.completedStatisticsLogicalFrameId =
                        latestRenderFeedback->logicalFrameId.value;
                }
            }
            editorInput.documentPath = m_editorScenePath;
            editorInput.adapterName = m_adapterName;
            editorInput.graphicsApiName = m_graphicsApiName;
            editorInput.environmentMapStatus = m_environmentMapStatus;
            editorInput.deltaTimeMs =
                m_frameTimer->GetDeltaMilliseconds();
            editorInput.framesPerSecond =
                m_frameTimer->GetFramesPerSecond();
            editorInput.navigationDeltaSeconds =
                ClampNavigationDeltaSeconds(
                    m_sceneSession->GetIdentity().activeDemoScene,
                    static_cast<float>(
                        m_frameTimer->GetDeltaSeconds()));
            editorInput.frameIndex = latestRenderFeedback.has_value()
                ? latestRenderFeedback->frameContextIndex
                : 0;
            editorInput.windowWidth = m_gameOutputWidth;
            editorInput.windowHeight = m_gameOutputHeight;
            editorInput.gltfImportEnabled = m_gltfImportEnabled;
            editorInput.demoSceneSwitchingEnabled =
                m_demoSceneSwitchingEnabled;
            editorInput.deterministicCaptureEnabled =
                Renderer::IsDeterministicRenderCaptureEnabled();
            if (m_assetRuntimeCoordinator->IsStreamingEnabled())
            {
                const Asset::AssetStreamingStatistics streaming =
                    m_assetRuntimeCoordinator
                        ->GetStreamingStatistics();
                editorInput.streaming.enabled = true;
                editorInput.streaming.queuedCount =
                    static_cast<std::uint32_t>(
                        streaming.queuedCount
                        + streaming.loadingCount
                        + streaming.readyCount
                        + streaming.uploadingCount);
                editorInput.streaming.residentCount =
                    static_cast<std::uint32_t>(
                        streaming.residentCount);
                editorInput.streaming.evictedCount =
                    static_cast<std::uint32_t>(
                        streaming.evictedCount);
                editorInput.streaming.failedCount =
                    static_cast<std::uint32_t>(
                        streaming.failedCount);
                editorInput.streaming.residentBytes =
                    streaming.residentBytes;
                editorInput.streaming.budgetBytes =
                    streaming.residentBudgetBytes;
                editorInput.streaming.completedUploads =
                    streaming.completedUploadCount;
                editorInput.streaming.evictionCount =
                    streaming.evictionCount;
            }
            if (Asset::AssetDatabase* const database =
                    m_assetRuntimeCoordinator
                        ->GetImportDatabase();
                database != nullptr)
            {
                editorInput.assetActions.database = database;
                editorInput.assetActions.importFile =
                    [&](const std::filesystem::path& path)
                    {
                        return ImportEditorAsset(path, false);
                    };
                editorInput.assetActions.reimportFile =
                    [&](const std::filesystem::path& path)
                    {
                        return ImportEditorAsset(path, true);
                    };
                editorInput.assetActions.instantiateAsset =
                    [&](const Asset::AssetRecord& asset)
                    {
                        return InstantiateEditorAsset(asset);
                    };
            }
            const UI::EditorCoordinatorFrameResult editorResult =
                m_editorCoordinator->DrawFrame(editorInput);
            m_sceneViewRequiresRefresh =
                m_sceneViewRequiresRefresh
                || editorResult.sceneViewRequiresRefresh;
            if (editorResult.requestedDemoScene.has_value())
            {
                ActivateDemoScene(
                    *editorResult.requestedDemoScene);
            }
            if (editorResult.requestedFramePacing.has_value())
            {
                pendingFramePacingConfiguration =
                    RHI::MakeFramePacingConfiguration(
                        *editorResult.requestedFramePacing);
            }
            uiDrawPacket =
                m_editorCoordinator->BuildDrawPacket();
            AddProfileElapsed(
                frameProfile.cpuPhases[
                    ProfileIndex(FrameCpuPhase::UiBuild)],
                uiBuildStart);
        }
        else
#endif
        if (!Renderer::IsDeterministicRenderCaptureEnabled())
        {
            const bool useGameCamera =
                m_gameRenderSettings.gameCameraEnabled;
            Scene::CameraController& controller =
                useGameCamera
                ? *m_gameCameraController
                : *m_cameraController;
            const float deltaSeconds = static_cast<float>(
                m_frameTimer->GetDeltaSeconds());
            const float navigationDeltaSeconds =
                ClampNavigationDeltaSeconds(
                    m_sceneSession->GetIdentity().activeDemoScene,
                    deltaSeconds);
            controller.Update(
                useGameCamera
                    ? m_scene->GetGameCamera()
                    : m_scene->GetCamera(),
                *m_window,
                navigationDeltaSeconds,
                true);
        }

        updateSpan.End();
        {
            CpuTraceSpan renderSpan(
                "FrameRender",
                "frame");
            if (m_pendingDemoScene.has_value())
            {
                const Scene::DemoSceneId sceneId =
                    *m_pendingDemoScene;
                const RenderEpoch nextSceneEpoch{
                    currentSceneEpoch.value + 1u};
                const float aspectRatio =
                    static_cast<float>(m_gameOutputWidth)
                    / static_cast<float>((std::max)(
                        m_gameOutputHeight,
                        1u));
                const RenderControlAcknowledgement sceneSwitch =
                    m_renderExecutionService->SubmitControl({
                        RenderControlCommandId{
                            nextRenderControlCommandId++},
                        Scene::LogicalFrameId{
                            logicalFrameId},
                        currentSceneEpoch,
                        currentViewEpoch,
                        RenderControlBoundary::BeforeFrame,
                        SwitchRenderSceneCommand{
                            nextSceneEpoch,
                            sceneId,
                            aspectRatio}}).Wait();
                Check(sceneSwitch.succeeded,
                    "Render lane rejected a demo scene switch.");
                currentSceneEpoch = nextSceneEpoch;
                if (sceneSwitch.sceneChanged)
                {
                    Renderer::ApplyDemoSceneSettings(
                        sceneId,
                        m_gameRenderSettings);
                    if (m_editorEnabled)
                    {
                        Renderer::ApplyDemoSceneSettings(
                            sceneId,
                            m_sceneRenderSettings);
                        m_sceneRenderSettings.gameCameraEnabled = false;
                    }
                    m_sceneViewRequiresRefresh = true;
                    m_sceneViewWasVisible = false;
                }
                m_pendingDemoScene.reset();
            }
            for (const FrameCaptureAction& action :
                 m_captureAutomationController->ConsumeActions(
                     logicalFrameId))
            {
                switch (action.type)
                {
                case FrameCaptureActionType::SetTemporalAntiAliasing:
                    m_gameRenderSettings
                        .temporalAntiAliasingEnabled = action.enabled;
                    m_sceneViewRequiresRefresh = true;
                    break;
                case FrameCaptureActionType::SetShadows:
                    m_gameRenderSettings.shadowsEnabled =
                        action.enabled;
                    m_sceneViewRequiresRefresh = true;
                    break;
                case FrameCaptureActionType::RefreshSceneView:
                    m_sceneViewRequiresRefresh = true;
                    break;
                case FrameCaptureActionType::ActivateStreamingScene:
                    sequenceStreamingActivationAllowed = true;
                    break;
                case FrameCaptureActionType::ActivateDemoScene:
                {
                    const auto sceneId =
                        Scene::DemoSceneCatalog::TryParse(action.demoScene);
                    Core::Check(sceneId.has_value(),
                        "Demo-scene capture action names an unknown scene.");
                    ActivateDemoScene(*sceneId);
                    break;
                }
                }
            }
            if (m_assetRuntimeCoordinator->IsStreamingEnabled())
            {
                Core::CpuTraceSpan streamingSpan(
                    "AssetStreamingTick",
                    "asset");
                (void)m_assetRuntimeCoordinator
                    ->ApplyCompletedStreamingBindings();
                if (sequenceStreamingActivationAllowed)
                    TryActivateStreamingScene();
                (void)m_sceneSession
                    ->SynchronizeRuntimeAssetBindingChanges();
            }
            // Simulation time is independent of TAA. In particular, the
            // forward Ocean Lab disables TAA but must keep advancing its FFT.
            // Captures use a fixed step so golden images remain repeatable.
            const double simulationTimeSeconds =
                Renderer::IsDeterministicRenderCaptureEnabled()
                ? static_cast<double>(logicalFrameId - 1u) / 60.0
                : m_frameTimer->GetTotalSeconds();
            const double simulationDeltaSeconds =
                Renderer::IsDeterministicRenderCaptureEnabled()
                ? 1.0 / 60.0
                : m_frameTimer->GetDeltaSeconds();
#if defined(PRISM_RENDER_HAS_EDITOR)
            // Resolve the Scene-view pipeline before freezing RenderView.
            // The packet must not observe settings from the previous frame.
            if (m_editorEnabled)
            {
                m_sceneRenderSettings = m_gameRenderSettings;
                Renderer::RenderSettings& sceneViewSettings =
                    m_sceneRenderSettings;
                sceneViewSettings.gameCameraEnabled = false;
                sceneViewSettings.viewportShadingMode =
                    m_editorCoordinator->GetSceneShadingMode();
                if (sceneViewSettings.viewportShadingMode
                    == Renderer::ViewportShadingMode::Unlit)
                {
                    sceneViewSettings.directLightingEnabled = false;
                    sceneViewSettings.pointLightsEnabled = false;
                    sceneViewSettings.spotLightsEnabled = false;
                    sceneViewSettings.iblEnabled = false;
                    sceneViewSettings.shadowsEnabled = false;
                    sceneViewSettings.gtaoEnabled = false;
                    sceneViewSettings.screenSpaceReflectionsEnabled = false;
                    sceneViewSettings.bloomEnabled = false;
                    sceneViewSettings.ambientIntensity = 1.0f;
                }
                else if (sceneViewSettings.viewportShadingMode
                         == Renderer::ViewportShadingMode::Wireframe)
                {
                    sceneViewSettings.temporalAntiAliasingEnabled = false;
                    sceneViewSettings.bloomEnabled = false;
                }
                sceneViewSettings.fluid.enabled =
                    sceneViewSettings.fluid.enabled
                    && sceneViewSettings.fluid.editorPreviewEnabled;
                sceneViewSettings.gpuDrivenEnabled = false;
                sceneViewSettings.frustumCullingEnabled = false;
                sceneViewSettings.occlusionCullingEnabled = false;
            }
#endif
            bool renderSceneView = false;
            bool captureSceneView = false;
            bool sceneViewVisible = false;
            SceneViewRefreshController::Decision
                sceneViewDecision{};
#if defined(PRISM_RENDER_HAS_EDITOR)
            if (m_editorEnabled)
            {
                sceneViewVisible = m_editorCoordinator
                    ->IsSceneViewportVisible();
                const bool sceneViewBecameVisible =
                    sceneViewVisible && !m_sceneViewWasVisible;
                const bool catalogSceneViewOnDemand =
                    Scene::DemoSceneCatalog::GetDescription(
                        m_sceneSession->GetIdentity().activeDemoScene)
                        .editorSceneViewUpdatePolicy
                    == Scene::EditorSceneViewUpdatePolicy::OnInteraction;
                const bool sceneViewInteracting =
                    m_cameraController->IsInteracting()
                    || m_editorCoordinator->IsGizmoActive()
                    || m_gameRenderSettings.terrainBrushActive;
                captureSceneView =
                    Renderer::ReadRenderEnvironmentVariable(
                        "PRISM_RENDER_CAPTURE_VIEW") == "scene";
                sceneViewDecision =
                    m_sceneViewRefreshController->Evaluate({
                        simulationTimeSeconds,
                        sceneViewVisible,
                        m_performanceGameOnly,
                        catalogSceneViewOnDemand,
                        m_sceneViewRequiresRefresh,
                        sceneViewBecameVisible,
                        sceneViewInteracting,
                        captureSceneView});
                renderSceneView = sceneViewDecision.render;
                m_sceneViewWasVisible = sceneViewVisible;
            }
#endif
            const Renderer::RenderHistorySettingsUpdate
                gameHistorySettings =
                    m_gameHistorySettingsTracker.Observe(
                        m_gameRenderSettings);
            m_sceneSession->BeginDynamicFrame(
                logicalFrameId,
                simulationTimeSeconds,
                simulationDeltaSeconds);
            m_sceneSession->SynchronizeDynamicLights();
            const bool activateGameCamera =
                m_editorEnabled
                || m_gameRenderSettings.gameCameraEnabled;
            m_sceneSession->SynchronizeDynamicView(
                Scene::GameRenderViewId,
                activateGameCamera
                    ? m_scene->GetGameCamera()
                    : m_scene->GetCamera(),
                m_gameOutputWidth,
                m_gameOutputHeight,
                gameHistorySettings.revision);
            if (m_editorEnabled)
            {
                const Renderer::RenderHistorySettingsUpdate
                    sceneHistorySettings =
                        m_sceneHistorySettingsTracker.Observe(
                            m_sceneRenderSettings);
                m_sceneSession->SynchronizeDynamicView(
                    Scene::SceneRenderViewId,
                    m_scene->GetCamera(),
                    m_sceneOutputWidth,
                    m_sceneOutputHeight,
                    sceneHistorySettings.revision);
            }
            const Scene::RenderSceneExtractionResult extractionResult =
                m_sceneSession->ExtractRenderSceneData();
            const std::shared_ptr<const Scene::RenderFramePacket>
                framePacket =
                    m_sceneSession->BuildRenderFramePacket(
                        extractionResult.sceneData);
            Scene::RenderSceneExtractionStatistics
                extractionStatistics{};
            if (scenePublicationStatistics.IsEnabled())
            {
                extractionStatistics.logicalFrameId =
                    logicalFrameId;
                extractionStatistics.sceneDataBuildCount =
                    extractionResult.rebuilt ? 1u : 0u;
                extractionStatistics.sceneDataReuseCount =
                    extractionResult.reused ? 1u : 0u;
                extractionStatistics.sceneGeneration =
                    extractionResult.sceneData
                        ->GetSceneGeneration().value;
                extractionStatistics.sceneDataRevision =
                    extractionResult.sceneData
                        ->GetDataRevision().value;
                extractionStatistics.sourceObjectVisitCount =
                    extractionResult.sourceObjectVisitCount;
                extractionStatistics.extractionReason =
                    extractionResult.reason;
                extractionStatistics.conservativeFallback =
                    extractionResult.conservativeFallback;
                extractionStatistics.frameEnvelopeCount = 1;
                extractionStatistics.framePacketSmallObjectCount = 1;
                const Scene::RenderSceneCopyEstimate
                    sourceCopyEstimate =
                        Scene::EstimateRenderSceneCopy(*m_scene);
                extractionStatistics.sourceObjectCount =
                    sourceCopyEstimate.objectCount;
                extractionStatistics.publicationFullCopyCount = 0;
            }
            const auto scenePublicationStart =
                FrameProfileClock::now();
            FramePerformanceRecorder::Scope publishScope(performance.get(), PerfMetric::Extraction);
            const std::uint64_t mailboxGeneration =
                m_renderSceneMailbox->PublishFramePacket(framePacket);
            const std::shared_ptr<const Scene::RenderFramePacket>
                publishedPacket =
                    m_renderSceneMailbox->AcquireLatestPacket();
            Core::Check(
                publishedPacket.get() == framePacket.get(),
                "RenderScene publication failed.");
            if (scenePublicationStatistics.IsEnabled())
            {
                extractionStatistics.publicationCpuMilliseconds =
                    std::chrono::duration<double, std::milli>(
                        FrameProfileClock::now()
                        - scenePublicationStart).count();
            }
            publishScope.End();
            AddProfileElapsed(
                frameProfile.cpuPhases[
                    ProfileIndex(FrameCpuPhase::ScenePublication)],
                scenePublicationStart);
            if (pendingFrameCompletion.has_value())
            {
                pendingPreparationOverlapMilliseconds =
                    std::chrono::duration<double, std::milli>(
                        FrameProfileClock::now()
                        - profileLoopStart).count();
                if (pendingProducerStart.has_value())
                {
                    pendingFrameIntervalMilliseconds =
                        std::chrono::duration<double, std::milli>(
                            profileLoopStart
                            - *pendingProducerStart).count();
                }
                const bool shouldExit =
                    (*pendingFrameCompletion)();
                pendingFrameCompletion.reset();
                pendingProducerStart.reset();
                pendingFrameIntervalMilliseconds.reset();
                if (shouldExit)
                {
                    break;
                }
            }
            const std::optional<RenderCaptureRequest> captureRequest =
                    m_captureAutomationController
                        ->PrepareFrameCapture(
                            logicalFrameId);
            FrameEnvelope frameEnvelope{};
            frameEnvelope.frame = framePacket;
            frameEnvelope.producerInputTimestampNanoseconds =
                producerInputTimestampNanoseconds;
            frameEnvelope.sceneEpoch = currentSceneEpoch;
            frameEnvelope.viewEpoch = currentViewEpoch;
            frameEnvelope.settingsRevision =
                gameHistorySettings.revision;
            frameEnvelope.profilingLevel =
                frameProfile.actualLevel;
            frameEnvelope.framePacingEnabled =
                performance != nullptr
                || frameProfile.actualLevel
                    != ProfilingLevel::Off;
            frameEnvelope.frameAdmission = frameAdmission;
            frameEnvelope.framePacingState = framePacingState;
            frameEnvelope.collectPerformanceStatistics =
                performance != nullptr;
            frameEnvelope.collectRenderGraphReport =
                m_captureAutomationController
                    ->ShouldCollectRenderGraphReport();
            frameEnvelope.collectGpuTimingReport =
                m_captureAutomationController
                    ->ShouldCollectGpuTimingReport();
            frameEnvelope.views.push_back({
                Scene::GameRenderViewId,
                m_gameRenderSettings,
                true});
            if (m_editorEnabled)
            {
                frameEnvelope.views.push_back({
                    Scene::SceneRenderViewId,
                    m_sceneRenderSettings,
                    renderSceneView});
            }
#if defined(PRISM_RENDER_HAS_EDITOR)
            frameEnvelope.uiDrawPacket = uiDrawPacket;
#endif
            if (captureRequest.has_value())
            {
                frameEnvelope.captureViewId = captureSceneView
                    ? Scene::SceneRenderViewId
                    : Scene::GameRenderViewId;
            }
            ValidateFrameEnvelope(frameEnvelope);

            const Scene::LogicalFrameId targetFrameId =
                framePacket->GetLogicalFrameId();
            if (pendingResize.has_value())
            {
                RenderControlTicket resizeTicket =
                    m_renderExecutionService->SubmitControl({
                        RenderControlCommandId{
                            nextRenderControlCommandId++},
                        targetFrameId,
                        frameEnvelope.sceneEpoch,
                        frameEnvelope.viewEpoch,
                        RenderControlBoundary::BeforeFrame,
                        *pendingResize});
                Check(resizeTicket.Wait().succeeded,
                    "Render lane rejected a swap-chain resize.");
                pendingResize.reset();
            }
            if (publishedSettingsRevision != 0
                && publishedSettingsRevision
                    != frameEnvelope.settingsRevision)
            {
                RenderControlTicket qualityTicket =
                    m_renderExecutionService->SubmitControl({
                        RenderControlCommandId{
                            nextRenderControlCommandId++},
                        targetFrameId,
                        frameEnvelope.sceneEpoch,
                        frameEnvelope.viewEpoch,
                        RenderControlBoundary::BeforeFrame,
                        ChangeRenderQualityCommand{
                            frameEnvelope.settingsRevision}});
                Check(qualityTicket.Wait().succeeded,
                    "Render lane rejected a settings revision change.");
            }
            if (captureRequest.has_value())
            {
                RenderControlTicket captureTicket =
                    m_renderExecutionService->SubmitControl({
                        RenderControlCommandId{
                            nextRenderControlCommandId++},
                        targetFrameId,
                        frameEnvelope.sceneEpoch,
                        frameEnvelope.viewEpoch,
                        RenderControlBoundary::BeforeFrame,
                        CaptureRenderFrameCommand{
                            captureRequest->requestId,
                            captureRequest->outputPath}});
                Check(captureTicket.Wait().succeeded,
                    "Render lane rejected an accepted capture request.");
            }

            const std::uint64_t acceptanceId =
                m_renderExecutionService->SubmitFrame(
                    std::move(frameEnvelope));
            auto completionWork = [&, acceptanceId,
                    logicalFrameId,
                    frameProfile = std::move(frameProfile),
                    profileLoopStart,
                    simulationTimeSeconds,
                    mailboxGeneration,
                    extractionStatistics =
                        std::move(extractionStatistics),
                    gameRenderSettings = m_gameRenderSettings,
                    sceneRenderSettings = m_sceneRenderSettings,
                    gameOutputWidth = m_gameOutputWidth,
                    gameOutputHeight = m_gameOutputHeight,
                    sceneOutputWidth = m_sceneOutputWidth,
                    sceneOutputHeight = m_sceneOutputHeight,
                    renderSceneView,
                    captureSceneView,
                    captureRequested = captureRequest.has_value(),
                    sceneViewVisible,
                    sceneViewDecision]() mutable -> bool
            {
            FrameCpuLaneProfile& mainLane =
                frameProfile.cpuLanes[
                    ProfileIndex(FrameCpuLane::Main)];
            FrameCpuLaneProfile& renderLane =
                frameProfile.cpuLanes[
                    ProfileIndex(FrameCpuLane::Render)];
            RenderFrameCompletion completedFrameFeedback{};
            frameProfile.pipeline.mainPreparationOverlap = {
                pendingPreparationOverlapMilliseconds,
                m_renderExecutionService->GetMode()
                    == RenderExecutionMode::Threaded
                    && pendingPreparationOverlapMilliseconds > 0.0};
            const auto renderCompletionWaitStart =
                FrameProfileClock::now();
            m_renderExecutionService->WaitUntilCompleted(
                acceptanceId);
            frameProfile.pipeline.completionWait = {
                std::chrono::duration<double, std::milli>(
                    FrameProfileClock::now()
                    - renderCompletionWaitStart).count(),
                true};
            std::vector<RenderFrameCompletion> completions =
                m_renderExecutionService
                    ->ConsumeFrameCompletions();
            Check(completions.size() == 1
                    && completions.front().acceptanceId
                        == acceptanceId,
                "Render execution completion did not match the accepted frame.");
            completedFrameFeedback =
                std::move(completions.front());
            latestRenderFeedback =
                m_renderExecutionService
                    ->ConsumeLatestFrameFeedback();
            Check(latestRenderFeedback.has_value()
                    && latestRenderFeedback->acceptanceId
                        == acceptanceId,
                "Render execution feedback did not match the accepted frame.");
            renderLane.threadId =
                latestRenderFeedback->executionThreadId;
            publishedSettingsRevision =
                completedFrameFeedback.settingsRevision;

            const RenderFrameExecutionTimings& executionTimings =
                latestRenderFeedback->timings;
            renderLane.active = {
                executionTimings.totalExecutionCpuMs,
                true};
            const RenderFramePipelineDiagnostics& pipeline =
                latestRenderFeedback->pipeline;
            frameProfile.pipeline.acceptanceId =
                latestRenderFeedback->acceptanceId;
            frameProfile.pipeline.logicalFrameId =
                latestRenderFeedback->logicalFrameId.value;
            frameProfile.pipeline.sceneEpoch =
                latestRenderFeedback->sceneEpoch.value;
            frameProfile.pipeline.viewEpoch =
                latestRenderFeedback->viewEpoch.value;
            frameProfile.pipeline.settingsRevision =
                latestRenderFeedback->settingsRevision;
            frameProfile.pipeline.waitingDepthAtAcceptance =
                static_cast<std::uint32_t>(pipeline.waitingDepthAtAcceptance);
            frameProfile.pipeline.peakWaitingDepth =
                static_cast<std::uint32_t>(pipeline.peakWaitingDepth);
            frameProfile.pipeline.queueWait = {
                pipeline.queueWaitCpuMs,
                true};
            frameProfile.pipeline.inputToPresent = {
                pipeline.inputToPresentCpuMs,
                pipeline.inputToPresentCpuMs > 0.0};
            AddProfileMilliseconds(
                frameProfile.cpuPhases[
                    ProfileIndex(FrameCpuPhase::BeginFrame)],
                executionTimings.beginFrameCpuMs);
            AddProfileMilliseconds(
                frameProfile.cpuPhases[
                    ProfileIndex(FrameCpuPhase::GameRender)],
                executionTimings.gameRenderCpuMs);
            AddProfileMilliseconds(
                frameProfile.cpuPhases[
                    ProfileIndex(FrameCpuPhase::SceneRender)],
                executionTimings.sceneRenderCpuMs);
            AddProfileMilliseconds(
                frameProfile.cpuPhases[
                    ProfileIndex(FrameCpuPhase::UiDraw)],
                executionTimings.uiDrawCpuMs);
            AddProfileMilliseconds(
                frameProfile.cpuPhases[
                    ProfileIndex(FrameCpuPhase::SubmitAndPresent)],
                executionTimings.submitAndPresentCpuMs);
            AddProfileMilliseconds(
                frameProfile.cpuPhases[
                    ProfileIndex(FrameCpuPhase::CaptureAndResolve)],
                executionTimings.captureAndResolveCpuMs);
            if (performance != nullptr)
            {
                performance->AddCpuMilliseconds(
                    PerfMetric::BeginFrame,
                    executionTimings.beginFrameCpuMs);
                performance->AddCpuMilliseconds(
                    PerfMetric::GameRender,
                    executionTimings.gameRenderCpuMs);
                performance->AddCpuMilliseconds(
                    PerfMetric::SceneRender,
                    executionTimings.sceneRenderCpuMs);
                performance->AddCpuMilliseconds(
                    PerfMetric::UiDraw,
                    executionTimings.uiDrawCpuMs);
                performance->AddCpuMilliseconds(
                    PerfMetric::Present,
                    executionTimings.submitAndPresentCpuMs);
            }
            if (Renderer::FrameDiagnostics* const diagnostics =
                    m_captureAutomationController
                        ->GetFrameDiagnostics();
                diagnostics != nullptr)
                diagnostics->BeginFrame(logicalFrameId,
                    latestRenderFeedback->frameContextIndex,
                    simulationTimeSeconds,
                    mailboxGeneration,
                    m_sceneSession->GetIdentity().sourceLabel);
            if (scenePublicationStatistics.IsEnabled())
            {
                extractionStatistics.gameViewFullCopyCount = 0;
                extractionStatistics.gameViewBuildCpuMilliseconds = 0.0;
            }
            const RenderViewRuntimeFeedback* const gameFeedback =
                latestRenderFeedback->FindView(
                    Scene::GameRenderViewId);
            Check(gameFeedback != nullptr,
                "Accepted frame did not produce Game-view feedback.");
            FrameViewProfile& gameViewProfile =
                frameProfile.views[
                    ProfileIndex(ProfiledView::Game)];
            gameViewProfile.active = true;
            gameViewProfile.rendered = true;
            gameViewProfile.refreshReason =
                SceneViewRefreshReason::Live;
            gameViewProfile.refreshPolicy =
                SceneViewRefreshPolicy::Live;
            gameViewProfile.width =
                gameOutputWidth;
            gameViewProfile.height =
                gameOutputHeight;
            gameViewProfile.cpuRender =
                frameProfile.cpuPhases[
                    ProfileIndex(FrameCpuPhase::GameRender)];
            const Renderer::RendererStatistics& gameStatistics =
                gameFeedback->statistics;
            AddProfileMilliseconds(
                frameProfile.cpuPhases[
                    ProfileIndex(FrameCpuPhase::RenderGraphBuild)],
                gameStatistics.renderGraphBuildCpuMs);
            AddProfileMilliseconds(
                frameProfile.cpuPhases[
                    ProfileIndex(FrameCpuPhase::RenderGraphExecute)],
                gameStatistics.renderGraphExecuteCpuMs);
            if (frameProfile.actualLevel != ProfilingLevel::Off)
            {
                AssociateViewGpuTiming(
                    *m_frameProfiler,
                    frameProfile,
                    ProfiledView::Game,
                    *gameFeedback,
                    logicalFrameId,
                    latestRenderFeedback->frameContextIndex);
            }
            if (performance)
            {
                RecordPerformanceView(
                    *performance,
                    *gameFeedback,
                    gameRenderSettings,
                    latestRenderFeedback->graphicsApi,
                    latestRenderFeedback->frameContextIndex,
                    gameOutputWidth,
                    gameOutputHeight,
                    "game",
                    simulationTimeSeconds,
                    mailboxGeneration);
            }
            if (Renderer::FrameDiagnostics* const diagnostics =
                    m_captureAutomationController
                        ->GetFrameDiagnostics();
                diagnostics != nullptr)
            {
                Check(gameFeedback->diagnostics.has_value(),
                    "Game-view diagnostics were not frozen on the render lane.");
                diagnostics->RecordView("game",
                    *gameFeedback->diagnostics);
            }
#if defined(PRISM_RENDER_HAS_EDITOR)
            // A docked ImGui viewport can be hidden by another tab, and
            // presentation-focused labs do not need a second full-resolution
            // ocean render while that editor camera is idle. Keep the last
            // Scene image as a snapshot and refresh it on interaction, edits,
            // resize, activation, or an explicit Scene capture.
            FrameViewProfile& sceneViewProfile =
                frameProfile.views[
                    ProfileIndex(ProfiledView::Scene)];
            sceneViewProfile.active =
                sceneViewVisible && !m_performanceGameOnly;
            sceneViewProfile.rendered = renderSceneView;
            sceneViewProfile.refreshPolicy =
                m_sceneViewRefreshController->GetPolicy();
            sceneViewProfile.width =
                m_editorEnabled
                    ? sceneOutputWidth
                    : 0;
            sceneViewProfile.height =
                m_editorEnabled
                    ? sceneOutputHeight
                    : 0;
            sceneViewProfile.refreshReason =
                sceneViewDecision.reason;
            const RenderViewRuntimeFeedback* const sceneFeedback =
                latestRenderFeedback->FindView(
                    Scene::SceneRenderViewId);
            Core::Check(
                (sceneFeedback != nullptr) == renderSceneView,
                "Frozen Scene-view activity disagrees with execution feedback.");
            if (renderSceneView)
            {
                if (scenePublicationStatistics.IsEnabled())
                {
                    extractionStatistics.sceneViewFullCopyCount = 0;
                    extractionStatistics.sceneViewBuildCpuMilliseconds = 0.0;
                }
                sceneViewProfile.cpuRender =
                    frameProfile.cpuPhases[
                        ProfileIndex(FrameCpuPhase::SceneRender)];
                const Renderer::RendererStatistics&
                    sceneStatistics =
                        sceneFeedback->statistics;
                AddProfileMilliseconds(
                    frameProfile.cpuPhases[
                        ProfileIndex(FrameCpuPhase::RenderGraphBuild)],
                    sceneStatistics.renderGraphBuildCpuMs);
                AddProfileMilliseconds(
                    frameProfile.cpuPhases[
                        ProfileIndex(FrameCpuPhase::RenderGraphExecute)],
                    sceneStatistics.renderGraphExecuteCpuMs);
                if (frameProfile.actualLevel
                    != ProfilingLevel::Off)
                {
                    AssociateViewGpuTiming(
                        *m_frameProfiler,
                        frameProfile,
                        ProfiledView::Scene,
                        *sceneFeedback,
                        logicalFrameId,
                        latestRenderFeedback->frameContextIndex);
                }
                if (performance)
                {
                    RecordPerformanceView(
                        *performance,
                        *sceneFeedback,
                        sceneRenderSettings,
                        latestRenderFeedback->graphicsApi,
                        latestRenderFeedback->frameContextIndex,
                        sceneOutputWidth,
                        sceneOutputHeight,
                        "scene",
                        simulationTimeSeconds,
                        mailboxGeneration);
                }
                if (Renderer::FrameDiagnostics* const diagnostics =
                        m_captureAutomationController
                            ->GetFrameDiagnostics();
                    diagnostics != nullptr)
                {
                    Check(sceneFeedback->diagnostics.has_value(),
                        "Scene-view diagnostics were not frozen on the render lane.");
                    diagnostics->RecordView("scene",
                        *sceneFeedback->diagnostics);
                }
                m_sceneViewRequiresRefresh = false;
            }
#endif
            if (captureRequested)
            {
                if (Renderer::FrameDiagnostics* const diagnostics =
                        m_captureAutomationController
                            ->GetFrameDiagnostics();
                    diagnostics != nullptr)
                    diagnostics->RecordCapture(
                        captureSceneView ? "scene" : "game");
            }
            scenePublicationStatistics.AddFrame(
                extractionStatistics);
        CpuTraceSpan captureResolveSpan(
            "CaptureAndTimingResolve",
            "readback");
        const auto captureAndResolveStart =
            FrameProfileClock::now();
        const CaptureResolveResult capture =
            m_captureAutomationController->ConsumeCompletedFrame(
                completedFrameFeedback);
        completedFrameCount =
            m_captureAutomationController->GetCompletedFrameCount();
        const std::string& captureError = capture.error;
        m_captureAutomationController
            ->WriteRenderGraphReportIfRequested(
                *latestRenderFeedback);
        m_captureAutomationController
            ->WriteGpuTimingReportIfReady(
                *latestRenderFeedback,
                m_sceneSession->GetIdentity().sourceLabel,
                m_adapterName);
        captureResolveSpan.End();
        AddProfileElapsed(
            frameProfile.cpuPhases[
                ProfileIndex(FrameCpuPhase::CaptureAndResolve)],
            captureAndResolveStart);
        if (performance)
        {
            CompletePerformanceFrame(
                *performance,
                *latestRenderFeedback,
                *m_window);
        }
        const auto profileAggregationStart =
            FrameProfileClock::now();
        const TaskExecutorStatistics& taskStatisticsAtFrameStart =
            latestRenderFeedback->taskStatisticsAtFrameStart;
        const TaskExecutorStatistics& taskStatisticsAtFrameEnd =
            latestRenderFeedback->taskStatisticsAtFrameEnd;
        const bool workerStatisticsAvailable =
            taskStatisticsAtFrameEnd.executorName == "pool";
        frameProfile.workers.available = workerStatisticsAvailable;
        frameProfile.workers.queued = {
            workerStatisticsAvailable
                ? NanosecondsToMilliseconds(SaturatingDelta(
                    taskStatisticsAtFrameEnd
                        .cumulativeQueueWaitNanoseconds,
                    taskStatisticsAtFrameStart
                        .cumulativeQueueWaitNanoseconds))
                : 0.0,
            workerStatisticsAvailable};
        frameProfile.workers.executing = {
            workerStatisticsAvailable
                ? NanosecondsToMilliseconds(SaturatingDelta(
                    taskStatisticsAtFrameEnd
                        .cumulativeExecutionNanoseconds,
                    taskStatisticsAtFrameStart
                        .cumulativeExecutionNanoseconds))
                : 0.0,
            workerStatisticsAvailable};
        frameProfile.workers.joining = {
            workerStatisticsAvailable
                ? NanosecondsToMilliseconds(SaturatingDelta(
                    taskStatisticsAtFrameEnd
                        .cumulativeJoinNanoseconds,
                    taskStatisticsAtFrameStart
                        .cumulativeJoinNanoseconds))
                : 0.0,
            workerStatisticsAvailable};
        frameProfile.workers.activeWorkers =
            static_cast<std::uint32_t>((std::min)(
                taskStatisticsAtFrameEnd.activeWorkerCount,
                static_cast<std::size_t>(
                    std::numeric_limits<std::uint32_t>::max())));
        frameProfile.workers.peakActiveWorkers =
            static_cast<std::uint32_t>((std::min)(
                taskStatisticsAtFrameEnd.peakActiveWorkerCount,
                static_cast<std::size_t>(
                    std::numeric_limits<std::uint32_t>::max())));
        frameProfile.workers.queuedTasks =
            static_cast<std::uint32_t>((std::min)(
                taskStatisticsAtFrameEnd.queuedTaskCount,
                static_cast<std::size_t>(
                    std::numeric_limits<std::uint32_t>::max())));
        frameProfile.workers.peakQueuedTasks =
            static_cast<std::uint32_t>((std::min)(
                taskStatisticsAtFrameEnd.peakQueuedTaskCount,
                static_cast<std::size_t>(
                    std::numeric_limits<std::uint32_t>::max())));
        FrameCpuLaneProfile& workerLane =
            frameProfile.cpuLanes[
                ProfileIndex(FrameCpuLane::Worker)];
        if (workerStatisticsAvailable)
        {
            // Worker timing is an aggregate across the fixed pool, so it has
            // no single OS thread identity. Keep the dedicated worker summary
            // authoritative and expose the same aggregate in the worker lane.
            workerLane.active = frameProfile.workers.executing;
            workerLane.waiting = frameProfile.workers.queued;
        }
        bool pacingAvailable = false;
        if (latestRenderFeedback->framePacingStatistics.has_value())
        {
            RecordFramePacing(
                frameProfile,
                *latestRenderFeedback->framePacingStatistics);
            pacingAvailable = true;
        }
        frameProfile.editorLoop = {
            pendingFrameIntervalMilliseconds.has_value()
                ? *pendingFrameIntervalMilliseconds
                : std::chrono::duration<double, std::milli>(
                    FrameProfileClock::now()
                    - profileLoopStart).count(),
            true};
        const double blockingWaitMilliseconds =
            GetBlockingWaitMilliseconds(frameProfile);
        const double admissionWaitMilliseconds =
            GetAdmissionWaitMilliseconds(frameProfile);
        const double frameExecutionWaitMilliseconds =
            GetFrameExecutionWaitMilliseconds(frameProfile);
        const double renderCompletionWaitMilliseconds =
            frameProfile.pipeline.completionWait.available
            ? frameProfile.pipeline.completionWait.milliseconds
            : 0.0;
        const bool threadedExecution =
            m_renderExecutionService->GetMode()
            == RenderExecutionMode::Threaded;
        if (pacingAvailable
            || frameProfile.pipeline.completionWait.available)
        {
            mainLane.waiting = {
                (threadedExecution ? admissionWaitMilliseconds
                    : blockingWaitMilliseconds)
                    + renderCompletionWaitMilliseconds,
                true};
            renderLane.waiting = {
                threadedExecution
                    ? blockingWaitMilliseconds
                    : 0.0,
                true};
        }
        mainLane.active = {
            std::max(
                0.0,
                frameProfile.editorLoop.milliseconds
                    - (threadedExecution
                        ? admissionWaitMilliseconds
                        : blockingWaitMilliseconds)
                    - renderCompletionWaitMilliseconds),
            true};
        if (threadedExecution && renderLane.active.available)
        {
            renderLane.active.milliseconds = std::max(
                0.0,
                renderLane.active.milliseconds
                    - frameExecutionWaitMilliseconds);
        }
        for (const FrameViewProfile& view : frameProfile.views)
        {
            if (view.gpuTotal.available
                && (!frameProfile.gpuFrame.available
                    || view.gpuTotal.milliseconds
                        > frameProfile.gpuFrame.milliseconds))
            {
                // Views are submitted sequentially today, but their resolved
                // samples can come from different historical CPU frames. The
                // frame field is therefore a critical-path hint, never a sum.
                frameProfile.gpuFrame = view.gpuTotal;
            }
        }
        frameProfile.profilerOverhead = {
            std::chrono::duration<double, std::milli>(
                FrameProfileClock::now()
                - profileAggregationStart).count(),
            true};
        m_frameProfiler->SubmitCompleted(
            std::move(frameProfile));
        if (frameProfilerReport != nullptr)
        {
            const auto completedProfile =
                m_frameProfiler->GetLatest();
            Check(completedProfile.has_value(),
                "Completed frame profiler snapshot was not retained.");
            frameProfilerReport->WriteCompleted(
                *completedProfile);
        }
        m_captureAutomationController->HandleCaptureError(
            captureError,
            *m_sceneSession);
        return m_captureAutomationController->ShouldExit();
            };
            if (m_renderExecutionService->GetMode()
                == RenderExecutionMode::Inline)
            {
                pendingPreparationOverlapMilliseconds = 0.0;
                pendingFrameIntervalMilliseconds.reset();
                if (completionWork())
                {
                    break;
                }
            }
            else
            {
                pendingFrameCompletion = std::move(completionWork);
                pendingProducerStart = profileLoopStart;
            }
        }
    }

    if (pendingFrameCompletion.has_value())
    {
        pendingPreparationOverlapMilliseconds = 0.0;
        pendingFrameIntervalMilliseconds.reset();
        (void)(*pendingFrameCompletion)();
        pendingFrameCompletion.reset();
        pendingProducerStart.reset();
    }
    m_captureAutomationController->RequireSequenceComplete();
    if (performance)
    {
        performance->Finish(maximumFrameCount);
    }
    if (frameProfilerReport != nullptr)
    {
        frameProfilerReport->Finish(maximumFrameCount);
    }
    scenePublicationStatistics.Write(
        m_sceneSession->GetIdentity().sourceLabel.c_str(),
        m_editorEnabled);
    if (completedFrameCount > 0)
    {
        const RenderControlAcknowledgement drain =
            m_renderExecutionService->SubmitControl({
                RenderControlCommandId{
                    nextRenderControlCommandId++},
                Scene::LogicalFrameId{completedFrameCount},
                currentSceneEpoch,
                currentViewEpoch,
                RenderControlBoundary::AfterFrame,
                DrainRenderExecutionCommand{}}).Wait();
        Check(drain.succeeded && drain.gpuDrained,
            "Render lane failed to drain completed GPU work.");
        m_gpuDrainedForShutdown = true;
    }
    const bool streamingEnabled =
        m_assetRuntimeCoordinator->IsStreamingEnabled();
    if (streamingEnabled)
    {
        RenderControlAcknowledgement renderWork{};
        if (completedFrameCount > 0)
        {
            renderWork = m_renderExecutionService->SubmitControl({
                RenderControlCommandId{
                    nextRenderControlCommandId++},
                Scene::LogicalFrameId{completedFrameCount},
                currentSceneEpoch,
                currentViewEpoch,
                RenderControlBoundary::AfterFrame,
                ProcessAssetRenderWorkCommand{
                    completedFrameCount + 1}}).Wait();
            Check(renderWork.succeeded,
                "Render lane rejected final asset render work.");
        }
        (void)m_assetRuntimeCoordinator
            ->ApplyCompletedStreamingBindings();
        if (renderWork.uploadedAssetCount > 0)
        {
            // The normal render workload is drained, but this final streaming
            // pump submitted new upload work that Shutdown must still await.
            m_gpuDrainedForShutdown = false;
        }
        TryActivateStreamingScene();
        (void)m_sceneSession
            ->SynchronizeRuntimeAssetBindingChanges();
    }
    const std::optional<std::filesystem::path>
        assetStreamingReportPath =
            m_captureAutomationController
                ->GetAssetStreamingReportPath(streamingEnabled);
    if (assetStreamingReportPath.has_value())
    {
        Check(completedFrameCount > 0,
            "Asset streaming report requires a completed render frame.");
        const RenderControlAcknowledgement report =
            m_renderExecutionService->SubmitControl({
                RenderControlCommandId{
                    nextRenderControlCommandId++},
                Scene::LogicalFrameId{completedFrameCount},
                currentSceneEpoch,
                currentViewEpoch,
                RenderControlBoundary::AfterFrame,
                WriteAssetStreamingReportCommand{
                    *assetStreamingReportPath}}).Wait();
        Check(report.succeeded
                && report.assetStreamingReportWritten,
            "Render lane failed to write the asset streaming report.");
    }
    m_captureAutomationController->WriteStreamingSceneReport(
        *m_assetRuntimeCoordinator,
        m_graphicsApiName,
        *m_scene);
    return EXIT_SUCCESS;
}

Asset::AssetImportResult ApplicationHost::ImportEditorAsset(
    const std::filesystem::path& sourcePath,
    const bool reimport)
{
    const RenderControlAcknowledgement acknowledgement =
        m_renderExecutionService->SubmitControl({
            RenderControlCommandId{
                m_nextRenderControlCommandId++},
            Scene::LogicalFrameId{
                m_captureAutomationController
                    ->GetAcceptedFrameCount() + 1u},
            m_currentSceneEpoch,
            m_currentViewEpoch,
            RenderControlBoundary::BeforeFrame,
            ImportRenderAssetCommand{
                sourcePath,
                reimport}}).Wait();
    Check(acknowledgement.succeeded
            && acknowledgement.assetImportResult.has_value(),
        "Render lane failed to return an asset import result.");
    Asset::AssetImportResult result =
        *acknowledgement.assetImportResult;
    if (result.success)
    {
        (void)m_sceneSession
            ->SynchronizeRuntimeAssetBindingChanges();
        m_sceneSession->MarkWorldChanged();
    }
    return result;
}

Engine::EntityId ApplicationHost::InstantiateEditorAsset(
    const Asset::AssetRecord& asset)
{
    Engine::EntityId firstEntity{};
    const Asset::AssetDatabase* const importDatabase =
        m_assetRuntimeCoordinator->GetImportDatabase();
    if (importDatabase == nullptr
        || (asset.type != Asset::AssetType::Scene
            && asset.type != Asset::AssetType::Mesh))
    {
        return firstEntity;
    }

    const Asset::AssetDatabase& database = *importDatabase;
    const auto execute = [&] (
        const std::string_view operation,
        const char* command,
        nlohmann::json arguments)
    {
        return m_sceneSession->ExecuteCommand(
            operation,
            command,
            std::move(arguments));
    };
    const auto fail = [&](const nlohmann::json& result)
    {
        (void)execute(
            "asset-instance-rollback",
            "transaction.rollback",
            nlohmann::json::object());
        m_sceneSession->SetLoadMessage(result.contains("error")
            ? result.at("error").value(
                "message",
                std::string("Asset instantiation failed."))
            : std::string("Asset instantiation failed."));
        return Engine::EntityId{};
    };
    const auto materialForMesh = [&](
        const std::string_view meshAssetId)
    {
        for (const Asset::AssetRecord& scene :
             database.List(Asset::AssetType::Scene))
        {
            for (const nlohmann::json& instance :
                 scene.metadata.value(
                     "instances", nlohmann::json::array()))
            {
                if (instance.value(
                        "meshAssetId", std::string{})
                    != meshAssetId)
                {
                    continue;
                }
                if (const Asset::AssetRecord* material =
                        database.FindById(instance.value(
                            "materialAssetId",
                            std::string{})))
                {
                    return material->assetPath;
                }
            }
        }
        for (const Asset::AssetRecord& material :
             database.List(Asset::AssetType::Material))
        {
            if (material.sourcePath == asset.sourcePath)
            {
                return material.assetPath;
            }
        }
        return std::string(
            "builtin://editor-preview/materials/Preview_Neutral");
    };

    nlohmann::json result = execute(
        "asset-instance-begin",
        "transaction.begin",
        nlohmann::json::object());
    if (!result.value("success", false))
    {
        m_sceneSession->SetLoadMessage(result.at("error").value(
            "message",
            std::string("Could not begin asset instantiation.")));
        return firstEntity;
    }

    const auto createMeshEntity = [&] (
        const std::string& name,
        const std::string& meshPath,
        const std::string& materialPath,
        const Scene::Transform& transform) -> bool
    {
        result = execute(
            "asset-instance-create",
            "entity.create",
            {{"name", name.empty() ? "Imported Mesh" : name}});
        if (!result.value("success", false))
        {
            return false;
        }
        const std::optional<Engine::EntityId> entity =
            Engine::EntityId::Parse(
                result.at("data").at("entity")
                    .get<std::string>());
        if (!entity.has_value())
        {
            result = {
                {"success", false},
                {"error", {
                    {"message", "The created entity returned an invalid identifier."}}}};
            return false;
        }
        if (!firstEntity.IsValid())
        {
            firstEntity = *entity;
        }

        const XMFLOAT3& position = transform.GetPosition();
        const XMFLOAT3& rotation =
            transform.GetRotationEulerRadians();
        const XMFLOAT3& scale = transform.GetScale();
        result = execute(
            "asset-instance-transform",
            "component.set",
            {{"entity", entity->ToString()},
             {"component", "Transform"},
             {"properties", {
                 {"position", {
                     position.x, position.y, position.z}},
                 {"rotation", {
                     rotation.x, rotation.y, rotation.z}},
                 {"scale", {
                     scale.x, scale.y, scale.z}}}}});
        if (!result.value("success", false))
        {
            return false;
        }
        result = execute(
            "asset-instance-renderer",
            "component.add",
            {{"entity", entity->ToString()},
             {"component", "MeshRenderer"},
             {"properties", {
                 {"meshAsset", meshPath},
                 {"materialAsset", materialPath},
                 {"visible", true}}}});
        return result.value("success", false);
    };

    try
    {
        if (asset.type == Asset::AssetType::Mesh)
        {
            const Scene::Transform identity{};
            if (!createMeshEntity(
                    asset.name,
                    asset.assetPath,
                    materialForMesh(asset.assetId),
                    identity))
            {
                return fail(result);
            }
        }
        else
        {
            const nlohmann::json instances =
                asset.metadata.value(
                    "instances", nlohmann::json::array());
            if (!instances.is_array() || instances.empty())
            {
                result = {
                    {"success", false},
                    {"error", {
                        {"message", "The Scene asset has no renderable instances."}}}};
                return fail(result);
            }
            for (const nlohmann::json& instance : instances)
            {
                const Asset::AssetRecord* mesh =
                    database.FindById(instance.at(
                        "meshAssetId").get<std::string>());
                const Asset::AssetRecord* material =
                    database.FindById(instance.at(
                        "materialAssetId").get<std::string>());
                const nlohmann::json& serializedMatrix =
                    instance.at("worldMatrix");
                if (mesh == nullptr || material == nullptr
                    || !serializedMatrix.is_array()
                    || serializedMatrix.size() != 16)
                {
                    result = {
                        {"success", false},
                        {"error", {{"message",
                            "The Scene asset contains an invalid instance binding."}}}};
                    return fail(result);
                }
                const XMFLOAT4X4 matrix{
                    serializedMatrix[0].get<float>(),
                    serializedMatrix[1].get<float>(),
                    serializedMatrix[2].get<float>(),
                    serializedMatrix[3].get<float>(),
                    serializedMatrix[4].get<float>(),
                    serializedMatrix[5].get<float>(),
                    serializedMatrix[6].get<float>(),
                    serializedMatrix[7].get<float>(),
                    serializedMatrix[8].get<float>(),
                    serializedMatrix[9].get<float>(),
                    serializedMatrix[10].get<float>(),
                    serializedMatrix[11].get<float>(),
                    serializedMatrix[12].get<float>(),
                    serializedMatrix[13].get<float>(),
                    serializedMatrix[14].get<float>(),
                    serializedMatrix[15].get<float>()};
                Scene::Transform transform;
                if (!transform.SetWorldMatrix(matrix)
                    || !createMeshEntity(
                        instance.value("name", mesh->name),
                        mesh->assetPath,
                        material->assetPath,
                        transform))
                {
                    if (result.value("success", true))
                    {
                        result = {
                            {"success", false},
                            {"error", {{"message",
                                "A Scene instance has a non-decomposable transform."}}}};
                    }
                    return fail(result);
                }
            }
        }
    }
    catch (const std::exception& exception)
    {
        result = {
            {"success", false},
            {"error", {{"message", exception.what()}}}};
        return fail(result);
    }

    result = execute(
        "asset-instance-commit",
        "transaction.commit",
        nlohmann::json::object());
    if (!result.value("success", false))
    {
        return fail(result);
    }
    m_sceneSession->SetLoadMessage(
        asset.name + " added to the Engine World.");
    m_sceneSession->MarkWorldChanged();
    return firstEntity;
}

void ApplicationHost::Shutdown() noexcept
{
    if (m_lifecycleState == LifecycleState::Uninitialized
        || m_lifecycleState == LifecycleState::ShuttingDown)
    {
        return;
    }
    m_lifecycleState = LifecycleState::ShuttingDown;

    const bool waitForGpu = m_backendInitialized
        && !m_gpuDrainedForShutdown;
    try
    {
        ProcessDiagnostics::Log(
            "info",
            "renderer.shutdown.begin",
            "ApplicationHost shutdown started.",
            {{"waitForGpu", waitForGpu}});
    }
    catch (...)
    {
    }
#if defined(PRISM_RENDER_HAS_EDITOR)
    if (m_editorCoordinator != nullptr)
    {
        // A main-lane UI exception may leave the packet-build critical
        // section open. Release it before waiting for render-lane shutdown.
        m_editorCoordinator->AbortFrameBuild();
    }
#endif
    // The renderers and asset registry retain their own references until the
    // execution target shuts down. Drop the host's extra reference now so the
    // final D3D12 texture release cannot occur after backend destruction.
    m_environmentCubemap.reset();
    // The service must stop while every dependency captured by the resident
    // execution target and its UI callbacks is still alive.
    if (m_renderExecutionService != nullptr)
    {
        m_renderExecutionService->Shutdown();
    }
    try
    {
        ProcessDiagnostics::Log(
            "debug",
            "renderer.shutdown.execution_complete",
            "Render execution service stopped.");
    }
    catch (...)
    {
    }
    m_captureAutomationController.reset();
    m_cameraController = nullptr;
    m_gameCameraController = nullptr;
    m_commandProcessor = nullptr;
    m_renderSceneMailbox = nullptr;
    m_scene = nullptr;
    m_sceneSession.reset();
    m_sceneViewRenderer = nullptr;
    m_sceneRenderer = nullptr;
    m_renderFrameCoordinator = nullptr;
    m_backend = nullptr;
    m_renderExecutionService.reset();
    m_renderExecutionTarget = nullptr;
    try
    {
        ProcessDiagnostics::Log(
            "debug",
            "renderer.shutdown.runtime_released",
            "Render runtime and scene owners released.");
    }
    catch (...)
    {
    }
#if defined(PRISM_RENDER_HAS_EDITOR)
    if (m_editorCoordinator != nullptr)
    {
        m_editorCoordinator->Shutdown();
    }
    m_editorCoordinator.reset();
#endif
    try
    {
        ProcessDiagnostics::Log(
            "debug",
            "renderer.shutdown.editor_released",
            "Editor UI owner released.");
    }
    catch (...)
    {
    }
    m_assetRuntimeCoordinator.reset();
    m_window.reset();
    if (m_frameProfiler != nullptr)
    {
        try
        {
            m_frameProfiler->Shutdown();
        }
        catch (...)
        {
        }
        m_frameProfiler.reset();
    }
    m_sceneViewRefreshController.reset();
    m_frameTimer.reset();
    m_environmentMapStatus.clear();
    m_editorScenePath.clear();
    m_gltfImportEnabled = false;
    m_editorEnabled = false;
    m_performanceGameOnly = false;
    m_demoSceneSwitchingEnabled = true;
    m_sceneViewRequiresRefresh = true;
    m_sceneViewWasVisible = false;
    m_backendInitialized = false;
    m_gpuDrainedForShutdown = false;
    m_lifecycleState = LifecycleState::Uninitialized;
    try
    {
        ProcessDiagnostics::Log(
            "info",
            "renderer.shutdown.completed",
            "ApplicationHost shutdown completed.");
    }
    catch (...)
    {
    }
}
} // namespace Prism::Core
