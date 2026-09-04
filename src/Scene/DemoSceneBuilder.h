#pragma once

#include "Asset/AssetHandle.h"

#include <DirectXMath.h>

#include <cstdint>
#include <memory>
#include <string>

namespace Prism::Asset
{
class AssetRegistry;
class Material;
class Mesh;
}

namespace Prism::RHI
{
class IGraphicsDevice;
}

namespace Prism::Scene
{
struct RenderObject;
class RenderScene;

struct DemoMaterialDescription
{
    std::string name;
    DirectX::XMFLOAT4 albedo{1.0f, 1.0f, 1.0f, 1.0f};
    float metallic = 0.0f;
    float roughness = 0.5f;
    DirectX::XMFLOAT3 emissive{};
    float emissiveStrength = 1.0f;
    bool doubleSided = false;
    // Optional procedural base-color checker. A zero tile count preserves the
    // existing solid-color material path and avoids adding scene draw calls.
    std::uint32_t checkerTileCount = 0;
    DirectX::XMFLOAT4 checkerAlbedo{1.0f, 1.0f, 1.0f, 1.0f};
};

struct DemoMaterialResources
{
    Asset::MaterialHandle handle;
    std::shared_ptr<Asset::Material> material;
};

struct DemoMeshResources
{
    Asset::MeshHandle cubeHandle;
    std::shared_ptr<Asset::Mesh> cube;
    Asset::MeshHandle sphereHandle;
    std::shared_ptr<Asset::Mesh> sphere;
};

class DemoSceneBuilder
{
public:
    static DemoMeshResources CreateMeshes(
        Asset::AssetRegistry& assetRegistry,
        RHI::IGraphicsDevice& device,
        const std::string& rootPath,
        const std::string& namePrefix,
        std::uint32_t sphereLatitudeSegments = 20,
        std::uint32_t sphereLongitudeSegments = 40);

    static DemoMaterialResources CreateMaterial(
        Asset::AssetRegistry& assetRegistry,
        RHI::IGraphicsDevice& device,
        const std::string& rootPath,
        const DemoMaterialDescription& description);

    static RenderObject& AddObject(
        RenderScene& scene,
        const std::string& name,
        Asset::MeshHandle meshHandle,
        const std::shared_ptr<Asset::Mesh>& mesh,
        const DemoMaterialResources& material,
        const DirectX::XMFLOAT3& position,
        const DirectX::XMFLOAT3& scale,
        const DirectX::XMFLOAT3& rotation = {});

    static void ResetLights(RenderScene& scene);
};
} // namespace Prism::Scene
