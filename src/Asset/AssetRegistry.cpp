#include "Asset/AssetRegistry.h"

#include "Asset/Material.h"
#include "Asset/MaterialAsset.h"
#include "Asset/Mesh.h"
#include "Asset/MeshAsset.h"
#include "Asset/Texture.h"
#include "Asset/TextureAsset.h"

#include <limits>
#include <stdexcept>

namespace Prism::Asset
{
MeshHandle AssetRegistry::RegisterMeshAsset(const std::string& path, std::shared_ptr<MeshAsset> meshAsset)
{
    return RegisterAsset(path, std::move(meshAsset), m_meshPathLookup, m_meshAssets, m_meshPaths);
}

TextureHandle AssetRegistry::RegisterTextureAsset(const std::string& path, std::shared_ptr<TextureAsset> textureAsset)
{
    return RegisterAsset(path, std::move(textureAsset), m_texturePathLookup, m_textureAssets, m_texturePaths);
}

MaterialHandle AssetRegistry::RegisterMaterialAsset(const std::string& path, std::shared_ptr<MaterialAsset> materialAsset)
{
    return RegisterAsset(path, std::move(materialAsset), m_materialPathLookup, m_materialAssets, m_materialPaths);
}

std::shared_ptr<MeshAsset> AssetRegistry::GetMeshAsset(const MeshHandle handle) const
{
    return GetAsset(handle, m_meshAssets);
}

std::shared_ptr<TextureAsset> AssetRegistry::GetTextureAsset(const TextureHandle handle) const
{
    return GetAsset(handle, m_textureAssets);
}

std::shared_ptr<MaterialAsset> AssetRegistry::GetMaterialAsset(const MaterialHandle handle) const
{
    return GetAsset(handle, m_materialAssets);
}

void AssetRegistry::SetRuntimeMesh(const MeshHandle handle, std::shared_ptr<Mesh> mesh)
{
    if (!handle.IsValid())
    {
        return;
    }

    const std::size_t index = static_cast<std::size_t>(handle.Value());
    if (index >= m_runtimeMeshes.size())
    {
        m_runtimeMeshes.resize(index + 1);
        m_runtimeMeshRevisions.resize(index + 1);
    }
    const bool cleared = mesh == nullptr;
    m_runtimeMeshes[index] = std::move(mesh);
    m_runtimeMeshRevisions[index] =
        AllocateRuntimeBindingRevision();
    m_pendingRuntimeBindingChanges.push_back({
        RuntimeAssetBindingType::Mesh,
        cleared ? RuntimeAssetBindingChangeKind::Cleared
                : RuntimeAssetBindingChangeKind::Published,
        handle.Value(),
        m_runtimeMeshRevisions[index]});
}

void AssetRegistry::SetRuntimeTexture(const TextureHandle handle, std::shared_ptr<Texture> texture)
{
    if (!handle.IsValid())
    {
        return;
    }

    const std::size_t index = static_cast<std::size_t>(handle.Value());
    if (index >= m_runtimeTextures.size())
    {
        m_runtimeTextures.resize(index + 1);
        m_runtimeTextureRevisions.resize(index + 1);
    }
    const bool cleared = texture == nullptr;
    m_runtimeTextures[index] = std::move(texture);
    m_runtimeTextureRevisions[index] =
        AllocateRuntimeBindingRevision();
    m_pendingRuntimeBindingChanges.push_back({
        RuntimeAssetBindingType::Texture,
        cleared ? RuntimeAssetBindingChangeKind::Cleared
                : RuntimeAssetBindingChangeKind::Published,
        handle.Value(),
        m_runtimeTextureRevisions[index]});
}

void AssetRegistry::SetRuntimeMaterial(const MaterialHandle handle, std::shared_ptr<Material> material)
{
    if (!handle.IsValid())
    {
        return;
    }

    const std::size_t index = static_cast<std::size_t>(handle.Value());
    if (index >= m_runtimeMaterials.size())
    {
        m_runtimeMaterials.resize(index + 1);
        m_runtimeMaterialRevisions.resize(index + 1);
    }
    const bool cleared = material == nullptr;
    m_runtimeMaterials[index] = std::move(material);
    m_runtimeMaterialRevisions[index] =
        AllocateRuntimeBindingRevision();
    m_pendingRuntimeBindingChanges.push_back({
        RuntimeAssetBindingType::Material,
        cleared ? RuntimeAssetBindingChangeKind::Cleared
                : RuntimeAssetBindingChangeKind::Published,
        handle.Value(),
        m_runtimeMaterialRevisions[index]});
}

std::shared_ptr<Mesh> AssetRegistry::GetRuntimeMesh(const MeshHandle handle) const
{
    return GetAsset(handle, m_runtimeMeshes);
}

std::shared_ptr<Texture> AssetRegistry::GetRuntimeTexture(const TextureHandle handle) const
{
    return GetAsset(handle, m_runtimeTextures);
}

std::shared_ptr<Material> AssetRegistry::GetRuntimeMaterial(const MaterialHandle handle) const
{
    return GetAsset(handle, m_runtimeMaterials);
}

RuntimeMeshBinding AssetRegistry::GetRuntimeMeshBinding(
    const MeshHandle handle) const
{
    const std::size_t index = static_cast<std::size_t>(handle.Value());
    return {
        handle,
        handle.IsValid() && index < m_runtimeMeshRevisions.size()
            ? m_runtimeMeshRevisions[index]
            : RuntimeAssetBindingRevision{},
        GetRuntimeMesh(handle)};
}

RuntimeTextureBinding AssetRegistry::GetRuntimeTextureBinding(
    const TextureHandle handle) const
{
    const std::size_t index = static_cast<std::size_t>(handle.Value());
    return {
        handle,
        handle.IsValid() && index < m_runtimeTextureRevisions.size()
            ? m_runtimeTextureRevisions[index]
            : RuntimeAssetBindingRevision{},
        GetRuntimeTexture(handle)};
}

RuntimeMaterialBinding AssetRegistry::GetRuntimeMaterialBinding(
    const MaterialHandle handle) const
{
    const std::size_t index = static_cast<std::size_t>(handle.Value());
    return {
        handle,
        handle.IsValid() && index < m_runtimeMaterialRevisions.size()
            ? m_runtimeMaterialRevisions[index]
            : RuntimeAssetBindingRevision{},
        GetRuntimeMaterial(handle)};
}

const std::vector<RuntimeAssetBindingChange>&
AssetRegistry::GetPendingRuntimeBindingChanges() const noexcept
{
    return m_pendingRuntimeBindingChanges;
}

std::vector<RuntimeAssetBindingChange>
AssetRegistry::ConsumeRuntimeBindingChanges()
{
    std::vector<RuntimeAssetBindingChange> result;
    result.swap(m_pendingRuntimeBindingChanges);
    return result;
}

RuntimeAssetBindingRevision
AssetRegistry::AllocateRuntimeBindingRevision()
{
    if (m_nextRuntimeBindingRevision
        == std::numeric_limits<std::uint64_t>::max())
    {
        throw std::overflow_error(
            "Runtime asset binding revision capacity exhausted.");
    }
    return RuntimeAssetBindingRevision{
        m_nextRuntimeBindingRevision++};
}

MeshHandle AssetRegistry::FindMeshByPath(const std::string& path) const
{
    return FindByPath(path, m_meshPathLookup);
}

TextureHandle AssetRegistry::FindTextureByPath(const std::string& path) const
{
    return FindByPath(path, m_texturePathLookup);
}

MaterialHandle AssetRegistry::FindMaterialByPath(const std::string& path) const
{
    return FindByPath(path, m_materialPathLookup);
}

const std::string& AssetRegistry::GetMeshPath(const MeshHandle handle) const
{
    static const std::string EmptyPath;
    const std::size_t index = static_cast<std::size_t>(handle.Value());
    return index < m_meshPaths.size() ? m_meshPaths[index] : EmptyPath;
}

const std::string& AssetRegistry::GetTexturePath(const TextureHandle handle) const
{
    static const std::string EmptyPath;
    const std::size_t index = static_cast<std::size_t>(handle.Value());
    return index < m_texturePaths.size() ? m_texturePaths[index] : EmptyPath;
}

const std::string& AssetRegistry::GetMaterialPath(const MaterialHandle handle) const
{
    static const std::string EmptyPath;
    const std::size_t index = static_cast<std::size_t>(handle.Value());
    return index < m_materialPaths.size() ? m_materialPaths[index] : EmptyPath;
}

std::uint32_t AssetRegistry::GetMeshAssetCount() const
{
    return static_cast<std::uint32_t>(m_meshAssets.size() - 1u);
}

std::uint32_t AssetRegistry::GetTextureAssetCount() const
{
    return static_cast<std::uint32_t>(m_textureAssets.size() - 1u);
}

std::uint32_t AssetRegistry::GetMaterialAssetCount() const
{
    return static_cast<std::uint32_t>(m_materialAssets.size() - 1u);
}

template<typename HandleT, typename AssetT>
HandleT AssetRegistry::RegisterAsset(
    const std::string& path,
    std::shared_ptr<AssetT> asset,
    std::unordered_map<std::string, HandleT>& pathLookup,
    std::vector<std::shared_ptr<AssetT>>& assets,
    std::vector<std::string>& paths)
{
    if (!path.empty())
    {
        const auto existing = pathLookup.find(path);
        if (existing != pathLookup.end())
        {
            const std::size_t existingIndex = static_cast<std::size_t>(existing->second.Value());
            if (existingIndex < assets.size())
            {
                assets[existingIndex] = std::move(asset);
            }
            return existing->second;
        }
    }

    const HandleT handle(static_cast<std::uint32_t>(assets.size()));
    assets.push_back(std::move(asset));
    paths.push_back(path);
    if (!path.empty())
    {
        pathLookup.emplace(path, handle);
    }
    return handle;
}

template<typename HandleT, typename AssetT>
std::shared_ptr<AssetT> AssetRegistry::GetAsset(const HandleT handle, const std::vector<std::shared_ptr<AssetT>>& assets) const
{
    if (!handle.IsValid())
    {
        return nullptr;
    }

    const std::size_t index = static_cast<std::size_t>(handle.Value());
    if (index >= assets.size())
    {
        return nullptr;
    }

    return assets[index];
}

template<typename HandleT>
HandleT AssetRegistry::FindByPath(const std::string& path, const std::unordered_map<std::string, HandleT>& pathLookup) const
{
    const auto iterator = pathLookup.find(path);
    if (iterator == pathLookup.end())
    {
        return {};
    }

    return iterator->second;
}
} // namespace Prism::Asset
