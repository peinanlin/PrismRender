#pragma once

#include <string>

#include <DirectXMath.h>

namespace Prism::Asset
{
class AssetRegistry;
}

namespace Prism::RHI
{
class IGraphicsDevice;
}

namespace Prism::Scene
{
class RenderScene;

class SceneLoader
{
public:
    bool LoadFromGltf(
        const std::string& path,
        RHI::IGraphicsDevice& device,
        Asset::AssetRegistry& assetRegistry,
        RenderScene& scene,
        std::string* outErrorMessage = nullptr,
        bool clearExistingObjects = true,
        DirectX::XMFLOAT3 rootOffset = {0.0f, 0.0f, 0.0f}) const;
    bool LoadFromGltf(
        const std::string& path,
        RHI::IGraphicsDevice& device,
        RenderScene& scene,
        std::string* outErrorMessage = nullptr,
        bool clearExistingObjects = true,
        DirectX::XMFLOAT3 rootOffset = {0.0f, 0.0f, 0.0f}) const;
};
} // namespace Prism::Scene
