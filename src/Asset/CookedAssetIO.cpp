#include "Asset/CookedAssetIO.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstring>
#include <fstream>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <vector>

namespace Prism::Asset
{
namespace
{
constexpr std::uint64_t MaximumPayloadBytes =
    1024ull * 1024ull * 1024ull;

enum class CookedKind : std::uint32_t
{
    Mesh = 1,
    Texture = 2,
    Material = 3
};

struct CookedHeaderV1
{
    std::array<char, 8> magic{
        'P', 'R', 'C', 'O', 'O', 'K', 'E', 'D'};
    std::uint32_t version = CookedAssetIO::LegacyVersion;
    CookedKind kind = CookedKind::Mesh;
};

struct CookedHeaderV2
{
    std::array<char, 8> magic{
        'P', 'R', 'C', 'O', 'O', 'K', 'E', 'D'};
    std::uint32_t version = CookedAssetIO::CurrentVersion;
    CookedKind kind = CookedKind::Mesh;
    std::uint32_t headerBytes = 56;
    std::uint32_t payloadVersion =
        CookedAssetIO::CurrentPayloadVersion;
    CookedAssetCompression compression =
        CookedAssetCompression::None;
    std::uint32_t flags = 0;
    std::uint64_t uncompressedBytes = 0;
    std::uint64_t storedBytes = 0;
    std::uint64_t payloadChecksum = 0;
};

static_assert(sizeof(CookedHeaderV1) == 16);
static_assert(sizeof(CookedHeaderV2) == 56);
static_assert(sizeof(MeshBounds) == 28);
static_assert(sizeof(MeshVertex) == 64);

template<typename T>
void WriteValue(std::ostream& output, const T& value)
{
    static_assert(std::is_trivially_copyable_v<T>);
    output.write(
        reinterpret_cast<const char*>(&value),
        static_cast<std::streamsize>(sizeof(T)));
}

template<typename T>
void ReadValue(std::istream& input, T& value)
{
    static_assert(std::is_trivially_copyable_v<T>);
    input.read(
        reinterpret_cast<char*>(&value),
        static_cast<std::streamsize>(sizeof(T)));
    if (!input)
    {
        throw std::runtime_error(
            "A Cooked Asset ended before the expected data.");
    }
}

void WriteString(
    std::ostream& output,
    const std::string_view value)
{
    if (value.size()
        > std::numeric_limits<std::uint32_t>::max())
    {
        throw std::runtime_error(
            "A Cooked Asset string is too large.");
    }
    const std::uint32_t size =
        static_cast<std::uint32_t>(value.size());
    WriteValue(output, size);
    output.write(
        value.data(),
        static_cast<std::streamsize>(value.size()));
}

std::string ReadString(std::istream& input)
{
    std::uint32_t size = 0;
    ReadValue(input, size);
    if (size > 16 * 1024 * 1024)
    {
        throw std::runtime_error(
            "A Cooked Asset string length is invalid.");
    }
    std::string value(size, '\0');
    input.read(
        value.data(),
        static_cast<std::streamsize>(value.size()));
    if (!input)
    {
        throw std::runtime_error(
            "A Cooked Asset string is truncated.");
    }
    return value;
}

std::uint64_t HashBytes(
    const unsigned char* bytes,
    const std::size_t byteCount)
{
    std::uint64_t hash = 14695981039346656037ull;
    for (std::size_t index = 0; index < byteCount; ++index)
    {
        hash ^= bytes[index];
        hash *= 1099511628211ull;
    }
    return hash;
}

std::vector<std::uint8_t> CompressRunLength(
    const std::vector<std::uint8_t>& input)
{
    std::vector<std::uint8_t> output;
    output.reserve(input.size());
    std::size_t position = 0;
    while (position < input.size())
    {
        std::size_t runLength = 1;
        while (position + runLength < input.size()
               && input[position + runLength] == input[position]
               && runLength < 130)
        {
            ++runLength;
        }
        if (runLength >= 3)
        {
            output.push_back(static_cast<std::uint8_t>(
                0x80u | (runLength - 3u)));
            output.push_back(input[position]);
            position += runLength;
            continue;
        }

        const std::size_t literalStart = position;
        std::size_t literalLength = 0;
        while (position < input.size()
               && literalLength < 128)
        {
            runLength = 1;
            while (position + runLength < input.size()
                   && input[position + runLength]
                       == input[position]
                   && runLength < 130)
            {
                ++runLength;
            }
            if (runLength >= 3)
            {
                break;
            }
            const std::size_t available =
                128 - literalLength;
            const std::size_t copied =
                std::min(runLength, available);
            position += copied;
            literalLength += copied;
            if (copied < runLength)
            {
                break;
            }
        }
        output.push_back(static_cast<std::uint8_t>(
            literalLength - 1u));
        output.insert(
            output.end(),
            input.begin()
                + static_cast<std::ptrdiff_t>(literalStart),
            input.begin()
                + static_cast<std::ptrdiff_t>(position));
    }
    return output;
}

std::vector<std::uint8_t> DecompressRunLength(
    const std::vector<std::uint8_t>& input,
    const std::uint64_t expectedBytes)
{
    if (expectedBytes > MaximumPayloadBytes)
    {
        throw std::runtime_error(
            "Cooked Asset payload exceeds the safety limit.");
    }
    std::vector<std::uint8_t> output;
    output.reserve(static_cast<std::size_t>(expectedBytes));
    std::size_t position = 0;
    while (position < input.size())
    {
        const std::uint8_t control = input[position++];
        if ((control & 0x80u) != 0)
        {
            if (position >= input.size())
            {
                throw std::runtime_error(
                    "The Cooked Asset RLE stream is truncated.");
            }
            const std::size_t length =
                static_cast<std::size_t>(control & 0x7fu) + 3u;
            if (output.size() + length > expectedBytes)
            {
                throw std::runtime_error(
                    "The Cooked Asset RLE stream expands beyond its declared size.");
            }
            output.insert(
                output.end(),
                length,
                input[position++]);
        }
        else
        {
            const std::size_t length =
                static_cast<std::size_t>(control) + 1u;
            if (position + length > input.size()
                || output.size() + length > expectedBytes)
            {
                throw std::runtime_error(
                    "The Cooked Asset RLE literal is invalid.");
            }
            output.insert(
                output.end(),
                input.begin()
                    + static_cast<std::ptrdiff_t>(position),
                input.begin()
                    + static_cast<std::ptrdiff_t>(
                        position + length));
            position += length;
        }
    }
    if (output.size() != expectedBytes)
    {
        throw std::runtime_error(
            "The Cooked Asset RLE output size is invalid.");
    }
    return output;
}

std::string KindName(const CookedKind kind)
{
    switch (kind)
    {
    case CookedKind::Mesh: return "mesh";
    case CookedKind::Texture: return "texture";
    case CookedKind::Material: return "material";
    }
    return "unknown";
}

bool IsKnownKind(const CookedKind kind)
{
    return kind == CookedKind::Mesh
        || kind == CookedKind::Texture
        || kind == CookedKind::Material;
}

void ValidateKind(
    const CookedKind actual,
    const CookedKind expected)
{
    if (actual != expected)
    {
        throw std::runtime_error(
            "The Cooked Asset kind does not match the requested asset type.");
    }
}

std::vector<std::uint8_t> ReadStoredBytes(
    std::istream& input,
    const std::uint64_t byteCount)
{
    if (byteCount > MaximumPayloadBytes)
    {
        throw std::runtime_error(
            "Cooked Asset stored data exceeds the safety limit.");
    }
    std::vector<std::uint8_t> bytes(
        static_cast<std::size_t>(byteCount));
    input.read(
        reinterpret_cast<char*>(bytes.data()),
        static_cast<std::streamsize>(byteCount));
    if (!input)
    {
        throw std::runtime_error(
            "The Cooked Asset payload is truncated.");
    }
    if (input.peek() != std::char_traits<char>::eof())
    {
        throw std::runtime_error(
            "The Cooked Asset contains unexpected trailing data.");
    }
    return bytes;
}

struct PayloadReadResult
{
    CookedAssetInfo info;
    std::vector<std::uint8_t> payload;
};

PayloadReadResult ReadPayload(
    std::istream& input,
    const CookedKind expectedKind,
    const bool loadPayload)
{
    CookedHeaderV1 prefix{};
    ReadValue(input, prefix);
    const CookedHeaderV1 expected{};
    if (prefix.magic != expected.magic)
    {
        throw std::runtime_error(
            "The Cooked Asset magic is invalid.");
    }
    if (!IsKnownKind(prefix.kind))
    {
        throw std::runtime_error(
            "The Cooked Asset kind is invalid.");
    }
    ValidateKind(prefix.kind, expectedKind);

    PayloadReadResult result{};
    result.info.kind = KindName(prefix.kind);
    result.info.containerVersion = prefix.version;
    if (prefix.version == CookedAssetIO::LegacyVersion)
    {
        result.info.payloadVersion = 1;
        result.info.compression =
            CookedAssetCompression::None;
        const std::streampos payloadStart = input.tellg();
        input.seekg(0, std::ios::end);
        const std::streamoff payloadBytes =
            input.tellg() - payloadStart;
        if (payloadBytes < 0
            || static_cast<std::uint64_t>(payloadBytes)
                > MaximumPayloadBytes)
        {
            throw std::runtime_error(
                "The legacy Cooked Asset payload size is invalid.");
        }
        result.info.uncompressedBytes =
            static_cast<std::uint64_t>(payloadBytes);
        result.info.storedBytes =
            result.info.uncompressedBytes;
        input.seekg(payloadStart);
        if (loadPayload)
        {
            result.payload =
                ReadStoredBytes(input, result.info.storedBytes);
            result.info.payloadChecksum = HashBytes(
                result.payload.data(),
                result.payload.size());
        }
        result.info.checksumVerified = false;
        result.info.valid = true;
        return result;
    }
    if (prefix.version != CookedAssetIO::CurrentVersion)
    {
        throw std::runtime_error(
            "The Cooked Asset container version is unsupported.");
    }

    CookedHeaderV2 header{};
    header.magic = prefix.magic;
    header.version = prefix.version;
    header.kind = prefix.kind;
    input.read(
        reinterpret_cast<char*>(&header)
            + sizeof(CookedHeaderV1),
        static_cast<std::streamsize>(
            sizeof(CookedHeaderV2)
            - sizeof(CookedHeaderV1)));
    if (!input)
    {
        throw std::runtime_error(
            "The Cooked Asset v2 header is truncated.");
    }
    if (header.headerBytes != sizeof(CookedHeaderV2)
        || header.payloadVersion
            != CookedAssetIO::CurrentPayloadVersion
        || header.flags != 0
        || header.uncompressedBytes > MaximumPayloadBytes
        || header.storedBytes > MaximumPayloadBytes)
    {
        throw std::runtime_error(
            "The Cooked Asset v2 header is invalid.");
    }
    if (header.compression != CookedAssetCompression::None
        && header.compression
            != CookedAssetCompression::RunLength)
    {
        throw std::runtime_error(
            "The Cooked Asset compression codec is unsupported.");
    }

    result.info.containerVersion = header.version;
    result.info.payloadVersion = header.payloadVersion;
    result.info.compression = header.compression;
    result.info.uncompressedBytes =
        header.uncompressedBytes;
    result.info.storedBytes = header.storedBytes;
    result.info.payloadChecksum =
        header.payloadChecksum;
    const std::vector<std::uint8_t> stored =
        ReadStoredBytes(input, header.storedBytes);
    if (loadPayload)
    {
        result.payload =
            header.compression
                == CookedAssetCompression::RunLength
            ? DecompressRunLength(
                stored,
                header.uncompressedBytes)
            : stored;
        if (result.payload.size()
            != header.uncompressedBytes)
        {
            throw std::runtime_error(
                "The Cooked Asset payload size does not match its header.");
        }
        if (HashBytes(
                result.payload.data(),
                result.payload.size())
            != header.payloadChecksum)
        {
            throw std::runtime_error(
                "The Cooked Asset payload checksum failed.");
        }
        result.info.checksumVerified = true;
    }
    result.info.valid = true;
    return result;
}

template<typename Callback>
std::vector<std::uint8_t> BuildPayload(Callback&& callback)
{
    std::ostringstream output(
        std::ios::binary | std::ios::out);
    callback(output);
    if (!output)
    {
        throw std::runtime_error(
            "Failed while serializing a Cooked Asset payload.");
    }
    const std::string serialized = output.str();
    return std::vector<std::uint8_t>(
        serialized.begin(),
        serialized.end());
}

template<typename Callback>
void ParsePayload(
    const std::vector<std::uint8_t>& payload,
    Callback&& callback)
{
    const std::string serialized(
        payload.empty()
            ? std::string{}
            : std::string(
                reinterpret_cast<const char*>(payload.data()),
                payload.size()));
    std::istringstream input(
        serialized,
        std::ios::binary | std::ios::in);
    callback(input);
    if (input.peek() != std::char_traits<char>::eof())
    {
        throw std::runtime_error(
            "The Cooked Asset payload contains unexpected trailing data.");
    }
}

void WriteContainer(
    std::ostream& output,
    const CookedKind kind,
    const std::vector<std::uint8_t>& payload,
    const CookedAssetWriteOptions& options)
{
    if (options.containerVersion
        == CookedAssetIO::LegacyVersion)
    {
        CookedHeaderV1 header{};
        header.kind = kind;
        WriteValue(output, header);
        output.write(
            reinterpret_cast<const char*>(payload.data()),
            static_cast<std::streamsize>(payload.size()));
        return;
    }
    if (options.containerVersion
        != CookedAssetIO::CurrentVersion)
    {
        throw std::runtime_error(
            "The requested Cooked Asset write version is unsupported.");
    }

    std::vector<std::uint8_t> stored = payload;
    CookedAssetCompression compression =
        CookedAssetCompression::None;
    if (options.enableCompression && !payload.empty())
    {
        std::vector<std::uint8_t> compressed =
            CompressRunLength(payload);
        if (compressed.size() < payload.size())
        {
            stored = std::move(compressed);
            compression =
                CookedAssetCompression::RunLength;
        }
    }

    CookedHeaderV2 header{};
    header.kind = kind;
    header.compression = compression;
    header.uncompressedBytes = payload.size();
    header.storedBytes = stored.size();
    header.payloadChecksum = HashBytes(
        payload.data(),
        payload.size());
    WriteValue(output, header);
    output.write(
        reinterpret_cast<const char*>(stored.data()),
        static_cast<std::streamsize>(stored.size()));
}

template<typename Callback>
bool WriteFile(
    const std::filesystem::path& path,
    const CookedKind kind,
    const CookedAssetWriteOptions& options,
    Callback&& callback,
    std::string* outError)
{
    const std::filesystem::path temporary =
        path.parent_path()
        / (path.filename().string() + ".tmp");
    try
    {
        const std::vector<std::uint8_t> payload =
            BuildPayload(std::forward<Callback>(callback));
        if (payload.size() > MaximumPayloadBytes)
        {
            throw std::runtime_error(
                "Cooked Asset payload exceeds the safety limit.");
        }
        if (!path.parent_path().empty())
        {
            std::filesystem::create_directories(
                path.parent_path());
        }
        {
            std::ofstream output(
                temporary,
                std::ios::binary | std::ios::trunc);
            if (!output)
            {
                throw std::runtime_error(
                    "Could not create the Cooked Asset.");
            }
            WriteContainer(
                output,
                kind,
                payload,
                options);
            if (!output)
            {
                throw std::runtime_error(
                    "Failed while writing the Cooked Asset.");
            }
        }
        {
            std::ifstream verification(
                temporary,
                std::ios::binary);
            if (!verification)
            {
                throw std::runtime_error(
                    "Could not reopen the Cooked Asset for validation.");
            }
            const PayloadReadResult verified =
                ReadPayload(verification, kind, true);
            if (!verified.info.valid
                || (options.containerVersion
                        == CookedAssetIO::CurrentVersion
                    && !verified.info.checksumVerified))
            {
                throw std::runtime_error(
                    "The newly written Cooked Asset failed validation.");
            }
        }
        if (std::filesystem::exists(path))
        {
            std::filesystem::remove(path);
        }
        std::filesystem::rename(temporary, path);
        return true;
    }
    catch (const std::exception& exception)
    {
        std::error_code removeError;
        std::filesystem::remove(temporary, removeError);
        if (outError != nullptr)
        {
            *outError = exception.what();
        }
        return false;
    }
}

template<typename Callback>
bool ReadFile(
    const std::filesystem::path& path,
    const CookedKind kind,
    Callback&& callback,
    std::string* outError)
{
    try
    {
        std::ifstream input(path, std::ios::binary);
        if (!input)
        {
            throw std::runtime_error(
                "Could not open the Cooked Asset.");
        }
        const PayloadReadResult result =
            ReadPayload(input, kind, true);
        ParsePayload(result.payload, callback);
        return true;
    }
    catch (const std::exception& exception)
    {
        if (outError != nullptr)
        {
            *outError = exception.what();
        }
        return false;
    }
}

void WriteMeshPayload(
    std::ostream& output,
    const MeshAsset& mesh)
{
    WriteString(output, mesh.GetName());
    WriteValue(output, mesh.GetBounds());
    const std::uint64_t vertexCount =
        mesh.GetVertices().size();
    const std::uint64_t indexCount =
        mesh.GetIndices().size();
    WriteValue(output, vertexCount);
    WriteValue(output, indexCount);
    output.write(
        reinterpret_cast<const char*>(
            mesh.GetVertices().data()),
        static_cast<std::streamsize>(
            vertexCount * sizeof(MeshVertex)));
    output.write(
        reinterpret_cast<const char*>(
            mesh.GetIndices().data()),
        static_cast<std::streamsize>(
            indexCount * sizeof(std::uint16_t)));
}

void ReadMeshPayload(
    std::istream& input,
    std::shared_ptr<MeshAsset>& mesh)
{
    const std::string name = ReadString(input);
    MeshBounds bounds{};
    std::uint64_t vertexCount = 0;
    std::uint64_t indexCount = 0;
    ReadValue(input, bounds);
    ReadValue(input, vertexCount);
    ReadValue(input, indexCount);
    if (vertexCount > 10'000'000
        || indexCount > 30'000'000)
    {
        throw std::runtime_error(
            "Cooked Mesh element counts exceed the safety limit.");
    }
    std::vector<MeshVertex> vertices(
        static_cast<std::size_t>(vertexCount));
    std::vector<std::uint16_t> indices(
        static_cast<std::size_t>(indexCount));
    input.read(
        reinterpret_cast<char*>(vertices.data()),
        static_cast<std::streamsize>(
            vertexCount * sizeof(MeshVertex)));
    input.read(
        reinterpret_cast<char*>(indices.data()),
        static_cast<std::streamsize>(
            indexCount * sizeof(std::uint16_t)));
    if (!input)
    {
        throw std::runtime_error(
            "The Cooked Mesh is truncated.");
    }
    mesh = std::make_shared<MeshAsset>();
    mesh->SetName(name);
    mesh->SetGeometry(
        std::move(vertices),
        std::move(indices),
        bounds);
}

void WriteTexturePayload(
    std::ostream& output,
    const TextureAsset& texture)
{
    WriteString(output, texture.GetName());
    WriteString(output, texture.GetSourcePath());
    WriteValue(output, texture.GetSolidColor());
    WriteValue(output, texture.GetWidth());
    WriteValue(output, texture.GetHeight());
    const std::uint64_t byteCount =
        texture.GetImageData().size();
    WriteValue(output, byteCount);
    output.write(
        reinterpret_cast<const char*>(
            texture.GetImageData().data()),
        static_cast<std::streamsize>(byteCount));
}

void ReadTexturePayload(
    std::istream& input,
    std::shared_ptr<TextureAsset>& texture)
{
    const std::string name = ReadString(input);
    const std::string sourcePath = ReadString(input);
    DirectX::XMFLOAT4 solidColor{};
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint64_t byteCount = 0;
    ReadValue(input, solidColor);
    ReadValue(input, width);
    ReadValue(input, height);
    ReadValue(input, byteCount);
    if (byteCount > MaximumPayloadBytes)
    {
        throw std::runtime_error(
            "Cooked Texture data exceeds the safety limit.");
    }
    std::vector<std::uint8_t> pixels(
        static_cast<std::size_t>(byteCount));
    input.read(
        reinterpret_cast<char*>(pixels.data()),
        static_cast<std::streamsize>(byteCount));
    if (!input)
    {
        throw std::runtime_error(
            "The Cooked Texture is truncated.");
    }
    texture = std::make_shared<TextureAsset>();
    texture->SetName(name);
    texture->SetSourcePath(sourcePath);
    texture->SetSolidColor(solidColor);
    if (!pixels.empty())
    {
        texture->SetImageData(
            width,
            height,
            std::move(pixels));
    }
}

void WriteMaterialPayload(
    std::ostream& output,
    const MaterialAsset& material,
    const std::array<std::string, 5>& textureAssetIds)
{
    WriteString(output, material.GetName());
    WriteValue(output, material.GetAlbedoColor());
    WriteValue(output, material.GetSpecularColor());
    WriteValue(output, material.GetEmissiveColor());
    const std::array<float, 8> values = {
        material.GetMetallic(),
        material.GetRoughness(),
        material.GetShininess(),
        material.GetNormalScale(),
        material.GetEmissiveStrength(),
        material.GetAlphaCutoff(),
        material.GetAlphaMode(),
        material.GetOcclusionStrength()};
    WriteValue(output, values);
    // Bit 0 retains the original texture flag. Bit 1 adds double-sided state
    // without changing the payload size, so existing cooked materials remain
    // readable without a format migration.
    const std::array<std::uint8_t, 5> flags = {
        static_cast<std::uint8_t>(
            (material.GetUseAlbedoTexture() ? 1u : 0u)
            | (material.GetDoubleSided() ? 2u : 0u)),
        static_cast<std::uint8_t>(
            material.GetUseMetallicRoughnessTexture()),
        static_cast<std::uint8_t>(
            material.GetUseNormalTexture()),
        static_cast<std::uint8_t>(
            material.GetUseOcclusionTexture()),
        static_cast<std::uint8_t>(
            material.GetUseEmissiveTexture())};
    WriteValue(output, flags);
    for (const std::string& textureAssetId : textureAssetIds)
    {
        WriteString(output, textureAssetId);
    }
}

void ReadMaterialPayload(
    std::istream& input,
    CookedMaterialData& material)
{
    const std::string name = ReadString(input);
    DirectX::XMFLOAT4 albedo{};
    DirectX::XMFLOAT3 specular{};
    DirectX::XMFLOAT3 emissive{};
    std::array<float, 8> values{};
    std::array<std::uint8_t, 5> flags{};
    ReadValue(input, albedo);
    ReadValue(input, specular);
    ReadValue(input, emissive);
    ReadValue(input, values);
    ReadValue(input, flags);
    for (std::string& textureAssetId :
         material.textureAssetIds)
    {
        textureAssetId = ReadString(input);
    }
    material.material = std::make_shared<MaterialAsset>();
    material.material->SetName(name);
    material.material->SetAlbedoColor(albedo);
    material.material->SetSpecularColor(specular);
    material.material->SetEmissiveColor(emissive);
    material.material->SetMetallic(values[0]);
    material.material->SetRoughness(values[1]);
    material.material->SetShininess(values[2]);
    material.material->SetNormalScale(values[3]);
    material.material->SetEmissiveStrength(values[4]);
    material.material->SetAlphaCutoff(values[5]);
    material.material->SetAlphaMode(values[6]);
    material.material->SetDoubleSided(
        (flags[0] & 2u) != 0u);
    material.material->SetOcclusionStrength(values[7]);
    material.material->SetUseAlbedoTexture(
        (flags[0] & 1u) != 0u);
    material.material->SetUseMetallicRoughnessTexture(
        flags[1] != 0);
    material.material->SetUseNormalTexture(flags[2] != 0);
    material.material->SetUseOcclusionTexture(
        flags[3] != 0);
    material.material->SetUseEmissiveTexture(
        flags[4] != 0);
}
} // namespace

bool CookedAssetIO::IsSupportedVersion(
    const std::uint32_t version)
{
    return version == LegacyVersion
        || version == CurrentVersion;
}

std::string CookedAssetIO::ToString(
    const CookedAssetCompression compression)
{
    switch (compression)
    {
    case CookedAssetCompression::None: return "none";
    case CookedAssetCompression::RunLength: return "rle";
    }
    return "unknown";
}

CookedAssetInfo CookedAssetIO::Inspect(
    const std::filesystem::path& path)
{
    CookedAssetInfo info{};
    try
    {
        std::ifstream input(path, std::ios::binary);
        if (!input)
        {
            throw std::runtime_error(
                "Could not open the Cooked Asset.");
        }
        CookedHeaderV1 prefix{};
        ReadValue(input, prefix);
        input.seekg(0);
        const CookedHeaderV1 expected{};
        if (prefix.magic != expected.magic)
        {
            throw std::runtime_error(
                "The Cooked Asset magic is invalid.");
        }
        const PayloadReadResult result =
            ReadPayload(input, prefix.kind, true);
        info = result.info;
    }
    catch (const std::exception& exception)
    {
        info.valid = false;
        info.errorMessage = exception.what();
    }
    return info;
}

std::filesystem::path CookedAssetIO::MakePath(
    const std::filesystem::path& projectRoot,
    const std::string_view contentHash,
    const std::string_view assetId,
    const std::string_view extension)
{
    return projectRoot
        / "automation/cache/cooked"
        / std::string(contentHash)
        / (std::string(assetId) + std::string(extension));
}

bool CookedAssetIO::WriteMesh(
    const std::filesystem::path& path,
    const MeshAsset& mesh,
    std::string* outError)
{
    return WriteMeshWithOptions(
        path,
        mesh,
        {},
        outError);
}

bool CookedAssetIO::WriteMeshWithOptions(
    const std::filesystem::path& path,
    const MeshAsset& mesh,
    const CookedAssetWriteOptions& options,
    std::string* outError)
{
    return WriteFile(
        path,
        CookedKind::Mesh,
        options,
        [&](std::ostream& output)
        {
            WriteMeshPayload(output, mesh);
        },
        outError);
}

bool CookedAssetIO::ReadMesh(
    const std::filesystem::path& path,
    std::shared_ptr<MeshAsset>& mesh,
    std::string* outError)
{
    return ReadFile(
        path,
        CookedKind::Mesh,
        [&](std::istream& input)
        {
            ReadMeshPayload(input, mesh);
        },
        outError);
}

bool CookedAssetIO::WriteTexture(
    const std::filesystem::path& path,
    const TextureAsset& texture,
    std::string* outError)
{
    return WriteTextureWithOptions(
        path,
        texture,
        {},
        outError);
}

bool CookedAssetIO::WriteTextureWithOptions(
    const std::filesystem::path& path,
    const TextureAsset& texture,
    const CookedAssetWriteOptions& options,
    std::string* outError)
{
    return WriteFile(
        path,
        CookedKind::Texture,
        options,
        [&](std::ostream& output)
        {
            WriteTexturePayload(output, texture);
        },
        outError);
}

bool CookedAssetIO::ReadTexture(
    const std::filesystem::path& path,
    std::shared_ptr<TextureAsset>& texture,
    std::string* outError)
{
    return ReadFile(
        path,
        CookedKind::Texture,
        [&](std::istream& input)
        {
            ReadTexturePayload(input, texture);
        },
        outError);
}

bool CookedAssetIO::WriteMaterial(
    const std::filesystem::path& path,
    const MaterialAsset& material,
    const std::array<std::string, 5>& textureAssetIds,
    std::string* outError)
{
    return WriteMaterialWithOptions(
        path,
        material,
        textureAssetIds,
        {},
        outError);
}

bool CookedAssetIO::WriteMaterialWithOptions(
    const std::filesystem::path& path,
    const MaterialAsset& material,
    const std::array<std::string, 5>& textureAssetIds,
    const CookedAssetWriteOptions& options,
    std::string* outError)
{
    return WriteFile(
        path,
        CookedKind::Material,
        options,
        [&](std::ostream& output)
        {
            WriteMaterialPayload(
                output,
                material,
                textureAssetIds);
        },
        outError);
}

bool CookedAssetIO::ReadMaterial(
    const std::filesystem::path& path,
    CookedMaterialData& material,
    std::string* outError)
{
    return ReadFile(
        path,
        CookedKind::Material,
        [&](std::istream& input)
        {
            ReadMaterialPayload(input, material);
        },
        outError);
}
} // namespace Prism::Asset
