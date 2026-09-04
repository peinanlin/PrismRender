#pragma once

#include "Engine/SceneChangeTracker.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace Prism::Asset
{
class AssetRegistry;
}

namespace Prism::Engine
{
class World;
}

namespace Prism::Scene
{
class RenderScene;

struct WorldRenderSyncOptions
{
    bool synchronizeCamera = true;
    bool synchronizeLights = true;
    Engine::SceneChangeSet committedChanges;
};

struct UnresolvedRenderAsset
{
    std::string entityId;
    std::string entityName;
    std::string assetType;
    std::string assetPath;
};

struct WorldRenderSyncResult
{
    std::size_t renderObjectCount = 0;
    std::vector<UnresolvedRenderAsset> unresolvedAssets;
    Engine::SceneChangeSet appliedChanges;
};

class WorldRenderSceneBridge
{
public:
    static void ImportRenderScene(
        RenderScene& scene,
        const Asset::AssetRegistry& assetRegistry,
        Engine::World& world,
        std::uint64_t idSeed = 0x505249534d53434eull);

    [[nodiscard]] static WorldRenderSyncResult SynchronizeToRenderScene(
        const Engine::World& world,
        const Asset::AssetRegistry& assetRegistry,
        RenderScene& scene,
        const WorldRenderSyncOptions& options = {});
};
} // namespace Prism::Scene
