#pragma once

#include <cstdint>
#include <compare>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "Asset/AssetHandle.h"

namespace Prism::Asset
{
class Material;
class MaterialAsset;
class Mesh;
class MeshAsset;
class Texture;
class TextureAsset;

struct RuntimeAssetBindingRevision
{
    std::uint64_t value = 0;

    explicit operator bool() const
    {
        return value != 0;
    }

    auto operator<=>(const RuntimeAssetBindingRevision&) const = default;
};

template<typename HandleT, typename ResourceT>
struct RuntimeAssetBinding
{
    HandleT handle;
    RuntimeAssetBindingRevision revision;
    std::shared_ptr<ResourceT> resource;
};

using RuntimeMeshBinding = RuntimeAssetBinding<MeshHandle, Mesh>;
using RuntimeTextureBinding = RuntimeAssetBinding<TextureHandle, Texture>;
using RuntimeMaterialBinding = RuntimeAssetBinding<MaterialHandle, Material>;

enum class RuntimeAssetBindingType : std::uint8_t
{
    Mesh,
    Texture,
    Material
};

enum class RuntimeAssetBindingChangeKind : std::uint8_t
{
    Published,
    Cleared
};

struct RuntimeAssetBindingChange
{
    RuntimeAssetBindingType type = RuntimeAssetBindingType::Mesh;
    RuntimeAssetBindingChangeKind kind =
        RuntimeAssetBindingChangeKind::Published;
    std::uint32_t handleValue = 0;
    RuntimeAssetBindingRevision revision;
};

class AssetRegistry
{
public:
    MeshHandle RegisterMeshAsset(const std::string& path, std::shared_ptr<MeshAsset> meshAsset);
    TextureHandle RegisterTextureAsset(const std::string& path, std::shared_ptr<TextureAsset> textureAsset);
    MaterialHandle RegisterMaterialAsset(const std::string& path, std::shared_ptr<MaterialAsset> materialAsset);

    std::shared_ptr<MeshAsset> GetMeshAsset(MeshHandle handle) const;
    std::shared_ptr<TextureAsset> GetTextureAsset(TextureHandle handle) const;
    std::shared_ptr<MaterialAsset> GetMaterialAsset(MaterialHandle handle) const;

    void SetRuntimeMesh(MeshHandle handle, std::shared_ptr<Mesh> mesh);
    void SetRuntimeTexture(TextureHandle handle, std::shared_ptr<Texture> texture);
    void SetRuntimeMaterial(MaterialHandle handle, std::shared_ptr<Material> material);

    std::shared_ptr<Mesh> GetRuntimeMesh(MeshHandle handle) const;
    std::shared_ptr<Texture> GetRuntimeTexture(TextureHandle handle) const;
    std::shared_ptr<Material> GetRuntimeMaterial(MaterialHandle handle) const;
    RuntimeMeshBinding GetRuntimeMeshBinding(MeshHandle handle) const;
    RuntimeTextureBinding GetRuntimeTextureBinding(TextureHandle handle) const;
    RuntimeMaterialBinding GetRuntimeMaterialBinding(MaterialHandle handle) const;
    [[nodiscard]] const std::vector<RuntimeAssetBindingChange>&
        GetPendingRuntimeBindingChanges() const noexcept;
    std::vector<RuntimeAssetBindingChange>
        ConsumeRuntimeBindingChanges();

    MeshHandle FindMeshByPath(const std::string& path) const;
    TextureHandle FindTextureByPath(const std::string& path) const;
    MaterialHandle FindMaterialByPath(const std::string& path) const;
    const std::string& GetMeshPath(MeshHandle handle) const;
    const std::string& GetTexturePath(TextureHandle handle) const;
    const std::string& GetMaterialPath(MaterialHandle handle) const;

    std::uint32_t GetMeshAssetCount() const;
    std::uint32_t GetTextureAssetCount() const;
    std::uint32_t GetMaterialAssetCount() const;

private:
    RuntimeAssetBindingRevision AllocateRuntimeBindingRevision();

    template<typename HandleT, typename AssetT>
    HandleT RegisterAsset(
        const std::string& path,
        std::shared_ptr<AssetT> asset,
        std::unordered_map<std::string, HandleT>& pathLookup,
        std::vector<std::shared_ptr<AssetT>>& assets,
        std::vector<std::string>& paths);

    template<typename HandleT, typename AssetT>
    std::shared_ptr<AssetT> GetAsset(HandleT handle, const std::vector<std::shared_ptr<AssetT>>& assets) const;

    template<typename HandleT>
    HandleT FindByPath(const std::string& path, const std::unordered_map<std::string, HandleT>& pathLookup) const;

    std::unordered_map<std::string, MeshHandle> m_meshPathLookup;
    std::unordered_map<std::string, TextureHandle> m_texturePathLookup;
    std::unordered_map<std::string, MaterialHandle> m_materialPathLookup;
    std::vector<std::shared_ptr<MeshAsset>> m_meshAssets{1};
    std::vector<std::shared_ptr<TextureAsset>> m_textureAssets{1};
    std::vector<std::shared_ptr<MaterialAsset>> m_materialAssets{1};
    std::vector<std::shared_ptr<Mesh>> m_runtimeMeshes{1};
    std::vector<std::shared_ptr<Texture>> m_runtimeTextures{1};
    std::vector<std::shared_ptr<Material>> m_runtimeMaterials{1};
    std::vector<RuntimeAssetBindingRevision> m_runtimeMeshRevisions{1};
    std::vector<RuntimeAssetBindingRevision> m_runtimeTextureRevisions{1};
    std::vector<RuntimeAssetBindingRevision> m_runtimeMaterialRevisions{1};
    std::uint64_t m_nextRuntimeBindingRevision = 1;
    std::vector<RuntimeAssetBindingChange>
        m_pendingRuntimeBindingChanges;
    std::vector<std::string> m_meshPaths{1};
    std::vector<std::string> m_texturePaths{1};
    std::vector<std::string> m_materialPaths{1};
};
} // namespace Prism::Asset
