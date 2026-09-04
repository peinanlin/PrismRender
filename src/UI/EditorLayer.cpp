#include "UI/EditorLayer.h"

#include "Asset/AssetRegistry.h"
#include "Asset/Material.h"
#include "Asset/Mesh.h"
#include "Engine/CommandSystem.h"
#include "Engine/Reflection.h"
#include "Engine/World.h"
#include "Renderer/RenderSettings.h"
#include "Scene/Camera.h"
#include "Scene/CameraController.h"
#include "Scene/RenderObject.h"
#include "Scene/RenderScene.h"

#include <imgui.h>
#include <imgui_internal.h>
#include <ImGuizmo.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <functional>
#include <limits>
#include <optional>
#include <utility>
#include <vector>

namespace Prism::UI
{
using namespace DirectX;

namespace
{
struct EntityDragPayload
{
    static constexpr const char* TypeName = "PRISM_ENTITY_ID";
    char entityId[40]{};
};

float GetObjectBoundsRadius(const Scene::RenderObject& renderObject)
{
    if (renderObject.mesh == nullptr) return 1.0f;
    const XMFLOAT3& scale = renderObject.transform.GetScale();
    const float maxScale = std::max(
        std::abs(scale.x),
        std::max(std::abs(scale.y), std::abs(scale.z)));
    return std::max(
        0.1f,
        renderObject.mesh->GetBoundsRadius() * maxScale);
}

XMFLOAT3 GetObjectBoundsCenterRelativeTo(
    const Scene::RenderObject& renderObject,
    const Core::Double3& renderOrigin)
{
    XMFLOAT3 center{};
    XMStoreFloat3(
        &center,
        XMVector3TransformCoord(
            XMLoadFloat3(
                &renderObject.mesh->GetBoundsCenter()),
            renderObject.transform.GetRelativeWorldMatrix(
                renderOrigin)));
    return center;
}

Core::Double3 GetObjectBoundsCenterWorld(
    const Scene::RenderObject& renderObject)
{
    XMFLOAT3 offset{};
    const XMMATRIX worldWithoutTranslation =
        XMMatrixScaling(
            renderObject.transform.GetScale().x,
            renderObject.transform.GetScale().y,
            renderObject.transform.GetScale().z)
        * XMMatrixRotationRollPitchYaw(
            renderObject.transform
                .GetRotationEulerRadians().x,
            renderObject.transform
                .GetRotationEulerRadians().y,
            renderObject.transform
                .GetRotationEulerRadians().z);
    XMStoreFloat3(
        &offset,
        XMVector3TransformCoord(
            XMLoadFloat3(
                &renderObject.mesh->GetBoundsCenter()),
            worldWithoutTranslation));
    const Core::Double3& position =
        renderObject.transform.GetWorldPosition();
    return {
        position.x + offset.x,
        position.y + offset.y,
        position.z + offset.z};
}

bool IntersectSphere(
    const XMVECTOR rayOrigin,
    const XMVECTOR rayDirection,
    const XMFLOAT3& center,
    const float radius,
    float& outDistance)
{
    const XMVECTOR offset =
        rayOrigin - XMLoadFloat3(&center);
    const float b = XMVectorGetX(
        XMVector3Dot(offset, rayDirection));
    const float c = XMVectorGetX(
        XMVector3Dot(offset, offset)) - radius * radius;
    const float discriminant = b * b - c;
    if (discriminant < 0.0f) return false;
    const float root = std::sqrt(discriminant);
    const float nearDistance = -b - root;
    const float farDistance = -b + root;
    outDistance = nearDistance >= 0.0f
        ? nearDistance : farDistance;
    return outDistance >= 0.0f;
}

const Scene::RenderObject* FindRenderObject(
    const Scene::RenderScene& scene,
    const Engine::EntityId entity)
{
    const auto found = std::ranges::find(
        scene.GetRenderObjects(), entity,
        &Scene::RenderObject::entityId);
    return found == scene.GetRenderObjects().end()
        ? nullptr : &*found;
}

bool HasComponent(
    const Engine::EntityRecord& entity,
    const std::string_view component)
{
    if (component == "Name" || component == "Transform") return true;
    if (component == "Hierarchy") return entity.hierarchy.has_value();
    if (component == "MeshRenderer") return entity.meshRenderer.has_value();
    if (component == "MaterialOverride") return entity.materialOverride.has_value();
    if (component == "Camera") return entity.camera.has_value();
    if (component == "DirectionalLight") return entity.directionalLight.has_value();
    if (component == "PointLight") return entity.pointLight.has_value();
    if (component == "SpotLight") return entity.spotLight.has_value();
    return false;
}

nlohmann::json MakeMaterialOverrideProperties(
    const Engine::EntityRecord& entity,
    const Asset::AssetRegistry* registry)
{
    Asset::Material::Parameters parameters{};
    if (registry != nullptr && entity.meshRenderer.has_value())
    {
        const Asset::MaterialHandle handle =
            registry->FindMaterialByPath(
                entity.meshRenderer->materialAsset);
        if (const std::shared_ptr<Asset::Material> material =
                registry->GetRuntimeMaterial(handle))
        {
            parameters = material->GetParameters();
        }
    }
    return {
        {"albedoColor", {
            parameters.albedoColor.x, parameters.albedoColor.y,
            parameters.albedoColor.z, parameters.albedoColor.w}},
        {"emissiveColor", {
            parameters.emissiveColor.x, parameters.emissiveColor.y,
            parameters.emissiveColor.z}},
        {"metallic", parameters.metallic},
        {"roughness", parameters.roughness},
        {"occlusionStrength", parameters.occlusionStrength},
        {"normalScale", parameters.normalScale},
        {"emissiveStrength", parameters.emissiveStrength},
        {"alphaCutoff", parameters.alphaCutoff},
        {"alphaMode", parameters.alphaMode},
        {"useAlbedoTexture", parameters.useAlbedoTexture},
        {"useMetallicRoughnessTexture", parameters.useMetallicRoughnessTexture},
        {"useNormalTexture", parameters.useNormalTexture},
        {"useOcclusionTexture", parameters.useOcclusionTexture},
        {"useEmissiveTexture", parameters.useEmissiveTexture}};
}

Core::Double3 OffsetWorldPosition(
    const Core::Double3& position,
    const XMFLOAT3& direction,
    const double distance)
{
    return {
        position.x
            + static_cast<double>(direction.x) * distance,
        position.y
            + static_cast<double>(direction.y) * distance,
        position.z
            + static_cast<double>(direction.z) * distance};
}

Core::Double3 GetEditorSpawnPosition(
    const Scene::Camera& camera)
{
    return OffsetWorldPosition(
        camera.GetWorldPosition(),
        camera.GetForwardVector(),
        5.0);
}

XMFLOAT3 GetForwardFromRotation(
    const XMFLOAT3& rotation)
{
    const float cosPitch = std::cos(rotation.x);
    return {
        std::sin(rotation.y) * cosPitch,
        std::sin(rotation.x),
        std::cos(rotation.y) * cosPitch};
}

bool ProjectWorldPoint(
    const Core::Double3& worldPosition,
    const Scene::Camera& camera,
    const XMFLOAT2& viewportMin,
    const XMFLOAT2& viewportSize,
    ImVec2& screenPosition)
{
    if (viewportSize.x <= 1.0f
        || viewportSize.y <= 1.0f)
    {
        return false;
    }
    const Core::Double3 relative =
        worldPosition - camera.GetWorldPosition();
    const XMVECTOR clip = XMVector4Transform(
        XMVectorSet(
            static_cast<float>(relative.x),
            static_cast<float>(relative.y),
            static_cast<float>(relative.z),
            1.0f),
        camera.GetRelativeViewProjectionMatrix());
    const float clipW = XMVectorGetW(clip);
    if (clipW <= 1.0e-4f)
    {
        return false;
    }
    const float ndcX = XMVectorGetX(clip) / clipW;
    const float ndcY = XMVectorGetY(clip) / clipW;
    screenPosition = {
        viewportMin.x
            + (ndcX * 0.5f + 0.5f)
                * viewportSize.x,
        viewportMin.y
            + (0.5f - ndcY * 0.5f)
                * viewportSize.y};
    return std::isfinite(screenPosition.x)
        && std::isfinite(screenPosition.y);
}

float ScreenDistanceSquared(
    const ImVec2& left,
    const XMFLOAT2& right)
{
    const float x = left.x - right.x;
    const float y = left.y - right.y;
    return x * x + y * y;
}
}

void EditorLayer::Draw(
    Scene::RenderScene& scene,
    Renderer::RenderSettings& settings,
    Scene::CameraController& sceneCameraController,
    Scene::CameraController& gameCameraController,
    const std::uint64_t sceneColor,
    const std::uint32_t sceneColorWidth,
    const std::uint32_t sceneColorHeight,
    const std::uint64_t gameColor,
    const std::uint32_t gameColorWidth,
    const std::uint32_t gameColorHeight,
    const ViewportPerformanceStats& performanceStats,
    const Renderer::RendererStatistics& rendererStatistics,
    const bool oceanLabActive,
    const bool waveWorksActive,
    const bool fluidLabActive,
    const SceneDocumentActions& documentActions,
    const EditorCommandActions& commandActions,
    const EditorAssetActions& assetActions)
{
    // Brush/reset are one-frame commands. Persistent controls remain in the
    // settings object and are copied to the Scene renderer after Game renders.
    settings.terrainBrushActive = false;
    settings.terrainResetRequested = false;
    settings.terrainFullUpdateRequested = false;
    settings.terrainBrushDelta = 0.0f;
    if (commandActions.processor != nullptr
        && m_selectedEntity.IsValid()
        && commandActions.processor->GetWorld().FindEntity(
            m_selectedEntity) == nullptr)
    {
        m_selectedEntity = {};
    }

    ImGuizmo::BeginFrame();
    DrawMainMenu(
        scene,
        documentActions,
        commandActions,
        assetActions);
    DrawDockSpace();
    DrawHierarchy(
        scene,
        sceneCameraController,
        commandActions,
        assetActions);
    DrawInspector(settings, commandActions, assetActions);
    DrawSceneViewport(
        scene, sceneCameraController, settings, sceneColor,
        sceneColorWidth, sceneColorHeight,
        commandActions, assetActions);
    DrawFinalOutput(
        gameCameraController,
        settings,
        rendererStatistics,
        performanceStats,
        oceanLabActive,
        waveWorksActive,
        fluidLabActive,
        gameColor,
        gameColorWidth,
        gameColorHeight);
    m_contentBrowser.Draw(assetActions);
}

bool EditorLayer::IsSceneViewportHovered() const
{
    return m_sceneViewportHovered;
}

bool EditorLayer::IsGameViewportHovered() const
{
    return m_gameViewportHovered;
}

bool EditorLayer::IsSceneViewportVisible() const noexcept
{
    return m_sceneViewportVisible;
}

bool EditorLayer::IsGameViewportVisible() const noexcept
{
    return m_gameViewportVisible;
}

bool EditorLayer::IsGizmoActive() const
{
    return m_gizmoActive;
}

Renderer::ViewportShadingMode
EditorLayer::GetSceneShadingMode() const
{
    return m_sceneShadingMode;
}

void EditorLayer::DrawMainMenu(
    Scene::RenderScene& scene,
    const SceneDocumentActions& documentActions,
    const EditorCommandActions& commandActions,
    const EditorAssetActions& assetActions)
{
    bool saveRequested = false;
    bool loadRequested = false;
    bool undoRequested = false;
    bool redoRequested = false;
    if (ImGui::BeginMainMenuBar())
    {
        if (ImGui::BeginMenu("File"))
        {
            saveRequested = ImGui::MenuItem("Save Scene", "Ctrl+S");
            loadRequested = ImGui::MenuItem("Load Scene", "Ctrl+O");
            ImGui::Separator();
            ImGui::TextDisabled("%s", documentActions.path.c_str());
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Edit"))
        {
            undoRequested = ImGui::MenuItem("Undo", "Ctrl+Z");
            redoRequested = ImGui::MenuItem("Redo", "Ctrl+Y");
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("GameObject"))
        {
            DrawCreateEntityMenu(
                scene,
                commandActions,
                assetActions);
            ImGui::EndMenu();
        }
        ImGui::EndMainMenuBar();
    }

    const ImGuiIO& io = ImGui::GetIO();
    if (!io.WantTextInput && io.KeyCtrl)
    {
        saveRequested |= ImGui::IsKeyPressed(ImGuiKey_S, false);
        loadRequested |= ImGui::IsKeyPressed(ImGuiKey_O, false);
        undoRequested |= ImGui::IsKeyPressed(ImGuiKey_Z, false);
        redoRequested |= ImGui::IsKeyPressed(ImGuiKey_Y, false);
    }
    if (saveRequested && documentActions.save)
        documentActions.save(m_documentStatus);
    if (loadRequested && documentActions.load
        && documentActions.load(m_documentStatus))
        m_selectedEntity = {};
    if (undoRequested)
        ExecuteWorldCommand(
            commandActions, "undo", "transaction.undo");
    if (redoRequested)
        ExecuteWorldCommand(
            commandActions, "redo", "transaction.redo");
}

void EditorLayer::DrawCreateEntityMenu(
    Scene::RenderScene& scene,
    const EditorCommandActions& actions,
    const EditorAssetActions& assetActions)
{
    if (ImGui::MenuItem("Create Empty"))
    {
        CreateWorldEntity(
            scene,
            actions,
            "Empty Entity");
    }
    if (ImGui::BeginMenu("3D Object"))
    {
        if (ImGui::MenuItem("Cube"))
        {
            CreatePrimitive(
                scene,
                actions,
                assetActions,
                "builtin://editor-preview/meshes/cube");
        }
        if (ImGui::MenuItem("UV Sphere"))
        {
            CreatePrimitive(
                scene,
                actions,
                assetActions,
                "builtin://editor-preview/meshes/uv-sphere");
        }
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Light"))
    {
        bool hasDirectionalLight = false;
        if (actions.processor != nullptr)
        {
            for (const auto& [id, entity] :
                 actions.processor->GetWorld().GetEntities())
            {
                (void)id;
                hasDirectionalLight |=
                    entity.directionalLight.has_value();
            }
        }
        if (hasDirectionalLight)
        {
            ImGui::BeginDisabled();
        }
        if (ImGui::MenuItem("Directional Light"))
        {
            const XMFLOAT3 direction =
                scene.GetCamera().GetForwardVector();
            CreateWorldEntity(
                scene,
                actions,
                "Directional Light",
                "DirectionalLight",
                {{"direction", {
                      direction.x,
                      direction.y,
                      direction.z}},
                 {"color", {1.0f, 0.96f, 0.88f}},
                 {"intensity", 4.0f}});
        }
        if (hasDirectionalLight)
        {
            ImGui::EndDisabled();
            if (ImGui::IsItemHovered(
                    ImGuiHoveredFlags_AllowWhenDisabled))
            {
                ImGui::SetTooltip(
                    "RenderScene currently supports one directional light.");
            }
        }
        if (ImGui::MenuItem("Point Light"))
        {
            CreateWorldEntity(
                scene,
                actions,
                "Point Light",
                "PointLight",
                {{"color", {1.0f, 0.82f, 0.64f}},
                 {"intensity", 4.0f},
                 {"range", 12.0f},
                 {"castsShadow", true}});
        }
        if (ImGui::MenuItem("Spot Light"))
        {
            const XMFLOAT3 direction =
                scene.GetCamera().GetForwardVector();
            CreateWorldEntity(
                scene,
                actions,
                "Spot Light",
                "SpotLight",
                {{"direction", {
                      direction.x,
                      direction.y,
                      direction.z}},
                 {"color", {1.0f, 0.88f, 0.72f}},
                 {"intensity", 6.0f},
                 {"range", 20.0f},
                 {"innerAngleRadians",
                  XMConvertToRadians(18.0f)},
                 {"outerAngleRadians",
                  XMConvertToRadians(28.0f)},
                 {"castsShadow", true}});
        }
        ImGui::EndMenu();
    }
}

void EditorLayer::DrawDockSpace()
{
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    const ImGuiID dockspaceId =
        ImGui::GetID("PrismRenderEditorDockSpace");
    const ImGuiDockNodeFlags flags =
        ImGuiDockNodeFlags_PassthruCentralNode;
    if (!m_layoutInitialized)
    {
        ImGui::DockBuilderRemoveNode(dockspaceId);
        ImGui::DockBuilderAddNode(
            dockspaceId,
            ImGuiDockNodeFlags_DockSpace | flags);
        ImGui::DockBuilderSetNodeSize(
            dockspaceId, viewport->Size);
        ImGuiID center = dockspaceId;
        const ImGuiID left = ImGui::DockBuilderSplitNode(
            center, ImGuiDir_Left, 0.20f, nullptr, &center);
        const ImGuiID right = ImGui::DockBuilderSplitNode(
            center, ImGuiDir_Right, 0.27f, nullptr, &center);
        ImGuiID bottom = ImGui::DockBuilderSplitNode(
            center, ImGuiDir_Down, 0.30f, nullptr, &center);
        const ImGuiID diagnostics = ImGui::DockBuilderSplitNode(
            bottom, ImGuiDir_Right, 0.38f, nullptr, &bottom);
        const ImGuiID game = ImGui::DockBuilderSplitNode(
            center, ImGuiDir_Right, 0.43f, nullptr, &center);
        // Scene and Game are visible together so moving the Game Camera can be
        // observed immediately through the Scene View culling visualization.
        ImGui::DockBuilderDockWindow("Scene", center);
        ImGui::DockBuilderDockWindow("Game", game);
        ImGui::DockBuilderDockWindow("Hierarchy", left);
        ImGui::DockBuilderDockWindow("Inspector", right);
        ImGui::DockBuilderDockWindow("Content Browser", bottom);
        ImGui::DockBuilderDockWindow("PrismRender Debug", diagnostics);
        ImGui::DockBuilderFinish(dockspaceId);
        m_layoutInitialized = true;
    }
    ImGui::DockSpaceOverViewport(dockspaceId, viewport, flags);
}

void EditorLayer::DrawSceneViewport(
    Scene::RenderScene& scene,
    Scene::CameraController& cameraController,
    Renderer::RenderSettings& settings,
    const std::uint64_t sceneColor,
    const std::uint32_t sceneColorWidth,
    const std::uint32_t sceneColorHeight,
    const EditorCommandActions& commandActions,
    const EditorAssetActions& assetActions)
{
    const bool open = ImGui::Begin(
        "Scene", nullptr,
        ImGuiWindowFlags_NoScrollbar
            | ImGuiWindowFlags_NoScrollWithMouse);
    m_sceneViewportVisible = open;
    if (!open)
    {
        m_sceneViewportHovered = false;
        ImGui::End();
        return;
    }
    const auto operationButton = [&](const char* label,
                                     const char* tooltip,
                                     const GizmoOperation operation)
    {
        const bool selected = m_gizmoOperation == operation;
        if (selected)
            ImGui::PushStyleColor(
                ImGuiCol_Button,
                ImVec4(0.22f, 0.38f, 0.62f, 1.0f));
        if (ImGui::Button(label)) m_gizmoOperation = operation;
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tooltip);
        if (selected) ImGui::PopStyleColor();
    };
    operationButton("W##Translate", "Translate", GizmoOperation::Translate);
    ImGui::SameLine();
    operationButton("E##Rotate", "Rotate", GizmoOperation::Rotate);
    ImGui::SameLine();
    operationButton("R##Scale", "Scale", GizmoOperation::Scale);
    ImGui::SameLine();
    constexpr std::array<const char*, 3> ShadingModeLabels = {
        "Shaded",
        "Unlit",
        "Wireframe"};
    int shadingMode = static_cast<int>(m_sceneShadingMode);
    ImGui::SetNextItemWidth(112.0f);
    if (ImGui::Combo(
            "##SceneShadingMode",
            &shadingMode,
            ShadingModeLabels.data(),
            static_cast<int>(ShadingModeLabels.size())))
    {
        m_sceneShadingMode =
            static_cast<Renderer::ViewportShadingMode>(
                shadingMode);
    }
    if (ImGui::IsItemHovered())
    {
        ImGui::SetTooltip(
            "Scene shading mode. Game output is unaffected.");
    }
    ImGui::SameLine();
    if (ImGui::Checkbox(
            "Sculpt Terrain",
            &settings.terrainSculptEnabled)
        && settings.terrainSculptEnabled)
    {
        settings.interactiveTerrainEnabled = true;
    }
    ImGui::SameLine();
    if (settings.terrainSculptEnabled)
    {
        ImGui::SetNextItemWidth(120.0f);
        ImGui::SliderFloat(
            "Brush Radius##SceneToolbar",
            &settings.terrainBrushRadius,
            24.0f,
            640.0f,
            "%.0f m");
        ImGui::SameLine();
        ImGui::TextDisabled(
            "LMB raise | Shift+LMB lower | RMB camera");
    }
    else
    {
        ImGui::TextDisabled(
            "Scene Camera | RMB+WASD/QE | MMB pan | Alt+LMB orbit | Wheel dolly");
    }
    const ImVec2 available = ImGui::GetContentRegionAvail();
    const float aspect = sceneColorHeight > 0
        ? static_cast<float>(sceneColorWidth)
            / static_cast<float>(sceneColorHeight)
        : 16.0f / 9.0f;
    ImVec2 imageSize = available;
    if (imageSize.y > 0.0f && imageSize.x / imageSize.y > aspect)
        imageSize.x = imageSize.y * aspect;
    else if (aspect > 0.0f)
        imageSize.y = imageSize.x / aspect;
    const ImVec2 start = ImGui::GetCursorScreenPos();
    const ImVec2 offset{
        std::max(0.0f, (available.x - imageSize.x) * 0.5f),
        std::max(0.0f, (available.y - imageSize.y) * 0.5f)};
    const ImVec2 imageMin{start.x + offset.x, start.y + offset.y};
    ImGui::SetCursorScreenPos(imageMin);
    ImGui::Image(
        static_cast<ImTextureID>(sceneColor),
        imageSize);
    m_viewportMin = {imageMin.x, imageMin.y};
    m_viewportSize = {imageSize.x, imageSize.y};
    const bool imageHovered = ImGui::IsItemHovered();

    if (ImGui::BeginDragDropTarget())
    {
        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(
                AssetDragPayload::TypeName))
        {
            const auto* asset = static_cast<const AssetDragPayload*>(payload->Data);
            if (assetActions.database != nullptr
                && assetActions.instantiateAsset)
            {
                if (const Asset::AssetRecord* record =
                        assetActions.database->FindByPath(asset->assetPath))
                {
                    m_selectedEntity = assetActions.instantiateAsset(*record);
                }
            }
        }
        ImGui::EndDragDropTarget();
    }

    m_sceneViewportHovered = imageHovered;

    if (m_sceneViewportHovered
        && !ImGui::GetIO().WantTextInput
        && !ImGui::IsMouseDown(ImGuiMouseButton_Right))
    {
        if (ImGui::IsKeyPressed(ImGuiKey_W, false)) m_gizmoOperation = GizmoOperation::Translate;
        if (ImGui::IsKeyPressed(ImGuiKey_E, false)) m_gizmoOperation = GizmoOperation::Rotate;
        if (ImGui::IsKeyPressed(ImGuiKey_R, false)) m_gizmoOperation = GizmoOperation::Scale;
        if (ImGui::IsKeyPressed(ImGuiKey_F, false))
        {
            FocusSelectedObject(
                scene,
                cameraController,
                commandActions);
        }
    }
    DrawTransformGizmo(scene, commandActions);
    DrawSceneOverlays(scene, commandActions);
    DrawSceneNavigationGizmo(scene, cameraController);
    const bool brushPreviewEnabled =
        settings.interactiveTerrainEnabled
        && settings.terrainSculptEnabled
        && m_sceneViewportHovered
        && !ImGui::GetIO().KeyAlt
        && !ImGuizmo::IsOver()
        && !ImGuizmo::IsUsing();
    if (brushPreviewEnabled && m_viewportSize.x > 1.0f
        && m_viewportSize.y > 1.0f)
    {
        const ImVec2 mouse = ImGui::GetMousePos();
        const float x = ((mouse.x - m_viewportMin.x)
            / m_viewportSize.x) * 2.0f - 1.0f;
        const float y = 1.0f - ((mouse.y - m_viewportMin.y)
            / m_viewportSize.y) * 2.0f;
        const Scene::Camera& camera = scene.GetCamera();
        const float tanHalfFov = std::tan(
            camera.GetFieldOfViewYRadians() * 0.5f);
        const XMFLOAT3 forward = camera.GetForwardVector();
        const XMFLOAT3 right = camera.GetRightVector();
        const XMFLOAT3 up = camera.GetUpVector();
        XMFLOAT3 direction{};
        XMStoreFloat3(
            &direction,
            XMVector3Normalize(
                XMLoadFloat3(&forward)
                + XMLoadFloat3(&right) * x
                    * camera.GetAspectRatio() * tanHalfFov
                + XMLoadFloat3(&up) * y * tanHalfFov));
        const Core::Double3 origin = camera.GetWorldPosition();
        const double paintPlaneHeight =
            static_cast<double>(settings.terrainBaseHeight)
            + 0.45 * static_cast<double>(
                settings.terrainHeightScale);
        if (std::abs(direction.y) > 1.0e-5f)
        {
            const double distance =
                (paintPlaneHeight - origin.y)
                / static_cast<double>(direction.y);
            if (distance > 0.0)
            {
                const double worldX = origin.x
                    + static_cast<double>(direction.x) * distance;
                const double worldZ = origin.z
                    + static_cast<double>(direction.z) * distance;
                const float inverseWorldSize = 1.0f
                    / std::max(settings.terrainWorldSize, 1.0f);
                const XMFLOAT2 brushUv{
                    static_cast<float>(worldX) * inverseWorldSize
                        + 0.5f,
                    static_cast<float>(worldZ) * inverseWorldSize
                        + 0.5f};
                if (brushUv.x >= 0.0f && brushUv.x <= 1.0f
                    && brushUv.y >= 0.0f && brushUv.y <= 1.0f)
                {
                    constexpr std::size_t BrushSegmentCount = 64;
                    std::array<ImVec2, BrushSegmentCount> brushPoints{};
                    bool brushVisible = true;
                    const XMMATRIX viewProjection =
                        camera.GetRelativeViewProjectionMatrix();
                    for (std::size_t index = 0;
                         index < BrushSegmentCount;
                         ++index)
                    {
                        const double angle = XM_2PI
                            * static_cast<double>(index)
                            / static_cast<double>(BrushSegmentCount);
                        const double ringX = worldX
                            + std::cos(angle)
                                * settings.terrainBrushRadius;
                        const double ringZ = worldZ
                            + std::sin(angle)
                                * settings.terrainBrushRadius;
                        const XMVECTOR clip = XMVector4Transform(
                            XMVectorSet(
                                static_cast<float>(ringX - origin.x),
                                static_cast<float>(paintPlaneHeight - origin.y),
                                static_cast<float>(ringZ - origin.z),
                                1.0f),
                            viewProjection);
                        const float clipW = XMVectorGetW(clip);
                        if (clipW <= 1.0e-4f)
                        {
                            brushVisible = false;
                            break;
                        }
                        const float ndcX = XMVectorGetX(clip) / clipW;
                        const float ndcY = XMVectorGetY(clip) / clipW;
                        brushPoints[index] = {
                            m_viewportMin.x
                                + (ndcX * 0.5f + 0.5f)
                                    * m_viewportSize.x,
                            m_viewportMin.y
                                + (0.5f - ndcY * 0.5f)
                                    * m_viewportSize.y};
                    }
                    if (brushVisible)
                    {
                        const bool lowering = ImGui::GetIO().KeyShift;
                        const bool painting =
                            ImGui::IsMouseDown(ImGuiMouseButton_Left);
                        const ImU32 outlineColor = lowering
                            ? IM_COL32(255, 118, 72, 235)
                            : IM_COL32(65, 205, 255, 235);
                        const ImU32 fillColor = lowering
                            ? IM_COL32(255, 82, 48, painting ? 42 : 24)
                            : IM_COL32(42, 176, 255, painting ? 42 : 24);
                        ImDrawList* drawList = ImGui::GetWindowDrawList();
                        drawList->PushClipRect(
                            ImVec2(m_viewportMin.x, m_viewportMin.y),
                            ImVec2(
                                m_viewportMin.x + m_viewportSize.x,
                                m_viewportMin.y + m_viewportSize.y),
                            true);
                        drawList->AddConvexPolyFilled(
                            brushPoints.data(),
                            static_cast<int>(brushPoints.size()),
                            fillColor);
                        drawList->AddPolyline(
                            brushPoints.data(),
                            static_cast<int>(brushPoints.size()),
                            outlineColor,
                            ImDrawFlags_Closed,
                            painting ? 3.0f : 2.0f);
                        drawList->AddCircleFilled(
                            mouse,
                            painting ? 4.0f : 3.0f,
                            outlineColor);
                        const std::string radiusLabel =
                            "R "
                            + std::to_string(static_cast<int>(
                                std::lround(settings.terrainBrushRadius)))
                            + " m";
                        drawList->AddText(
                            ImVec2(mouse.x + 12.0f, mouse.y + 10.0f),
                            outlineColor,
                            radiusLabel.c_str());
                        drawList->PopClipRect();
                    }
                    if (ImGui::IsMouseDown(ImGuiMouseButton_Left))
                    {
                        settings.terrainBrushUv = brushUv;
                        settings.terrainBrushLower =
                            ImGui::GetIO().KeyShift;
                        settings.terrainBrushDelta =
                            settings.terrainBrushStrength
                            * std::clamp(
                                ImGui::GetIO().DeltaTime,
                                1.0f / 240.0f,
                                1.0f / 15.0f)
                            * (settings.terrainBrushLower
                                ? -1.0f
                                : 1.0f);
                        settings.terrainBrushActive = true;
                        ++settings.terrainBrushCommandRevision;
                    }
                }
            }
        }
    }
    if (m_sceneViewportHovered
        && ImGui::IsMouseClicked(ImGuiMouseButton_Left)
        && !settings.terrainSculptEnabled
        && !ImGuizmo::IsOver()
        && !ImGuizmo::IsUsing()
        && !ImGuizmo::IsViewManipulateHovered())
    {
        const ImVec2 mouse = ImGui::GetMousePos();
        if (!SelectEditorIconAtViewportPosition(
                scene,
                commandActions,
                {mouse.x, mouse.y}))
        {
            SelectObjectAtViewportPosition(
                scene,
                {mouse.x, mouse.y});
        }
    }
    ImGui::End();
}

void EditorLayer::DrawFinalOutput(
    const Scene::CameraController& gameCameraController,
    Renderer::RenderSettings& settings,
    const Renderer::RendererStatistics& rendererStatistics,
    const ViewportPerformanceStats& performanceStats,
    const bool oceanLabActive,
    const bool waveWorksActive,
    const bool fluidLabActive,
    const std::uint64_t gameColor,
    const std::uint32_t gameColorWidth,
    const std::uint32_t gameColorHeight)
{
    m_gameViewportHovered = false;
    ImGui::PushStyleColor(
        ImGuiCol_WindowBg,
        ImVec4(0.012f, 0.014f, 0.018f, 1.0f));
    const bool open = ImGui::Begin(
        "Game",
        nullptr,
        ImGuiWindowFlags_NoScrollbar
            | ImGuiWindowFlags_NoScrollWithMouse);
    m_gameViewportVisible = open;
    if (!open)
    {
        ImGui::End();
        ImGui::PopStyleColor();
        return;
    }

    ImGui::TextDisabled(
        "Game Camera | RMB+WASD/QE | Shift fast | RMB+Wheel speed | %.1f m/s",
        gameCameraController.GetMoveSpeed());
    m_gameDebugOverlay.DrawVisibilityToggle();
    const ImVec2 available = ImGui::GetContentRegionAvail();
    if (gameColor == 0
        || gameColorWidth == 0
        || gameColorHeight == 0
        || available.x <= 0.0f
        || available.y <= 0.0f)
    {
        ImGui::TextDisabled("Game output is unavailable.");
        ImGui::End();
        ImGui::PopStyleColor();
        return;
    }

    const float aspect =
        static_cast<float>(gameColorWidth)
        / static_cast<float>(gameColorHeight);
    ImVec2 imageSize = available;
    if (imageSize.x / imageSize.y > aspect)
    {
        imageSize.x = imageSize.y * aspect;
    }
    else
    {
        imageSize.y = imageSize.x / aspect;
    }
    const ImVec2 start = ImGui::GetCursorScreenPos();
    const ImVec2 imagePosition{
        start.x + std::max(
            0.0f,
            (available.x - imageSize.x) * 0.5f),
        start.y + std::max(
            0.0f,
            (available.y - imageSize.y) * 0.5f)};
    ImGui::SetCursorScreenPos(imagePosition);
    ImGui::Image(
        static_cast<ImTextureID>(gameColor),
        imageSize);
    const bool imageHovered = ImGui::IsItemHovered();
    const bool debugOverlayHovered = m_gameDebugOverlay.Draw(
        performanceStats,
        settings,
        rendererStatistics,
        oceanLabActive,
        waveWorksActive,
        fluidLabActive,
        imagePosition.x,
        imagePosition.y,
        imageSize.x,
        imageSize.y);
    m_gameViewportHovered = imageHovered && !debugOverlayHovered;
    if (m_gameViewportHovered)
    {
        ImGui::SetTooltip(
            "Game Camera output (%u x %u)",
            gameColorWidth,
            gameColorHeight);
    }
    ImGui::End();
    ImGui::PopStyleColor();
}

void EditorLayer::DrawHierarchy(
    Scene::RenderScene& scene,
    Scene::CameraController& cameraController,
    const EditorCommandActions& actions,
    const EditorAssetActions& assetActions)
{
    ImGui::Begin("Hierarchy");
    if (actions.processor == nullptr)
    {
        ImGui::TextDisabled("World is unavailable.");
        ImGui::End();
        return;
    }
    if (ImGui::Button("+ Create"))
    {
        ImGui::OpenPopup("CreateEntityPopup");
    }
    if (ImGui::BeginPopup("CreateEntityPopup"))
    {
        DrawCreateEntityMenu(
            scene,
            actions,
            assetActions);
        ImGui::EndPopup();
    }
    ImGui::SameLine();
    if (!m_selectedEntity.IsValid()) ImGui::BeginDisabled();
    if (ImGui::Button("Delete"))
    {
        if (ExecuteWorldCommand(
                actions, "delete", "entity.delete",
                {{"entity", m_selectedEntity.ToString()}}))
            m_selectedEntity = {};
    }
    if (!m_selectedEntity.IsValid()) ImGui::EndDisabled();
    ImGui::Separator();

    Engine::World& world = actions.processor->GetWorld();
    std::function<void(const Engine::EntityRecord&)> drawNode;
    drawNode = [&](const Engine::EntityRecord& entity)
    {
        bool hasChildren = false;
        for (const auto& [candidateId, candidate] : world.GetEntities())
        {
            (void)candidateId;
            if (candidate.hierarchy.has_value()
                && candidate.hierarchy->parent == entity.id)
            {
                hasChildren = true;
                break;
            }
        }
        ImGuiTreeNodeFlags flags =
            ImGuiTreeNodeFlags_OpenOnArrow
            | ImGuiTreeNodeFlags_SpanAvailWidth;
        if (!hasChildren) flags |= ImGuiTreeNodeFlags_Leaf;
        if (entity.id == m_selectedEntity) flags |= ImGuiTreeNodeFlags_Selected;
        const bool open = ImGui::TreeNodeEx(
            entity.id.ToString().c_str(), flags,
            "%s", entity.name.value.c_str());
        if (ImGui::IsItemClicked()) m_selectedEntity = entity.id;
        if (ImGui::IsItemHovered()
            && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
        {
            m_selectedEntity = entity.id;
            FocusSelectedObject(
                scene,
                cameraController,
                actions);
        }
        if (ImGui::BeginDragDropSource())
        {
            EntityDragPayload payload{};
            strncpy_s(
                payload.entityId,
                entity.id.ToString().c_str(),
                _TRUNCATE);
            ImGui::SetDragDropPayload(EntityDragPayload::TypeName, &payload, sizeof(payload));
            ImGui::TextUnformatted(entity.name.value.c_str());
            ImGui::EndDragDropSource();
        }
        if (ImGui::BeginDragDropTarget())
        {
            if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(EntityDragPayload::TypeName))
            {
                const auto* dropped = static_cast<const EntityDragPayload*>(payload->Data);
                const std::optional<Engine::EntityId> childId = Engine::EntityId::Parse(dropped->entityId);
                Engine::EntityRecord* child = childId.has_value() ? world.FindEntity(*childId) : nullptr;
                if (child != nullptr && child->id != entity.id)
                {
                    const char* command = child->hierarchy.has_value() ? "component.set" : "component.add";
                    ExecuteWorldCommand(actions, "parent", command,
                        {{"entity", child->id.ToString()}, {"component", "Hierarchy"},
                         {"properties", {{"parent", entity.id.ToString()}}}});
                }
            }
            ImGui::EndDragDropTarget();
        }
        if (open)
        {
            for (const auto& [candidateId, candidate] : world.GetEntities())
            {
                (void)candidateId;
                if (candidate.hierarchy.has_value()
                    && candidate.hierarchy->parent == entity.id)
                    drawNode(candidate);
            }
            ImGui::TreePop();
        }
    };
    for (const auto& [id, entity] : world.GetEntities())
    {
        (void)id;
        if (!entity.hierarchy.has_value()
            || !entity.hierarchy->parent.has_value())
            drawNode(entity);
    }
    if (!m_documentStatus.empty())
    {
        ImGui::Separator();
        ImGui::TextWrapped("%s", m_documentStatus.c_str());
    }
    ImGui::End();
}

void EditorLayer::DrawInspector(
    Renderer::RenderSettings& settings,
    const EditorCommandActions& actions,
    const EditorAssetActions& assetActions)
{
    ImGui::Begin("Inspector");
    if (actions.processor == nullptr || !m_selectedEntity.IsValid())
    {
        ImGui::TextDisabled("Select an entity in the viewport or hierarchy.");
    }
    else if (Engine::EntityRecord* entity =
                 actions.processor->GetWorld().FindEntity(m_selectedEntity))
    {
        const PropertyGridActions gridActions{
            actions.processor, assetActions.database, actions.worldChanged};
        const Engine::ReflectionRegistry& reflection = actions.processor->GetReflection();
        for (const Engine::ComponentDescriptor& component : reflection.GetComponents())
        {
            if (!HasComponent(*entity, component.name)) continue;
            const bool open = ImGui::CollapsingHeader(
                component.name.c_str(),
                ImGuiTreeNodeFlags_DefaultOpen);
            if (open)
            {
                m_propertyGrid.DrawComponent(
                    entity->id, component, gridActions);
                if (!component.required)
                {
                    ImGui::PushID(component.name.c_str());
                    if (ImGui::SmallButton("Remove Component"))
                    {
                        ExecuteWorldCommand(
                            actions, "remove-component", "component.remove",
                            {{"entity", entity->id.ToString()},
                             {"component", component.name}});
                    }
                    ImGui::PopID();
                }
            }
        }

        if (ImGui::BeginCombo("Add Component", "Select..."))
        {
            for (const Engine::ComponentDescriptor& component : reflection.GetComponents())
            {
                if (component.required || HasComponent(*entity, component.name)) continue;
                if (component.name == "MaterialOverride"
                    && !entity->meshRenderer.has_value()) continue;
                if (ImGui::Selectable(component.name.c_str()))
                {
                    nlohmann::json arguments = {
                        {"entity", entity->id.ToString()},
                        {"component", component.name}};
                    if (component.name == "MaterialOverride")
                        arguments["properties"] = MakeMaterialOverrideProperties(*entity, actions.runtimeAssets);
                    ExecuteWorldCommand(
                        actions, "add-component", "component.add", std::move(arguments));
                }
            }
            ImGui::EndCombo();
        }
        if (!m_propertyGrid.GetStatus().empty())
            ImGui::TextWrapped("%s", m_propertyGrid.GetStatus().c_str());
    }

    ImGui::SeparatorText("Environment");
    ImGui::Checkbox(
        "Relative-to-Eye Coordinates",
        &settings.relativeToEyeEnabled);
    ImGui::Checkbox("Atmosphere", &settings.skyboxEnabled);
    ImGui::Checkbox(
        "Physical Atmosphere LUT",
        &settings.physicalAtmosphereEnabled);
    ImGui::Checkbox("Virtual Terrain", &settings.virtualTerrainEnabled);
    ImGui::Checkbox(
        "Terrain Virtual Texture",
        &settings.terrainVirtualTextureEnabled);
    ImGui::Checkbox(
        "Terrain Material Colors",
        &settings.terrainMaterialColorsEnabled);
    ImGui::Checkbox(
        "Terrain Tile Boundaries",
        &settings.terrainTileDebugEnabled);
    ImGui::Checkbox(
        "Interactive Terrain Heightfield",
        &settings.interactiveTerrainEnabled);
    if (ImGui::Checkbox(
            "Terrain Erosion Filter",
            &settings.terrainErosionEnabled))
    {
        // Re-run the point-wise filter over the full map without discarding
        // the painted raw height values.
        settings.terrainFullUpdateRequested = true;
        ++settings.terrainBrushCommandRevision;
    }
    if (ImGui::Checkbox(
            "Terrain Sculpt Mode",
            &settings.terrainSculptEnabled)
        && settings.terrainSculptEnabled)
    {
        settings.interactiveTerrainEnabled = true;
    }
    if (settings.terrainSculptEnabled)
    {
        ImGui::Indent();
        ImGui::SliderFloat(
            "Terrain Brush Radius",
            &settings.terrainBrushRadius,
            24.0f,
            640.0f,
            "%.0f m");
        ImGui::SliderFloat(
            "Terrain Brush Strength",
            &settings.terrainBrushStrength,
            0.05f,
            2.0f,
            "%.2f / s");
        ImGui::TextDisabled(
            "Radius controls the mouse-painted area in world meters.");
        ImGui::Unindent();
    }
    if (ImGui::SliderFloat(
        "Terrain Height Scale",
        &settings.terrainHeightScale,
        200.0f,
        1600.0f,
        "%.0f m"))
    {
        settings.terrainBaseHeight =
            -0.45f * settings.terrainHeightScale;
    }
    if (ImGui::Button("Reset Painted Terrain"))
    {
        settings.terrainResetRequested = true;
        ++settings.terrainBrushCommandRevision;
    }
    ImGui::TextDisabled(
        "Game View uses the independent Game Camera.");
    ImGui::Checkbox("FFT Ocean", &settings.fftOceanEnabled);
    ImGui::Checkbox(
        "Ocean Camera Follow",
        &settings.oceanCameraFollowEnabled);
    ImGui::Checkbox("Infinite Grid", &settings.editorGridEnabled);
    if (!settings.physicalAtmosphereEnabled)
    {
        ImGui::ColorEdit3(
            "Sky Zenith",
            &settings.skyZenithColor.x);
        ImGui::ColorEdit3(
            "Sky Horizon",
            &settings.skyHorizonColor.x);
        ImGui::ColorEdit3(
            "Ground Hemisphere",
            &settings.groundColor.x);
        ImGui::SliderFloat(
            "Atmosphere Density",
            &settings.atmosphereDensity,
            0.1f,
            2.5f);
    }
    else
    {
        ImGui::TextDisabled(
            "Zenith and horizon colors come from the atmosphere LUT.");
        if (settings.editorGridEnabled)
        {
            ImGui::ColorEdit3(
                "Editor Grid Ground",
                &settings.groundColor.x);
        }
    }
    ImGui::SliderFloat(
        "Atmosphere Brightness",
        &settings.atmosphereBrightness,
        0.25f,
        4.0f,
        "%.2f");
    if (settings.physicalAtmosphereEnabled)
    {
        ImGui::SliderFloat(
            "Atmosphere Height (km)",
            &settings.atmosphereHeightKm,
            20.0f,
            200.0f);
        ImGui::SliderFloat(
            "Mie Anisotropy",
            &settings.atmosphereMieAnisotropy,
            0.0f,
            0.95f);
        ImGui::SliderFloat(
            "Multiple Scattering",
            &settings.atmosphereMultipleScattering,
            0.0f,
            2.0f);
    }
    if (settings.fftOceanEnabled
        && settings.ocean.implementation
            == Renderer::OceanImplementation::LegacyFft)
    {
        ImGui::ColorEdit3(
            "Ocean Deep Water",
            &settings.oceanDeepWaterColor.x);
        if (ImGui::IsItemHovered())
        {
            ImGui::SetTooltip(
                "Absorbed deep-water body color. Keep this dark and low-saturation.");
        }
        ImGui::ColorEdit3(
            "Ocean Scattering",
            &settings.oceanScatteringColor.x);
        if (ImGui::IsItemHovered())
        {
            ImGui::SetTooltip(
                "Light returned by wave slopes and grazing-angle water scattering.");
        }
        ImGui::ColorEdit3(
            "Ocean Foam",
            &settings.oceanFoamColor.x);
        ImGui::SliderFloat(
            "Ocean Wind Speed",
            &settings.oceanWindSpeed,
            1.0f,
            40.0f,
            "%.1f m/s");
        ImGui::SliderFloat(
            "Ocean Spectrum",
            &settings.oceanSpectrumAmplitude,
            0.05f,
            50.0f,
            "%.2f",
            ImGuiSliderFlags_Logarithmic);
        ImGui::SliderFloat(
            "Ocean Choppiness",
            &settings.oceanChoppiness,
            0.0f,
            2.5f);
    }
    ImGui::SliderFloat("Grid Scale", &settings.gridScale, 0.25f, 10.0f);
    ImGui::SliderFloat("Grid Fade", &settings.gridFadeDistance, 20.0f, 500.0f);
    ImGui::End();
}

void EditorLayer::DrawTransformGizmo(
    Scene::RenderScene& scene,
    const EditorCommandActions& actions)
{
    m_gizmoActive = false;
    Engine::EntityRecord* entity =
        actions.processor != nullptr
        ? actions.processor->GetWorld().FindEntity(
              m_selectedEntity)
        : nullptr;
    const Scene::RenderObject* renderObject =
        FindRenderObject(scene, m_selectedEntity);
    const bool rendererOwnedGeometry = renderObject != nullptr
        && (renderObject->surfaceType == Scene::RenderSurfaceType::VirtualTerrain
            || renderObject->surfaceType == Scene::RenderSurfaceType::EditorDebugLine);
    if (entity == nullptr
        || rendererOwnedGeometry
        || m_viewportSize.x <= 1.0f
        || m_viewportSize.y <= 1.0f)
    {
        // Terrain patches and editor debug primitives are rebuilt by their
        // feature systems. Their object transforms are not authoring data, so
        // presenting a transform gizmo is both misleading and unsafe.
        if (m_gizmoWasActive) CommitEditTransaction(actions);
        m_gizmoWasActive = false;
        return;
    }
    XMFLOAT4X4 view{};
    XMFLOAT4X4 projection{};
    XMFLOAT4X4 world{};
    const Core::Double3 renderOrigin =
        scene.GetCamera().GetWorldPosition();
    const Core::Double3 relativePosition =
        entity->transform.position - renderOrigin;
    const Engine::Float3& entityRotation =
        entity->transform.rotation;
    const Engine::Float3& entityScale =
        entity->transform.scale;
    XMStoreFloat4x4(
        &view,
        XMMatrixTranspose(
            scene.GetCamera().GetRelativeViewMatrix()));
    XMStoreFloat4x4(&projection, XMMatrixTranspose(scene.GetCamera().GetProjectionMatrix()));
    XMStoreFloat4x4(
        &world,
        XMMatrixTranspose(
            XMMatrixScaling(
                entityScale.x,
                entityScale.y,
                entityScale.z)
            * XMMatrixRotationRollPitchYaw(
                entityRotation.x,
                entityRotation.y,
                entityRotation.z)
            * XMMatrixTranslation(
                static_cast<float>(relativePosition.x),
                static_cast<float>(relativePosition.y),
                static_cast<float>(relativePosition.z))));
    ImGuizmo::SetOrthographic(false);
    ImGuizmo::SetDrawlist(ImGui::GetWindowDrawList());
    ImGuizmo::SetRect(m_viewportMin.x, m_viewportMin.y, m_viewportSize.x, m_viewportSize.y);
    ImGuizmo::OPERATION operation = ImGuizmo::TRANSLATE;
    if (m_gizmoOperation == GizmoOperation::Rotate) operation = ImGuizmo::ROTATE;
    if (m_gizmoOperation == GizmoOperation::Scale) operation = ImGuizmo::SCALE;
    ImGuizmo::Manipulate(&view._11, &projection._11, operation, ImGuizmo::LOCAL, &world._11);
    m_gizmoActive = ImGuizmo::IsUsing();
    if (m_gizmoActive && !m_gizmoWasActive) BeginEditTransaction(actions);
    if (m_gizmoActive)
    {
        float translation[3]{};
        float rotation[3]{};
        float scale[3]{};
        ImGuizmo::DecomposeMatrixToComponents(&world._11, translation, rotation, scale);
        const Core::Double3 absolutePosition{
            renderOrigin.x + static_cast<double>(translation[0]),
            renderOrigin.y + static_cast<double>(translation[1]),
            renderOrigin.z + static_cast<double>(translation[2])};
        SubmitTransform(actions, entity->id,
            absolutePosition,
            {XMConvertToRadians(rotation[0]), XMConvertToRadians(rotation[1]), XMConvertToRadians(rotation[2])},
            {scale[0], scale[1], scale[2]});
    }
    if (!m_gizmoActive && m_gizmoWasActive) CommitEditTransaction(actions);
    m_gizmoWasActive = m_gizmoActive;
}

void EditorLayer::DrawSceneNavigationGizmo(
    Scene::RenderScene& scene,
    Scene::CameraController& cameraController)
{
    if (m_viewportSize.x <= 1.0f
        || m_viewportSize.y <= 1.0f)
    {
        return;
    }

    Scene::Camera& camera = scene.GetCamera();
    XMFLOAT4X4 view{};
    XMFLOAT4X4 projection{};
    XMFLOAT4X4 identity{};
    XMStoreFloat4x4(
        &view,
        XMMatrixTranspose(
            camera.GetRelativeViewMatrix()));
    XMStoreFloat4x4(
        &projection,
        XMMatrixTranspose(
            camera.GetProjectionMatrix()));
    XMStoreFloat4x4(
        &identity,
        XMMatrixIdentity());

    constexpr float GizmoSize = 104.0f;
    const ImVec2 position{
        m_viewportMin.x
            + m_viewportSize.x
            - GizmoSize
            - 10.0f,
        m_viewportMin.y + 10.0f};
    ImGuizmo::SetDrawlist(
        ImGui::GetWindowDrawList());
    ImGuizmo::ViewManipulate(
        &view._11,
        &projection._11,
        ImGuizmo::TRANSLATE,
        ImGuizmo::LOCAL,
        &identity._11,
        std::max(
            cameraController.GetOrbitDistance(),
            0.1f),
        position,
        {GizmoSize, GizmoSize},
        IM_COL32(20, 24, 31, 185));

    if (ImGuizmo::IsViewManipulateHovered())
    {
        m_sceneViewportHovered = false;
    }
    if (!ImGuizmo::IsUsingViewManipulate())
    {
        return;
    }

    m_gizmoActive = true;
    const float orbitDistance = std::max(
        cameraController.GetOrbitDistance(),
        0.1f);
    const Core::Double3 originalPosition =
        camera.GetWorldPosition();
    const Core::Double3 target =
        OffsetWorldPosition(
            originalPosition,
            camera.GetForwardVector(),
            orbitDistance);
    const XMMATRIX manipulatedView =
        XMMatrixTranspose(
            XMLoadFloat4x4(&view));
    XMVECTOR determinant{};
    const XMMATRIX inverseView =
        XMMatrixInverse(
            &determinant,
            manipulatedView);
    XMFLOAT3 relativeEye{};
    XMStoreFloat3(
        &relativeEye,
        inverseView.r[3]);
    const Core::Double3 newPosition{
        originalPosition.x + relativeEye.x,
        originalPosition.y + relativeEye.y,
        originalPosition.z + relativeEye.z};
    camera.SetWorldLookAt(
        newPosition,
        target,
        {0.0f, 1.0f, 0.0f});
    cameraController.SynchronizeOrbitPivot(
        camera,
        target);
}

void EditorLayer::DrawSceneOverlays(
    const Scene::RenderScene& scene,
    const EditorCommandActions& actions)
{
    if (m_viewportSize.x <= 1.0f
        || m_viewportSize.y <= 1.0f)
    {
        return;
    }
    ImDrawList* drawList =
        ImGui::GetWindowDrawList();
    drawList->PushClipRect(
        {m_viewportMin.x, m_viewportMin.y},
        {m_viewportMin.x + m_viewportSize.x,
         m_viewportMin.y + m_viewportSize.y},
        true);

    const Scene::Camera& sceneCamera =
        scene.GetCamera();
    const auto drawWorldLine = [&] (
        const Core::Double3& start,
        const Core::Double3& end,
        const ImU32 color,
        const float thickness)
    {
        ImVec2 screenStart{};
        ImVec2 screenEnd{};
        if (ProjectWorldPoint(
                start,
                sceneCamera,
                m_viewportMin,
                m_viewportSize,
                screenStart)
            && ProjectWorldPoint(
                end,
                sceneCamera,
                m_viewportMin,
                m_viewportSize,
                screenEnd))
        {
            drawList->AddLine(
                screenStart,
                screenEnd,
                color,
                thickness);
        }
    };

    // Game Camera is renderer-owned rather than an Engine entity. Drawing its
    // live projection here keeps Scene and Game visibly coupled while the user
    // flies the independent Game viewport.
    const Scene::Camera& gameCamera =
        scene.GetGameCamera();
    const Core::Double3 cameraPosition =
        gameCamera.GetWorldPosition();
    const XMFLOAT3 forward =
        gameCamera.GetForwardVector();
    const XMFLOAT3 right =
        gameCamera.GetRightVector();
    const XMFLOAT3 up =
        gameCamera.GetUpVector();
    const float nearDistance = std::max(
        gameCamera.GetNearPlane(),
        0.01f);
    const float farDistance = std::max(
        nearDistance + 0.01f,
        std::min(
            gameCamera.GetFarPlane(),
            5000.0f));
    const float tangent = std::tan(
        gameCamera.GetFieldOfViewYRadians()
        * 0.5f);
    const auto makeFrustumCorner = [&] (
        const float distance,
        const float horizontalSign,
        const float verticalSign)
    {
        const float halfHeight =
            distance * tangent;
        const float halfWidth =
            halfHeight
            * gameCamera.GetAspectRatio();
        return Core::Double3{
            cameraPosition.x
                + static_cast<double>(forward.x)
                    * distance
                + static_cast<double>(right.x)
                    * halfWidth
                    * horizontalSign
                + static_cast<double>(up.x)
                    * halfHeight
                    * verticalSign,
            cameraPosition.y
                + static_cast<double>(forward.y)
                    * distance
                + static_cast<double>(right.y)
                    * halfWidth
                    * horizontalSign
                + static_cast<double>(up.y)
                    * halfHeight
                    * verticalSign,
            cameraPosition.z
                + static_cast<double>(forward.z)
                    * distance
                + static_cast<double>(right.z)
                    * halfWidth
                    * horizontalSign
                + static_cast<double>(up.z)
                    * halfHeight
                    * verticalSign};
    };
    const std::array<Core::Double3, 8> corners = {
        makeFrustumCorner(nearDistance, -1.0f, -1.0f),
        makeFrustumCorner(nearDistance, 1.0f, -1.0f),
        makeFrustumCorner(nearDistance, 1.0f, 1.0f),
        makeFrustumCorner(nearDistance, -1.0f, 1.0f),
        makeFrustumCorner(farDistance, -1.0f, -1.0f),
        makeFrustumCorner(farDistance, 1.0f, -1.0f),
        makeFrustumCorner(farDistance, 1.0f, 1.0f),
        makeFrustumCorner(farDistance, -1.0f, 1.0f)};
    constexpr std::array<std::array<std::size_t, 2>, 12>
        FrustumEdges = {{
            {{0, 1}}, {{1, 2}}, {{2, 3}}, {{3, 0}},
            {{4, 5}}, {{5, 6}}, {{6, 7}}, {{7, 4}},
            {{0, 4}}, {{1, 5}}, {{2, 6}}, {{3, 7}}}};
    constexpr ImU32 CameraColor =
        IM_COL32(255, 196, 64, 235);
    for (const auto& edge : FrustumEdges)
    {
        drawWorldLine(
            corners[edge[0]],
            corners[edge[1]],
            CameraColor,
            1.7f);
    }
    ImVec2 cameraScreen{};
    if (ProjectWorldPoint(
            cameraPosition,
            sceneCamera,
            m_viewportMin,
            m_viewportSize,
            cameraScreen))
    {
        drawList->AddRectFilled(
            {cameraScreen.x - 8.0f,
             cameraScreen.y - 5.0f},
            {cameraScreen.x + 6.0f,
             cameraScreen.y + 5.0f},
            IM_COL32(30, 32, 38, 230),
            2.0f);
        drawList->AddRect(
            {cameraScreen.x - 8.0f,
             cameraScreen.y - 5.0f},
            {cameraScreen.x + 6.0f,
             cameraScreen.y + 5.0f},
            CameraColor,
            2.0f,
            0,
            2.0f);
        drawList->AddTriangleFilled(
            {cameraScreen.x + 6.0f,
             cameraScreen.y - 4.0f},
            {cameraScreen.x + 13.0f,
             cameraScreen.y - 8.0f},
            {cameraScreen.x + 13.0f,
             cameraScreen.y + 3.0f},
            CameraColor);
        drawList->AddText(
            {cameraScreen.x + 17.0f,
             cameraScreen.y - 9.0f},
            CameraColor,
            "Game Camera");
    }

    if (actions.processor != nullptr)
    {
        for (const auto& [id, entity] :
             actions.processor->GetWorld().GetEntities())
        {
            const bool directional =
                entity.directionalLight.has_value();
            const bool point =
                entity.pointLight.has_value();
            const bool spot =
                entity.spotLight.has_value();
            if (!directional && !point && !spot)
            {
                continue;
            }
            ImVec2 screen{};
            if (!ProjectWorldPoint(
                    entity.transform.position,
                    sceneCamera,
                    m_viewportMin,
                    m_viewportSize,
                    screen))
            {
                continue;
            }
            const ImU32 color = directional
                ? IM_COL32(255, 218, 92, 245)
                : point
                    ? IM_COL32(255, 154, 68, 245)
                    : IM_COL32(92, 178, 255, 245);
            drawList->AddCircleFilled(
                screen,
                8.0f,
                IM_COL32(24, 27, 34, 230));
            drawList->AddCircle(
                screen,
                8.0f,
                id == m_selectedEntity
                    ? IM_COL32(255, 255, 255, 255)
                    : color,
                0,
                id == m_selectedEntity
                    ? 3.0f
                    : 2.0f);
            const char* label = directional
                ? "D" : point ? "P" : "S";
            drawList->AddText(
                {screen.x - 4.0f,
                 screen.y - 7.0f},
                color,
                label);

            if (directional || spot)
            {
                const Engine::Float3 sourceDirection =
                    directional
                    ? entity.directionalLight->direction
                    : entity.spotLight->direction;
                const XMFLOAT3 direction{
                    sourceDirection.x,
                    sourceDirection.y,
                    sourceDirection.z};
                const float length = spot
                    ? std::min(
                          entity.spotLight->range,
                          6.0f)
                    : 4.0f;
                drawWorldLine(
                    entity.transform.position,
                    OffsetWorldPosition(
                        entity.transform.position,
                        direction,
                        length),
                    color,
                    1.5f);
            }
        }
    }
    drawList->PopClipRect();
}

bool EditorLayer::SelectEditorIconAtViewportPosition(
    const Scene::RenderScene& scene,
    const EditorCommandActions& actions,
    const XMFLOAT2& mousePosition)
{
    if (actions.processor == nullptr)
    {
        return false;
    }
    float closestDistanceSquared =
        12.0f * 12.0f;
    Engine::EntityId closest{};
    for (const auto& [id, entity] :
         actions.processor->GetWorld().GetEntities())
    {
        if (!entity.directionalLight.has_value()
            && !entity.pointLight.has_value()
            && !entity.spotLight.has_value())
        {
            continue;
        }
        ImVec2 screen{};
        if (!ProjectWorldPoint(
                entity.transform.position,
                scene.GetCamera(),
                m_viewportMin,
                m_viewportSize,
                screen))
        {
            continue;
        }
        const float distanceSquared =
            ScreenDistanceSquared(
                screen,
                mousePosition);
        if (distanceSquared
            <= closestDistanceSquared)
        {
            closestDistanceSquared = distanceSquared;
            closest = id;
        }
    }
    if (!closest.IsValid())
    {
        return false;
    }
    m_selectedEntity = closest;
    return true;
}

void EditorLayer::SelectObjectAtViewportPosition(
    Scene::RenderScene& scene,
    const XMFLOAT2& mousePosition)
{
    if (m_viewportSize.x <= 1.0f || m_viewportSize.y <= 1.0f) return;
    const float x = ((mousePosition.x - m_viewportMin.x) / m_viewportSize.x) * 2.0f - 1.0f;
    const float y = 1.0f - ((mousePosition.y - m_viewportMin.y) / m_viewportSize.y) * 2.0f;
    const Scene::Camera& camera = scene.GetCamera();
    const float tanHalfFov = std::tan(camera.GetFieldOfViewYRadians() * 0.5f);
    const XMFLOAT3 forward = camera.GetForwardVector();
    const XMFLOAT3 right = camera.GetRightVector();
    const XMFLOAT3 up = camera.GetUpVector();
    const XMVECTOR direction = XMVector3Normalize(
        XMLoadFloat3(&forward)
        + XMLoadFloat3(&right) * x * camera.GetAspectRatio() * tanHalfFov
        + XMLoadFloat3(&up) * y * tanHalfFov);
    // Perform picking in camera-relative coordinates. At planetary-scale
    // absolute positions, converting both endpoints to float first would lose
    // the small separations that make nearby objects selectable.
    const Core::Double3 renderOrigin = camera.GetWorldPosition();
    const XMVECTOR origin = XMVectorZero();
    float closest = std::numeric_limits<float>::max();
    m_selectedEntity = {};
    for (const Scene::RenderObject& object : scene.GetRenderObjects())
    {
        if (!object.visible || object.mesh == nullptr) continue;
        const XMFLOAT3 relativeCenter =
            GetObjectBoundsCenterRelativeTo(
                object,
                renderOrigin);
        float distance = 0.0f;
        if (IntersectSphere(origin, direction, relativeCenter, GetObjectBoundsRadius(object), distance)
            && distance < closest)
        {
            closest = distance;
            m_selectedEntity = object.entityId;
        }
    }
}

void EditorLayer::FocusSelectedObject(
    Scene::RenderScene& scene,
    Scene::CameraController& cameraController,
    const EditorCommandActions& actions)
{
    if (const Scene::RenderObject* object = FindRenderObject(scene, m_selectedEntity))
    {
        cameraController.FocusOnWorldPosition(
            scene.GetCamera(),
            GetObjectBoundsCenterWorld(*object),
            GetObjectBoundsRadius(*object));
        return;
    }
    if (actions.processor != nullptr)
    {
        if (const Engine::EntityRecord* entity =
                actions.processor->GetWorld().FindEntity(
                    m_selectedEntity))
        {
            cameraController.FocusOnWorldPosition(
                scene.GetCamera(),
                entity->transform.position,
                1.0f);
        }
    }
}

Engine::EntityId EditorLayer::CreateWorldEntity(
    Scene::RenderScene& scene,
    const EditorCommandActions& actions,
    const std::string_view name,
    const std::string_view component,
    nlohmann::json componentProperties)
{
    if (actions.processor == nullptr)
    {
        m_documentStatus = "Engine World is unavailable.";
        return {};
    }

    std::string uniqueName(name);
    std::uint32_t matchingNames = 0;
    for (const auto& [id, entity] :
         actions.processor->GetWorld().GetEntities())
    {
        (void)id;
        if (entity.name.value == name
            || entity.name.value.starts_with(
                std::string(name) + " ("))
        {
            ++matchingNames;
        }
    }
    if (matchingNames > 0)
    {
        uniqueName += " ("
            + std::to_string(matchingNames + 1u)
            + ')';
    }

    const auto execute = [&] (
        const std::string_view operation,
        const std::string_view command,
        nlohmann::json arguments)
    {
        return actions.processor->Execute({
            {"requestId", NextRequestId(operation)},
            {"command", command},
            {"arguments", std::move(arguments)}});
    };
    const auto fail = [&] (
        const nlohmann::json& result,
        const std::string_view fallback)
    {
        (void)execute(
            "create-rollback",
            "transaction.rollback",
            nlohmann::json::object());
        m_documentStatus = result.contains("error")
            ? result.at("error").value(
                  "message",
                  std::string(fallback))
            : std::string(fallback);
        return Engine::EntityId{};
    };

    nlohmann::json result = execute(
        "create-begin",
        "transaction.begin",
        nlohmann::json::object());
    if (!result.value("success", false))
    {
        m_documentStatus = result.at("error").value(
            "message",
            std::string("Could not begin entity creation."));
        return {};
    }
    result = execute(
        "create-entity",
        "entity.create",
        {{"name", uniqueName}});
    if (!result.value("success", false))
    {
        return fail(result, "Could not create entity.");
    }
    const std::optional<Engine::EntityId> entityId =
        Engine::EntityId::Parse(
            result.at("data").at("entity")
                .get<std::string>());
    if (!entityId.has_value())
    {
        return fail(
            result,
            "The created entity returned an invalid identifier.");
    }

    const Core::Double3 position =
        GetEditorSpawnPosition(scene.GetCamera());
    const bool orientFromView =
        component == "DirectionalLight"
        || component == "SpotLight";
    const XMFLOAT3 rotation = orientFromView
        ? XMFLOAT3{
              scene.GetCamera().GetPitch(),
              scene.GetCamera().GetYaw(),
              0.0f}
        : XMFLOAT3{};
    result = execute(
        "create-transform",
        "component.set",
        {{"entity", entityId->ToString()},
         {"component", "Transform"},
         {"properties", {
             {"position", {
                 position.x,
                 position.y,
                 position.z}},
             {"rotation", {
                 rotation.x,
                 rotation.y,
                 rotation.z}},
             {"scale", {1.0f, 1.0f, 1.0f}}}}});
    if (!result.value("success", false))
    {
        return fail(result, "Could not position the new entity.");
    }
    if (!component.empty())
    {
        result = execute(
            "create-component",
            "component.add",
            {{"entity", entityId->ToString()},
             {"component", component},
             {"properties", std::move(
                 componentProperties)}});
        if (!result.value("success", false))
        {
            return fail(
                result,
                "Could not add the requested component.");
        }
    }
    result = execute(
        "create-commit",
        "transaction.commit",
        nlohmann::json::object());
    if (!result.value("success", false))
    {
        return fail(result, "Could not commit entity creation.");
    }

    m_selectedEntity = *entityId;
    m_documentStatus = uniqueName + " created.";
    if (actions.worldChanged)
    {
        actions.worldChanged();
    }
    return *entityId;
}

Engine::EntityId EditorLayer::CreatePrimitive(
    Scene::RenderScene& scene,
    const EditorCommandActions& actions,
    const EditorAssetActions& assetActions,
    const std::string_view assetPath)
{
    if (assetActions.database == nullptr
        || !assetActions.instantiateAsset)
    {
        m_documentStatus =
            "The editor asset database is unavailable.";
        return {};
    }
    const Asset::AssetRecord* asset =
        assetActions.database->FindByPath(assetPath);
    if (asset == nullptr)
    {
        m_documentStatus =
            "The built-in primitive asset is unavailable.";
        return {};
    }
    const Engine::EntityId entity =
        assetActions.instantiateAsset(*asset);
    if (!entity.IsValid())
    {
        m_documentStatus =
            "Could not instantiate the primitive asset.";
        return {};
    }
    const Core::Double3 position =
        GetEditorSpawnPosition(scene.GetCamera());
    if (!SubmitTransform(
            actions,
            entity,
            position,
            {},
            {1.0f, 1.0f, 1.0f}))
    {
        return {};
    }
    m_selectedEntity = entity;
    return entity;
}

void EditorLayer::BeginEditTransaction(const EditorCommandActions& actions)
{
    if (m_editTransactionActive || actions.processor == nullptr) return;
    const nlohmann::json result = actions.processor->Execute({
        {"requestId", NextRequestId("begin")},
        {"command", "transaction.begin"},
        {"arguments", nlohmann::json::object()}});
    m_editTransactionActive = result.value("success", false);
}

void EditorLayer::CommitEditTransaction(const EditorCommandActions& actions)
{
    if (!m_editTransactionActive || actions.processor == nullptr) return;
    const nlohmann::json result = actions.processor->Execute({
        {"requestId", NextRequestId("commit")},
        {"command", "transaction.commit"},
        {"arguments", nlohmann::json::object()}});
    m_editTransactionActive = false;
    if (!result.value("success", false))
        m_documentStatus = result.at("error").value("message", std::string("Could not commit editor transaction."));
}

bool EditorLayer::SubmitTransform(
    const EditorCommandActions& actions,
    const Engine::EntityId entity,
    const Core::Double3& position,
    const XMFLOAT3& rotation,
    const XMFLOAT3& scale)
{
    if (!ExecuteWorldCommand(
            actions,
            "transform",
            "component.set",
            {{"entity", entity.ToString()},
             {"component", "Transform"},
             {"properties", {
                 {"position", {
                     position.x,
                     position.y,
                     position.z}},
                 {"rotation", {
                     rotation.x,
                     rotation.y,
                     rotation.z}},
                 {"scale", {
                     scale.x,
                     scale.y,
                     scale.z}}}}}))
    {
        return false;
    }

    if (actions.processor == nullptr)
    {
        return true;
    }
    const Engine::EntityRecord* record =
        actions.processor->GetWorld().FindEntity(entity);
    if (record == nullptr
        || (!record->directionalLight.has_value()
            && !record->spotLight.has_value()))
    {
        return true;
    }
    const XMFLOAT3 direction =
        GetForwardFromRotation(rotation);
    return ExecuteWorldCommand(
        actions,
        "light-direction",
        "component.set",
        {{"entity", entity.ToString()},
         {"component", record->directionalLight.has_value()
             ? "DirectionalLight"
             : "SpotLight"},
         {"properties", {{"direction", {
             direction.x,
             direction.y,
             direction.z}}}}});
}

bool EditorLayer::ExecuteWorldCommand(
    const EditorCommandActions& actions,
    const std::string_view operation,
    const std::string_view command,
    nlohmann::json arguments)
{
    if (actions.processor == nullptr) return false;
    const nlohmann::json result = actions.processor->Execute({
        {"requestId", NextRequestId(operation)},
        {"command", command},
        {"arguments", std::move(arguments)}});
    if (!result.value("success", false))
    {
        m_documentStatus = result.at("error").value(
            "message", std::string("Editor command failed."));
        return false;
    }
    m_documentStatus.clear();
    if (actions.worldChanged) actions.worldChanged();
    return true;
}

std::string EditorLayer::NextRequestId(const std::string_view operation)
{
    return "editor-" + std::string(operation)
        + '-' + std::to_string(++m_requestCounter);
}
}
