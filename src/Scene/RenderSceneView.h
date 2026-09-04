#pragma once

#include "Scene/RenderFramePacket.h"
#include "Scene/RenderScene.h"
#include "Scene/RenderViewFeedback.h"

#include <algorithm>
#include <cstddef>
#include <memory>
#include <span>
#include <stdexcept>

namespace Prism::Scene
{
enum class RenderTerrainSelectionPolicy
{
    PreserveHierarchy,
    LeafOnly,
    RootOnly
};

struct RenderSceneControlViewTag final
{
    explicit constexpr RenderSceneControlViewTag() = default;
};
inline constexpr RenderSceneControlViewTag RenderSceneControlView{};

// Per-frame rendering always uses a retained packet. The tagged mutable-scene
// form is restricted to initialization and scene-change lifecycle control,
// where no frame is submitted. Packet-backed views never copy the shared
// object array and retain immutable bindings through graph execution.
class RenderSceneView final
{
public:
    RenderSceneView(
        const RenderSceneControlViewTag,
        const RenderScene& controlScene)
        : m_controlScene(&controlScene)
    {
    }

    RenderSceneView(
        std::shared_ptr<const RenderFramePacket> packet,
        const RenderViewId viewId,
        const RenderTerrainSelectionPolicy terrainPolicy =
            RenderTerrainSelectionPolicy::PreserveHierarchy,
        std::shared_ptr<const RenderViewFeedback> visibilityFeedback = {},
        const RenderViewId expectedFeedbackViewId = GameRenderViewId)
        : m_packet(std::move(packet)),
          m_terrainPolicy(terrainPolicy)
    {
        if (m_packet == nullptr)
        {
            throw std::invalid_argument(
                "RenderSceneView requires a retained frame packet.");
        }
        m_view = m_packet->FindView(viewId);
        if (m_view == nullptr)
        {
            throw std::invalid_argument(
                "RenderSceneView requires a view contained in the packet.");
        }
        if (visibilityFeedback != nullptr
            && visibilityFeedback->Matches(
                *m_packet, expectedFeedbackViewId))
        {
            m_visibilityFeedback = std::move(visibilityFeedback);
        }
    }

    [[nodiscard]] bool IsPacketBacked() const noexcept
    {
        return m_packet != nullptr;
    }

    [[nodiscard]] const std::shared_ptr<const RenderFramePacket>&
        GetPacket() const noexcept
    {
        return m_packet;
    }

    [[nodiscard]] const RenderView* GetView() const noexcept
    {
        return m_view;
    }

    [[nodiscard]] const std::shared_ptr<const RenderViewFeedback>&
        GetVisibilityFeedback() const noexcept
    {
        return m_visibilityFeedback;
    }

    [[nodiscard]] GpuVisibilityReason GetGpuVisibilityReason(
        const std::size_t objectIndex) const
    {
        if (m_visibilityFeedback != nullptr)
        {
            return m_visibilityFeedback
                ->GetVisibilityReasons().at(objectIndex);
        }
        return GetRenderObjects().at(objectIndex).gpuVisibilityReason;
    }

    [[nodiscard]] const Camera& GetCamera() const
    {
        return m_view != nullptr
            ? m_view->camera
            : m_controlScene->GetCamera();
    }

    [[nodiscard]] const Camera& GetPreviousCamera() const
    {
        return m_view != nullptr && m_view->previousCameraValid
            ? m_view->previousCamera
            : GetCamera();
    }

    [[nodiscard]] bool HasPreviousCamera() const noexcept
    {
        return m_view != nullptr && m_view->previousCameraValid;
    }

    [[nodiscard]] const Camera& GetGameCamera() const
    {
        if (m_controlScene != nullptr)
        {
            return m_controlScene->GetGameCamera();
        }
        if (const RenderView* const game =
                m_packet->FindView(GameRenderViewId))
        {
            return game->camera;
        }
        return GetCamera();
    }

    [[nodiscard]] const std::vector<RenderObject>&
        GetRenderObjects() const
    {
        return m_packet != nullptr
            ? m_packet->GetSceneData()->GetRenderObjects()
            : m_controlScene->GetRenderObjects();
    }

    [[nodiscard]] const RenderSceneFeatureUsage&
        GetFeatureUsage() const
    {
        return m_packet != nullptr
            ? m_packet->GetSceneData()->GetFeatureUsage()
            : m_controlSceneFeatureUsage;
    }

    [[nodiscard]] const std::vector<RenderSceneObjectAssetBindings>*
        GetObjectBindings() const noexcept
    {
        return m_packet != nullptr
            ? &m_packet->GetSceneData()->GetObjectBindings()
            : nullptr;
    }

    [[nodiscard]] const RenderSceneObjectAssetBindings*
        FindObjectBindings(const std::size_t objectIndex) const
    {
        const auto* const bindings = GetObjectBindings();
        if (bindings == nullptr)
        {
            return nullptr;
        }
        return &bindings->at(objectIndex);
    }

    [[nodiscard]] const Asset::Mesh* GetMesh(
        const std::size_t objectIndex) const
    {
        if (const RenderSceneObjectAssetBindings* const bindings =
                FindObjectBindings(objectIndex))
        {
            return bindings->mesh.get();
        }
        return GetRenderObjects().at(objectIndex).mesh.get();
    }

    [[nodiscard]] const RenderSceneMaterialBinding*
        FindMaterialBinding(const std::size_t objectIndex) const
    {
        if (const RenderSceneObjectAssetBindings* const bindings =
                FindObjectBindings(objectIndex))
        {
            return bindings->material.hasParameters
                ? &bindings->material
                : nullptr;
        }
        return nullptr;
    }

    [[nodiscard]] const Asset::Texture* GetMaterialTexture(
        const std::size_t objectIndex,
        const MaterialTextureSlot slot) const
    {
        if (const RenderSceneMaterialBinding* const material =
                FindMaterialBinding(objectIndex))
        {
            return material->textures[static_cast<std::size_t>(slot)]
                .resource.get();
        }
        const std::shared_ptr<Asset::Material>& material =
            GetRenderObjects().at(objectIndex).material;
        if (material == nullptr)
        {
            return nullptr;
        }
        switch (slot)
        {
        case MaterialTextureSlot::Albedo:
            return material->GetAlbedoTexture().get();
        case MaterialTextureSlot::MetallicRoughness:
            return material->GetMetallicRoughnessTexture().get();
        case MaterialTextureSlot::Normal:
            return material->GetNormalTexture().get();
        case MaterialTextureSlot::Occlusion:
            return material->GetOcclusionTexture().get();
        case MaterialTextureSlot::Emissive:
            return material->GetEmissiveTexture().get();
        case MaterialTextureSlot::Count:
            break;
        }
        return nullptr;
    }

    [[nodiscard]] bool IsObjectSelected(
        const std::size_t objectIndex) const
    {
        const RenderObject& object = GetRenderObjects().at(objectIndex);
        if (object.quadtreePatch.enabled)
        {
            if (m_visibilityFeedback != nullptr)
            {
                if (object.quadtreePatch.level == 0u)
                {
                    if (GetGpuVisibilityReason(objectIndex)
                        == GpuVisibilityReason::LodRejected)
                    {
                        return false;
                    }
                }
                else
                {
                    const std::uint32_t parentIndex =
                        m_packet->GetSceneData()
                            ->GetObjectMetadata().at(objectIndex)
                            .parentIndex;
                    if (parentIndex
                            == RenderSceneObjectMetadata::InvalidParent
                        || GetGpuVisibilityReason(parentIndex)
                            != GpuVisibilityReason::LodRejected)
                    {
                        return false;
                    }
                }
            }
            else
            {
                if (m_terrainPolicy
                        == RenderTerrainSelectionPolicy::LeafOnly
                    && object.quadtreePatch.level
                        < object.quadtreePatch.maxLevel)
                {
                    return false;
                }
                if (m_terrainPolicy
                        == RenderTerrainSelectionPolicy::RootOnly
                    && object.quadtreePatch.level != 0u)
                {
                    return false;
                }
            }
        }
        if (m_view == nullptr || m_view->selection == nullptr)
        {
            return true;
        }
        const auto& indices = m_view->selection->objectIndices;
        return std::binary_search(
            indices.begin(), indices.end(),
            static_cast<std::uint32_t>(objectIndex));
    }

    [[nodiscard]] DirectX::XMMATRIX GetObjectWorldMatrix(
        const std::size_t objectIndex) const
    {
        const RenderObject& object = GetRenderObjects().at(objectIndex);
        if (m_packet == nullptr
            || m_view == nullptr
            || m_view->id != SceneRenderViewId
            || !object.editorOnly
            || object.surfaceType != RenderSurfaceType::EditorDebugLine
            || object.name != "GameCameraFrustum")
        {
            return object.transform.GetWorldMatrix();
        }
        Transform transform = object.transform;
        const Camera& gameCamera = m_visibilityFeedback != nullptr
            ? m_visibilityFeedback->GetCullingCamera()
            : GetGameCamera();
        transform.SetWorldPosition(gameCamera.GetWorldPosition());
        transform.SetRotationEulerRadians({
            gameCamera.GetPitch(), gameCamera.GetYaw(), 0.0f});
        return transform.GetWorldMatrix();
    }

    [[nodiscard]] DirectX::XMMATRIX GetObjectRelativeWorldMatrix(
        const std::size_t objectIndex,
        const Core::Double3& renderOrigin) const
    {
        const RenderObject& object = GetRenderObjects().at(objectIndex);
        if (m_packet == nullptr
            || m_view == nullptr
            || m_view->id != SceneRenderViewId
            || !object.editorOnly
            || object.surfaceType != RenderSurfaceType::EditorDebugLine
            || object.name != "GameCameraFrustum")
        {
            return object.transform.GetRelativeWorldMatrix(renderOrigin);
        }
        Transform transform = object.transform;
        const Camera& gameCamera = m_visibilityFeedback != nullptr
            ? m_visibilityFeedback->GetCullingCamera()
            : GetGameCamera();
        transform.SetWorldPosition(gameCamera.GetWorldPosition());
        transform.SetRotationEulerRadians({
            gameCamera.GetPitch(), gameCamera.GetYaw(), 0.0f});
        return transform.GetRelativeWorldMatrix(renderOrigin);
    }

    [[nodiscard]] const DirectionalLight&
        GetDirectionalLight() const
    {
        return m_packet != nullptr
            ? m_packet->GetDynamicData().directionalLight
            : m_controlScene->GetDirectionalLight();
    }

    [[nodiscard]] std::span<const DirectionalLight>
        GetAuxiliaryDirectionalLights() const
    {
        if (m_packet != nullptr)
        {
            return m_packet->GetDynamicData()
                .auxiliaryDirectionalLights;
        }
        return m_controlScene->GetAuxiliaryDirectionalLights();
    }

    [[nodiscard]] std::span<const PointLight> GetPointLights() const
    {
        if (m_packet != nullptr)
        {
            return m_packet->GetDynamicData().pointLights;
        }
        return m_controlScene->GetPointLights();
    }

    [[nodiscard]] std::uint32_t GetActivePointLightCount() const
    {
        return m_packet != nullptr
            ? m_packet->GetDynamicData().activePointLightCount
            : m_controlScene->GetActivePointLightCount();
    }

    [[nodiscard]] std::span<const SpotLight> GetSpotLights() const
    {
        if (m_packet != nullptr)
        {
            return m_packet->GetDynamicData().spotLights;
        }
        return m_controlScene->GetSpotLights();
    }

    [[nodiscard]] std::uint32_t GetActiveSpotLightCount() const
    {
        return m_packet != nullptr
            ? m_packet->GetDynamicData().activeSpotLightCount
            : m_controlScene->GetActiveSpotLightCount();
    }

private:
    std::shared_ptr<const RenderFramePacket> m_packet;
    RenderSceneFeatureUsage m_controlSceneFeatureUsage;
    std::shared_ptr<const RenderViewFeedback> m_visibilityFeedback;
    const RenderView* m_view = nullptr;
    const RenderScene* m_controlScene = nullptr;
    RenderTerrainSelectionPolicy m_terrainPolicy =
        RenderTerrainSelectionPolicy::PreserveHierarchy;
};
} // namespace Prism::Scene
