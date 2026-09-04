#pragma once

#include "Engine/SceneChangeTracker.h"
#include "Scene/RenderSceneIdentity.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

namespace Prism::Asset
{
class AssetRegistry;
}

namespace Prism::Scene
{
class RenderObjectIdentityRegistry;
class RenderScene;
class RenderSceneData;

enum class RenderScenePublicationMode
{
    FullRebuild,
    Versioned
};

[[nodiscard]] const char* ToString(
    RenderScenePublicationMode mode) noexcept;
[[nodiscard]] RenderScenePublicationMode
ParseRenderScenePublicationMode(std::string_view value);
[[nodiscard]] RenderScenePublicationMode
ReadRenderScenePublicationMode();

struct RenderSceneExtractionRequest
{
    const RenderScene& scene;
    SceneGeneration sceneGeneration;
    RenderSceneDataRevision dataRevision;
    std::uint64_t runtimeAssetBindingRevision = 0;
    Engine::SceneChangeSet changes{};
};

struct RenderSceneExtractionResult
{
    std::shared_ptr<const RenderSceneData> sceneData;
    RenderScenePublicationMode configuredMode =
        RenderScenePublicationMode::Versioned;
    RenderScenePublicationMode effectiveMode =
        RenderScenePublicationMode::Versioned;
    bool rebuilt = false;
    bool reused = false;
    bool conservativeFallback = false;
    // Reuse returns zero. Rebuild reports the number of source objects copied
    // while constructing immutable data; no content hash is evaluated.
    std::size_t sourceObjectVisitCount = 0;
    std::string reason;
};

// Stateful extraction boundary shared by both diagnostic strategies. The
// versioned strategy reuses only a complete immutable value; unknown writes
// visibly fall back to the full rebuild strategy.
class RenderSceneExtractor final
{
public:
    explicit RenderSceneExtractor(
        const Asset::AssetRegistry& assetRegistry,
        RenderScenePublicationMode mode =
            RenderScenePublicationMode::Versioned);
    ~RenderSceneExtractor();

    RenderSceneExtractor(const RenderSceneExtractor&) = delete;
    RenderSceneExtractor& operator=(const RenderSceneExtractor&) = delete;

    [[nodiscard]] RenderSceneExtractionResult Extract(
        const RenderSceneExtractionRequest& request);
    [[nodiscard]] RenderScenePublicationMode GetMode() const noexcept;
    void SetMode(RenderScenePublicationMode mode) noexcept;
    void Reset() noexcept;

private:
    [[nodiscard]] std::shared_ptr<const RenderSceneData> Rebuild(
        const RenderSceneExtractionRequest& request);

    const Asset::AssetRegistry* m_assetRegistry = nullptr;
    RenderScenePublicationMode m_mode =
        RenderScenePublicationMode::Versioned;
    std::unique_ptr<RenderObjectIdentityRegistry> m_identities;
    std::shared_ptr<const RenderSceneData> m_cachedSceneData;
    SceneGeneration m_cachedSceneGeneration{};
    RenderSceneDataRevision m_cachedDataRevision{};
    std::uint64_t m_cachedBindingRevision = 0;
    std::uint64_t m_cachedTopologyRevision = 0;
    std::uint64_t m_cachedRawMutationRevision = 0;
};
} // namespace Prism::Scene
