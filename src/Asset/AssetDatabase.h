#pragma once

#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <json.hpp>

namespace Prism::Asset
{
enum class AssetType
{
    Scene,
    Mesh,
    Material,
    Texture,
    Unknown
};

enum class AssetDiagnosticSeverity
{
    Info,
    Warning,
    Error
};

struct AssetDiagnostic
{
    AssetDiagnosticSeverity severity = AssetDiagnosticSeverity::Info;
    std::string code;
    std::string message;
    std::string path;
};

struct AssetRecord
{
    std::string assetId;
    std::string assetPath;
    AssetType type = AssetType::Unknown;
    std::string name;
    std::string sourcePath;
    std::string subresource;
    std::string contentHash;
    std::uint64_t importRevision = 0;
    bool builtIn = false;
    std::vector<std::string> dependencies;
    std::vector<AssetDiagnostic> diagnostics;
    nlohmann::json metadata = nlohmann::json::object();
};

struct AssetImportResult
{
    bool success = false;
    bool reimported = false;
    std::string errorCode;
    std::string errorMessage;
    std::string sourceAssetId;
    std::string sourceAssetPath;
    std::string sourcePath;
    std::string contentHash;
    std::string manifestHash;
    std::uint64_t importRevision = 0;
    std::size_t addedCount = 0;
    std::size_t updatedCount = 0;
    std::size_t removedCount = 0;
    std::vector<AssetRecord> assets;
    std::vector<AssetDiagnostic> diagnostics;
};

class AssetDatabase
{
public:
    static constexpr std::uint32_t CurrentVersion = 1;

    explicit AssetDatabase(
        std::filesystem::path projectRoot,
        std::filesystem::path manifestPath = {});

    bool Load(std::string* outError = nullptr);
    bool Save(std::string* outError = nullptr) const;

    [[nodiscard]] AssetImportResult ImportGltf(
        const std::filesystem::path& sourcePath,
        bool reimport);
    [[nodiscard]] AssetImportResult ImportTexture(
        const std::filesystem::path& sourcePath,
        bool reimport);

    [[nodiscard]] std::vector<AssetRecord> List(
        std::optional<AssetType> type = std::nullopt,
        std::string_view query = {}) const;
    [[nodiscard]] const AssetRecord* FindById(std::string_view assetId) const;
    [[nodiscard]] const AssetRecord* FindByPath(std::string_view assetPath) const;
    [[nodiscard]] std::vector<AssetRecord> GetImportedScenes() const;
    [[nodiscard]] bool ValidateImportedContent(
        std::vector<AssetDiagnostic>& diagnostics) const;

    [[nodiscard]] std::string ComputeManifestHash() const;
    [[nodiscard]] const std::filesystem::path& GetManifestPath() const;
    [[nodiscard]] const std::filesystem::path& GetProjectRoot() const;
    [[nodiscard]] std::uint64_t GetManifestRevision() const;

    [[nodiscard]] static std::string MakeStableAssetId(
        std::string_view sourceIdentity,
        std::string_view subresource);
    [[nodiscard]] static std::string MakeAssetPath(std::string_view assetId);
    [[nodiscard]] static std::string MakeMeshSubresource(
        int meshIndex,
        std::uint32_t primitiveIndex);
    [[nodiscard]] static std::string MakeMaterialSubresource(int materialIndex);
    [[nodiscard]] static std::string MakeTextureSubresource(
        int materialIndex,
        std::string_view role);
    [[nodiscard]] static std::string ToString(AssetType type);
    [[nodiscard]] static std::optional<AssetType> ParseAssetType(std::string_view type);
    [[nodiscard]] static std::string ToString(AssetDiagnosticSeverity severity);

private:
    [[nodiscard]] std::filesystem::path ResolveProjectPath(
        const std::filesystem::path& path) const;
    void AddBuiltInRecords();
    [[nodiscard]] nlohmann::json SerializeImportedManifest() const;

    std::filesystem::path m_projectRoot;
    std::filesystem::path m_manifestPath;
    std::uint64_t m_manifestRevision = 0;
    std::map<std::string, AssetRecord> m_records;
};
} // namespace Prism::Asset
