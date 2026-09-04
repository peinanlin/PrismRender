#pragma once

#include <DirectXMath.h>

#include <cstddef>
#include <filesystem>
#include <string>
#include <string_view>

namespace Prism::Asset
{
class AssetRegistry;
class AssetStreamingManager;
}

namespace Prism::Scene
{
class RenderScene;

struct AssetStreamingSceneActivationResult
{
    bool ready = false;
    bool success = true;
    bool topologyChanged = false;
    std::size_t renderObjectCount = 0;
    std::string errorCode;
    std::string errorMessage;
};

class AssetStreamingSceneBridge
{
public:
    static AssetStreamingSceneActivationResult Activate(
        std::string_view sceneAssetIdOrPath,
        Asset::AssetStreamingManager& manager,
        Asset::AssetRegistry& registry,
        RenderScene& scene,
        bool clearExistingObjects,
        const DirectX::XMFLOAT3& rootOffset = {});
    static bool WriteReport(
        const std::filesystem::path& path,
        std::string_view graphicsApi,
        std::string_view sceneAssetIdOrPath,
        bool activationAttempted,
        bool activationSucceeded,
        std::size_t activatedObjectCount,
        std::string_view errorCode,
        std::string_view errorMessage,
        const Asset::AssetRegistry& registry,
        const RenderScene& scene,
        std::string* outError = nullptr);
};
} // namespace Prism::Scene
