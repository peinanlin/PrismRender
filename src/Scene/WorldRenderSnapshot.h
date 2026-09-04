#pragma once

#include "Asset/AssetDatabase.h"
#include "Scene/WorldRenderSceneBridge.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace Prism::Asset
{
class AssetRegistry;
}

namespace Prism::Engine
{
class CommandProcessor;
}

namespace Prism::Scene
{
class RenderScene;

struct WorldRenderSnapshotResult
{
    bool success = false;
    std::filesystem::path snapshotPath;
    std::string expectedWorldHash;
    std::string renderedWorldHash;
    std::string expectedAssetManifestHash;
    std::string renderedAssetManifestHash;
    std::size_t loadedAssetSourceCount = 0;
    std::size_t assetCacheHitCount = 0;
    std::size_t assetCacheMissCount = 0;
    std::uintmax_t assetCachedBytes = 0;
    std::size_t cookedAssetCount = 0;
    std::uintmax_t cookedAssetBytes = 0;
    std::size_t entityCount = 0;
    std::size_t cameraCount = 0;
    std::size_t directionalLightCount = 0;
    std::size_t renderObjectCount = 0;
    std::vector<UnresolvedRenderAsset> unresolvedAssets;
    std::vector<Asset::AssetDiagnostic> assetDiagnostics;
    std::string errorCode;
    std::string errorMessage;
};

[[nodiscard]] WorldRenderSnapshotResult LoadWorldRenderSnapshot(
    const std::filesystem::path& snapshotPath,
    std::string expectedWorldHash,
    Engine::CommandProcessor& processor,
    const Asset::AssetRegistry& assetRegistry,
    RenderScene& scene);

bool WriteWorldRenderReceipt(
    const std::filesystem::path& path,
    const WorldRenderSnapshotResult& result,
    std::string_view graphicsApi,
    std::string* outError = nullptr);
} // namespace Prism::Scene
