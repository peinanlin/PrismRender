#pragma once

#include "Core/Application/RenderControlCommand.h"
#include "RHI/GraphicsApi.h"
#include "Renderer/RenderHistorySettingsTracker.h"
#include "Renderer/RenderSettings.h"
#include "Scene/DemoSceneCatalog.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

#include <json.hpp>

namespace Prism::Platform
{
    class Window;
}

namespace Prism::Engine
{
    class CommandProcessor;
    struct EntityId;
}

namespace Prism::Renderer
{
    class SceneRenderer;
    class RenderFrameCoordinator;
    class FrameDiagnostics;
}

namespace Prism::Asset
{
    class AssetRegistry;
    class Texture;
    struct AssetImportResult;
    struct AssetRecord;
}

namespace Prism::RHI
{
    class IRenderBackend;
}

namespace Prism::UI
{
    class EditorCoordinator;
}

namespace Prism::Scene
{
    class CameraController;
    class RenderScene;
    class RenderSceneMailbox;
    class SceneSession;
}

namespace Prism::Core
{
    class AssetRuntimeCoordinator;
    class CaptureAutomationController;
    class FrameTimer;
    class FrameProfiler;
    class RenderExecutionService;
    class RenderRuntimeExecutionTarget;
    class SceneViewRefreshController;

    // The single process-level composition root. Graphics API differences are
    // selected through IRenderBackend; editor UI is shared across supported APIs.
    class ApplicationHost
    {
    public:
        explicit ApplicationHost(
            RHI::GraphicsApi graphicsApi,
            std::optional<Scene::DemoSceneId> startupDemoScene =
                std::nullopt);
        ~ApplicationHost();

        void Initialize();
        int Run();

    private:
        enum class LifecycleState
        {
            Uninitialized,
            Initializing,
            Initialized,
            ShuttingDown
        };

        void TryActivateStreamingScene();
        void ActivateDemoScene(Scene::DemoSceneId sceneId);
        void UpdateFluidInput();
        Asset::AssetImportResult ImportEditorAsset(
            const std::filesystem::path& sourcePath,
            bool reimport);
        Engine::EntityId InstantiateEditorAsset(
            const Asset::AssetRecord& asset);
        void Shutdown() noexcept;

        std::unique_ptr<Platform::Window> m_window;
        RHI::GraphicsApi m_graphicsApi;
        std::unique_ptr<RenderExecutionService>
            m_renderExecutionService;
        // Startup-only aliases borrowed from the execution target while the
        // composition root wires GPU resources and UI callbacks. Initialize
        // clears every alias before threaded runtime execution begins.
        RenderRuntimeExecutionTarget* m_renderExecutionTarget = nullptr;
        RHI::IRenderBackend* m_backend = nullptr;
        std::unique_ptr<CaptureAutomationController>
            m_captureAutomationController;
        Renderer::RenderFrameCoordinator*
            m_renderFrameCoordinator = nullptr;
        // Non-owning hot-path aliases owned by RenderFrameCoordinator.
        Renderer::SceneRenderer* m_sceneRenderer = nullptr;
        Renderer::SceneRenderer* m_sceneViewRenderer = nullptr;
        std::unique_ptr<AssetRuntimeCoordinator>
            m_assetRuntimeCoordinator;
        std::shared_ptr<Asset::Texture>
            m_environmentCubemap;
        std::unique_ptr<Scene::SceneSession> m_sceneSession;
        // Non-owning hot-path aliases. SceneSession remains the sole owner.
        Scene::RenderScene* m_scene = nullptr;
        Scene::RenderSceneMailbox* m_renderSceneMailbox = nullptr;
        Engine::CommandProcessor* m_commandProcessor = nullptr;
        Scene::CameraController* m_cameraController = nullptr;
        Scene::CameraController* m_gameCameraController = nullptr;
#if defined(PRISM_RENDER_HAS_EDITOR)
        std::unique_ptr<UI::EditorCoordinator>
            m_editorCoordinator;
#endif
        std::unique_ptr<FrameTimer> m_frameTimer;
        std::unique_ptr<FrameProfiler> m_frameProfiler;
        std::unique_ptr<SceneViewRefreshController>
            m_sceneViewRefreshController;
        std::string m_environmentMapStatus;
        std::string m_adapterName;
        std::string m_graphicsApiName;
        nlohmann::json m_runtimePerformanceIdentity =
            nlohmann::json::object();
        std::filesystem::path m_editorScenePath;
        bool m_gltfImportEnabled = false;
        bool m_editorEnabled = false;
        bool m_performanceGameOnly = false;
        // Parsed once during initialization. Startup initializes graphics
        // objects inline, then the default threaded mode hands ownership to
        // the resident render lane. Explicit inline remains the fallback.
        bool m_threadedRenderExecutionRequested = false;
        std::optional<Scene::DemoSceneId> m_startupDemoScene;
        bool m_demoSceneSwitchingEnabled = true;
        bool m_sceneViewRequiresRefresh = true;
        bool m_sceneViewWasVisible = false;
        std::optional<Scene::DemoSceneId> m_pendingDemoScene;
        // Main-lane state used to build immutable FrameEnvelope values. The
        // SceneRenderer copies become execution-lane implementation details
        // once threaded execution is enabled.
        Renderer::RenderSettings m_gameRenderSettings{};
        Renderer::RenderSettings m_sceneRenderSettings{};
        Renderer::RenderHistorySettingsTracker
            m_gameHistorySettingsTracker;
        Renderer::RenderHistorySettingsTracker
            m_sceneHistorySettingsTracker;
        std::uint32_t m_gameOutputWidth = 0;
        std::uint32_t m_gameOutputHeight = 0;
        std::uint32_t m_sceneOutputWidth = 0;
        std::uint32_t m_sceneOutputHeight = 0;
        LifecycleState m_lifecycleState = LifecycleState::Uninitialized;
        bool m_backendInitialized = false;
        bool m_gpuDrainedForShutdown = false;
        std::uint64_t m_nextRenderControlCommandId = 1;
        std::uint64_t m_completedFrameCount = 0;
        RenderEpoch m_currentSceneEpoch{1};
        RenderEpoch m_currentViewEpoch{1};
    };
} // namespace Prism::Core
