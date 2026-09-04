#pragma once

#include "Asset/AssetDatabase.h"

#include <cstddef>
#include <filesystem>
#include <string>
#include <vector>

namespace Prism::Asset
{
class AssetRegistry;
}

namespace Prism::RHI
{
class IGraphicsDevice;
}

namespace Prism::Asset
{
struct AssetRuntimeLoadResult
{
    bool success = false;
    std::filesystem::path manifestPath;
    std::string manifestHash;
    std::size_t sourceCount = 0;
    std::size_t meshCount = 0;
    std::size_t materialCount = 0;
    std::size_t textureCount = 0;
    std::size_t cacheHitCount = 0;
    std::size_t cacheMissCount = 0;
    std::uintmax_t cachedBytes = 0;
    std::size_t cookedAssetCount = 0;
    std::uintmax_t cookedBytes = 0;
    std::string errorCode;
    std::string errorMessage;
    std::vector<AssetDiagnostic> diagnostics;
};

class AssetRuntimeLoader
{
public:
    [[nodiscard]] static AssetRuntimeLoadResult LoadManifest(
        const std::filesystem::path& projectRoot,
        const std::filesystem::path& manifestPath,
        RHI::IGraphicsDevice& device,
        AssetRegistry& registry);
};
} // namespace Prism::Asset
