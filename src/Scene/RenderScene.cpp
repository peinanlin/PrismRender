#include "Scene/RenderScene.h"

#include <algorithm>
#include <limits>
#include <stdexcept>
#include <unordered_map>
#include <utility>

namespace Prism::Scene
{
Camera& RenderScene::GetCamera()
{
    return m_camera;
}

const Camera& RenderScene::GetCamera() const
{
    return m_camera;
}

Camera& RenderScene::GetGameCamera()
{
    return m_gameCamera;
}

const Camera& RenderScene::GetGameCamera() const
{
    return m_gameCamera;
}

RenderScene RenderScene::CreateGameView(
    const bool activateGameCamera,
    const bool preserveGpuTerrainHierarchy) const
{
    RenderScene gameView = *this;
    if (activateGameCamera)
    {
        gameView.m_camera = m_gameCamera;
    }
    for (RenderObject& object : gameView.m_renderObjects)
    {
        if (object.editorOnly)
        {
            object.visible = false;
        }
    }
    if (!preserveGpuTerrainHierarchy)
    {
        // Backends without the indexed indirect path cannot let the GPU pick
        // quadtree nodes. Draw only leaf patches on the CPU fallback; drawing
        // parents and children together creates overlapping terrain surfaces.
        std::erase_if(
            gameView.m_renderObjects,
            [](const RenderObject& object)
            {
                return object.quadtreePatch.enabled
                    && object.quadtreePatch.level
                        < object.quadtreePatch.maxLevel;
            });
        for (RenderObject& object : gameView.m_renderObjects)
        {
            if (object.quadtreePatch.enabled)
            {
                object.quadtreePatch.enabled = false;
                object.quadtreePatch.parentObjectIndex =
                    GpuQuadtreePatch::InvalidParent;
                object.quadtreePatch.parentPatchName.clear();
            }
        }
    }
    return gameView;
}

RenderScene RenderScene::CreateEditorView() const
{
    RenderScene editorView = *this;
    for (RenderObject& object : editorView.m_renderObjects)
    {
        if (object.editorOnly
            && object.surfaceType
                == RenderSurfaceType::EditorDebugLine
            && object.name == "GameCameraFrustum")
        {
            object.transform.SetWorldPosition(
                editorView.m_gameCamera.GetWorldPosition());
            object.transform.SetRotationEulerRadians({
                editorView.m_gameCamera.GetPitch(),
                editorView.m_gameCamera.GetYaw(),
                0.0f});
        }
    }
    std::unordered_map<std::string, GpuVisibilityReason>
        hierarchyStates;
    for (const RenderObject& object : editorView.m_renderObjects)
    {
        if (object.quadtreePatch.enabled)
        {
            hierarchyStates.emplace(
                object.name,
                object.gpuVisibilityReason);
        }
    }
    // Match the node granularity selected by the GPU: a split node is replaced
    // by its children, while an unsplit or wholly culled node remains a single
    // tile. This avoids root/child overlap and keeps the colors aligned with
    // the actual culling decisions.
    std::erase_if(
        editorView.m_renderObjects,
        [&hierarchyStates](const RenderObject& object)
        {
            if (!object.quadtreePatch.enabled)
            {
                return false;
            }
            if (object.quadtreePatch.level == 0u)
            {
                return object.gpuVisibilityReason
                    == GpuVisibilityReason::LodRejected;
            }
            const auto parent = hierarchyStates.find(
                object.quadtreePatch.parentPatchName);
            return parent == hierarchyStates.end()
                || parent->second
                    != GpuVisibilityReason::LodRejected;
        });
    for (RenderObject& object : editorView.m_renderObjects)
    {
        if (object.quadtreePatch.enabled)
        {
            object.quadtreePatch.enabled = false;
            object.quadtreePatch.parentObjectIndex =
                GpuQuadtreePatch::InvalidParent;
            object.quadtreePatch.parentPatchName.clear();
        }
    }
    return editorView;
}

const std::vector<RenderObject>& RenderScene::GetRenderObjects() const
{
    return m_renderObjects;
}

std::vector<RenderObject>&
RenderScene::EditRenderObjectsForFullRebuild()
{
    if (m_rawMutationRevision
        == std::numeric_limits<std::uint64_t>::max())
    {
        throw std::overflow_error(
            "RenderScene raw mutation revision capacity exhausted.");
    }
    ++m_rawMutationRevision;
    return m_renderObjects;
}

std::uint64_t RenderScene::GetRawMutationRevision() const noexcept
{
    return m_rawMutationRevision;
}

RenderObject& RenderScene::AddRenderObject(RenderObject renderObject)
{
    if (m_topologyRevision
        == std::numeric_limits<std::uint64_t>::max())
    {
        throw std::overflow_error(
            "RenderScene topology revision capacity exhausted.");
    }
    m_renderObjects.push_back(std::move(renderObject));
    ++m_topologyRevision;
    return m_renderObjects.back();
}

void RenderScene::ClearRenderObjects()
{
    if (m_topologyRevision
        == std::numeric_limits<std::uint64_t>::max())
    {
        throw std::overflow_error(
            "RenderScene topology revision capacity exhausted.");
    }
    m_renderObjects.clear();
    ++m_topologyRevision;
}

void RenderScene::ReplaceRenderObjects(
    std::vector<RenderObject> renderObjects)
{
    if (m_topologyRevision
        == std::numeric_limits<std::uint64_t>::max())
    {
        throw std::overflow_error(
            "RenderScene topology revision capacity exhausted.");
    }
    m_renderObjects = std::move(renderObjects);
    ++m_topologyRevision;
}

std::uint64_t RenderScene::GetTopologyRevision() const noexcept
{
    return m_topologyRevision;
}

DirectionalLight& RenderScene::GetDirectionalLight()
{
    return m_directionalLight;
}

const DirectionalLight& RenderScene::GetDirectionalLight() const
{
    return m_directionalLight;
}

std::array<DirectionalLight, RenderScene::MaxAuxiliaryDirectionalLights>&
RenderScene::GetAuxiliaryDirectionalLights()
{
    return m_auxiliaryDirectionalLights;
}

const std::array<DirectionalLight,
    RenderScene::MaxAuxiliaryDirectionalLights>&
RenderScene::GetAuxiliaryDirectionalLights() const
{
    return m_auxiliaryDirectionalLights;
}

std::array<PointLight, RenderScene::MaxPointLights>& RenderScene::GetPointLights()
{
    return m_pointLights;
}

const std::array<PointLight, RenderScene::MaxPointLights>& RenderScene::GetPointLights() const
{
    return m_pointLights;
}

std::uint32_t RenderScene::GetActivePointLightCount() const
{
    return m_activePointLightCount;
}

void RenderScene::SetActivePointLightCount(const std::uint32_t activePointLightCount)
{
    m_activePointLightCount = std::min<std::uint32_t>(activePointLightCount, static_cast<std::uint32_t>(MaxPointLights));
}

std::array<SpotLight, RenderScene::MaxSpotLights>&
RenderScene::GetSpotLights()
{
    return m_spotLights;
}

const std::array<
    SpotLight,
    RenderScene::MaxSpotLights>&
RenderScene::GetSpotLights() const
{
    return m_spotLights;
}

std::uint32_t
RenderScene::GetActiveSpotLightCount() const
{
    return m_activeSpotLightCount;
}

void RenderScene::SetActiveSpotLightCount(
    const std::uint32_t activeSpotLightCount)
{
    m_activeSpotLightCount =
        std::min<std::uint32_t>(
            activeSpotLightCount,
            static_cast<std::uint32_t>(
                MaxSpotLights));
}
} // namespace Prism::Scene
