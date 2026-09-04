#pragma once

#include "Scene/RenderSceneAssetBindings.h"
#include "Scene/RenderSceneObjectMapping.h"

#include <cstdint>
#include <optional>
#include <unordered_map>
#include <vector>

namespace Prism::Scene
{
struct RenderSceneFeatureUsage
{
    bool hasOpaqueGeometry = false;
    bool hasTransparentGeometry = false;
    bool hasShadowCaster = false;
    bool hasTerrainSurface = false;
    bool hasGpuDrivenCandidate = false;
};

// Immutable CPU scene values shared by every view of a frame. Runtime asset
// binding versions are added in 5.4; until then RenderObject retains the
// established shared_ptr bindings without changing WorldRenderSnapshot.
class RenderSceneData final
{
public:
    RenderSceneData(
        SceneGeneration sceneGeneration,
        RenderSceneDataRevision dataRevision,
        std::vector<RenderObject> renderObjects,
        std::vector<RenderSceneObjectMetadata> objectMetadata,
        std::vector<RenderSceneObjectAssetBindings> objectBindings);

    [[nodiscard]] SceneGeneration GetSceneGeneration() const;
    [[nodiscard]] RenderSceneDataRevision GetDataRevision() const;
    [[nodiscard]] const std::vector<RenderObject>&
        GetRenderObjects() const;
    [[nodiscard]] const std::vector<RenderSceneObjectMetadata>&
        GetObjectMetadata() const;
    [[nodiscard]] const std::vector<RenderSceneObjectAssetBindings>&
        GetObjectBindings() const;
    [[nodiscard]] const RenderSceneFeatureUsage&
        GetFeatureUsage() const;
    [[nodiscard]] std::optional<std::uint32_t> FindObjectIndex(
        RenderObjectId id) const;

private:
    SceneGeneration m_sceneGeneration;
    RenderSceneDataRevision m_dataRevision;
    std::vector<RenderObject> m_renderObjects;
    std::vector<RenderSceneObjectMetadata> m_objectMetadata;
    std::vector<RenderSceneObjectAssetBindings> m_objectBindings;
    RenderSceneFeatureUsage m_featureUsage;
    std::unordered_map<RenderObjectId,
        std::uint32_t,
        RenderObjectIdHash> m_objectIndices;
};
} // namespace Prism::Scene
