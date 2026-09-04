#include "Asset/AssetRuntimeLoader.h"

#include "Asset/AssetCache.h"
#include "Asset/AssetRegistry.h"
#include "Asset/CookedAssetIO.h"
#include "Asset/Material.h"
#include "Asset/MaterialAsset.h"
#include "Asset/Mesh.h"
#include "Asset/Texture.h"
#include "Asset/TextureAsset.h"
#include "Core/CpuTrace.h"
#include "RHI/IGraphicsDevice.h"

#include <array>
#include <cctype>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

namespace Prism::Asset
{
namespace
{
std::string Lowercase(std::string value)
{
    for (char& character : value)
    {
        character = static_cast<char>(
            std::tolower(static_cast<unsigned char>(character)));
    }
    return value;
}

bool IsPathWithin(
    const std::filesystem::path& root,
    const std::filesystem::path& candidate)
{
    auto rootIterator = root.begin();
    auto candidateIterator = candidate.begin();
    for (; rootIterator != root.end();
         ++rootIterator, ++candidateIterator)
    {
        if (candidateIterator == candidate.end()
            || Lowercase(rootIterator->string())
                != Lowercase(candidateIterator->string()))
        {
            return false;
        }
    }
    return true;
}

bool ResolveCookedPath(
    const std::filesystem::path& projectRoot,
    const AssetRecord& record,
    const std::string_view expectedFormat,
    std::filesystem::path& path,
    std::string& errorCode,
    std::string& errorMessage)
{
    const nlohmann::json cooked = record.metadata.value(
        "cooked",
        nlohmann::json::object());
    if (cooked.value("format", std::string{})
            != expectedFormat
        || !CookedAssetIO::IsSupportedVersion(
            cooked.value("version", 0u))
        || cooked.value("path", std::string{}).empty())
    {
        errorCode = "cooked_asset_metadata_invalid";
        errorMessage =
            "An imported asset has no compatible Cooked Asset metadata. Run asset.reimport.";
        return false;
    }

    const std::filesystem::path normalizedRoot =
        std::filesystem::absolute(projectRoot).lexically_normal();
    path = std::filesystem::absolute(
        normalizedRoot
        / cooked.at("path").get<std::string>()).lexically_normal();
    if (!IsPathWithin(normalizedRoot, path))
    {
        errorCode = "cooked_asset_path_invalid";
        errorMessage =
            "A Cooked Asset path escapes the project root.";
        return false;
    }
    if (!std::filesystem::is_regular_file(path))
    {
        errorCode = "cooked_asset_missing";
        errorMessage =
            "A Cooked Asset is missing. Run asset.reimport.";
        return false;
    }
    return true;
}

Material::Parameters CreateMaterialParameters(const MaterialAsset& asset)
{
    Material::Parameters parameters{};
    parameters.albedoColor = asset.GetAlbedoColor();
    parameters.specularColor = asset.GetSpecularColor();
    parameters.emissiveColor = asset.GetEmissiveColor();
    parameters.metallic = asset.GetMetallic();
    parameters.roughness = asset.GetRoughness();
    parameters.shininess = asset.GetShininess();
    parameters.occlusionStrength = asset.GetOcclusionStrength();
    parameters.normalScale = asset.GetNormalScale();
    parameters.emissiveStrength = asset.GetEmissiveStrength();
    parameters.alphaCutoff = asset.GetAlphaCutoff();
    parameters.alphaMode = asset.GetAlphaMode();
    parameters.doubleSided = asset.GetDoubleSided();
    parameters.useAlbedoTexture = asset.GetUseAlbedoTexture();
    parameters.useMetallicRoughnessTexture = asset.GetUseMetallicRoughnessTexture();
    parameters.useNormalTexture = asset.GetUseNormalTexture();
    parameters.useOcclusionTexture = asset.GetUseOcclusionTexture();
    parameters.useEmissiveTexture = asset.GetUseEmissiveTexture();
    return parameters;
}

std::shared_ptr<Texture> CreateTexture(
    RHI::IGraphicsDevice& device,
    const std::shared_ptr<TextureAsset>& asset)
{
    auto texture = std::make_shared<Texture>();
    if (asset->HasImageData())
    {
        texture->InitializeRgba8(
            device,
            asset->GetWidth(),
            asset->GetHeight(),
            asset->GetImageData().data());
    }
    else
    {
        texture->InitializeSolidColor(device, asset->GetSolidColor());
    }
    texture->SetDebugName(asset->GetName());
    return texture;
}

std::shared_ptr<Material> CreateMaterial(
    const Material::Parameters& parameters,
    const std::shared_ptr<Texture>& albedo,
    const std::shared_ptr<Texture>& metallicRoughness,
    const std::shared_ptr<Texture>& normal,
    const std::shared_ptr<Texture>& occlusion,
    const std::shared_ptr<Texture>& emissive)
{
    auto material = std::make_shared<Material>();
    material->Initialize(
        parameters,
        albedo,
        metallicRoughness,
        normal,
        occlusion,
        emissive);
    return material;
}

AssetRuntimeLoadResult LoadManifestInternal(
    const std::filesystem::path& projectRoot,
    const std::filesystem::path& manifestPath,
    RHI::IGraphicsDevice& device,
    AssetRegistry& registry)
{
    Core::CpuTraceSpan runtimeLoadSpan(
        "AssetRuntimeLoad",
        "asset");
    AssetRuntimeLoadResult result{};
    AssetDatabase database(projectRoot, manifestPath);
    result.manifestPath = database.GetManifestPath();
    std::string manifestError;
    Core::CpuTraceSpan manifestSpan(
        "AssetManifestLoad",
        "asset");
    if (!database.Load(&manifestError))
    {
        result.errorCode = "asset_manifest_load_failed";
        result.errorMessage = manifestError;
        return result;
    }
    manifestSpan.End();
    result.manifestHash = database.ComputeManifestHash();
    Core::CpuTraceSpan validationSpan(
        "AssetManifestValidation",
        "asset");
    if (!database.ValidateImportedContent(result.diagnostics))
    {
        result.errorCode = "asset_source_stale";
        result.errorMessage =
            "Imported asset content differs from the Asset Manifest. Run asset.reimport.";
        return result;
    }
    validationSpan.End();

    Core::CpuTraceSpan cacheSpan(
        "AssetSourceCacheResolve",
        "asset");
    for (const AssetRecord& source : database.GetImportedScenes())
    {
        const AssetCacheBundle cache =
            AssetCache(database.GetProjectRoot()).Resolve(source);
        if (cache.valid)
        {
            ++result.cacheHitCount;
            result.cachedBytes += cache.totalBytes;
        }
        else
        {
            ++result.cacheMissCount;
            result.diagnostics.push_back({
                AssetDiagnosticSeverity::Warning,
                cache.errorCode,
                cache.errorMessage,
                cache.bundlePath.generic_string()});
        }
        ++result.sourceCount;
    }
    cacheSpan.End();

    const std::vector<AssetRecord> records = database.List();
    const auto resolvePath = [&](
        const AssetRecord& record,
        const std::string_view expectedFormat,
        std::filesystem::path& path) -> bool
    {
        if (!ResolveCookedPath(
                database.GetProjectRoot(),
                record,
                expectedFormat,
                path,
                result.errorCode,
                result.errorMessage))
        {
            result.diagnostics.push_back({
                AssetDiagnosticSeverity::Error,
                result.errorCode,
                result.errorMessage,
                record.assetPath});
            return false;
        }
        ++result.cookedAssetCount;
        result.cookedBytes += std::filesystem::file_size(path);
        return true;
    };

    Core::CpuTraceSpan textureSpan(
        "CookedTextureLoad",
        "asset");
    for (const AssetRecord& record : records)
    {
        if (record.builtIn || record.type != AssetType::Texture)
        {
            continue;
        }
        std::filesystem::path cookedPath;
        if (!resolvePath(record, "PrismCookedTexture", cookedPath))
        {
            return result;
        }
        std::shared_ptr<TextureAsset> textureAsset;
        std::string readError;
        if (!CookedAssetIO::ReadTexture(
                cookedPath,
                textureAsset,
                &readError))
        {
            result.errorCode = "cooked_texture_read_failed";
            result.errorMessage = readError;
            result.diagnostics.push_back({
                AssetDiagnosticSeverity::Error,
                result.errorCode,
                result.errorMessage,
                cookedPath.generic_string()});
            return result;
        }
        const TextureHandle handle = registry.RegisterTextureAsset(
            record.assetPath,
            textureAsset);
        registry.SetRuntimeTexture(
            handle,
            CreateTexture(device, textureAsset));
        ++result.textureCount;
    }
    textureSpan.End();

    Core::CpuTraceSpan meshSpan(
        "CookedMeshLoad",
        "asset");
    for (const AssetRecord& record : records)
    {
        if (record.builtIn || record.type != AssetType::Mesh)
        {
            continue;
        }
        std::filesystem::path cookedPath;
        if (!resolvePath(record, "PrismCookedMesh", cookedPath))
        {
            return result;
        }
        std::shared_ptr<MeshAsset> meshAsset;
        std::string readError;
        if (!CookedAssetIO::ReadMesh(
                cookedPath,
                meshAsset,
                &readError))
        {
            result.errorCode = "cooked_mesh_read_failed";
            result.errorMessage = readError;
            result.diagnostics.push_back({
                AssetDiagnosticSeverity::Error,
                result.errorCode,
                result.errorMessage,
                cookedPath.generic_string()});
            return result;
        }
        const MeshHandle handle = registry.RegisterMeshAsset(
            record.assetPath,
            meshAsset);
        registry.SetRuntimeMesh(
            handle,
            Mesh::CreateFromAsset(device, *meshAsset));
        ++result.meshCount;
    }
    meshSpan.End();

    Core::CpuTraceSpan materialSpan(
        "CookedMaterialLoad",
        "asset");
    for (const AssetRecord& record : records)
    {
        if (record.builtIn || record.type != AssetType::Material)
        {
            continue;
        }
        std::filesystem::path cookedPath;
        if (!resolvePath(record, "PrismCookedMaterial", cookedPath))
        {
            return result;
        }
        CookedMaterialData materialData;
        std::string readError;
        if (!CookedAssetIO::ReadMaterial(
                cookedPath,
                materialData,
                &readError))
        {
            result.errorCode = "cooked_material_read_failed";
            result.errorMessage = readError;
            result.diagnostics.push_back({
                AssetDiagnosticSeverity::Error,
                result.errorCode,
                result.errorMessage,
                cookedPath.generic_string()});
            return result;
        }

        std::array<TextureHandle, 5> textureHandles{};
        std::array<std::shared_ptr<Texture>, 5> runtimeTextures{};
        for (std::size_t index = 0;
             index < materialData.textureAssetIds.size();
             ++index)
        {
            const AssetRecord* textureRecord =
                database.FindById(materialData.textureAssetIds[index]);
            if (textureRecord == nullptr
                || textureRecord->type != AssetType::Texture)
            {
                result.errorCode =
                    "cooked_material_dependency_missing";
                result.errorMessage =
                    "A Cooked Material references a missing Texture Asset.";
                result.diagnostics.push_back({
                    AssetDiagnosticSeverity::Error,
                    result.errorCode,
                    result.errorMessage,
                    record.assetPath});
                return result;
            }
            textureHandles[index] =
                registry.FindTextureByPath(textureRecord->assetPath);
            runtimeTextures[index] =
                registry.GetRuntimeTexture(textureHandles[index]);
            if (!textureHandles[index].IsValid()
                || runtimeTextures[index] == nullptr)
            {
                result.errorCode =
                    "cooked_material_texture_unavailable";
                result.errorMessage =
                    "A Cooked Material Texture was not loaded into the runtime registry.";
                result.diagnostics.push_back({
                    AssetDiagnosticSeverity::Error,
                    result.errorCode,
                    result.errorMessage,
                    textureRecord->assetPath});
                return result;
            }
        }

        materialData.material->SetBaseColorTexture(
            textureHandles[0]);
        materialData.material->SetMetallicRoughnessTexture(
            textureHandles[1]);
        materialData.material->SetNormalTexture(
            textureHandles[2]);
        materialData.material->SetOcclusionTexture(
            textureHandles[3]);
        materialData.material->SetEmissiveTexture(
            textureHandles[4]);

        const MaterialHandle handle = registry.RegisterMaterialAsset(
            record.assetPath,
            materialData.material);
        registry.SetRuntimeMaterial(
            handle,
            CreateMaterial(
                CreateMaterialParameters(
                    *materialData.material),
                runtimeTextures[0],
                runtimeTextures[1],
                runtimeTextures[2],
                runtimeTextures[3],
                runtimeTextures[4]));
        ++result.materialCount;
    }
    materialSpan.End();

    result.success = true;
    return result;
}
} // namespace

AssetRuntimeLoadResult AssetRuntimeLoader::LoadManifest(
    const std::filesystem::path& projectRoot,
    const std::filesystem::path& manifestPath,
    RHI::IGraphicsDevice& device,
    AssetRegistry& registry)
{
    return LoadManifestInternal(projectRoot, manifestPath, device, registry);
}
} // namespace Prism::Asset
