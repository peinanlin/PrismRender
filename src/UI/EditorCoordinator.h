#pragma once

#include "RHI/FramePacing.h"
#include "RHI/GraphicsApi.h"
#include "UI/EditorLayer.h"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>

namespace Prism::Asset
{
class AssetRegistry;
}

namespace Prism::Core
{
class FrameProfiler;
class SceneViewRefreshController;
}

namespace Prism::Platform
{
class Window;
}

namespace Prism::RHI
{
class IRenderBackend;
}

namespace Prism::Renderer
{
struct RendererStatistics;
struct RenderSettings;
class SceneRenderer;
enum class ViewportShadingMode : std::uint32_t;
}

namespace Prism::Scene
{
enum class DemoSceneId : std::uint32_t;
class SceneSession;
}

namespace Prism::UI
{
class DebugPanel;
class ImGuiSystem;
class UiDrawPacket;
class PerformanceProfilerPanel;

enum class EditorNavigationTarget
{
    None,
    Game,
    Scene
};

struct EditorStreamingSnapshot
{
    bool enabled = false;
    std::uint32_t queuedCount = 0;
    std::uint32_t residentCount = 0;
    std::uint32_t evictedCount = 0;
    std::uint32_t failedCount = 0;
    std::uint64_t residentBytes = 0;
    std::uint64_t budgetBytes = 0;
    std::uint64_t completedUploads = 0;
    std::uint64_t evictionCount = 0;
};

struct EditorSessionActionBundle
{
    SceneDocumentActions document;
    EditorCommandActions commands;
    EditorAssetActions assets;
};

struct EditorCoordinatorFrameInput
{
    EditorCoordinatorFrameInput(
        Scene::SceneSession& sceneSessionValue,
        const Asset::AssetRegistry& assetRegistryValue,
        Renderer::RenderSettings& gameSettingsValue,
        std::uint32_t gameViewWidthValue,
        std::uint32_t gameViewHeightValue,
        std::uint32_t sceneViewWidthValue,
        std::uint32_t sceneViewHeightValue,
        Core::FrameProfiler& frameProfilerValue,
        Core::SceneViewRefreshController& sceneViewRefreshValue,
        Platform::Window& windowValue) noexcept;

    Scene::SceneSession& sceneSession;
    const Asset::AssetRegistry& assetRegistry;
    Renderer::RenderSettings& gameSettings;
    std::uint32_t gameViewWidth = 0;
    std::uint32_t gameViewHeight = 0;
    std::uint32_t sceneViewWidth = 0;
    std::uint32_t sceneViewHeight = 0;
    Core::FrameProfiler& frameProfiler;
    Core::SceneViewRefreshController& sceneViewRefresh;
    Platform::Window& window;
    const Renderer::RendererStatistics*
        completedGameStatistics = nullptr;
    std::uint64_t completedStatisticsLogicalFrameId = 0;
    EditorAssetActions assetActions;
    EditorStreamingSnapshot streaming;
    std::filesystem::path documentPath;
    std::string adapterName;
    std::string graphicsApiName;
    std::string environmentMapStatus;
    float deltaTimeMs = 0.0f;
    float framesPerSecond = 0.0f;
    float navigationDeltaSeconds = 0.0f;
    std::uint32_t frameIndex = 0;
    std::uint32_t windowWidth = 0;
    std::uint32_t windowHeight = 0;
    bool gltfImportEnabled = false;
    bool demoSceneSwitchingEnabled = false;
    bool deterministicCaptureEnabled = false;
};

struct EditorCoordinatorFrameResult
{
    std::optional<Scene::DemoSceneId> requestedDemoScene;
    std::optional<RHI::FramePacingProfile> requestedFramePacing;
    bool sceneViewRequiresRefresh = false;
};

// Owns the complete editor UI session. It consumes narrow scene/actions/stats
// inputs and never stores or reaches back into ApplicationHost.
class EditorCoordinator final
{
public:
    static std::unique_ptr<EditorCoordinator> Create(
        RHI::GraphicsApi graphicsApi);

    ~EditorCoordinator();
    EditorCoordinator(const EditorCoordinator&) = delete;
    EditorCoordinator& operator=(const EditorCoordinator&) = delete;

    void Initialize(
        Platform::Window& window,
        RHI::IRenderBackend& backend,
        Renderer::SceneRenderer& gameRenderer,
        Renderer::SceneRenderer& sceneRenderer,
        bool useGameTextureForScene);
    void RefreshViewportTextures(
        Renderer::SceneRenderer& gameRenderer,
        Renderer::SceneRenderer& sceneRenderer,
        bool useGameTextureForScene);
    void PollPlatformEvents(Platform::Window& window);
    [[nodiscard]] EditorCoordinatorFrameResult DrawFrame(
        EditorCoordinatorFrameInput& input);
    [[nodiscard]] std::shared_ptr<const UiDrawPacket>
        BuildDrawPacket();
    void AbortFrameBuild() noexcept;
    void Render(const UiDrawPacket& drawPacket);
    void Shutdown() noexcept;

    [[nodiscard]] bool IsSceneViewportVisible() const noexcept;
    [[nodiscard]] bool IsGizmoActive() const noexcept;
    [[nodiscard]] Renderer::ViewportShadingMode
        GetSceneShadingMode() const noexcept;

    [[nodiscard]] static EditorNavigationTarget SelectNavigationTarget(
        bool sceneInteracting,
        bool gameInteracting,
        bool gizmoActive,
        bool gameViewportHovered,
        bool sceneViewportHovered) noexcept;
    [[nodiscard]] static EditorSessionActionBundle BuildSessionActions(
        Scene::SceneSession& session,
        const Asset::AssetRegistry& runtimeAssets,
        const std::filesystem::path& documentPath,
        EditorAssetActions assetActions);

private:
    explicit EditorCoordinator(
        std::unique_ptr<ImGuiSystem> imguiSystem);

    std::unique_ptr<DebugPanel> m_debugPanel;
    std::unique_ptr<EditorLayer> m_editorLayer;
    std::unique_ptr<ImGuiSystem> m_imguiSystem;
    std::unique_ptr<PerformanceProfilerPanel>
        m_performanceProfilerPanel;
};
} // namespace Prism::UI
