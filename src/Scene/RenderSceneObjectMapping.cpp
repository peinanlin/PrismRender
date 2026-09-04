#include "Scene/RenderSceneObjectMapping.h"

#include "Asset/AssetRegistry.h"
#include "Asset/Material.h"
#include "Asset/MaterialAsset.h"
#include "Scene/RenderSceneData.h"
#include "Scene/RenderView.h"

#include <algorithm>
#include <limits>
#include <stdexcept>
#include <unordered_set>
#include <utility>

namespace Prism::Scene
{
namespace
{
RenderSceneObjectAssetBindings CaptureAssetBindings(
    const RenderObject& object,
    const Asset::AssetRegistry* const assetRegistry)
{
    RenderSceneObjectAssetBindings result{};
    result.meshHandle = object.meshHandle;
    result.material.handle = object.materialHandle;

    std::shared_ptr<Asset::Material> material = object.material;
    if (assetRegistry != nullptr && object.meshHandle.IsValid())
    {
        const Asset::RuntimeMeshBinding binding =
            assetRegistry->GetRuntimeMeshBinding(object.meshHandle);
        result.meshRevision = binding.revision;
        result.mesh = binding.revision
            ? std::move(binding.resource)
            : object.mesh;
    }
    else
    {
        result.mesh = object.mesh;
    }

    if (assetRegistry != nullptr && object.materialHandle.IsValid())
    {
        const Asset::RuntimeMaterialBinding binding =
            assetRegistry->GetRuntimeMaterialBinding(
                object.materialHandle);
        result.material.revision = binding.revision;
        if (binding.revision)
        {
            material = binding.resource;
        }
    }
    if (!material)
    {
        return result;
    }

    result.material.hasParameters = true;
    result.material.parameters = material->GetParameters();
    const std::array<std::shared_ptr<Asset::Texture>, 5>
        materialTextures{
            material->GetAlbedoTexture(),
            material->GetMetallicRoughnessTexture(),
            material->GetNormalTexture(),
            material->GetOcclusionTexture(),
            material->GetEmissiveTexture()};
    std::array<Asset::TextureHandle, 5> textureHandles{};
    if (assetRegistry != nullptr && object.materialHandle.IsValid())
    {
        const std::shared_ptr<Asset::MaterialAsset> materialAsset =
            assetRegistry->GetMaterialAsset(object.materialHandle);
        if (materialAsset)
        {
            textureHandles = {
                materialAsset->GetBaseColorTexture(),
                materialAsset->GetMetallicRoughnessTexture(),
                materialAsset->GetNormalTexture(),
                materialAsset->GetOcclusionTexture(),
                materialAsset->GetEmissiveTexture()};
        }
    }
    for (std::size_t index = 0; index < textureHandles.size(); ++index)
    {
        RenderSceneTextureBinding& texture =
            result.material.textures[index];
        texture.handle = textureHandles[index];
        texture.resource = materialTextures[index];
        if (assetRegistry != nullptr && texture.handle.IsValid())
        {
            const Asset::RuntimeTextureBinding binding =
                assetRegistry->GetRuntimeTextureBinding(texture.handle);
            texture.revision = binding.revision;
            if (binding.revision)
            {
                texture.resource = binding.resource;
            }
        }
    }
    return result;
}

void ValidateParentGraph(
    const std::vector<RenderSceneObjectMetadata>& metadata)
{
    enum class VisitState : std::uint8_t
    {
        Unvisited,
        Visiting,
        Complete
    };
    std::vector<VisitState> states(
        metadata.size(),
        VisitState::Unvisited);
    const auto visit = [&](const auto& self,
                           const std::uint32_t index) -> void
    {
        if (states[index] == VisitState::Complete) return;
        if (states[index] == VisitState::Visiting)
        {
            throw std::invalid_argument(
                "RenderScene object hierarchy contains a cycle.");
        }
        states[index] = VisitState::Visiting;
        const std::uint32_t parentIndex =
            metadata[index].parentIndex;
        if (parentIndex != RenderSceneObjectMetadata::InvalidParent)
        {
            if (parentIndex >= metadata.size())
            {
                throw std::invalid_argument(
                    "RenderScene object hierarchy contains an invalid parent index.");
            }
            self(self, parentIndex);
        }
        states[index] = VisitState::Complete;
    };
    for (std::uint32_t index = 0;
         index < metadata.size();
         ++index)
    {
        visit(visit, index);
    }
}
} // namespace

RenderObjectIdentityRegistry::RenderObjectIdentityRegistry(
    const SceneGeneration sceneGeneration)
    : m_sceneGeneration(sceneGeneration)
{
    if (!m_sceneGeneration)
    {
        throw std::invalid_argument(
            "RenderObject identity registry requires a scene generation.");
    }
}

void RenderObjectIdentityRegistry::ResetTopology(
    const SceneGeneration sceneGeneration)
{
    if (!sceneGeneration)
    {
        throw std::invalid_argument(
            "Topology reset requires a scene generation.");
    }
    if (sceneGeneration == m_sceneGeneration)
    {
        throw std::invalid_argument(
            "Topology reset requires a new scene generation.");
    }
    m_sceneGeneration = sceneGeneration;
    m_nextProgrammaticId = 1;
    m_nextFirstInstance = 0;
    m_programmaticIds.clear();
    m_firstInstances.clear();
}

SceneGeneration
RenderObjectIdentityRegistry::GetSceneGeneration() const
{
    return m_sceneGeneration;
}

RenderObjectId RenderObjectIdentityRegistry::ResolveObjectId(
    const RenderObject& object,
    const std::string_view programmaticKey)
{
    if (object.entityId.IsValid())
    {
        if (!programmaticKey.empty())
        {
            throw std::invalid_argument(
                "World RenderObjects cannot also declare a programmatic key.");
        }
        return RenderObjectId::FromEntityId(object.entityId);
    }
    if (programmaticKey.empty())
    {
        throw std::invalid_argument(
            "Programmatic RenderObjects require a stable key.");
    }
    const std::string key(programmaticKey);
    if (const auto found = m_programmaticIds.find(key);
        found != m_programmaticIds.end())
    {
        return found->second;
    }
    if (m_nextProgrammaticId
        == std::numeric_limits<std::uint64_t>::max())
    {
        throw std::overflow_error(
            "Programmatic RenderObject ID capacity exhausted.");
    }
    const RenderObjectId id{
        RenderObjectIdDomain::Programmatic,
        m_sceneGeneration.value,
        m_nextProgrammaticId++};
    m_programmaticIds.emplace(key, id);
    return id;
}

std::uint32_t RenderObjectIdentityRegistry::ResolveFirstInstance(
    const RenderObjectId id)
{
    if (!id.IsValid())
    {
        throw std::invalid_argument(
            "Cannot assign an instance index to an invalid RenderObject ID.");
    }
    if (const auto found = m_firstInstances.find(id);
        found != m_firstInstances.end())
    {
        return found->second;
    }
    if (m_nextFirstInstance
        == std::numeric_limits<std::uint32_t>::max())
    {
        throw std::overflow_error(
            "RenderObject instance index capacity exhausted.");
    }
    const std::uint32_t firstInstance =
        m_nextFirstInstance++;
    m_firstInstances.emplace(id, firstInstance);
    return firstInstance;
}

std::size_t RenderObjectIdentityRegistry::EntityIdHash::operator()(
    const Engine::EntityId& id) const noexcept
{
    std::size_t result = std::hash<std::uint64_t>{}(id.high);
    result ^= std::hash<std::uint64_t>{}(id.low)
        + 0x9e3779b97f4a7c15ull
        + (result << 6u)
        + (result >> 2u);
    return result;
}

RenderSceneData BuildRenderSceneData(
    RenderObjectIdentityRegistry& identities,
    const RenderSceneDataRevision dataRevision,
    std::vector<RenderSceneObjectSource> sources,
    const Asset::AssetRegistry* const assetRegistry)
{
    if (!dataRevision)
    {
        throw std::invalid_argument(
            "RenderScene data build requires a data revision.");
    }
    if (sources.size()
        > std::numeric_limits<std::uint32_t>::max())
    {
        throw std::overflow_error(
            "RenderScene object count exceeds 32-bit index capacity.");
    }

    std::vector<RenderObject> objects;
    std::vector<RenderSceneObjectMetadata> metadata;
    std::vector<RenderSceneObjectAssetBindings> bindings;
    objects.reserve(sources.size());
    metadata.reserve(sources.size());
    bindings.reserve(sources.size());
    std::unordered_map<Engine::EntityId,
        std::uint32_t,
        RenderObjectIdentityRegistry::EntityIdHash> worldIndices;
    std::unordered_map<std::string, std::uint32_t>
        programmaticIndices;
    std::unordered_set<RenderObjectId, RenderObjectIdHash> objectIds;

    for (std::uint32_t index = 0;
         index < sources.size();
         ++index)
    {
        RenderSceneObjectSource& source = sources[index];
        const RenderObjectId id = identities.ResolveObjectId(
            source.object,
            source.programmaticKey);
        if (!objectIds.insert(id).second)
        {
            throw std::invalid_argument(
                "RenderScene contains a duplicate stable object ID.");
        }
        if (source.object.entityId.IsValid())
        {
            if (!worldIndices.emplace(
                    source.object.entityId,
                    index).second)
            {
                throw std::invalid_argument(
                    "RenderScene contains a duplicate World entity.");
            }
        }
        else if (!programmaticIndices.emplace(
                     source.programmaticKey,
                     index).second)
        {
            throw std::invalid_argument(
                "RenderScene contains a duplicate programmatic key.");
        }
        metadata.push_back({
            id,
            RenderSceneObjectMetadata::InvalidParent,
            identities.ResolveFirstInstance(id)});
        bindings.push_back(CaptureAssetBindings(
            source.object,
            assetRegistry));
        objects.push_back(std::move(source.object));
        // Published CPU values never expose the source's mutable runtime
        // asset objects. Consumers use the versioned binding leases above.
        objects.back().mesh.reset();
        objects.back().material.reset();
    }

    for (std::uint32_t index = 0;
         index < sources.size();
         ++index)
    {
        const RenderSceneObjectSource& source = sources[index];
        if (source.parentEntityId.has_value()
            && !source.parentProgrammaticKey.empty())
        {
            throw std::invalid_argument(
                "RenderScene object declares two parent identities.");
        }
        if (source.parentEntityId.has_value())
        {
            const auto parent = worldIndices.find(
                *source.parentEntityId);
            if (parent == worldIndices.end())
            {
                throw std::invalid_argument(
                    "RenderScene World parent is not present in the snapshot.");
            }
            metadata[index].parentIndex = parent->second;
        }
        else if (!source.parentProgrammaticKey.empty())
        {
            const auto parent = programmaticIndices.find(
                source.parentProgrammaticKey);
            if (parent == programmaticIndices.end())
            {
                throw std::invalid_argument(
                    "RenderScene programmatic parent is not present in the snapshot.");
            }
            metadata[index].parentIndex = parent->second;
        }
    }
    ValidateParentGraph(metadata);
    return RenderSceneData(
        identities.GetSceneGeneration(),
        dataRevision,
        std::move(objects),
        std::move(metadata),
        std::move(bindings));
}

std::shared_ptr<const RenderViewSelection>
BuildRenderViewSelection(
    const RenderSceneData& sceneData,
    const std::uint64_t revision,
    const std::function<bool(
        const RenderObject&,
        const RenderSceneObjectMetadata&)>& predicate)
{
    if (!predicate)
    {
        throw std::invalid_argument(
            "RenderView selection requires a predicate.");
    }
    auto selection = std::make_shared<RenderViewSelection>();
    selection->revision = revision;
    const std::vector<RenderObject>& objects =
        sceneData.GetRenderObjects();
    const std::vector<RenderSceneObjectMetadata>& metadata =
        sceneData.GetObjectMetadata();
    selection->objectIndices.reserve(objects.size());
    for (std::uint32_t index = 0;
         index < objects.size();
         ++index)
    {
        if (predicate(objects[index], metadata[index]))
        {
            selection->objectIndices.push_back(index);
        }
    }
    return selection;
}
} // namespace Prism::Scene
