#pragma once

#include "Asset/AssetDatabase.h"

#include <filesystem>
#include <string>

namespace Prism::Asset
{
class AssetImportService
{
public:
    explicit AssetImportService(
        std::filesystem::path projectRoot,
        std::filesystem::path manifestPath = {});

    bool Initialize(std::string* outError = nullptr);
    [[nodiscard]] AssetImportResult Import(
        const std::filesystem::path& sourcePath);
    [[nodiscard]] AssetImportResult Reimport(
        const std::filesystem::path& projectSourcePath);

    [[nodiscard]] const AssetDatabase& GetDatabase() const;
    [[nodiscard]] AssetDatabase& GetDatabase();
    [[nodiscard]] static bool Supports(
        const std::filesystem::path& sourcePath);

private:
    bool StageExternalSource(
        const std::filesystem::path& sourcePath,
        std::filesystem::path& outProjectSource,
        std::string& outError) const;
    bool StageGltfDocument(
        const std::filesystem::path& sourcePath,
        const std::filesystem::path& targetPath,
        std::string& outError) const;
    [[nodiscard]] bool IsInsideProject(
        const std::filesystem::path& path) const;

    std::filesystem::path m_projectRoot;
    AssetDatabase m_database;
};
}
