#pragma once

#include "Core/Application/RenderExecutionService.h"
#include "Core/Application/RenderFrameExecutionState.h"
#include "Core/Application/FrameRateLimiter.h"
#include "Core/Threading/TaskExecutorFactory.h"
#include "RHI/GraphicsApi.h"

#include <cstdint>
#include <filesystem>
#include <array>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <thread>

namespace Prism::Platform
{
class Window;
}

namespace Prism::RHI
{
enum class FrameResult;
class IRenderBackend;
}

namespace Prism::Asset
{
struct AssetImportResult;
struct AssetRuntimeLoadResult;
class Texture;
}

namespace Prism::Scene
{
enum class DemoSceneId : unsigned int;
struct DemoSceneBuildResult;
class RenderScene;
class SceneSession;
struct SceneSessionDemoActivationResult;
}

namespace Prism::Renderer
{
class RenderFrameCoordinator;
class SceneRenderer;
}

namespace Prism::UI
{
class UiDrawPacket;
}

namespace Prism::Core
{
class AssetRuntimeCoordinator;
struct AssetRuntimeRenderWorkResult;
// Production target shared by threaded execution and inline fallback. The
// selected execution lane solely owns the backend and renderer coordinator.
class RenderRuntimeExecutionTarget final
    : public IRenderExecutionTarget
{
public:
    RenderRuntimeExecutionTarget(
        Platform::Window& window,
        RHI::GraphicsApi graphicsApi,
        bool sceneViewEnabled,
        TaskExecutorKind taskExecutorKind,
        RHI::FramePacingConfiguration framePacing,
        AssetRuntimeCoordinator& assetRuntimeCoordinator);
    ~RenderRuntimeExecutionTarget() override;

    RenderRuntimeExecutionTarget(
        const RenderRuntimeExecutionTarget&) = delete;
    RenderRuntimeExecutionTarget& operator=(
        const RenderRuntimeExecutionTarget&) = delete;

    void Initialize() override;
    void AdoptExecutionLane() override;
    [[nodiscard]] RenderTargetFrameResult ExecuteFrame(
        const FrameEnvelope& frame) override;
    [[nodiscard]] RenderControlAcknowledgement ExecuteControl(
        const RenderControlCommand& command) override;
    void Shutdown() noexcept override;

    [[nodiscard]] RHI::IRenderBackend& GetBackend();
    [[nodiscard]] Renderer::RenderFrameCoordinator&
        GetFrameCoordinator();
    [[nodiscard]] bool IsOnExecutionLane() const noexcept;
    [[nodiscard]] bool IsBackendInitialized() const noexcept;

    // Installed during startup while the target is still on the caller lane.
    // The callbacks only perform GPU-facing UI work and are invoked later on
    // whichever execution lane owns the target.
    void ConfigureUiExecutionCallbacks(
        std::function<void(const UI::UiDrawPacket&)> draw,
        std::function<void(
            Renderer::SceneRenderer&,
            Renderer::SceneRenderer&)> refresh,
        std::function<void()> shutdown);
    void ConfigureSceneSession(Scene::SceneSession& session) noexcept;

    void InitializeRenderers(
        const std::filesystem::path& shaderPath,
        const Scene::RenderScene& scene,
        std::shared_ptr<Asset::Texture> environmentCubemap);
    [[nodiscard]] RHI::FrameResult BeginRenderFrame();
    [[nodiscard]] RHI::FrameResult EndRenderFrame();
    [[nodiscard]] bool RequestFrameCapture(
        std::uint64_t requestId,
        const std::filesystem::path& outputPath);
    [[nodiscard]] RenderTargetFrameResult ResolveFrameCapture();
    [[nodiscard]] RenderTargetFrameResult CollectFrameResult(
        bool gameRendered,
        bool sceneRendered,
        const FrameEnvelope& frame);
    void ResizeSwapChain(
        std::uint32_t width,
        std::uint32_t height);
    void WaitForGpu();
    [[nodiscard]] AssetRuntimeRenderWorkResult ProcessAssetRenderWork(
        std::uint64_t logicalFrameId);
    [[nodiscard]] Asset::AssetRuntimeLoadResult LoadRuntimeAssets();
    [[nodiscard]] Asset::AssetImportResult ImportAndReload(
        const std::filesystem::path& sourcePath,
        bool reimport);
    [[nodiscard]] std::shared_ptr<Asset::Texture>
        LoadEnvironmentCubemap(
            const std::filesystem::path& directory,
            std::string* outStatus);
    [[nodiscard]] Scene::DemoSceneBuildResult PopulateDemoScene(
        Scene::DemoSceneId sceneId,
        Scene::RenderScene& scene,
        float aspectRatio);
    [[nodiscard]] bool LoadStartupSceneFromGltf(
        const std::filesystem::path& path,
        Scene::RenderScene& scene,
        std::string* outError);
    [[nodiscard]] Scene::SceneSessionDemoActivationResult
        ActivateDemoScene(
            Scene::SceneSession& session,
            Scene::DemoSceneId sceneId,
            float aspectRatio);

private:
    void RecordFrameCapture(
        const PreparedRenderFrame& prepared,
        bool gameRendered,
        bool sceneRendered);
    void RequireExecutionLane() const;

    Platform::Window& m_window;
    AssetRuntimeCoordinator& m_assetRuntimeCoordinator;
    Scene::SceneSession* m_sceneSession = nullptr;
    RHI::GraphicsApi m_graphicsApi;
    RHI::FramePacingConfiguration m_framePacingConfiguration;
    FrameRateLimiter m_frameRateLimiter;
    std::optional<RHI::FrameAdmissionResult>
        m_prefetchedFrameAdmission;
    bool m_sceneViewEnabled = false;
    TaskExecutorKind m_taskExecutorKind = TaskExecutorKind::Inline;
    std::thread::id m_executionThread;
    std::unique_ptr<RHI::IRenderBackend> m_backend;
    std::unique_ptr<Renderer::RenderFrameCoordinator>
        m_frameCoordinator;
    bool m_initializationAttempted = false;
    bool m_backendInitialized = false;
    bool m_renderersInitialized = false;
    bool m_gpuDrainedForShutdown = false;
    bool m_shutdown = false;
    RenderFrameExecutionState m_frameExecutionState;
    std::uint64_t m_settingsRevision = 0;
    RenderEpoch m_sceneEpoch;
    RenderEpoch m_viewEpoch;
    std::uint64_t m_captureRequestId = 0;
    std::uint64_t m_activeCaptureRequestId = 0;
    std::shared_ptr<const Scene::RenderViewFeedback>
        m_completedGameVisibilityFeedback;
    std::function<void(const UI::UiDrawPacket&)> m_uiDraw;
    std::function<void(
        Renderer::SceneRenderer&,
        Renderer::SceneRenderer&)> m_uiRefresh;
    std::function<void()> m_uiShutdown;
    bool m_stopRequested = false;
};
} // namespace Prism::Core
