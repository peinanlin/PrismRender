#include "Asset/AssetDatabase.h"

#include "Asset/AssetCache.h"
#include "Asset/CookedAssetIO.h"
#include "Asset/GltfLoader.h"
#include "Asset/ImageLoader.h"
#include "Asset/MaterialAsset.h"
#include "Asset/MeshAsset.h"
#include "Asset/TextureAsset.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <fstream>
#include <iomanip>
#include <set>
#include <sstream>
#include <stdexcept>
#include <utility>

namespace Prism::Asset
{
namespace
{
using json = nlohmann::json;

std::string Lowercase(std::string value)
{
    std::ranges::transform(value, value.begin(), [](const unsigned char character)
    {
        return static_cast<char>(std::tolower(character));
    });
    return value;
}

std::uint64_t HashBytes(
    std::uint64_t hash,
    const unsigned char* bytes,
    const std::size_t byteCount)
{
    for (std::size_t index = 0; index < byteCount; ++index)
    {
        hash ^= bytes[index];
        hash *= 1099511628211ull;
    }
    return hash;
}

std::uint64_t HashText(std::string_view text, const std::uint64_t seed)
{
    return HashBytes(
        seed,
        reinterpret_cast<const unsigned char*>(text.data()),
        text.size());
}

std::string Hex64(const std::uint64_t value)
{
    std::ostringstream output;
    output << std::hex << std::setfill('0') << std::setw(16) << value;
    return output.str();
}

std::string HashFile(const std::filesystem::path& path)
{
    std::ifstream input(path, std::ios::binary);
    if (!input)
    {
        throw std::runtime_error("Could not open asset dependency for hashing: " + path.string());
    }

    std::uint64_t hash = 14695981039346656037ull;
    std::array<char, 64 * 1024> buffer{};
    while (input)
    {
        input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        const std::streamsize count = input.gcount();
        hash = HashBytes(
            hash,
            reinterpret_cast<const unsigned char*>(buffer.data()),
            static_cast<std::size_t>(count));
    }
    return Hex64(hash);
}

std::string CombineContentHashes(
    const std::vector<std::pair<std::string, std::string>>& hashes)
{
    std::uint64_t hash = 14695981039346656037ull;
    for (const auto& [path, contentHash] : hashes)
    {
        hash = HashText(path, hash);
        hash = HashText("=", hash);
        hash = HashText(contentHash, hash);
        hash = HashText(";", hash);
    }
    return Hex64(hash);
}

json SerializeDiagnostic(const AssetDiagnostic& diagnostic)
{
    return {
        {"severity", AssetDatabase::ToString(diagnostic.severity)},
        {"code", diagnostic.code},
        {"message", diagnostic.message},
        {"path", diagnostic.path}};
}

AssetDiagnostic DeserializeDiagnostic(const json& document)
{
    AssetDiagnostic diagnostic{};
    const std::string severity = document.value("severity", std::string("info"));
    diagnostic.severity = severity == "error"
        ? AssetDiagnosticSeverity::Error
        : severity == "warning"
            ? AssetDiagnosticSeverity::Warning
            : AssetDiagnosticSeverity::Info;
    diagnostic.code = document.value("code", std::string{});
    diagnostic.message = document.value("message", std::string{});
    diagnostic.path = document.value("path", std::string{});
    return diagnostic;
}

json SerializeRecord(const AssetRecord& record)
{
    json diagnostics = json::array();
    for (const AssetDiagnostic& diagnostic : record.diagnostics)
    {
        diagnostics.push_back(SerializeDiagnostic(diagnostic));
    }
    return {
        {"assetId", record.assetId},
        {"assetPath", record.assetPath},
        {"type", AssetDatabase::ToString(record.type)},
        {"name", record.name},
        {"sourcePath", record.sourcePath},
        {"subresource", record.subresource},
        {"contentHash", record.contentHash},
        {"importRevision", record.importRevision},
        {"dependencies", record.dependencies},
        {"diagnostics", std::move(diagnostics)},
        {"metadata", record.metadata}};
}

AssetRecord DeserializeRecord(const json& document)
{
    AssetRecord record{};
    record.assetId = document.at("assetId").get<std::string>();
    record.assetPath = document.at("assetPath").get<std::string>();
    record.type = AssetDatabase::ParseAssetType(
        document.at("type").get<std::string>()).value_or(AssetType::Unknown);
    record.name = document.value("name", std::string{});
    record.sourcePath = document.value("sourcePath", std::string{});
    record.subresource = document.value("subresource", std::string{});
    record.contentHash = document.value("contentHash", std::string{});
    record.importRevision = document.value("importRevision", 0ull);
    record.dependencies = document.value("dependencies", std::vector<std::string>{});
    record.metadata = document.value("metadata", json::object());
    for (const json& diagnostic : document.value("diagnostics", json::array()))
    {
        record.diagnostics.push_back(DeserializeDiagnostic(diagnostic));
    }
    return record;
}

std::vector<std::filesystem::path> ReadGltfDependencies(
    const std::filesystem::path& sourcePath,
    std::vector<AssetDiagnostic>& diagnostics)
{
    std::vector<std::filesystem::path> dependencies;
    if (Lowercase(sourcePath.extension().string()) != ".gltf")
    {
        return dependencies;
    }

    try
    {
        std::ifstream input(sourcePath, std::ios::binary);
        if (!input)
        {
            throw std::runtime_error("Could not open the glTF document.");
        }
        json document;
        input >> document;
        std::set<std::string> uniqueUris;
        const auto collect = [&](const char* field)
        {
            for (const json& entry : document.value(field, json::array()))
            {
                const std::string uri = entry.value("uri", std::string{});
                if (uri.empty() || uri.starts_with("data:") || uri.find("://") != std::string::npos)
                {
                    continue;
                }
                uniqueUris.insert(uri);
            }
        };
        collect("buffers");
        collect("images");
        for (const std::string& uri : uniqueUris)
        {
            dependencies.push_back((sourcePath.parent_path() / uri).lexically_normal());
        }
    }
    catch (const std::exception& exception)
    {
        diagnostics.push_back({
            AssetDiagnosticSeverity::Error,
            "gltf_dependency_parse_failed",
            exception.what(),
            sourcePath.generic_string()});
    }
    return dependencies;
}

AssetRecord MakeRecord(
    const std::string& sourceIdentity,
    const std::string& sourcePath,
    const std::string& subresource,
    const AssetType type,
    std::string name,
    const std::string& contentHash,
    const std::uint64_t revision)
{
    AssetRecord record{};
    record.assetId = AssetDatabase::MakeStableAssetId(sourceIdentity, subresource);
    record.assetPath = AssetDatabase::MakeAssetPath(record.assetId);
    record.type = type;
    record.name = std::move(name);
    record.sourcePath = sourcePath;
    record.subresource = subresource;
    record.contentHash = contentHash;
    record.importRevision = revision;
    return record;
}

std::vector<std::string> UniqueSorted(std::vector<std::string> values)
{
    std::ranges::sort(values);
    values.erase(std::unique(values.begin(), values.end()), values.end());
    return values;
}

std::shared_ptr<TextureAsset> ResolveTextureAsset(
    std::shared_ptr<TextureAsset> texture,
    std::string name,
    const DirectX::XMFLOAT4& fallbackColor)
{
    if (texture != nullptr)
    {
        return texture;
    }
    texture = std::make_shared<TextureAsset>();
    texture->SetName(std::move(name));
    texture->SetSolidColor(fallbackColor);
    return texture;
}

void SetCookedMetadata(
    AssetRecord& record,
    const std::filesystem::path& projectRoot,
    const std::filesystem::path& path,
    const std::string_view format)
{
    const CookedAssetInfo info =
        CookedAssetIO::Inspect(path);
    if (!info.valid
        || info.containerVersion
            != CookedAssetIO::CurrentVersion
        || !info.checksumVerified)
    {
        throw std::runtime_error(
            "A newly written Cooked Asset failed immediate validation: "
            + info.errorMessage);
    }
    record.metadata["cooked"] = {
        {"format", format},
        {"version", CookedAssetIO::CurrentVersion},
        {"payloadVersion", info.payloadVersion},
        {"compression", CookedAssetIO::ToString(
            info.compression)},
        {"uncompressedBytes", info.uncompressedBytes},
        {"storedBytes", info.storedBytes},
        {"payloadChecksum", info.payloadChecksum},
        {"checksumVerified", info.checksumVerified},
        {"path", path.lexically_relative(projectRoot).generic_string()},
        {"bytes", std::filesystem::file_size(path)}};
}
} // namespace

AssetDatabase::AssetDatabase(
    std::filesystem::path projectRoot,
    std::filesystem::path manifestPath)
    : m_projectRoot(std::filesystem::absolute(std::move(projectRoot)).lexically_normal())
{
    if (manifestPath.empty())
    {
        manifestPath = m_projectRoot / "automation/assets/AssetManifest.json";
    }
    m_manifestPath = std::filesystem::absolute(
        manifestPath.is_absolute() ? manifestPath : m_projectRoot / manifestPath).lexically_normal();
}

bool AssetDatabase::Load(std::string* outError)
{
    try
    {
        m_records.clear();
        m_manifestRevision = 0;
        AddBuiltInRecords();
        if (!std::filesystem::exists(m_manifestPath))
        {
            return true;
        }

        const std::filesystem::path resolvedManifest = ResolveProjectPath(m_manifestPath);
        std::ifstream input(resolvedManifest, std::ios::binary);
        if (!input)
        {
            throw std::runtime_error("Could not open the Asset Manifest.");
        }
        json manifest;
        input >> manifest;
        if (manifest.value("format", std::string{}) != "PrismAssetManifest"
            || manifest.value("version", 0u) != CurrentVersion)
        {
            throw std::runtime_error("The Asset Manifest format or version is not supported.");
        }

        m_manifestRevision = manifest.value("manifestRevision", 0ull);
        for (const json& serialized : manifest.value("assets", json::array()))
        {
            AssetRecord record = DeserializeRecord(serialized);
            if (record.assetId.empty() || record.assetPath.empty()
                || record.type == AssetType::Unknown)
            {
                throw std::runtime_error("The Asset Manifest contains an invalid asset record.");
            }
            m_records[record.assetId] = std::move(record);
        }
        return true;
    }
    catch (const std::exception& exception)
    {
        if (outError != nullptr) *outError = exception.what();
        return false;
    }
}

bool AssetDatabase::Save(std::string* outError) const
{
    try
    {
        const std::filesystem::path resolvedManifest = ResolveProjectPath(m_manifestPath);
        if (!resolvedManifest.parent_path().empty())
        {
            std::filesystem::create_directories(resolvedManifest.parent_path());
        }
        const std::filesystem::path temporary = resolvedManifest.string() + ".tmp";
        {
            std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
            if (!output)
            {
                throw std::runtime_error("Could not open the temporary Asset Manifest.");
            }
            output << SerializeImportedManifest().dump(2) << '\n';
            if (!output)
            {
                throw std::runtime_error("Failed while writing the Asset Manifest.");
            }
        }
        if (std::filesystem::exists(resolvedManifest))
        {
            std::filesystem::remove(resolvedManifest);
        }
        std::filesystem::rename(temporary, resolvedManifest);
        return true;
    }
    catch (const std::exception& exception)
    {
        if (outError != nullptr) *outError = exception.what();
        return false;
    }
}

AssetImportResult AssetDatabase::ImportGltf(
    const std::filesystem::path& sourcePath,
    const bool reimport)
{
    AssetImportResult result{};
    result.reimported = reimport;
    try
    {
        const std::filesystem::path resolvedSource = ResolveProjectPath(sourcePath);
        if (!std::filesystem::is_regular_file(resolvedSource))
        {
            throw std::runtime_error("The asset source file does not exist.");
        }
        const std::string extension = Lowercase(resolvedSource.extension().string());
        if (extension != ".gltf" && extension != ".glb")
        {
            result.errorCode = "unsupported_asset_format";
            result.errorMessage = "Only .gltf and .glb imports are supported in this stage.";
            return result;
        }

        const std::string relativeSource =
            resolvedSource.lexically_relative(m_projectRoot).generic_string();
        const std::string sourceIdentity = Lowercase(relativeSource);
        bool alreadyImported = false;
        std::uint64_t previousRevision = 0;
        std::set<std::string> oldIds;
        for (const auto& [id, record] : m_records)
        {
            if (!record.builtIn && Lowercase(record.sourcePath) == sourceIdentity)
            {
                alreadyImported = true;
                previousRevision = std::max(previousRevision, record.importRevision);
                oldIds.insert(id);
            }
        }
        if (alreadyImported && !reimport)
        {
            result.errorCode = "asset_already_imported";
            result.errorMessage = "The source is already present in the Asset Manifest. Use asset.reimport.";
            return result;
        }
        if (!alreadyImported && reimport)
        {
            result.errorCode = "asset_not_imported";
            result.errorMessage = "The source is not present in the Asset Manifest.";
            return result;
        }

        std::vector<std::filesystem::path> dependencyPaths =
            ReadGltfDependencies(resolvedSource, result.diagnostics);
        std::vector<std::pair<std::string, std::string>> dependencyHashes;
        dependencyHashes.emplace_back(relativeSource, HashFile(resolvedSource));
        for (const std::filesystem::path& dependency : dependencyPaths)
        {
            const std::filesystem::path resolvedDependency = ResolveProjectPath(dependency);
            if (!std::filesystem::is_regular_file(resolvedDependency))
            {
                result.diagnostics.push_back({
                    AssetDiagnosticSeverity::Error,
                    "asset_dependency_missing",
                    "A glTF external dependency does not exist.",
                    resolvedDependency.generic_string()});
                continue;
            }
            dependencyHashes.emplace_back(
                resolvedDependency.lexically_relative(m_projectRoot).generic_string(),
                HashFile(resolvedDependency));
        }
        if (std::ranges::any_of(result.diagnostics, [](const AssetDiagnostic& diagnostic)
            {
                return diagnostic.severity == AssetDiagnosticSeverity::Error;
            }))
        {
            result.errorCode = "asset_dependency_failed";
            result.errorMessage = "The glTF source contains invalid or missing dependencies.";
            return result;
        }
        std::ranges::sort(dependencyHashes, {}, &std::pair<std::string, std::string>::first);
        const std::string contentHash = CombineContentHashes(dependencyHashes);

        GltfLoader loader;
        GltfLoader::SceneData sceneData;
        std::string loadError;
        if (!loader.LoadScene(resolvedSource.string(), sceneData, &loadError))
        {
            result.diagnostics.push_back({
                AssetDiagnosticSeverity::Error,
                "gltf_import_failed",
                loadError.empty() ? "The glTF loader returned no renderable scene." : loadError,
                relativeSource});
            result.errorCode = "asset_import_failed";
            result.errorMessage = "The glTF source could not be imported.";
            return result;
        }

        const std::uint64_t revision = previousRevision + 1;
        std::map<std::string, AssetRecord> imported;
        std::vector<std::string> sceneDependencies;
        json sceneInstances = json::array();
        for (const GltfLoader::MeshInstanceRecord& instance : sceneData.instances)
        {
            if (instance.mesh == nullptr)
            {
                result.errorCode = "cooked_mesh_missing";
                result.errorMessage =
                    "The glTF importer produced a Mesh instance without geometry.";
                return result;
            }
            std::shared_ptr<MaterialAsset> materialAsset = instance.material;
            if (materialAsset == nullptr)
            {
                materialAsset = std::make_shared<MaterialAsset>();
                materialAsset->SetName("DefaultMaterial");
            }

            const std::string meshSubresource =
                MakeMeshSubresource(instance.meshIndex, instance.primitiveIndex);
            AssetRecord meshRecord = MakeRecord(
                sourceIdentity,
                relativeSource,
                meshSubresource,
                AssetType::Mesh,
                instance.mesh->GetName(),
                contentHash,
                revision);
            const MeshBounds& bounds = instance.mesh->GetBounds();
            meshRecord.metadata = {
                {"meshIndex", instance.meshIndex},
                {"primitiveIndex", instance.primitiveIndex},
                {"vertexCount", instance.mesh->GetVertices().size()},
                {"indexCount", instance.mesh->GetIndices().size()},
                {"boundsRadius", bounds.radius}};
            const std::filesystem::path cookedMeshPath =
                CookedAssetIO::MakePath(
                    m_projectRoot,
                    contentHash,
                    meshRecord.assetId,
                    ".prismmesh");
            std::string cookedError;
            if (!CookedAssetIO::WriteMesh(
                    cookedMeshPath,
                    *instance.mesh,
                    &cookedError))
            {
                result.diagnostics.push_back({
                    AssetDiagnosticSeverity::Error,
                    "cooked_mesh_write_failed",
                    cookedError,
                    cookedMeshPath.generic_string()});
                result.errorCode = "cooked_mesh_write_failed";
                result.errorMessage =
                    "The imported Mesh could not be written as a Cooked Asset.";
                return result;
            }
            SetCookedMetadata(
                meshRecord,
                m_projectRoot,
                cookedMeshPath,
                "PrismCookedMesh");
            imported[meshRecord.assetId] = meshRecord;
            sceneDependencies.push_back(meshRecord.assetId);

            const std::string materialSubresource =
                MakeMaterialSubresource(instance.materialIndex);
            AssetRecord materialRecord = MakeRecord(
                sourceIdentity,
                relativeSource,
                materialSubresource,
                AssetType::Material,
                materialAsset->GetName(),
                contentHash,
                revision);
            materialRecord.metadata = {
                {"materialIndex", instance.materialIndex},
                {"metallic", materialAsset->GetMetallic()},
                {"roughness", materialAsset->GetRoughness()},
                {"alphaMode", materialAsset->GetAlphaMode()},
                {"doubleSided", materialAsset->GetDoubleSided()}};

            const std::array<
                std::pair<std::string_view, std::shared_ptr<TextureAsset>>,
                5> textures = {{
                    {"baseColor", ResolveTextureAsset(
                        instance.baseColorTexture,
                        materialRecord.name + "_BaseColor",
                        materialAsset->GetAlbedoColor())},
                    {"metallicRoughness", ResolveTextureAsset(
                        instance.metallicRoughnessTexture,
                        materialRecord.name + "_MetallicRoughness",
                        {0.0f, materialAsset->GetRoughness(),
                         materialAsset->GetMetallic(), 1.0f})},
                    {"normal", ResolveTextureAsset(
                        instance.normalTexture,
                        materialRecord.name + "_Normal",
                        {0.5f, 0.5f, 1.0f, 1.0f})},
                    {"occlusion", ResolveTextureAsset(
                        instance.occlusionTexture,
                        materialRecord.name + "_Occlusion",
                        {1.0f, 1.0f, 1.0f, 1.0f})},
                    {"emissive", ResolveTextureAsset(
                        instance.emissiveTexture,
                        materialRecord.name + "_Emissive",
                        {0.0f, 0.0f, 0.0f, 1.0f})}}};
            std::array<std::string, 5> textureAssetIds;
            for (std::size_t textureIndex = 0;
                 textureIndex < textures.size();
                 ++textureIndex)
            {
                const auto& [role, texture] = textures[textureIndex];
                const std::string textureSubresource =
                    MakeTextureSubresource(instance.materialIndex, role);
                AssetRecord textureRecord = MakeRecord(
                    sourceIdentity,
                    relativeSource,
                    textureSubresource,
                    AssetType::Texture,
                    texture->GetName(),
                    contentHash,
                    revision);
                textureRecord.metadata = {
                    {"materialIndex", instance.materialIndex},
                    {"role", std::string(role)},
                    {"generatedFallback", !texture->HasImageData()},
                    {"width", texture->HasImageData()
                         ? texture->GetWidth()
                         : 1u},
                    {"height", texture->HasImageData()
                         ? texture->GetHeight()
                         : 1u},
                    {"sourceTexturePath", texture->GetSourcePath()}};
                const std::filesystem::path cookedTexturePath =
                    CookedAssetIO::MakePath(
                        m_projectRoot,
                        contentHash,
                        textureRecord.assetId,
                        ".prismtex");
                cookedError.clear();
                if (!CookedAssetIO::WriteTexture(
                        cookedTexturePath,
                        *texture,
                        &cookedError))
                {
                    result.diagnostics.push_back({
                        AssetDiagnosticSeverity::Error,
                        "cooked_texture_write_failed",
                        cookedError,
                        cookedTexturePath.generic_string()});
                    result.errorCode =
                        "cooked_texture_write_failed";
                    result.errorMessage =
                        "An imported Texture could not be written as a Cooked Asset.";
                    return result;
                }
                SetCookedMetadata(
                    textureRecord,
                    m_projectRoot,
                    cookedTexturePath,
                    "PrismCookedTexture");
                imported[textureRecord.assetId] = textureRecord;
                materialRecord.dependencies.push_back(textureRecord.assetId);
                textureAssetIds[textureIndex] =
                    textureRecord.assetId;
            }
            materialRecord.dependencies = UniqueSorted(std::move(materialRecord.dependencies));
            const std::filesystem::path cookedMaterialPath =
                CookedAssetIO::MakePath(
                    m_projectRoot,
                    contentHash,
                    materialRecord.assetId,
                    ".prismmat");
            cookedError.clear();
            if (!CookedAssetIO::WriteMaterial(
                    cookedMaterialPath,
                    *materialAsset,
                    textureAssetIds,
                    &cookedError))
            {
                result.diagnostics.push_back({
                    AssetDiagnosticSeverity::Error,
                    "cooked_material_write_failed",
                    cookedError,
                    cookedMaterialPath.generic_string()});
                result.errorCode = "cooked_material_write_failed";
                result.errorMessage =
                    "The imported Material could not be written as a Cooked Asset.";
                return result;
            }
            SetCookedMetadata(
                materialRecord,
                m_projectRoot,
                cookedMaterialPath,
                "PrismCookedMaterial");
            imported[materialRecord.assetId] = materialRecord;
            sceneDependencies.push_back(materialRecord.assetId);

            const DirectX::XMFLOAT4X4& matrix =
                instance.worldMatrix;
            sceneInstances.push_back({
                {"name", instance.name},
                {"meshAssetId", meshRecord.assetId},
                {"materialAssetId",
                 materialRecord.assetId},
                {"worldMatrix", {
                    matrix._11, matrix._12,
                    matrix._13, matrix._14,
                    matrix._21, matrix._22,
                    matrix._23, matrix._24,
                    matrix._31, matrix._32,
                    matrix._33, matrix._34,
                    matrix._41, matrix._42,
                    matrix._43, matrix._44}}});
        }

        AssetRecord sourceRecord = MakeRecord(
            sourceIdentity,
            relativeSource,
            "scene",
            AssetType::Scene,
            resolvedSource.stem().string(),
            contentHash,
            revision);
        sourceRecord.dependencies = UniqueSorted(std::move(sceneDependencies));
        json dependencyFiles = json::array();
        for (const auto& [path, hash] : dependencyHashes)
        {
            dependencyFiles.push_back({{"path", path}, {"contentHash", hash}});
        }
        std::size_t cookedAssetCount = 0;
        std::uintmax_t cookedBytes = 0;
        for (const auto& [assetId, record] : imported)
        {
            (void)assetId;
            const json cooked = record.metadata.value(
                "cooked",
                json::object());
            if (!cooked.empty())
            {
                ++cookedAssetCount;
                cookedBytes += cooked.value(
                    "bytes",
                    static_cast<std::uintmax_t>(0));
            }
        }
        sourceRecord.metadata = {
            {"instanceCount", sceneData.instances.size()},
            {"instances", std::move(sceneInstances)},
            {"dependencyFiles", std::move(dependencyFiles)},
            {"cookedAssetCount", cookedAssetCount},
            {"cookedBytes", cookedBytes}};
        const AssetCacheBundle cacheBundle = AssetCache(m_projectRoot).Build(
            contentHash,
            relativeSource,
            sourceRecord.metadata.at("dependencyFiles"));
        if (!cacheBundle.valid)
        {
            result.diagnostics.push_back({
                AssetDiagnosticSeverity::Error,
                cacheBundle.errorCode,
                cacheBundle.errorMessage,
                cacheBundle.bundlePath.generic_string()});
            result.errorCode = cacheBundle.errorCode;
            result.errorMessage = cacheBundle.errorMessage;
            return result;
        }
        sourceRecord.metadata["cache"] = {
            {"key", cacheBundle.contentHash},
            {"bundlePath", cacheBundle.bundlePath.lexically_relative(m_projectRoot).generic_string()},
            {"cachedSourcePath", cacheBundle.cachedSourcePath.lexically_relative(m_projectRoot).generic_string()},
            {"fileCount", cacheBundle.files.size()},
            {"totalBytes", cacheBundle.totalBytes}};
        sourceRecord.diagnostics = result.diagnostics;
        imported[sourceRecord.assetId] = sourceRecord;

        std::set<std::string> newIds;
        for (const auto& [id, record] : imported)
        {
            newIds.insert(id);
            if (oldIds.contains(id)) ++result.updatedCount;
            else ++result.addedCount;
            m_records[id] = record;
            result.assets.push_back(record);
        }
        for (const std::string& oldId : oldIds)
        {
            if (!newIds.contains(oldId))
            {
                m_records.erase(oldId);
                ++result.removedCount;
            }
        }
        ++m_manifestRevision;
        std::string saveError;
        if (!Save(&saveError))
        {
            result.errorCode = "asset_manifest_save_failed";
            result.errorMessage = saveError;
            return result;
        }

        result.success = true;
        result.sourceAssetId = sourceRecord.assetId;
        result.sourceAssetPath = sourceRecord.assetPath;
        result.sourcePath = relativeSource;
        result.contentHash = contentHash;
        result.importRevision = revision;
        result.manifestHash = ComputeManifestHash();
        std::ranges::sort(result.assets, {}, &AssetRecord::assetId);
        return result;
    }
    catch (const std::exception& exception)
    {
        result.errorCode = "asset_import_failed";
        result.errorMessage = exception.what();
        return result;
    }
}

AssetImportResult AssetDatabase::ImportTexture(
    const std::filesystem::path& sourcePath,
    const bool reimport)
{
    AssetImportResult result{};
    result.reimported = reimport;
    try
    {
        const std::filesystem::path resolvedSource =
            ResolveProjectPath(sourcePath);
        if (!std::filesystem::is_regular_file(resolvedSource))
        {
            throw std::runtime_error(
                "The texture source file does not exist.");
        }
        const std::string extension =
            Lowercase(resolvedSource.extension().string());
        if (extension != ".png" && extension != ".jpg"
            && extension != ".jpeg" && extension != ".tga"
            && extension != ".bmp")
        {
            result.errorCode = "unsupported_asset_format";
            result.errorMessage =
                "Texture import supports PNG, JPEG, TGA, and BMP files.";
            return result;
        }

        const std::string relativeSource = resolvedSource
            .lexically_relative(m_projectRoot).generic_string();
        const std::string sourceIdentity = Lowercase(relativeSource);
        bool alreadyImported = false;
        std::uint64_t previousRevision = 0;
        std::set<std::string> oldIds;
        for (const auto& [id, record] : m_records)
        {
            if (!record.builtIn
                && Lowercase(record.sourcePath) == sourceIdentity)
            {
                alreadyImported = true;
                previousRevision = std::max(
                    previousRevision, record.importRevision);
                oldIds.insert(id);
            }
        }
        if (alreadyImported && !reimport)
        {
            result.errorCode = "asset_already_imported";
            result.errorMessage =
                "The texture is already present in the Asset Manifest. Use reimport.";
            return result;
        }
        if (!alreadyImported && reimport)
        {
            result.errorCode = "asset_not_imported";
            result.errorMessage =
                "The texture is not present in the Asset Manifest.";
            return result;
        }

        const std::string contentHash = HashFile(resolvedSource);
        std::shared_ptr<TextureAsset> texture;
        std::string loadError;
        if (!ImageLoader::LoadRgba8(
                resolvedSource, texture, &loadError))
        {
            result.errorCode = "texture_import_failed";
            result.errorMessage = loadError;
            result.diagnostics.push_back({
                AssetDiagnosticSeverity::Error,
                result.errorCode,
                loadError,
                relativeSource});
            return result;
        }

        const std::uint64_t revision = previousRevision + 1;
        AssetRecord record = MakeRecord(
            sourceIdentity,
            relativeSource,
            "texture",
            AssetType::Texture,
            texture->GetName(),
            contentHash,
            revision);
        record.metadata = {
            {"standalone", true},
            {"width", texture->GetWidth()},
            {"height", texture->GetHeight()},
            {"colorSpace", "sRGB"},
            {"dependencyFiles", json::array({
                {{"path", relativeSource},
                 {"contentHash", contentHash}}})}};

        const std::filesystem::path cookedPath =
            CookedAssetIO::MakePath(
                m_projectRoot,
                contentHash,
                record.assetId,
                ".prismtex");
        std::string cookedError;
        if (!CookedAssetIO::WriteTexture(
                cookedPath, *texture, &cookedError))
        {
            result.errorCode = "cooked_texture_write_failed";
            result.errorMessage = cookedError;
            return result;
        }
        SetCookedMetadata(
            record,
            m_projectRoot,
            cookedPath,
            "PrismCookedTexture");

        const AssetCacheBundle cacheBundle =
            AssetCache(m_projectRoot).Build(
                contentHash,
                relativeSource,
                record.metadata.at("dependencyFiles"));
        if (!cacheBundle.valid)
        {
            result.errorCode = cacheBundle.errorCode;
            result.errorMessage = cacheBundle.errorMessage;
            return result;
        }
        record.metadata["cache"] = {
            {"key", cacheBundle.contentHash},
            {"bundlePath", cacheBundle.bundlePath
                 .lexically_relative(m_projectRoot)
                 .generic_string()},
            {"cachedSourcePath", cacheBundle.cachedSourcePath
                 .lexically_relative(m_projectRoot)
                 .generic_string()},
            {"fileCount", cacheBundle.files.size()},
            {"totalBytes", cacheBundle.totalBytes}};

        const bool updated = m_records.contains(record.assetId);
        m_records[record.assetId] = record;
        if (updated)
        {
            ++result.updatedCount;
        }
        else
        {
            ++result.addedCount;
        }
        for (const std::string& oldId : oldIds)
        {
            if (oldId != record.assetId)
            {
                m_records.erase(oldId);
                ++result.removedCount;
            }
        }

        ++m_manifestRevision;
        std::string saveError;
        if (!Save(&saveError))
        {
            result.errorCode = "asset_manifest_save_failed";
            result.errorMessage = saveError;
            return result;
        }

        result.success = true;
        result.sourceAssetId = record.assetId;
        result.sourceAssetPath = record.assetPath;
        result.sourcePath = relativeSource;
        result.contentHash = contentHash;
        result.importRevision = revision;
        result.manifestHash = ComputeManifestHash();
        result.assets.push_back(std::move(record));
        return result;
    }
    catch (const std::exception& exception)
    {
        result.errorCode = "asset_import_failed";
        result.errorMessage = exception.what();
        return result;
    }
}

std::vector<AssetRecord> AssetDatabase::List(
    const std::optional<AssetType> type,
    const std::string_view query) const
{
    std::vector<AssetRecord> result;
    const std::string normalizedQuery = Lowercase(std::string(query));
    for (const auto& [id, record] : m_records)
    {
        (void)id;
        if (type.has_value() && record.type != *type)
        {
            continue;
        }
        if (!normalizedQuery.empty())
        {
            const std::string searchable = Lowercase(
                record.name + '\n' + record.assetId + '\n'
                + record.assetPath + '\n' + record.sourcePath);
            if (searchable.find(normalizedQuery) == std::string::npos)
            {
                continue;
            }
        }
        result.push_back(record);
    }
    return result;
}

const AssetRecord* AssetDatabase::FindById(const std::string_view assetId) const
{
    const auto iterator = m_records.find(std::string(assetId));
    return iterator != m_records.end() ? &iterator->second : nullptr;
}

const AssetRecord* AssetDatabase::FindByPath(const std::string_view assetPath) const
{
    const auto iterator = std::ranges::find_if(m_records, [&](const auto& entry)
    {
        return entry.second.assetPath == assetPath;
    });
    return iterator != m_records.end() ? &iterator->second : nullptr;
}

std::vector<AssetRecord> AssetDatabase::GetImportedScenes() const
{
    std::vector<AssetRecord> scenes;
    for (const auto& [id, record] : m_records)
    {
        (void)id;
        if (!record.builtIn && record.type == AssetType::Scene)
        {
            scenes.push_back(record);
        }
    }
    return scenes;
}

bool AssetDatabase::ValidateImportedContent(
    std::vector<AssetDiagnostic>& diagnostics) const
{
    bool valid = true;
    for (const AssetRecord& source : GetImportedScenes())
    {
        const AssetCacheBundle cache = AssetCache(m_projectRoot).Resolve(source);
        if (!cache.valid)
        {
            diagnostics.push_back({
                AssetDiagnosticSeverity::Error,
                cache.errorCode,
                cache.errorMessage,
                cache.bundlePath.generic_string()});
            valid = false;
        }
        for (const json& dependency : source.metadata.value(
                 "dependencyFiles",
                 json::array()))
        {
            const std::string path = dependency.value("path", std::string{});
            const std::string expectedHash =
                dependency.value("contentHash", std::string{});
            try
            {
                const std::filesystem::path resolved = ResolveProjectPath(path);
                if (!std::filesystem::is_regular_file(resolved))
                {
                    diagnostics.push_back(cache.valid
                        ? AssetDiagnostic{
                            AssetDiagnosticSeverity::Warning,
                            "asset_source_using_cache",
                            "An imported source is missing; the validated offline cache will be used.",
                            path}
                        : AssetDiagnostic{
                            AssetDiagnosticSeverity::Error,
                            "asset_dependency_missing",
                            "An imported asset dependency no longer exists and no valid cache is available.",
                            path});
                    valid = valid && cache.valid;
                    continue;
                }
                const std::string actualHash = HashFile(resolved);
                if (actualHash != expectedHash)
                {
                    diagnostics.push_back({
                        AssetDiagnosticSeverity::Error,
                        "asset_source_stale",
                        "An imported source or dependency changed after import. Run asset.reimport.",
                        path});
                    valid = false;
                }
            }
            catch (const std::exception& exception)
            {
                diagnostics.push_back({
                    AssetDiagnosticSeverity::Error,
                    "asset_validation_failed",
                    exception.what(),
                    path});
                valid = false;
            }
        }
    }
    return valid;
}

std::string AssetDatabase::ComputeManifestHash() const
{
    json assets = json::array();
    for (const auto& [id, record] : m_records)
    {
        (void)id;
        json serialized = SerializeRecord(record);
        serialized.erase("diagnostics");
        serialized.erase("importRevision");
        assets.push_back(std::move(serialized));
    }
    const std::string canonical = assets.dump();
    return Hex64(HashText(canonical, 14695981039346656037ull));
}

const std::filesystem::path& AssetDatabase::GetManifestPath() const
{
    return m_manifestPath;
}

const std::filesystem::path& AssetDatabase::GetProjectRoot() const
{
    return m_projectRoot;
}

std::uint64_t AssetDatabase::GetManifestRevision() const
{
    return m_manifestRevision;
}

std::string AssetDatabase::MakeStableAssetId(
    const std::string_view sourceIdentity,
    const std::string_view subresource)
{
    const std::string key = Lowercase(
        std::string(sourceIdentity) + "|" + std::string(subresource));
    std::uint64_t high = HashText(key, 14695981039346656037ull);
    std::uint64_t low = HashText(key, 1099511628211ull);
    high = (high & 0xffffffffffff0fffull) | 0x0000000000005000ull;
    low = (low & 0x3fffffffffffffffull) | 0x8000000000000000ull;

    std::ostringstream output;
    output << std::hex << std::setfill('0')
           << std::setw(8) << static_cast<std::uint32_t>(high >> 32u) << '-'
           << std::setw(4) << static_cast<std::uint16_t>(high >> 16u) << '-'
           << std::setw(4) << static_cast<std::uint16_t>(high) << '-'
           << std::setw(4) << static_cast<std::uint16_t>(low >> 48u) << '-'
           << std::setw(12) << (low & 0x0000ffffffffffffull);
    return output.str();
}

std::string AssetDatabase::MakeAssetPath(const std::string_view assetId)
{
    return "prism-asset://" + std::string(assetId);
}

std::string AssetDatabase::MakeMeshSubresource(
    const int meshIndex,
    const std::uint32_t primitiveIndex)
{
    return "mesh/" + std::to_string(meshIndex)
        + "/primitive/" + std::to_string(primitiveIndex);
}

std::string AssetDatabase::MakeMaterialSubresource(const int materialIndex)
{
    return materialIndex >= 0
        ? "material/" + std::to_string(materialIndex)
        : "material/default";
}

std::string AssetDatabase::MakeTextureSubresource(
    const int materialIndex,
    const std::string_view role)
{
    return MakeMaterialSubresource(materialIndex)
        + "/texture/" + std::string(role);
}

std::string AssetDatabase::ToString(const AssetType type)
{
    switch (type)
    {
    case AssetType::Scene: return "scene";
    case AssetType::Mesh: return "mesh";
    case AssetType::Material: return "material";
    case AssetType::Texture: return "texture";
    case AssetType::Unknown: return "unknown";
    }
    return "unknown";
}

std::optional<AssetType> AssetDatabase::ParseAssetType(const std::string_view type)
{
    const std::string normalized = Lowercase(std::string(type));
    if (normalized == "scene") return AssetType::Scene;
    if (normalized == "mesh") return AssetType::Mesh;
    if (normalized == "material") return AssetType::Material;
    if (normalized == "texture") return AssetType::Texture;
    return std::nullopt;
}

std::string AssetDatabase::ToString(const AssetDiagnosticSeverity severity)
{
    switch (severity)
    {
    case AssetDiagnosticSeverity::Info: return "info";
    case AssetDiagnosticSeverity::Warning: return "warning";
    case AssetDiagnosticSeverity::Error: return "error";
    }
    return "info";
}

std::filesystem::path AssetDatabase::ResolveProjectPath(
    const std::filesystem::path& path) const
{
    const std::filesystem::path candidate = std::filesystem::absolute(
        path.is_absolute() ? path : m_projectRoot / path).lexically_normal();
    auto rootIterator = m_projectRoot.begin();
    auto candidateIterator = candidate.begin();
    for (; rootIterator != m_projectRoot.end(); ++rootIterator, ++candidateIterator)
    {
        if (candidateIterator == candidate.end()
            || Lowercase(rootIterator->string()) != Lowercase(candidateIterator->string()))
        {
            throw std::runtime_error("Asset file access is restricted to the project root.");
        }
    }
    return candidate;
}

void AssetDatabase::AddBuiltInRecords()
{
    const std::string source = "builtin://editor-preview";
    const auto add = [&](const AssetType type,
                         const std::string& subresource,
                         const std::string& path,
                         const std::string& name,
                         std::vector<std::string> dependencies = {})
    {
        AssetRecord record{};
        record.assetId = MakeStableAssetId(source, subresource);
        record.assetPath = path;
        record.type = type;
        record.name = name;
        record.sourcePath = source;
        record.subresource = subresource;
        record.contentHash = "builtin-preview-v1";
        record.builtIn = true;
        record.dependencies = std::move(dependencies);
        m_records[record.assetId] = std::move(record);
    };

    add(
        AssetType::Mesh,
        "mesh/cube",
        "builtin://editor-preview/meshes/cube",
        "Cube");
    add(
        AssetType::Mesh,
        "mesh/uv-sphere",
        "builtin://editor-preview/meshes/uv-sphere",
        "UV Sphere");

    for (const std::string name : {"Preview_Neutral", "Preview_Warm", "Preview_Cool"})
    {
        const std::string textureId = MakeStableAssetId(source, "texture/" + name);
        add(
            AssetType::Texture,
            "texture/" + name,
            "builtin://editor-preview/textures/" + name,
            name + " Base Color");
        add(
            AssetType::Material,
            "material/" + name,
            "builtin://editor-preview/materials/" + name,
            name,
            {textureId});
    }
}

nlohmann::json AssetDatabase::SerializeImportedManifest() const
{
    json assets = json::array();
    for (const auto& [id, record] : m_records)
    {
        (void)id;
        if (!record.builtIn)
        {
            assets.push_back(SerializeRecord(record));
        }
    }
    return {
        {"format", "PrismAssetManifest"},
        {"version", CurrentVersion},
        {"manifestRevision", m_manifestRevision},
        {"manifestHash", ComputeManifestHash()},
        {"assets", std::move(assets)}};
}
} // namespace Prism::Asset
