#pragma once

#include <cstdint>
#include <filesystem>
#include <string>

#include <json.hpp>

namespace Prism::Core
{
struct CodeViewIdentity
{
    bool available = false;
    std::string guid;
    std::uint32_t age = 0;
    std::uint32_t signature = 0;
    std::uint32_t timestamp = 0;
    std::uint32_t imageSize = 0;
    std::string pdbFile;
    std::string errorMessage;
};

struct BuildSymbolIdentity
{
    bool valid = false;
    std::string buildId;
    std::filesystem::path executablePath;
    std::uintmax_t executableBytes = 0;
    std::string executableHash;
    CodeViewIdentity executableCodeView;
    std::filesystem::path pdbPath;
    std::uintmax_t pdbBytes = 0;
    std::string pdbHash;
    CodeViewIdentity pdbCodeView;
    bool pdbMatchesExecutable = false;
    std::string errorMessage;
};

[[nodiscard]] std::string HashFileFNV1a64(
    const std::filesystem::path& path);
[[nodiscard]] std::string FormatGuid(
    const void* guidBytes);
[[nodiscard]] BuildSymbolIdentity CaptureBuildSymbolIdentity(
    const std::filesystem::path& executablePath = {},
    const std::filesystem::path& pdbPath = {});
[[nodiscard]] nlohmann::json SerializeBuildSymbolIdentity(
    const BuildSymbolIdentity& identity);
bool WriteBuildSymbolIdentity(
    const std::filesystem::path& path,
    const BuildSymbolIdentity& identity,
    std::string* outError = nullptr);
} // namespace Prism::Core
