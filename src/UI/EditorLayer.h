#pragma once

#include "Core/Math/Double3.h"
#include "Engine/EntityId.h"
#include "UI/ContentBrowserPanel.h"
#include "UI/PropertyGrid.h"
#include "UI/GameViewportDebugOverlay.h"

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>

#include <DirectXMath.h>

namespace Prism::Asset
{
class AssetRegistry;
}

namespace Prism::Engine
{
class CommandProcessor;
}

namespace Prism::Renderer
{
struct RenderSettings;
struct RendererStatistics;
enum class ViewportShadingMode : std::uint32_t;
}

namespace Prism::Scene
{
class CameraController;
struct RenderObject;
class RenderScene;
}

namespace Prism::UI
{
struct SceneDocumentActions
{
    std::function<bool(std::string&)> save;
    std::function<bool(std::string&)> load;
    std::string path;
};

struct EditorCommandActions
{
    Engine::CommandProcessor* processor = nullptr;
    const Asset::AssetRegistry* runtimeAssets = nullptr;
    std::function<void()> worldChanged;
};

class EditorLayer
{
public:
    void Draw(
        Scene::RenderScene& scene,
        Renderer::RenderSettings& settings,
        Scene::CameraController& sceneCameraController,
        Scene::CameraController& gameCameraController,
        std::uint64_t sceneColor,
        std::uint32_t sceneColorWidth,
        std::uint32_t sceneColorHeight,
        std::uint64_t gameColor,
        std::uint32_t gameColorWidth,
        std::uint32_t gameColorHeight,
        const ViewportPerformanceStats& performanceStats,
        const Renderer::RendererStatistics& rendererStatistics,
        bool oceanLabActive,
        bool waveWorksActive,
        bool fluidLabActive,
        const SceneDocumentActions& documentActions,
        const EditorCommandActions& commandActions,
        const EditorAssetActions& assetActions);

    bool IsSceneViewportHovered() const;
    bool IsGameViewportHovered() const;
    [[nodiscard]] bool IsSceneViewportVisible() const noexcept;
    [[nodiscard]] bool IsGameViewportVisible() const noexcept;
    bool IsGizmoActive() const;
    [[nodiscard]] Renderer::ViewportShadingMode
        GetSceneShadingMode() const;

private:
    enum class GizmoOperation
    {
        Translate,
        Rotate,
        Scale
    };

    void DrawDockSpace();
    void DrawMainMenu(
        Scene::RenderScene& scene,
        const SceneDocumentActions& documentActions,
        const EditorCommandActions& commandActions,
        const EditorAssetActions& assetActions);
    void DrawCreateEntityMenu(
        Scene::RenderScene& scene,
        const EditorCommandActions& commandActions,
        const EditorAssetActions& assetActions);
    void DrawSceneViewport(
        Scene::RenderScene& scene,
        Scene::CameraController& cameraController,
        Renderer::RenderSettings& settings,
        std::uint64_t sceneColor,
        std::uint32_t sceneColorWidth,
        std::uint32_t sceneColorHeight,
        const EditorCommandActions& commandActions,
        const EditorAssetActions& assetActions);
    void DrawFinalOutput(
        const Scene::CameraController& gameCameraController,
        Renderer::RenderSettings& settings,
        const Renderer::RendererStatistics& rendererStatistics,
        const ViewportPerformanceStats& performanceStats,
        bool oceanLabActive,
        bool waveWorksActive,
        bool fluidLabActive,
        std::uint64_t gameColor,
        std::uint32_t gameColorWidth,
        std::uint32_t gameColorHeight);
    void DrawHierarchy(
        Scene::RenderScene& scene,
        Scene::CameraController& cameraController,
        const EditorCommandActions& commandActions,
        const EditorAssetActions& assetActions);
    void DrawInspector(
        Renderer::RenderSettings& settings,
        const EditorCommandActions& commandActions,
        const EditorAssetActions& assetActions);
    void DrawTransformGizmo(
        Scene::RenderScene& scene,
        const EditorCommandActions& commandActions);
    void DrawSceneNavigationGizmo(
        Scene::RenderScene& scene,
        Scene::CameraController& cameraController);
    void DrawSceneOverlays(
        const Scene::RenderScene& scene,
        const EditorCommandActions& commandActions);
    bool SelectEditorIconAtViewportPosition(
        const Scene::RenderScene& scene,
        const EditorCommandActions& commandActions,
        const DirectX::XMFLOAT2& mousePosition);
    void SelectObjectAtViewportPosition(
        Scene::RenderScene& scene,
        const DirectX::XMFLOAT2& mousePosition);
    void FocusSelectedObject(
        Scene::RenderScene& scene,
        Scene::CameraController& cameraController,
        const EditorCommandActions& commandActions);
    Engine::EntityId CreateWorldEntity(
        Scene::RenderScene& scene,
        const EditorCommandActions& commandActions,
        std::string_view name,
        std::string_view component = {},
        nlohmann::json componentProperties =
            nlohmann::json::object());
    Engine::EntityId CreatePrimitive(
        Scene::RenderScene& scene,
        const EditorCommandActions& commandActions,
        const EditorAssetActions& assetActions,
        std::string_view assetPath);
    void BeginEditTransaction(
        const EditorCommandActions& commandActions);
    void CommitEditTransaction(
        const EditorCommandActions& commandActions);
    bool SubmitTransform(
        const EditorCommandActions& commandActions,
        Engine::EntityId entity,
        const Core::Double3& position,
        const DirectX::XMFLOAT3& rotation,
        const DirectX::XMFLOAT3& scale);
    bool ExecuteWorldCommand(
        const EditorCommandActions& actions,
        std::string_view operation,
        std::string_view command,
        nlohmann::json arguments = nlohmann::json::object());
    std::string NextRequestId(std::string_view operation);

    Engine::EntityId m_selectedEntity;
    DirectX::XMFLOAT2 m_viewportMin{};
    DirectX::XMFLOAT2 m_viewportSize{};
    GizmoOperation m_gizmoOperation = GizmoOperation::Translate;
    Renderer::ViewportShadingMode m_sceneShadingMode{};
    ContentBrowserPanel m_contentBrowser;
    PropertyGrid m_propertyGrid;
    GameViewportDebugOverlay m_gameDebugOverlay;
    bool m_layoutInitialized = false;
    bool m_sceneViewportHovered = false;
    bool m_gameViewportHovered = false;
    bool m_sceneViewportVisible = true;
    bool m_gameViewportVisible = true;
    bool m_gizmoActive = false;
    bool m_gizmoWasActive = false;
    bool m_editTransactionActive = false;
    std::uint64_t m_requestCounter = 0;
    std::string m_documentStatus;
};
}
