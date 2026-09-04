#pragma once

#include "Engine/EntityId.h"
#include "Engine/SceneChangeTracker.h"
#include "Scene/DemoSceneCatalog.h"
#include "Scene/RenderSceneExtractor.h"
#include "Scene/RenderSceneIdentity.h"
#include "Scene/RenderView.h"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>

#include <json.hpp>

namespace Prism::Asset
{
class AssetRegistry;
}

namespace Prism::Engine
{
class CommandProcessor;
}

namespace Prism::RHI
{
class IGraphicsDevice;
}

namespace Prism::Scene
{
class CameraController;
class Camera;
class RenderDynamicInputState;
class RenderFramePacket;
class RenderScene;
class RenderSceneData;
class RenderSceneMailbox;
class SceneSession;

struct SceneSessionIdentity
{
    DemoSceneId activeDemoScene = DemoSceneId::EditorPreview;
    std::string sourceLabel;
    std::string loadMessage;
    bool usingFallbackScene = true;
};

struct SceneSessionDemoActivationResult
{
    bool changed = false;
    DemoSceneBuildResult build;
};

// Narrow editor/automation command surface. It references a SceneSession but
// has no ApplicationHost, renderer, graphics-device or ImGui dependency.
class SceneSessionActions final
{
public:
    explicit SceneSessionActions(SceneSession& session) noexcept;

    bool SaveWorld(
        const std::filesystem::path& path,
        std::string& message) const;
    bool LoadWorld(
        const std::filesystem::path& path,
        std::string& message) const;
    nlohmann::json Execute(
        std::string_view operation,
        const char* command,
        nlohmann::json arguments) const;
    void MarkWorldChanged(bool synchronizeCamera = false) const;

private:
    SceneSession* m_session = nullptr;
};

// Owns mutable scene/editor state while borrowing immutable service
// dependencies. GPU scene construction is invoked synchronously by the
// current application lane; ownership of the device remains outside.
class SceneSession final
{
public:
    SceneSession(
        std::filesystem::path projectRoot,
        Asset::AssetRegistry& assetRegistry);
    ~SceneSession();

    SceneSession(const SceneSession&) = delete;
    SceneSession& operator=(const SceneSession&) = delete;

    [[nodiscard]] RenderScene& GetRenderScene() noexcept;
    [[nodiscard]] const RenderScene& GetRenderScene() const noexcept;
    [[nodiscard]] RenderSceneMailbox& GetMailbox() noexcept;
    [[nodiscard]] Engine::CommandProcessor& GetCommandProcessor() noexcept;
    [[nodiscard]] const Engine::CommandProcessor& GetCommandProcessor() const noexcept;
    [[nodiscard]] CameraController& GetSceneCameraController() noexcept;
    [[nodiscard]] CameraController& GetGameCameraController() noexcept;
    [[nodiscard]] RenderDynamicInputState&
        GetDynamicInputState() noexcept;
    [[nodiscard]] const RenderDynamicInputState&
        GetDynamicInputState() const noexcept;
    [[nodiscard]] RenderSceneExtractor&
        GetRenderSceneExtractor() noexcept;
    [[nodiscard]] const RenderSceneExtractor&
        GetRenderSceneExtractor() const noexcept;

    [[nodiscard]] SceneSessionActions GetActions() noexcept;
    [[nodiscard]] const SceneSessionIdentity& GetIdentity() const noexcept;
    [[nodiscard]] SceneGeneration GetSceneGeneration() const noexcept;
    [[nodiscard]] RenderSceneDataRevision GetSceneDataRevision() const noexcept;
    [[nodiscard]] std::uint64_t GetRuntimeAssetBindingRevision() const noexcept;
    std::size_t SynchronizeRuntimeAssetBindingChanges();
    void SetIdentity(SceneSessionIdentity identity);
    void SetSourceLabel(std::string label);
    void SetLoadMessage(std::string message);
    void SetActiveDemoScene(DemoSceneId sceneId) noexcept;
    void SetUsingFallbackScene(bool usingFallback) noexcept;

    void InitializeWorldFromRenderScene();
    void MarkWorldChanged(bool synchronizeCamera = false) noexcept;
    [[nodiscard]] bool IsWorldDirty() const noexcept;
    void SynchronizeWorldToRenderScene(bool synchronizeCamera);
    void SynchronizePendingWorldChanges();
    bool SynchronizeViewportCameraToWorld(std::string& message);

    nlohmann::json ExecuteCommand(
        std::string_view operation,
        const char* command,
        nlohmann::json arguments);
    [[nodiscard]] SceneSessionDemoActivationResult ActivateDemoScene(
        DemoSceneId sceneId,
        RHI::IGraphicsDevice& device,
        float aspectRatio);
    void ConfigureViewportNavigation(DemoSceneId sceneId);
    void BeginDynamicFrame(
        std::uint64_t logicalFrameId,
        double simulationTimeSeconds,
        double simulationDeltaSeconds = 1.0 / 60.0);
    void SynchronizeDynamicLights();
    void SynchronizeDynamicView(
        RenderViewId viewId,
        const Camera& camera,
        std::uint32_t width,
        std::uint32_t height,
        std::uint64_t historySettingsRevision);
    void NotifyDynamicViewHistory(
        RenderViewId viewId,
        RenderViewHistoryInvalidation reason);
    [[nodiscard]] RenderSceneExtractionResult
        ExtractRenderSceneData();
    [[nodiscard]] std::shared_ptr<const RenderFramePacket>
        BuildRenderFramePacket(
            std::shared_ptr<const RenderSceneData> sceneData) const;
    // Drops every retained runtime render binding while its creating RHI
    // context is still alive. Mutable world/editor state remains valid for
    // the remainder of application teardown.
    void ReleaseRuntimeRenderResources();

private:
    friend class SceneSessionActions;

    std::string NextRequestId(std::string_view operation);

    std::filesystem::path m_projectRoot;
    Asset::AssetRegistry* m_assetRegistry = nullptr;
    std::unique_ptr<RenderScene> m_scene;
    std::unique_ptr<RenderSceneMailbox> m_mailbox;
    std::unique_ptr<Engine::CommandProcessor> m_commandProcessor;
    std::unique_ptr<CameraController> m_sceneCameraController;
    std::unique_ptr<CameraController> m_gameCameraController;
    std::unique_ptr<RenderDynamicInputState> m_dynamicInputState;
    std::unique_ptr<RenderSceneExtractor> m_sceneExtractor;
    SceneSessionIdentity m_identity;
    bool m_worldSyncCameraRequested = false;
    std::uint64_t m_requestCounter = 0;
    SceneGeneration m_sceneGeneration{1};
    RenderSceneDataRevision m_sceneDataRevision{1};
    std::uint64_t m_runtimeAssetBindingRevision = 0;
    Engine::SceneChangeSet m_extractionChanges{};
};
} // namespace Prism::Scene
