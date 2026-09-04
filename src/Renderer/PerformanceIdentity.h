#pragma once

#include "RHI/GraphicsAdapterInfo.h"
#include "RHI/GraphicsApi.h"

#include <cstdint>
#include <filesystem>
#include <string>

#include <json.hpp>

namespace Prism::Renderer
{
struct RuntimePerformanceIdentity
{
    std::string graphicsApi;
    RHI::GraphicsAdapterInfo adapter;
    std::string cpuName;
    std::string buildConfiguration;
    std::string compiler;
    std::string architecture;
    std::string shaderRevision;
    std::uint32_t shaderFileCount = 0;
    std::string executableHash;
};

[[nodiscard]] RuntimePerformanceIdentity CaptureRuntimePerformanceIdentity(
    RHI::GraphicsApi graphicsApi,
    const RHI::GraphicsAdapterInfo& adapter,
    const std::filesystem::path& shaderDirectory);

[[nodiscard]] nlohmann::json SerializeRuntimePerformanceIdentity(
    const RuntimePerformanceIdentity& identity);

bool WriteRuntimePerformanceIdentity(
    const std::filesystem::path& path,
    const RuntimePerformanceIdentity& identity,
    std::string* outError = nullptr);
} // namespace Prism::Renderer
