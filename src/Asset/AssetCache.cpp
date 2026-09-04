#include "Asset/AssetCache.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cwchar>
#include <fstream>
#include <iomanip>
#include <limits>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>
#include <unordered_set>

namespace Prism::Asset
{
namespace
{
using json = nlohmann::json;

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

std::string Hex64(const std::uint64_t value)
{
    std::ostringstream output;
    output << std::hex << std::setfill('0') << std::setw(16) << value;
    return output.str();
}

bool IsSafeCacheKey(const std::string_view value)
{
    if (value.empty() || value.size() > 128)
    {
        return false;
    }
    for (const unsigned char character : value)
    {
        if (!std::isalnum(character) && character != '-' && character != '_')
        {
            return false;
        }
    }
    return true;
}

std::uint64_t CurrentUnixMilliseconds()
{
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now()
                .time_since_epoch())
            .count());
}

std::uintmax_t DirectorySize(
    const std::filesystem::path& path)
{
    if (!std::filesystem::is_directory(path))
    {
        return 0;
    }
    std::uintmax_t total = 0;
    for (const std::filesystem::directory_entry& entry :
         std::filesystem::recursive_directory_iterator(path))
    {
        if (entry.is_regular_file())
        {
            total += entry.file_size();
        }
    }
    return total;
}

std::uint64_t ReadLastAccess(
    const std::filesystem::path& bundleDirectory)
{
    std::ifstream input(
        bundleDirectory / "access.json",
        std::ios::binary);
    if (!input)
    {
        return 0;
    }
    try
    {
        json document;
        input >> document;
        if (document.value("format", std::string{})
                != "PrismAssetCacheAccess"
            || document.value("version", 0u) != 1u)
        {
            return 0;
        }
        return document.value(
            "lastAccessUnixMilliseconds",
            0ull);
    }
    catch (const std::exception&)
    {
        return 0;
    }
}
} // namespace

AssetCache::AssetCache(std::filesystem::path projectRoot)
    : m_projectRoot(std::filesystem::absolute(std::move(projectRoot)).lexically_normal())
{
}

AssetCacheBundle AssetCache::Build(
    const std::string_view contentHash,
    const std::string_view sourcePath,
    const nlohmann::json& dependencyFiles) const
{
    AssetCacheBundle result{};
    result.contentHash = contentHash;
    result.sourcePath = sourcePath;
    try
    {
        if (!IsSafeCacheKey(contentHash))
        {
            throw std::runtime_error("The asset cache key contains unsupported characters.");
        }

        const std::filesystem::path bundleDirectory =
            GetBundleDirectory(contentHash);
        const std::filesystem::path filesDirectory = bundleDirectory / "files";
        std::filesystem::create_directories(filesDirectory);

        std::set<std::string> uniquePaths;
        for (const json& dependency : dependencyFiles)
        {
            const std::string projectPath =
                dependency.value("path", std::string{});
            if (projectPath.empty() || !uniquePaths.insert(projectPath).second)
            {
                continue;
            }

            const std::filesystem::path source = ResolveProjectPath(projectPath);
            if (!std::filesystem::is_regular_file(source))
            {
                throw std::runtime_error(
                    "Could not cache missing asset dependency: " + projectPath);
            }

            const std::filesystem::path relative(projectPath);
            const std::filesystem::path cached =
                (filesDirectory / relative).lexically_normal();
            const std::filesystem::path relativeToCache =
                cached.lexically_relative(bundleDirectory);
            if (relativeToCache.empty() || relativeToCache.native().starts_with(L".."))
            {
                throw std::runtime_error(
                    "An asset dependency escaped the cache bundle directory.");
            }
            if (!cached.parent_path().empty())
            {
                std::filesystem::create_directories(cached.parent_path());
            }
            std::filesystem::copy_file(
                source,
                cached,
                std::filesystem::copy_options::overwrite_existing);

            AssetCacheFile file{};
            file.projectPath = std::filesystem::path(projectPath).generic_string();
            file.cachePath = relativeToCache.generic_string();
            file.contentHash = HashFile(cached);
            file.byteSize = std::filesystem::file_size(cached);
            result.totalBytes += file.byteSize;
            result.files.push_back(std::move(file));
        }

        const std::filesystem::path cachedSource =
            filesDirectory / std::filesystem::path(sourcePath);
        if (!std::filesystem::is_regular_file(cachedSource))
        {
            throw std::runtime_error(
                "The asset cache bundle does not contain its source document.");
        }

        json files = json::array();
        for (const AssetCacheFile& file : result.files)
        {
            files.push_back({
                {"projectPath", file.projectPath},
                {"cachePath", file.cachePath},
                {"contentHash", file.contentHash},
                {"byteSize", file.byteSize}});
        }
        const json bundle = {
            {"format", "PrismAssetCacheBundle"},
            {"version", CurrentVersion},
            {"contentHash", result.contentHash},
            {"sourcePath", result.sourcePath},
            {"cachedSourcePath", cachedSource.lexically_relative(bundleDirectory).generic_string()},
            {"totalBytes", result.totalBytes},
            {"files", std::move(files)}};

        result.bundlePath = bundleDirectory / "bundle.json";
        const std::filesystem::path temporary =
            bundleDirectory / "bundle.json.tmp";
        {
            std::ofstream output(
                temporary,
                std::ios::binary | std::ios::trunc);
            if (!output)
            {
                throw std::runtime_error(
                    "Could not create the asset cache bundle manifest.");
            }
            output << std::setw(2) << bundle << '\n';
            if (!output)
            {
                throw std::runtime_error(
                    "Could not write the asset cache bundle manifest.");
            }
        }
        if (std::filesystem::exists(result.bundlePath))
        {
            std::filesystem::remove(result.bundlePath);
        }
        std::filesystem::rename(temporary, result.bundlePath);
        result.cachedSourcePath = cachedSource;
        result.valid = true;
        Touch(contentHash);
        return result;
    }
    catch (const std::exception& exception)
    {
        result.errorCode = "asset_cache_build_failed";
        result.errorMessage = exception.what();
        return result;
    }
}

AssetCacheBundle AssetCache::Inspect(const std::string_view contentHash) const
{
    AssetCacheBundle result{};
    result.contentHash = contentHash;
    try
    {
        if (!IsSafeCacheKey(contentHash))
        {
            throw std::runtime_error("The asset cache key contains unsupported characters.");
        }
        const std::filesystem::path bundleDirectory =
            GetBundleDirectory(contentHash);
        result.bundlePath = bundleDirectory / "bundle.json";
        std::ifstream input(result.bundlePath, std::ios::binary);
        if (!input)
        {
            result.errorCode = "asset_cache_missing";
            result.errorMessage = "The content-addressed asset cache bundle is missing.";
            return result;
        }
        json bundle;
        input >> bundle;
        if (bundle.value("format", std::string{}) != "PrismAssetCacheBundle"
            || bundle.value("version", 0u) != CurrentVersion
            || bundle.value("contentHash", std::string{}) != contentHash)
        {
            throw std::runtime_error("The asset cache bundle header is invalid.");
        }

        result.sourcePath = bundle.value("sourcePath", std::string{});
        result.totalBytes = bundle.value("totalBytes", 0ull);
        result.cachedSourcePath =
            bundleDirectory / bundle.value("cachedSourcePath", std::string{});
        for (const json& serialized : bundle.value("files", json::array()))
        {
            AssetCacheFile file{};
            file.projectPath = serialized.value("projectPath", std::string{});
            file.cachePath = serialized.value("cachePath", std::string{});
            file.contentHash = serialized.value("contentHash", std::string{});
            file.byteSize = serialized.value("byteSize", 0ull);
            const std::filesystem::path cached =
                bundleDirectory / file.cachePath;
            if (!std::filesystem::is_regular_file(cached))
            {
                throw std::runtime_error(
                    "A file is missing from the asset cache bundle: "
                    + file.cachePath);
            }
            if (std::filesystem::file_size(cached) != file.byteSize
                || HashFile(cached) != file.contentHash)
            {
                throw std::runtime_error(
                    "A file in the asset cache bundle failed validation: "
                    + file.cachePath);
            }
            result.files.push_back(std::move(file));
        }
        if (!std::filesystem::is_regular_file(result.cachedSourcePath))
        {
            throw std::runtime_error(
                "The cached source document is missing.");
        }
        result.valid = true;
        return result;
    }
    catch (const std::exception& exception)
    {
        result.errorCode = "asset_cache_invalid";
        result.errorMessage = exception.what();
        return result;
    }
}

AssetCacheBundle AssetCache::Resolve(const AssetRecord& sourceRecord) const
{
    AssetCacheBundle result = Inspect(sourceRecord.contentHash);
    if (result.valid)
    {
        Touch(sourceRecord.contentHash);
    }
    return result;
}

AssetCacheGarbageCollectionResult AssetCache::CollectGarbage(
    const std::vector<std::string>& referencedContentHashes,
    const AssetCacheGarbageCollectionOptions& options) const
{
    AssetCacheGarbageCollectionResult result{};
    result.dryRun = options.dryRun;
    result.maximumBytes = options.maximumBytes;
    try
    {
        const std::filesystem::path assetRoot =
            std::filesystem::absolute(
                GetCacheRoot()).lexically_normal();
        const std::filesystem::path cookedRoot =
            std::filesystem::absolute(
                GetCookedCacheRoot()).lexically_normal();
        std::filesystem::create_directories(assetRoot);
        std::filesystem::create_directories(cookedRoot);

        std::unordered_set<std::string> referenced;
        for (const std::string& contentHash :
             referencedContentHashes)
        {
            if (IsSafeCacheKey(contentHash))
            {
                referenced.insert(contentHash);
            }
        }

        std::map<std::string, AssetCacheGarbageCollectionEntry>
            entries;
        const auto scanRoot =
            [&](const std::filesystem::path& root,
                const bool assetBundles)
            {
                for (const std::filesystem::directory_entry& entry :
                     std::filesystem::directory_iterator(root))
                {
                    if (!entry.is_directory())
                    {
                        continue;
                    }
                    const std::string contentHash =
                        entry.path().filename().string();
                    if (!IsSafeCacheKey(contentHash))
                    {
                        continue;
                    }
                    AssetCacheGarbageCollectionEntry& record =
                        entries[contentHash];
                    record.contentHash = contentHash;
                    if (assetBundles)
                    {
                        record.assetBundleExists = true;
                        record.lastAccessUnixMilliseconds =
                            ReadLastAccess(entry.path());
                    }
                    else
                    {
                        record.cookedAssetsExist = true;
                    }
                    record.byteSize +=
                        DirectorySize(entry.path());
                }
            };
        scanRoot(assetRoot, true);
        scanRoot(cookedRoot, false);

        const std::uint64_t now =
            CurrentUnixMilliseconds();
        for (auto& [contentHash, entry] : entries)
        {
            entry.referenced =
                referenced.contains(contentHash);
            result.bytesBefore += entry.byteSize;
            result.referencedEntryCount +=
                entry.referenced ? 1u : 0u;
            result.entries.push_back(entry);
        }
        result.scannedEntryCount = result.entries.size();

        std::vector<AssetCacheGarbageCollectionEntry*>
            candidates;
        for (AssetCacheGarbageCollectionEntry& entry :
             result.entries)
        {
            if (!entry.referenced)
            {
                candidates.push_back(&entry);
            }
        }
        std::ranges::sort(
            candidates,
            [](const AssetCacheGarbageCollectionEntry* left,
               const AssetCacheGarbageCollectionEntry* right)
            {
                if (left->lastAccessUnixMilliseconds
                    != right->lastAccessUnixMilliseconds)
                {
                    return left->lastAccessUnixMilliseconds
                        < right->lastAccessUnixMilliseconds;
                }
                return left->contentHash < right->contentHash;
            });

        std::uintmax_t projectedBytes = result.bytesBefore;
        const std::uint64_t minimumAgeMilliseconds =
            options.minimumUnusedAgeSeconds
            > std::numeric_limits<std::uint64_t>::max()
                / 1000ull
            ? std::numeric_limits<std::uint64_t>::max()
            : options.minimumUnusedAgeSeconds * 1000ull;
        for (AssetCacheGarbageCollectionEntry* entry :
             candidates)
        {
            const std::uint64_t age =
                entry->lastAccessUnixMilliseconds == 0
                || entry->lastAccessUnixMilliseconds > now
                ? std::numeric_limits<std::uint64_t>::max()
                : now - entry->lastAccessUnixMilliseconds;
            if (age >= minimumAgeMilliseconds)
            {
                entry->selected = true;
                entry->reason = "unreferenced";
                projectedBytes -= entry->byteSize;
            }
        }
        if (options.maximumBytes > 0
            && projectedBytes > options.maximumBytes)
        {
            for (AssetCacheGarbageCollectionEntry* entry :
                 candidates)
            {
                if (entry->selected)
                {
                    continue;
                }
                entry->selected = true;
                entry->reason = "capacity";
                projectedBytes -= entry->byteSize;
                if (projectedBytes <= options.maximumBytes)
                {
                    break;
                }
            }
        }

        for (const AssetCacheGarbageCollectionEntry& entry :
             result.entries)
        {
            if (entry.selected)
            {
                ++result.selectedEntryCount;
                result.reclaimableBytes += entry.byteSize;
            }
        }
        result.budgetSatisfied =
            options.maximumBytes == 0
            || projectedBytes <= options.maximumBytes;
        result.bytesAfter = projectedBytes;

        if (!options.dryRun)
        {
            for (AssetCacheGarbageCollectionEntry& entry :
                 result.entries)
            {
                if (!entry.selected)
                {
                    continue;
                }
                const std::filesystem::path assetTarget =
                    std::filesystem::absolute(
                        assetRoot / entry.contentHash)
                        .lexically_normal();
                const std::filesystem::path cookedTarget =
                    std::filesystem::absolute(
                        cookedRoot / entry.contentHash)
                        .lexically_normal();
                if (assetTarget.parent_path() != assetRoot
                    || cookedTarget.parent_path() != cookedRoot
                    || !IsSafeCacheKey(entry.contentHash))
                {
                    throw std::runtime_error(
                        "Asset Cache GC resolved an unsafe deletion target.");
                }
                if (std::filesystem::exists(assetTarget))
                {
                    std::filesystem::remove_all(assetTarget);
                }
                if (std::filesystem::exists(cookedTarget))
                {
                    std::filesystem::remove_all(cookedTarget);
                }
                entry.removed = true;
                ++result.removedEntryCount;
                result.reclaimedBytes += entry.byteSize;
            }
            result.bytesAfter =
                result.bytesBefore - result.reclaimedBytes;
        }
        result.success = true;
        return result;
    }
    catch (const std::exception& exception)
    {
        result.errorCode = "asset_cache_gc_failed";
        result.errorMessage = exception.what();
        return result;
    }
}

std::filesystem::path AssetCache::GetCacheRoot() const
{
    return m_projectRoot / "automation/cache/assets";
}

std::filesystem::path AssetCache::GetCookedCacheRoot() const
{
    return m_projectRoot / "automation/cache/cooked";
}

std::filesystem::path AssetCache::GetBundleDirectory(
    const std::string_view contentHash) const
{
    return GetCacheRoot() / std::string(contentHash);
}

std::filesystem::path AssetCache::ResolveProjectPath(
    const std::filesystem::path& path) const
{
    const std::filesystem::path candidate = std::filesystem::absolute(
        path.is_absolute() ? path : m_projectRoot / path).lexically_normal();
    auto rootIterator = m_projectRoot.begin();
    auto candidateIterator = candidate.begin();
    for (; rootIterator != m_projectRoot.end(); ++rootIterator, ++candidateIterator)
    {
        if (candidateIterator == candidate.end()
            || _wcsicmp(rootIterator->c_str(), candidateIterator->c_str()) != 0)
        {
            throw std::runtime_error(
                "Asset cache file access is restricted to the project root.");
        }
    }
    return candidate;
}

std::string AssetCache::HashFile(const std::filesystem::path& path)
{
    std::ifstream input(path, std::ios::binary);
    if (!input)
    {
        throw std::runtime_error(
            "Could not open cached asset file for hashing: " + path.string());
    }
    std::uint64_t hash = 14695981039346656037ull;
    std::array<char, 64 * 1024> buffer{};
    while (input)
    {
        input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        hash = HashBytes(
            hash,
            reinterpret_cast<const unsigned char*>(buffer.data()),
            static_cast<std::size_t>(input.gcount()));
    }
    return Hex64(hash);
}

void AssetCache::Touch(const std::string_view contentHash) const
{
    if (!IsSafeCacheKey(contentHash))
    {
        return;
    }
    try
    {
        const std::filesystem::path bundleDirectory =
            GetBundleDirectory(contentHash);
        std::filesystem::create_directories(bundleDirectory);
        const std::filesystem::path path =
            bundleDirectory / "access.json";
        const std::filesystem::path temporary =
            bundleDirectory / "access.json.tmp";
        {
            std::ofstream output(
                temporary,
                std::ios::binary | std::ios::trunc);
            if (!output)
            {
                return;
            }
            output << json{
                {"format", "PrismAssetCacheAccess"},
                {"version", 1},
                {"lastAccessUnixMilliseconds",
                 CurrentUnixMilliseconds()}}.dump(2)
                   << '\n';
        }
        if (std::filesystem::exists(path))
        {
            std::filesystem::remove(path);
        }
        std::filesystem::rename(temporary, path);
    }
    catch (const std::exception&)
    {
        // Access metadata must not make a valid cache unusable.
    }
}
} // namespace Prism::Asset
