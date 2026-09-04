#pragma once

#include "Core/Math/Double3.h"
#include "Engine/EntityId.h"

#include <optional>
#include <string>

namespace Prism::Engine
{
struct Float3
{
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;

    auto operator<=>(const Float3&) const = default;
};

struct Float4
{
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
    float w = 0.0f;

    auto operator<=>(const Float4&) const = default;
};

struct NameComponent
{
    std::string value = "Entity";
};

struct TransformComponent
{
    Core::Double3 position{};
    Float3 rotation{};
    Float3 scale{1.0f, 1.0f, 1.0f};
};

struct HierarchyComponent
{
    std::optional<EntityId> parent;
};

struct MeshRendererComponent
{
    std::string meshAsset;
    std::string materialAsset;
    bool visible = true;
};

// Per-entity material instance parameters. Texture bindings continue to come
// from the base Material Asset, so editing these values never recompiles a
// shader or duplicates texture memory.
struct MaterialOverrideComponent
{
    Float4 albedoColor{1.0f, 1.0f, 1.0f, 1.0f};
    Float3 emissiveColor{0.0f, 0.0f, 0.0f};
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

struct CameraComponent
{
    float fieldOfViewY = 0.785398163f;
    float nearPlane = 0.1f;
    float farPlane = 1000.0f;
};

struct DirectionalLightComponent
{
    Float3 direction{0.0f, -1.0f, 0.0f};
    Float3 color{1.0f, 1.0f, 1.0f};
    float intensity = 4.0f;
};

struct PointLightComponent
{
    Float3 color{1.0f, 1.0f, 1.0f};
    float intensity = 1.0f;
    float range = 10.0f;
    bool castsShadow = true;
};

struct SpotLightComponent
{
    Float3 direction{0.0f, -1.0f, 0.0f};
    Float3 color{1.0f, 1.0f, 1.0f};
    float intensity = 1.0f;
    float range = 10.0f;
    float innerAngleRadians = 0.31415927f;
    float outerAngleRadians = 0.48869219f;
    bool castsShadow = true;
};
} // namespace Prism::Engine
