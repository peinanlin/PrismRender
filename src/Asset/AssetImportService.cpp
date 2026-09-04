#include "Asset/AssetImportService.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <fstream>
#include <map>
#include <stdexcept>
#include <system_error>

namespace Prism::Asset
{
namespace
{
using json = nlohmann::json;

std::string Lowercase(std::string value)
{
    std::ranges::transform(
        value,
        value.begin(),
        [](const unsigned char character)
        {
            return static_cast<char>(std::tolower(character));
        });
    return value;
}

bool IsExternalUri(const std::string& uri)
{
    return !uri.empty()
        && !uri.starts_with("data:")
        && uri.find("://") == std::string::npos;
}
}

AssetImportService::AssetImportService(
    std::filesystem::path projectRoot,
    std::filesystem::path manifestPath)
    : m_projectRoot(
          std::filesystem::absolute(std::move(projectRoot))
              .lexically_normal()),
      m_database(m_projectRoot, std::move(manifestPath))
{
}

bool AssetImportService::Initialize(std::string* outError)
{
    return m_database.Load(outError);
}

AssetImportResult AssetImportService::Import(
    const std::filesystem::path& sourcePath)
{
    std::filesystem::path projectSource;
    std::string stageError;
    if (!StageExternalSource(
            sourcePath, projectSource, stageError))
    {
        AssetImportResult result{};
        result.errorCode = "asset_stage_failed";
        result.errorMessage = std::move(stageError);
        return result;
    }

    const std::string extension =
        Lowercase(projectSource.extension().string());
    if (extension == ".gltf" || extension == ".glb")
    {
        return m_database.ImportGltf(projectSource, false);
    }
    return m_database.ImportTexture(projectSource, false);
}

AssetImportResult AssetImportService::Reimport(
    const std::filesystem::path& projectSourcePath)
{
    const std::filesystem::path resolved =
        projectSourcePath.is_absolute()
        ? projectSourcePath
        : m_projectRoot / projectSourcePath;
    const std::string extension =
        Lowercase(resolved.extension().string());
    if (extension == ".gltf" || extension == ".glb")
    {
        return m_database.ImportGltf(resolved, true);
    }
    return m_database.ImportTexture(resolved, true);
}

const AssetDatabase& AssetImportService::GetDatabase() const
{
    return m_database;
}

AssetDatabase& AssetImportService::GetDatabase()
{
    return m_database;
}

bool AssetImportService::Supports(
    const std::filesystem::path& sourcePath)
{
    const std::string extension =
        Lowercase(sourcePath.extension().string());
    return extension == ".gltf" || extension == ".glb"
        || extension == ".png" || extension == ".jpg"
        || extension == ".jpeg" || extension == ".tga"
        || extension == ".bmp";
}

bool AssetImportService::StageExternalSource(
    const std::filesystem::path& sourcePath,
    std::filesystem::path& outProjectSource,
    std::string& outError) const
{
    try
    {
        const std::filesystem::path source =
            std::filesystem::absolute(sourcePath)
                .lexically_normal();
        if (!std::filesystem::is_regular_file(source))
        {
            outError = "The selected source file does not exist.";
            return false;
        }
        if (!Supports(source))
        {
            outError =
                "Supported imports are glTF/GLB, PNG, JPEG, TGA, and BMP.";
            return false;
        }
        if (IsInsideProject(source))
        {
            outProjectSource = source;
            return true;
        }

        const std::filesystem::path importRoot =
            m_projectRoot / "assets/imported";
        std::filesystem::create_directories(importRoot);
        const std::string baseName = source.stem().string().empty()
            ? "ImportedAsset"
            : source.stem().string();
        std::filesystem::path targetDirectory =
            importRoot / baseName;
        for (std::uint32_t suffix = 2;
             std::filesystem::exists(targetDirectory);
             ++suffix)
        {
            targetDirectory = importRoot
                / (baseName + "_" + std::to_string(suffix));
        }
        std::filesystem::create_directories(targetDirectory);
        const std::filesystem::path target =
            targetDirectory / source.filename();

        const std::string extension =
            Lowercase(source.extension().string());
        if (extension == ".gltf")
        {
            if (!StageGltfDocument(source, target, outError))
            {
                std::error_code cleanupError;
                std::filesystem::remove_all(
                    targetDirectory, cleanupError);
                return false;
            }
        }
        else
        {
            std::filesystem::copy_file(
                source,
                target,
                std::filesystem::copy_options::none);
        }
        outProjectSource = target;
        return true;
    }
    catch (const std::exception& exception)
    {
        outError = exception.what();
        return false;
    }
}

bool AssetImportService::StageGltfDocument(
    const std::filesystem::path& sourcePath,
    const std::filesystem::path& targetPath,
    std::string& outError) const
{
    try
    {
        std::ifstream input(sourcePath, std::ios::binary);
        if (!input)
        {
            throw std::runtime_error(
                "Could not open the glTF source document.");
        }
        json document;
        input >> document;

        const std::filesystem::path dependenciesDirectory =
            targetPath.parent_path() / "dependencies";
        std::map<std::string, std::string> stagedUris;
        std::size_t dependencyIndex = 0;
        const auto stageEntries = [&](const char* field)
        {
            for (json& entry : document[field])
            {
                const std::string uri =
                    entry.value("uri", std::string{});
                if (!IsExternalUri(uri))
                {
                    continue;
                }
                if (const auto existing = stagedUris.find(uri);
                    existing != stagedUris.end())
                {
                    entry["uri"] = existing->second;
                    continue;
                }
                const std::filesystem::path dependency =
                    (sourcePath.parent_path()
                     / std::filesystem::path(uri))
                        .lexically_normal();
                if (!std::filesystem::is_regular_file(dependency))
                {
                    throw std::runtime_error(
                        "A glTF dependency is missing: "
                        + dependency.string());
                }
                std::filesystem::create_directories(
                    dependenciesDirectory);
                const std::string stagedName =
                    std::to_string(dependencyIndex++) + "_"
                    + dependency.filename().string();
                const std::filesystem::path stagedPath =
                    dependenciesDirectory / stagedName;
                std::filesystem::copy_file(
                    dependency,
                    stagedPath,
                    std::filesystem::copy_options::none);
                const std::string stagedUri =
                    (std::filesystem::path("dependencies")
                     / stagedName).generic_string();
                stagedUris.emplace(uri, stagedUri);
                entry["uri"] = stagedUri;
            }
        };
        if (document.contains("buffers"))
        {
            stageEntries("buffers");
        }
        if (document.contains("images"))
        {
            stageEntries("images");
        }

        std::ofstream output(
            targetPath,
            std::ios::binary | std::ios::trunc);
        if (!output)
        {
            throw std::runtime_error(
                "Could not create the staged glTF document.");
        }
        output << document.dump(2) << '\n';
        if (!output)
        {
            throw std::runtime_error(
                "Failed while writing the staged glTF document.");
        }
        return true;
    }
    catch (const std::exception& exception)
    {
        outError = exception.what();
        return false;
    }
}

bool AssetImportService::IsInsideProject(
    const std::filesystem::path& path) const
{
    auto root = m_projectRoot.begin();
    auto candidate = path.begin();
    for (; root != m_projectRoot.end(); ++root, ++candidate)
    {
        if (candidate == path.end()
            || Lowercase(root->string())
                != Lowercase(candidate->string()))
        {
            return false;
        }
    }
    return true;
}
}
