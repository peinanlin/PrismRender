#include "UI/EditorCoordinator.h"

#include "Asset/AssetRegistry.h"
#include "Core/Assert.h"
#include "Core/Profiling/FrameProfiler.h"
#include "Core/Profiling/SceneViewRefreshController.h"
#include "Platform/Window.h"
#include "Renderer/RendererStatistics.h"
#include "Renderer/SceneRenderer.h"
#include "Scene/CameraController.h"
#include "Scene/DemoSceneCatalog.h"
#include "Scene/RenderScene.h"
#include "Scene/SceneSession.h"
#include "UI/DebugPanel.h"
#include "UI/ImGuiFactory.h"
#include "UI/ImGuiSystem.h"
#include "UI/PerformanceProfilerPanel.h"

#include <DirectXMath.h>

#include <algorithm>
#include <sstream>
#include <utility>

namespace Prism::UI
{
namespace
{
constexpr std::size_t MaxListedObjects = 12;

ViewportPerformanceStats BuildViewportPerformanceStats(
    const Renderer::RendererStatistics& rendererStatistics,
    const float deltaTimeMs,
    const float framesPerSecond,
    const Core::FrameProfiler& frameProfiler)
{
    ViewportPerformanceStats stats{};
    stats.framesPerSecond = framesPerSecond;
    stats.frameTimeMs = deltaTimeMs;
    stats.rendererCpuMs = rendererStatistics.totalRendererCpuMs;
    stats.rendererGpuMs = rendererStatistics.totalRendererGpuMs;
    stats.drawCalls = rendererStatistics.drawCalls;
    stats.shadowDrawCalls = rendererStatistics.shadowDrawCalls;
    stats.directionalShadowDrawCalls =
        rendererStatistics.directionalShadowDrawCalls;
    stats.localShadowDrawCalls = rendererStatistics.localShadowDrawCalls;
    stats.shadowLayersRendered = rendererStatistics.shadowLayersRendered;
    stats.shadowLayersCached = rendererStatistics.shadowLayersCached;
    stats.geometryDrawCalls = rendererStatistics.forwardDrawCalls;
    stats.postProcessDrawCalls = rendererStatistics.postProcessDrawCalls;
    stats.visibleObjects = rendererStatistics.visibleObjects;
    stats.renderedObjects = rendererStatistics.renderedObjects;
    stats.culledObjects = rendererStatistics.culledObjects;

    const auto completedProfile = frameProfiler.GetLatest();
    if (!completedProfile.has_value())
    {
        return stats;
    }

    const Core::FrameProfilerSnapshot& completed = *completedProfile;
    if (completed.editorLoop.available
        && completed.editorLoop.milliseconds > 0.0)
    {
        stats.frameTimeMs = static_cast<float>(
            completed.editorLoop.milliseconds);
        stats.framesPerSecond = 1000.0f / stats.frameTimeMs;
    }
    const Core::FrameCpuLaneProfile& mainLane =
        completed.cpuLanes[static_cast<std::size_t>(
            Core::FrameCpuLane::Main)];
    const Core::FrameCpuLaneProfile& renderLane =
        completed.cpuLanes[static_cast<std::size_t>(
            Core::FrameCpuLane::Render)];
    const Core::FrameCpuLaneProfile& workerLane =
        completed.cpuLanes[static_cast<std::size_t>(
            Core::FrameCpuLane::Worker)];
    stats.mainCpuAvailable = mainLane.active.available;
    stats.mainCpuMs = static_cast<float>(mainLane.active.milliseconds);
    stats.renderCpuAvailable = renderLane.active.available;
    stats.renderCpuMs = static_cast<float>(renderLane.active.milliseconds);
    stats.workerCpuAvailable = workerLane.active.available;
    stats.workerCpuMs = static_cast<float>(workerLane.active.milliseconds);
    stats.gpuFrameAvailable = completed.gpuFrame.available;
    stats.gpuFrameMs = static_cast<float>(
        completed.gpuFrame.milliseconds);
    const Core::FrameProfileDuration& submitWait =
        completed.waits[static_cast<std::size_t>(
            Core::FrameWaitPhase::Submit)];
    const Core::FrameProfileDuration& presentWait =
        completed.waits[static_cast<std::size_t>(
            Core::FrameWaitPhase::NativePresent)];
    stats.submitPresentWaitAvailable =
        submitWait.available || presentWait.available;
    stats.submitPresentWaitMs = static_cast<float>(
        submitWait.milliseconds + presentWait.milliseconds);
    stats.activeViewMask = completed.activeViewMask;
    stats.profilingLevel = Core::ToString(completed.actualLevel);
    return stats;
}

DebugPanel::FrameStats BuildDebugFrameStats(
    const EditorCoordinatorFrameInput& input)
{
    DebugPanel::FrameStats stats{};
    const Scene::RenderScene& scene =
        input.sceneSession.GetRenderScene();
    const Scene::SceneSessionIdentity& identity =
        input.sceneSession.GetIdentity();
    stats.frameIndex = input.frameIndex;
    stats.windowWidth = input.windowWidth;
    stats.windowHeight = input.windowHeight;
    stats.adapterName = input.adapterName;
    stats.graphicsApiName = input.graphicsApiName;
    stats.renderObjectCount = static_cast<std::uint32_t>(
        scene.GetRenderObjects().size());
    stats.activePointLightCount = scene.GetActivePointLightCount();
    stats.activeSpotLightCount = scene.GetActiveSpotLightCount();
    stats.meshAssetCount = input.assetRegistry.GetMeshAssetCount();
    stats.textureAssetCount = input.assetRegistry.GetTextureAssetCount();
    stats.materialAssetCount = input.assetRegistry.GetMaterialAssetCount();
    stats.sceneSourceLabel = identity.sourceLabel;
    stats.sceneLoadMessage = identity.loadMessage;
    stats.environmentMapStatus = input.environmentMapStatus;
    stats.gltfImportEnabled = input.gltfImportEnabled;
    stats.usingFallbackScene = identity.usingFallbackScene;
    stats.activeDemoSceneIndex = static_cast<std::uint32_t>(
        identity.activeDemoScene);
    stats.demoSceneSwitchingEnabled =
        input.demoSceneSwitchingEnabled;
    if (input.streaming.enabled)
    {
        stats.assetStreamingEnabled = true;
        stats.streamingQueuedCount = input.streaming.queuedCount;
        stats.streamingResidentCount = input.streaming.residentCount;
        stats.streamingEvictedCount = input.streaming.evictedCount;
        stats.streamingFailedCount = input.streaming.failedCount;
        stats.streamingResidentBytes = input.streaming.residentBytes;
        stats.streamingBudgetBytes = input.streaming.budgetBytes;
        stats.streamingCompletedUploads = input.streaming.completedUploads;
        stats.streamingEvictionCount = input.streaming.evictionCount;
    }
    for (const Scene::DemoSceneDescription& description :
         Scene::DemoSceneCatalog::GetDescriptions())
    {
        stats.demoScenes.push_back({
            std::string(description.displayName),
            std::string(description.purpose)});
    }
    const std::vector<Scene::RenderObject>& objects =
        scene.GetRenderObjects();
    stats.sceneObjectSummaries.reserve(
        std::min(objects.size(), MaxListedObjects)
        + (objects.size() > MaxListedObjects ? 1u : 0u));
    for (std::size_t index = 0;
         index < objects.size() && index < MaxListedObjects;
         ++index)
    {
        const Scene::RenderObject& object = objects[index];
        const DirectX::XMFLOAT3& position =
            object.transform.GetPosition();
        std::ostringstream builder;
        builder
            << (object.name.empty()
                    ? "Object_" + std::to_string(index)
                    : object.name)
            << " | mesh=" << object.meshHandle.Value()
            << " material=" << object.materialHandle.Value()
            << " visible=" << (object.visible ? "true" : "false")
            << " pos=(" << position.x << ", " << position.y
            << ", " << position.z << ")";
        stats.sceneObjectSummaries.push_back(builder.str());
    }
    if (objects.size() > MaxListedObjects)
    {
        stats.sceneObjectSummaries.push_back(
            "... "
            + std::to_string(objects.size() - MaxListedObjects)
            + " more objects");
    }
    const Scene::Camera& camera = scene.GetCamera();
    stats.cameraPosition = camera.GetPosition();
    stats.cameraPitch = DirectX::XMConvertToDegrees(camera.GetPitch());
    stats.cameraYaw = DirectX::XMConvertToDegrees(camera.GetYaw());
    stats.cameraFovYDegrees = DirectX::XMConvertToDegrees(
        camera.GetFieldOfViewYRadians());
    stats.cameraNearPlane = camera.GetNearPlane();
    stats.cameraFarPlane = camera.GetFarPlane();
    stats.cameraMoveSpeed =
        input.sceneSession.GetSceneCameraController().GetMoveSpeed();
    stats.cameraLookActive =
        input.sceneSession.GetSceneCameraController().IsLooking();
    return stats;
}
} // namespace

EditorCoordinatorFrameInput::EditorCoordinatorFrameInput(
    Scene::SceneSession& sceneSessionValue,
    const Asset::AssetRegistry& assetRegistryValue,
    Renderer::RenderSettings& gameSettingsValue,
    const std::uint32_t gameViewWidthValue,
    const std::uint32_t gameViewHeightValue,
    const std::uint32_t sceneViewWidthValue,
    const std::uint32_t sceneViewHeightValue,
    Core::FrameProfiler& frameProfilerValue,
    Core::SceneViewRefreshController& sceneViewRefreshValue,
    Platform::Window& windowValue) noexcept
    : sceneSession(sceneSessionValue),
      assetRegistry(assetRegistryValue),
      gameSettings(gameSettingsValue),
      gameViewWidth(gameViewWidthValue),
      gameViewHeight(gameViewHeightValue),
      sceneViewWidth(sceneViewWidthValue),
      sceneViewHeight(sceneViewHeightValue),
      frameProfiler(frameProfilerValue),
      sceneViewRefresh(sceneViewRefreshValue),
      window(windowValue)
{
}

std::unique_ptr<EditorCoordinator> EditorCoordinator::Create(
    const RHI::GraphicsApi graphicsApi)
{
    std::unique_ptr<ImGuiSystem> imgui =
        CreateImGuiSystem(graphicsApi);
    if (imgui == nullptr)
    {
        return nullptr;
    }
    return std::unique_ptr<EditorCoordinator>(
        new EditorCoordinator(std::move(imgui)));
}

EditorCoordinator::EditorCoordinator(
    std::unique_ptr<ImGuiSystem> imguiSystem)
    : m_debugPanel(std::make_unique<DebugPanel>()),
      m_editorLayer(std::make_unique<EditorLayer>()),
      m_imguiSystem(std::move(imguiSystem)),
      m_performanceProfilerPanel(
          std::make_unique<PerformanceProfilerPanel>())
{
}

EditorCoordinator::~EditorCoordinator()
{
    Shutdown();
}

void EditorCoordinator::Initialize(
    Platform::Window& window,
    RHI::IRenderBackend& backend,
    Renderer::SceneRenderer& gameRenderer,
    Renderer::SceneRenderer& sceneRenderer,
    const bool useGameTextureForScene)
{
    m_imguiSystem->Initialize(window, backend);
    RefreshViewportTextures(
        gameRenderer,
        sceneRenderer,
        useGameTextureForScene);
}

void EditorCoordinator::RefreshViewportTextures(
    Renderer::SceneRenderer& gameRenderer,
    Renderer::SceneRenderer& sceneRenderer,
    const bool useGameTextureForScene)
{
    Core::Check(
        gameRenderer.GetFinalOutputSampledView() != nullptr
            && sceneRenderer.GetFinalOutputSampledView() != nullptr,
        "The editor renderers did not create both viewport outputs.");
    m_imguiSystem->SetViewportTextures(
        useGameTextureForScene
            ? gameRenderer.GetFinalOutputSampledView()
            : sceneRenderer.GetFinalOutputSampledView(),
        gameRenderer.GetFinalOutputSampledView());
}

EditorCoordinatorFrameResult EditorCoordinator::DrawFrame(
    EditorCoordinatorFrameInput& input)
{
    m_imguiSystem->BeginFrame();
    EditorCoordinatorFrameResult result{};
    DebugPanel::FrameStats stats = BuildDebugFrameStats(input);
    EditorSessionActionBundle actions = BuildSessionActions(
        input.sceneSession,
        input.assetRegistry,
        input.documentPath,
        std::move(input.assetActions));
    const Renderer::RendererStatistics emptyStatistics{};
    const Renderer::RendererStatistics& rendererStatistics =
        input.completedGameStatistics != nullptr
        ? *input.completedGameStatistics
        : emptyStatistics;
    const ViewportPerformanceStats viewportPerformance =
        BuildViewportPerformanceStats(
            rendererStatistics,
            input.deltaTimeMs,
            input.framesPerSecond,
            input.frameProfiler);
    const Scene::DemoSceneId activeDemoScene =
        input.sceneSession.GetIdentity().activeDemoScene;
    const bool oceanLabActive =
        activeDemoScene == Scene::DemoSceneId::OceanLab
        || activeDemoScene == Scene::DemoSceneId::WaveWorksLab
        || activeDemoScene == Scene::DemoSceneId::HpWaterOceanLab;
    const bool waveWorksActive =
        activeDemoScene == Scene::DemoSceneId::WaveWorksLab;
    const bool fluidLabActive =
        activeDemoScene == Scene::DemoSceneId::PbfLab
        || activeDemoScene == Scene::DemoSceneId::FluidRenderLab
        || activeDemoScene == Scene::DemoSceneId::FluidCausticsLab
        || activeDemoScene == Scene::DemoSceneId::FluidToonLab;

    Scene::RenderScene& scene =
        input.sceneSession.GetRenderScene();
    m_editorLayer->Draw(
        scene,
        input.gameSettings,
        input.sceneSession.GetSceneCameraController(),
        input.sceneSession.GetGameCameraController(),
        m_imguiSystem->GetSceneTextureId(),
        input.sceneViewWidth,
        input.sceneViewHeight,
        m_imguiSystem->GetGameTextureId(),
        input.gameViewWidth,
        input.gameViewHeight,
        viewportPerformance,
        rendererStatistics,
        oceanLabActive,
        waveWorksActive,
        fluidLabActive,
        actions.document,
        actions.commands,
        actions.assets);
    // EditorLayer establishes DockSpace and viewport windows first.
    result.requestedFramePacing = m_performanceProfilerPanel->Draw(
        input.frameProfiler,
        input.sceneViewRefresh);
    if (input.sceneSession.IsWorldDirty())
    {
        input.sceneSession.SynchronizePendingWorldChanges();
        result.sceneViewRequiresRefresh = true;
    }
    if (const std::optional<std::uint32_t> requested =
            m_debugPanel->Draw(
                stats,
                input.gameSettings,
                rendererStatistics);
        requested.has_value()
        && *requested < static_cast<std::uint32_t>(
            Scene::DemoSceneId::Count))
    {
        result.requestedDemoScene =
            static_cast<Scene::DemoSceneId>(*requested);
    }

    if (!input.deterministicCaptureEnabled)
    {
        Scene::CameraController& sceneController =
            input.sceneSession.GetSceneCameraController();
        Scene::CameraController& gameController =
            input.sceneSession.GetGameCameraController();
        switch (SelectNavigationTarget(
            sceneController.IsInteracting(),
            gameController.IsInteracting(),
            m_editorLayer->IsGizmoActive(),
            m_editorLayer->IsGameViewportHovered(),
            m_editorLayer->IsSceneViewportHovered()))
        {
        case EditorNavigationTarget::Game:
            gameController.Update(
                scene.GetGameCamera(),
                input.window,
                input.navigationDeltaSeconds,
                true);
            break;
        case EditorNavigationTarget::Scene:
            sceneController.Update(
                scene.GetCamera(),
                input.window,
                input.navigationDeltaSeconds,
                true);
            break;
        case EditorNavigationTarget::None:
            (void)input.window.ConsumeScrollDelta();
            break;
        }
    }
    return result;
}

std::shared_ptr<const UiDrawPacket>
EditorCoordinator::BuildDrawPacket()
{
    return m_imguiSystem->BuildDrawPacket();
}

void EditorCoordinator::PollPlatformEvents(
    Platform::Window& window)
{
    m_imguiSystem->PollPlatformEvents(window);
}

void EditorCoordinator::AbortFrameBuild() noexcept
{
    if (m_imguiSystem != nullptr)
    {
        m_imguiSystem->AbortFrameBuild();
    }
}

void EditorCoordinator::Render(
    const UiDrawPacket& drawPacket)
{
    m_imguiSystem->Render(drawPacket);
}

void EditorCoordinator::Shutdown() noexcept
{
    if (m_imguiSystem != nullptr)
    {
        m_imguiSystem->Shutdown();
    }
    m_performanceProfilerPanel.reset();
    m_editorLayer.reset();
    m_debugPanel.reset();
    m_imguiSystem.reset();
}

bool EditorCoordinator::IsSceneViewportVisible() const noexcept
{
    return m_editorLayer != nullptr
        && m_editorLayer->IsSceneViewportVisible();
}

bool EditorCoordinator::IsGizmoActive() const noexcept
{
    return m_editorLayer != nullptr && m_editorLayer->IsGizmoActive();
}

Renderer::ViewportShadingMode
EditorCoordinator::GetSceneShadingMode() const noexcept
{
    return m_editorLayer->GetSceneShadingMode();
}

EditorNavigationTarget EditorCoordinator::SelectNavigationTarget(
    const bool sceneInteracting,
    const bool gameInteracting,
    const bool gizmoActive,
    const bool gameViewportHovered,
    const bool sceneViewportHovered) noexcept
{
    const bool controlGame = gameInteracting
        || (!gizmoActive && !sceneInteracting
            && gameViewportHovered);
    if (controlGame)
    {
        return EditorNavigationTarget::Game;
    }
    if (sceneInteracting
        || (!gizmoActive && sceneViewportHovered))
    {
        return EditorNavigationTarget::Scene;
    }
    return EditorNavigationTarget::None;
}

EditorSessionActionBundle EditorCoordinator::BuildSessionActions(
    Scene::SceneSession& session,
    const Asset::AssetRegistry& runtimeAssets,
    const std::filesystem::path& documentPath,
    EditorAssetActions assetActions)
{
    EditorSessionActionBundle bundle{};
    bundle.document.path = documentPath.string();
    bundle.document.save = [&session, documentPath](std::string& message)
    {
        return session.GetActions().SaveWorld(documentPath, message);
    };
    bundle.document.load = [&session, documentPath](std::string& message)
    {
        return session.GetActions().LoadWorld(documentPath, message);
    };
    bundle.commands.processor = &session.GetCommandProcessor();
    bundle.commands.runtimeAssets = &runtimeAssets;
    bundle.commands.worldChanged = [&session]()
    {
        session.MarkWorldChanged();
    };
    bundle.assets = std::move(assetActions);
    return bundle;
}
} // namespace Prism::UI
