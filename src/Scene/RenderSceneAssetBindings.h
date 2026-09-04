#pragma once

#include "Asset/AssetHandle.h"
#include "Asset/AssetRegistry.h"
#include "Asset/Material.h"

#include <array>
#include <memory>

namespace Prism::Asset
{
class Mesh;
class Texture;
}

namespace Prism::Scene
{
enum class MaterialTextureSlot : std::size_t
{
    Albedo = 0,
    MetallicRoughness,
    Normal,
    Occlusion,
    Emissive,
    Count
};

struct RenderSceneTextureBinding
{
    Asset::TextureHandle handle;
    Asset::RuntimeAssetBindingRevision revision;
    // This lease keeps the selected runtime resource alive. The resource's
    // internal GPU contents continue to follow the existing upload/retire
    // synchronization contract and are intentionally not deep-copied.
    std::shared_ptr<const Asset::Texture> resource;
};

struct RenderSceneMaterialBinding
{
    Asset::MaterialHandle handle;
    Asset::RuntimeAssetBindingRevision revision;
    bool hasParameters = false;
    // Material CPU parameters are copied so a later reimport or replacement
    // cannot mutate an already-published scene revision.
    Asset::Material::Parameters parameters{};
    std::array<RenderSceneTextureBinding,
        static_cast<std::size_t>(MaterialTextureSlot::Count)> textures{};
};

struct RenderSceneObjectAssetBindings
{
    Asset::MeshHandle meshHandle;
    Asset::RuntimeAssetBindingRevision meshRevision;
    std::shared_ptr<const Asset::Mesh> mesh;
    RenderSceneMaterialBinding material;
};
} // namespace Prism::Scene
