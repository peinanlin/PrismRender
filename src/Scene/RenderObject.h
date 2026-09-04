#pragma once

#include <memory>
#include <string>
#include <cstdint>

#include "Asset/AssetHandle.h"
#include "Engine/EntityId.h"
#include "Scene/Transform.h"

namespace Prism::Asset
{
class Mesh;
class Material;
}

namespace Prism::Scene
{
enum class RenderSurfaceType : std::uint32_t
{
    Default = 0,
    VirtualTerrain = 1,
    FftOcean = 2,
    EditorDebugLine = 3
};

// Values intentionally match GpuCulling.hlsl so asynchronous GPU readback can
// be applied directly to the editor visualization without translation tables.
enum class GpuVisibilityReason : std::uint32_t
{
    Unknown = 0,
    Visible = 1,
    LodRejected = 2,
    FrustumCulled = 3,
    OcclusionCulled = 4,
    Disabled = 5
};

struct GpuQuadtreePatch
{
    static constexpr std::uint32_t InvalidParent = 0xffffffffu;

    bool enabled = false;
    std::uint32_t parentObjectIndex = InvalidParent;
    std::string parentPatchName;
    std::uint32_t level = 0;
    std::uint32_t maxLevel = 0;
    DirectX::XMFLOAT3 boundsCenter{};
    float boundsRadius = 1.0f;
    float halfExtent = 1.0f;
    float splitThresholdPixels = 150.0f;
};

struct MaterialParameterOverride
{
    DirectX::XMFLOAT4 albedoColor{1.0f, 1.0f, 1.0f, 1.0f};
    DirectX::XMFLOAT3 emissiveColor{0.0f, 0.0f, 0.0f};
    float metallic = 0.0f;
    float roughness = 0.5f;
    float occlusionStrength = 1.0f;
    float normalScale = 1.0f;
    float emissiveStrength = 1.0f;
    float alphaCutoff = 0.5f;
    float alphaMode = 0.0f;
    bool useAlbedoTexture = true;
    bool useMetallicRoughnessTexture = false;
    bool useNormalTexture = false;
    bool useOcclusionTexture = false;
    bool useEmissiveTexture = false;
};

struct RenderObject
{
    Engine::EntityId entityId;
    std::string name;
    Prism::Asset::MeshHandle meshHandle;
    Prism::Asset::MaterialHandle materialHandle;
    std::shared_ptr<Prism::Asset::Mesh> mesh;
    std::shared_ptr<Prism::Asset::Material> material;
    MaterialParameterOverride materialOverride{};
    std::uint64_t materialOverrideSignature = 0;
    bool hasMaterialOverride = false;
    RenderSurfaceType surfaceType =
        RenderSurfaceType::Default;
    // Feature-specific mesh LOD. FFT ocean uses 0/1/2 for its clipmap rings.
    std::uint32_t surfaceLodLevel = 0;
    GpuQuadtreePatch quadtreePatch{};
    GpuVisibilityReason gpuVisibilityReason =
        GpuVisibilityReason::Unknown;
    Transform transform;
    bool visible = true;
    // Editor helpers keep stable object indices but are disabled in the
    // immutable Game View scene before GPU visibility records are generated.
    bool editorOnly = false;
};
} // namespace Prism::Scene
