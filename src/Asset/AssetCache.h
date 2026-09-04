#pragma once

#include "Asset/AssetDatabase.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace Prism::Asset
{
struct AssetCacheFile
{
    std::string projectPath;
    std::string cachePath;
    std::string contentHash;
    std::uintmax_t byteSize = 0;
};

struct AssetCacheBundle
{
    bool valid = false;
    std::string contentHash;
    std::string sourcePath;
    std::filesystem::path bundlePath;
    std::filesystem::path cachedSourcePath;
    std::uintmax_t totalBytes = 0;
    std::vector<AssetCacheFile> files;
    std::string errorCode;
    std::string errorMessage;
};

struct AssetCacheGarbageCollectionOptions
{
    bool dryRun = true;
    std::uintmax_t maximumBytes = 0;
    std::uint64_t minimumUnusedAgeSeconds = 0;
};

struct AssetCacheGarbageCollectionEntry
{
    std::string contentHash;
    bool referenced = false;
    bool assetBundleExists = false;
    bool cookedAssetsExist = false;
    std::uintmax_t byteSize = 0;
    std::uint64_t lastAccessUnixMilliseconds = 0;
    bool selected = false;
    bool removed = false;
    std::string reason;
};

struct AssetCacheGarbageCollectionResult
{
    bool success = false;
    bool dryRun = true;
    bool budgetSatisfied = true;
    std::uintmax_t maximumBytes = 0;
    std::uintmax_t bytesBefore = 0;
    std::uintmax_t bytesAfter = 0;
    std::uintmax_t reclaimableBytes = 0;
    std::uintmax_t reclaimedBytes = 0;
    std::size_t scannedEntryCount = 0;
    std::size_t referencedEntryCount = 0;
    std::size_t selectedEntryCount = 0;
    std::size_t removedEntryCount = 0;
    std::vector<AssetCacheGarbageCollectionEntry> entries;
    std::string errorCode;
    std::string errorMessage;
};

class AssetCache
{
public:
    static constexpr std::uint32_t CurrentVersion = 1;

    explicit AssetCache(std::filesystem::path projectRoot);

    [[nodiscard]] AssetCacheBundle Build(
        std::string_view contentHash,
        std::string_view sourcePath,
        const nlohmann::json& dependencyFiles) const;
    [[nodiscard]] AssetCacheBundle Inspect(
        std::string_view contentHash) const;
    [[nodiscard]] AssetCacheBundle Resolve(
        const AssetRecord& sourceRecord) const;
    [[nodiscard]] AssetCacheGarbageCollectionResult
        CollectGarbage(
            const std::vector<std::string>&
                referencedContentHashes,
            const AssetCacheGarbageCollectionOptions&
                options) const;

    [[nodiscard]] std::filesystem::path GetCacheRoot() const;
    [[nodiscard]] std::filesystem::path GetCookedCacheRoot() const;
    [[nodiscard]] std::filesystem::path GetBundleDirectory(
        std::string_view contentHash) const;

private:
    [[nodiscard]] std::filesystem::path ResolveProjectPath(
        const std::filesystem::path& path) const;
    [[nodiscard]] static std::string HashFile(
        const std::filesystem::path& path);
    void Touch(std::string_view contentHash) const;

    std::filesystem::path m_projectRoot;
};
} // namespace Prism::Asset
