#include "Scene/RenderSceneExtractor.h"

#include "Asset/AssetRegistry.h"
#include "Core/Environment.h"
#include "Scene/RenderScene.h"
#include "Scene/RenderSceneData.h"
#include "Scene/RenderSceneObjectMapping.h"

#include <stdexcept>
#include <unordered_set>
#include <utility>
#include <vector>

namespace Prism::Scene
{
namespace
{
std::vector<RenderSceneObjectSource> BuildSources(
    const RenderScene& scene)
{
    std::vector<RenderSceneObjectSource> sources;
    const std::vector<RenderObject>& objects =
        scene.GetRenderObjects();
    sources.reserve(objects.size());
    std::unordered_set<std::string> programmaticKeys;
    for (const RenderObject& object : objects)
    {
        RenderSceneObjectSource source{};
        source.object = object;
        if (!object.entityId.IsValid())
        {
            if (object.name.empty())
            {
                throw std::invalid_argument(
                    "Programmatic RenderScene objects require a stable name for extraction.");
            }
            source.programmaticKey = object.name;
            if (!programmaticKeys.insert(source.programmaticKey).second)
            {
                throw std::invalid_argument(
                    "Programmatic RenderScene object names must be unique for extraction.");
            }
            if (!object.quadtreePatch.parentPatchName.empty())
            {
                source.parentProgrammaticKey =
                    object.quadtreePatch.parentPatchName;
            }
        }
        sources.push_back(std::move(source));
    }
    return sources;
}
} // namespace

const char* ToString(const RenderScenePublicationMode mode) noexcept
{
    switch (mode)
    {
    case RenderScenePublicationMode::FullRebuild:
        return "full-rebuild";
    case RenderScenePublicationMode::Versioned:
        return "versioned";
    }
    return "full-rebuild";
}

RenderScenePublicationMode ParseRenderScenePublicationMode(
    const std::string_view value)
{
    if (value == "full-rebuild")
    {
        return RenderScenePublicationMode::FullRebuild;
    }
    if (value.empty() || value == "versioned")
    {
        return RenderScenePublicationMode::Versioned;
    }
    throw std::invalid_argument(
        "PRISM_RENDER_SCENE_PUBLICATION_MODE must be full-rebuild or versioned.");
}

RenderScenePublicationMode ReadRenderScenePublicationMode()
{
    return ParseRenderScenePublicationMode(
        Core::ReadEnvironmentVariableValue(
            "PRISM_RENDER_SCENE_PUBLICATION_MODE"));
}

RenderSceneExtractor::RenderSceneExtractor(
    const Asset::AssetRegistry& assetRegistry,
    const RenderScenePublicationMode mode)
    : m_assetRegistry(&assetRegistry),
      m_mode(mode)
{
}

RenderSceneExtractor::~RenderSceneExtractor() = default;

RenderSceneExtractionResult RenderSceneExtractor::Extract(
    const RenderSceneExtractionRequest& request)
{
    if (!request.sceneGeneration || !request.dataRevision)
    {
        throw std::invalid_argument(
            "RenderScene extraction requires non-zero scene and data revisions.");
    }

    RenderSceneExtractionResult result{};
    result.configuredMode = m_mode;
    result.effectiveMode = m_mode;
    const bool unknownWrite = request.changes.RequiresFullRebuild();
    const bool hasCachedInput = m_cachedSceneData != nullptr;
    const bool sceneGenerationChanged = hasCachedInput
        && m_cachedSceneGeneration != request.sceneGeneration;
    const bool dataRevisionChanged = hasCachedInput
        && m_cachedDataRevision != request.dataRevision;
    const bool topologyRevisionChanged = hasCachedInput
        && m_cachedTopologyRevision
            != request.scene.GetTopologyRevision();
    const bool rawMutationRevisionChanged = hasCachedInput
        && m_cachedRawMutationRevision
            != request.scene.GetRawMutationRevision();
    const bool untrackedTopologyWrite = topologyRevisionChanged
        && !sceneGenerationChanged
        && !dataRevisionChanged
        && !request.changes.HasChanges();
    const bool untrackedRawWrite = rawMutationRevisionChanged
        && !sceneGenerationChanged
        && !dataRevisionChanged
        && !request.changes.HasChanges();
    if (m_mode == RenderScenePublicationMode::Versioned
        && (unknownWrite || untrackedTopologyWrite
            || untrackedRawWrite))
    {
        result.effectiveMode =
            RenderScenePublicationMode::FullRebuild;
        result.conservativeFallback = true;
        result.reason = unknownWrite
            ? "unknown-write-full-rebuild"
            : (untrackedTopologyWrite
                ? "untracked-topology-full-rebuild"
                : "raw-mutable-write-full-rebuild");
    }

    const bool cacheMatches = m_cachedSceneData != nullptr
        && m_cachedSceneGeneration == request.sceneGeneration
        && m_cachedDataRevision == request.dataRevision
        && m_cachedBindingRevision
            == request.runtimeAssetBindingRevision
        && m_cachedTopologyRevision
            == request.scene.GetTopologyRevision();
    if (result.effectiveMode
            == RenderScenePublicationMode::Versioned
        && cacheMatches)
    {
        result.sceneData = m_cachedSceneData;
        result.reused = true;
        result.reason = "unchanged-versioned-input";
        return result;
    }

    const bool firstExtractionOrSceneGenerationChanged =
        m_cachedSceneGeneration != request.sceneGeneration;
    result.sceneData = Rebuild(request);
    result.rebuilt = true;
    result.sourceObjectVisitCount =
        request.scene.GetRenderObjects().size();
    if (result.reason.empty())
    {
        result.reason = m_mode
                == RenderScenePublicationMode::FullRebuild
            ? "configured-full-rebuild"
            : (firstExtractionOrSceneGenerationChanged
                ? "scene-generation-changed"
                : "versioned-input-changed");
    }
    return result;
}

RenderScenePublicationMode RenderSceneExtractor::GetMode() const noexcept
{
    return m_mode;
}

void RenderSceneExtractor::SetMode(
    const RenderScenePublicationMode mode) noexcept
{
    m_mode = mode;
}

void RenderSceneExtractor::Reset() noexcept
{
    m_identities.reset();
    m_cachedSceneData.reset();
    m_cachedSceneGeneration = {};
    m_cachedDataRevision = {};
    m_cachedBindingRevision = 0;
    m_cachedTopologyRevision = 0;
    m_cachedRawMutationRevision = 0;
}

std::shared_ptr<const RenderSceneData> RenderSceneExtractor::Rebuild(
    const RenderSceneExtractionRequest& request)
{
    if (m_identities == nullptr
        || m_identities->GetSceneGeneration()
            != request.sceneGeneration)
    {
        m_identities =
            std::make_unique<RenderObjectIdentityRegistry>(
                request.sceneGeneration);
    }
    auto sceneData = std::make_shared<const RenderSceneData>(
        BuildRenderSceneData(
            *m_identities,
            request.dataRevision,
            BuildSources(request.scene),
            m_assetRegistry));
    m_cachedSceneData = sceneData;
    m_cachedSceneGeneration = request.sceneGeneration;
    m_cachedDataRevision = request.dataRevision;
    m_cachedBindingRevision = request.runtimeAssetBindingRevision;
    m_cachedTopologyRevision =
        request.scene.GetTopologyRevision();
    m_cachedRawMutationRevision =
        request.scene.GetRawMutationRevision();
    return sceneData;
}
} // namespace Prism::Scene
