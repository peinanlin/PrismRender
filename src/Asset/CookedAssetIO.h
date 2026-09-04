#pragma once

#include "Asset/MaterialAsset.h"
#include "Asset/MeshAsset.h"
#include "Asset/TextureAsset.h"

#include <array>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>

namespace Prism::Asset
{
struct CookedMaterialData
{
    std::shared_ptr<MaterialAsset> material;
    std::array<std::string, 5> textureAssetIds;
};

enum class CookedAssetCompression : std::uint32_t
{
    None = 0,
    RunLength = 1
};

struct CookedAssetInfo
{
    bool valid = false;
    std::uint32_t containerVersion = 0;
    std::uint32_t payloadVersion = 0;
    std::string kind;
    CookedAssetCompression compression =
        CookedAssetCompression::None;
    std::uint64_t uncompressedBytes = 0;
    std::uint64_t storedBytes = 0;
    std::uint64_t payloadChecksum = 0;
    bool checksumVerified = false;
    std::string errorMessage;
};

struct CookedAssetWriteOptions
{
    std::uint32_t containerVersion = 2;
    bool enableCompression = true;
};

class CookedAssetIO
{
public:
    static constexpr std::uint32_t LegacyVersion = 1;
    static constexpr std::uint32_t CurrentVersion = 2;
    static constexpr std::uint32_t CurrentPayloadVersion = 1;

    [[nodiscard]] static bool IsSupportedVersion(
        std::uint32_t version);
    [[nodiscard]] static std::string ToString(
        CookedAssetCompression compression);
    [[nodiscard]] static CookedAssetInfo Inspect(
        const std::filesystem::path& path);

    [[nodiscard]] static std::filesystem::path MakePath(
        const std::filesystem::path& projectRoot,
        std::string_view contentHash,
        std::string_view assetId,
        std::string_view extension);

    static bool WriteMesh(
        const std::filesystem::path& path,
        const MeshAsset& mesh,
        std::string* outError = nullptr);
    static bool WriteMeshWithOptions(
        const std::filesystem::path& path,
        const MeshAsset& mesh,
        const CookedAssetWriteOptions& options,
        std::string* outError = nullptr);
    static bool ReadMesh(
        const std::filesystem::path& path,
        std::shared_ptr<MeshAsset>& mesh,
        std::string* outError = nullptr);

    static bool WriteTexture(
        const std::filesystem::path& path,
        const TextureAsset& texture,
        std::string* outError = nullptr);
    static bool WriteTextureWithOptions(
        const std::filesystem::path& path,
        const TextureAsset& texture,
        const CookedAssetWriteOptions& options,
        std::string* outError = nullptr);
    static bool ReadTexture(
        const std::filesystem::path& path,
        std::shared_ptr<TextureAsset>& texture,
        std::string* outError = nullptr);

    static bool WriteMaterial(
        const std::filesystem::path& path,
        const MaterialAsset& material,
        const std::array<std::string, 5>& textureAssetIds,
        std::string* outError = nullptr);
    static bool WriteMaterialWithOptions(
        const std::filesystem::path& path,
        const MaterialAsset& material,
        const std::array<std::string, 5>& textureAssetIds,
        const CookedAssetWriteOptions& options,
        std::string* outError = nullptr);
    static bool ReadMaterial(
        const std::filesystem::path& path,
        CookedMaterialData& material,
        std::string* outError = nullptr);
};
} // namespace Prism::Asset
