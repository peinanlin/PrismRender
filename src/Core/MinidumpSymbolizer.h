#pragma once

#include <cstddef>
#include <filesystem>
#include <string>
#include <vector>

#include <json.hpp>

namespace Prism::Core
{
struct MinidumpSymbolizationOptions
{
    std::filesystem::path dumpPath;
    std::filesystem::path executablePath;
    std::filesystem::path pdbPath;
    std::vector<std::filesystem::path> symbolPaths;
    std::size_t maximumFrames = 128;
    bool requireIdentityMatch = true;
};

struct MinidumpSymbolizationResult
{
    bool success = false;
    std::string errorCode;
    std::string errorMessage;
    nlohmann::json report = nlohmann::json::object();
};

[[nodiscard]] MinidumpSymbolizationResult SymbolizeMinidump(
    const MinidumpSymbolizationOptions& options);
bool WriteMinidumpSymbolizationReport(
    const std::filesystem::path& path,
    const MinidumpSymbolizationResult& result,
    std::string* outError = nullptr);
} // namespace Prism::Core
