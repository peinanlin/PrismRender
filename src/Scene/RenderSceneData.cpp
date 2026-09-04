#include "Scene/RenderSceneData.h"

#include <stdexcept>
#include <unordered_set>
#include <utility>

namespace Prism::Scene
{
namespace
{
float ResolveAlphaMode(
    const RenderObject& object,
    const RenderSceneObjectAssetBindings& bindings)
{
    if (object.hasMaterialOverride)
    {
        return object.materialOverride.alphaMode;
    }
    return bindings.material.hasParameters
        ? bindings.material.parameters.alphaMode
        : 0.0f;
}
} // namespace

RenderSceneData::RenderSceneData(
    const SceneGeneration sceneGeneration,
    const RenderSceneDataRevision dataRevision,
    std::vector<RenderObject> renderObjects,
    std::vector<RenderSceneObjectMetadata> objectMetadata,
    std::vector<RenderSceneObjectAssetBindings> objectBindings)
    : m_sceneGeneration(sceneGeneration),
      m_dataRevision(dataRevision),
      m_renderObjects(std::move(renderObjects)),
      m_objectMetadata(std::move(objectMetadata)),
      m_objectBindings(std::move(objectBindings))
{
    if (!m_sceneGeneration)
    {
        throw std::invalid_argument(
            "RenderSceneData requires a non-zero scene generation.");
    }
    if (!m_dataRevision)
    {
        throw std::invalid_argument(
            "RenderSceneData requires a non-zero data revision.");
    }
    if (m_renderObjects.size() != m_objectMetadata.size())
    {
        throw std::invalid_argument(
            "RenderSceneData object and metadata counts differ.");
    }
    if (m_renderObjects.size() != m_objectBindings.size())
    {
        throw std::invalid_argument(
            "RenderSceneData object and binding counts differ.");
    }
    std::unordered_set<std::uint32_t> firstInstances;
    for (std::uint32_t index = 0;
         index < m_objectMetadata.size();
         ++index)
    {
        const RenderSceneObjectMetadata& metadata =
            m_objectMetadata[index];
        if (!metadata.id.IsValid()
            || !m_objectIndices.emplace(
                    metadata.id,
                    index).second)
        {
            throw std::invalid_argument(
                "RenderSceneData requires unique valid object IDs.");
        }
        if (!firstInstances.insert(metadata.firstInstance).second)
        {
            throw std::invalid_argument(
                "RenderSceneData requires unique firstInstance values.");
        }
        if (metadata.parentIndex
                != RenderSceneObjectMetadata::InvalidParent
            && metadata.parentIndex >= m_objectMetadata.size())
        {
            throw std::invalid_argument(
                "RenderSceneData contains an invalid parent index.");
        }

        const RenderObject& object = m_renderObjects[index];
        const RenderSceneObjectAssetBindings& bindings =
            m_objectBindings[index];
        const bool renderable = object.visible
            && !object.editorOnly
            && object.surfaceType != RenderSurfaceType::EditorDebugLine
            && bindings.mesh != nullptr
            && bindings.material.hasParameters;
        if (!renderable)
        {
            continue;
        }

        const bool transparent =
            ResolveAlphaMode(object, bindings) >= 1.5f;
        m_featureUsage.hasOpaqueGeometry |= !transparent;
        m_featureUsage.hasTransparentGeometry |= transparent;
        m_featureUsage.hasShadowCaster |= !transparent;
        m_featureUsage.hasTerrainSurface |=
            object.surfaceType == RenderSurfaceType::VirtualTerrain;
        m_featureUsage.hasGpuDrivenCandidate |=
            !transparent
            && object.surfaceType == RenderSurfaceType::Default;
    }
}

SceneGeneration RenderSceneData::GetSceneGeneration() const
{
    return m_sceneGeneration;
}

RenderSceneDataRevision RenderSceneData::GetDataRevision() const
{
    return m_dataRevision;
}

const std::vector<RenderObject>&
RenderSceneData::GetRenderObjects() const
{
    return m_renderObjects;
}

const std::vector<RenderSceneObjectMetadata>&
RenderSceneData::GetObjectMetadata() const
{
    return m_objectMetadata;
}

const std::vector<RenderSceneObjectAssetBindings>&
RenderSceneData::GetObjectBindings() const
{
    return m_objectBindings;
}

const RenderSceneFeatureUsage&
RenderSceneData::GetFeatureUsage() const
{
    return m_featureUsage;
}

std::optional<std::uint32_t> RenderSceneData::FindObjectIndex(
    const RenderObjectId id) const
{
    const auto found = m_objectIndices.find(id);
    return found != m_objectIndices.end()
        ? std::optional<std::uint32_t>(found->second)
        : std::nullopt;
}
} // namespace Prism::Scene
