#pragma once

#include "Scene/RenderObject.h"
#include "Scene/RenderSceneIdentity.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace Prism::Asset
{
class AssetRegistry;
}

namespace Prism::Scene
{
class RenderSceneData;
struct RenderViewSelection;

struct RenderSceneObjectSource
{
    RenderObject object;
    // Required only when object.entityId is invalid. Keys are scoped to the
    // active SceneGeneration and survive data-revision rebuilds.
    std::string programmaticKey;
    std::optional<Engine::EntityId> parentEntityId;
    std::string parentProgrammaticKey;
};

struct RenderSceneObjectMetadata
{
    static constexpr std::uint32_t InvalidParent = 0xffffffffu;

    RenderObjectId id;
    std::uint32_t parentIndex = InvalidParent;
    // Stable GPU/object-record index within the current topology generation.
    std::uint32_t firstInstance = 0;

    auto operator<=>(const RenderSceneObjectMetadata&) const = default;
};

class RenderObjectIdentityRegistry final
{
public:
    explicit RenderObjectIdentityRegistry(
        SceneGeneration sceneGeneration);

    void ResetTopology(SceneGeneration sceneGeneration);
    [[nodiscard]] SceneGeneration GetSceneGeneration() const;
    [[nodiscard]] RenderObjectId ResolveObjectId(
        const RenderObject& object,
        std::string_view programmaticKey);
    [[nodiscard]] std::uint32_t ResolveFirstInstance(
        RenderObjectId id);

public:
    struct EntityIdHash
    {
        [[nodiscard]] std::size_t operator()(
            const Engine::EntityId& id) const noexcept;
    };

private:
    SceneGeneration m_sceneGeneration;
    std::uint64_t m_nextProgrammaticId = 1;
    std::uint32_t m_nextFirstInstance = 0;
    std::unordered_map<std::string, RenderObjectId>
        m_programmaticIds;
    std::unordered_map<RenderObjectId,
        std::uint32_t,
        RenderObjectIdHash> m_firstInstances;
};

[[nodiscard]] RenderSceneData BuildRenderSceneData(
    RenderObjectIdentityRegistry& identities,
    RenderSceneDataRevision dataRevision,
    std::vector<RenderSceneObjectSource> sources,
    const Asset::AssetRegistry* assetRegistry = nullptr);

[[nodiscard]] std::shared_ptr<const RenderViewSelection>
BuildRenderViewSelection(
    const RenderSceneData& sceneData,
    std::uint64_t revision,
    const std::function<bool(
        const RenderObject&,
        const RenderSceneObjectMetadata&)>& predicate);
} // namespace Prism::Scene
