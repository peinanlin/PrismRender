#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include <json.hpp>

namespace Prism::Automation
{
struct PerformanceStatistics
{
    double minimumMilliseconds = 0.0;
    double maximumMilliseconds = 0.0;
    double meanMilliseconds = 0.0;
    double medianMilliseconds = 0.0;
    double p95Milliseconds = 0.0;
};

struct PerformanceIdentity
{
    std::string api;
    std::string stage;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::string worldHash;
    std::string assetManifestHash;
    std::string adapterName;
    std::uint32_t adapterVendorId = 0;
    std::uint32_t adapterDeviceId = 0;
    std::uint64_t dedicatedVideoMemoryBytes = 0;
    std::uint64_t sharedSystemMemoryBytes = 0;
    std::uint64_t driverVersionRaw = 0;
    std::string driverVersion;
    std::string apiVersion;
    std::string cpuName;
    std::string buildConfiguration;
    std::string compiler;
    std::string architecture;
    std::string shaderRevision;
    std::uint32_t shaderFileCount = 0;
    std::string executableHash;
};

struct PerformanceBaselineRecord
{
    PerformanceIdentity identity;
    PerformanceStatistics statistics;
    std::size_t sampleCount = 0;
};

struct PerformanceComparison
{
    bool identityMatches = false;
    bool passed = false;
    double maximumRegressionPercent = 0.0;
    double medianRegressionPercent = 0.0;
    double p95RegressionPercent = 0.0;
};

class PerformanceBaseline
{
public:
    static constexpr std::uint32_t CurrentVersion = 2;

    [[nodiscard]] static PerformanceStatistics ComputeStatistics(
        const std::vector<double>& samplesMilliseconds);
    [[nodiscard]] static PerformanceComparison Compare(
        const PerformanceBaselineRecord& baseline,
        const PerformanceBaselineRecord& current,
        double maximumRegressionPercent);
    [[nodiscard]] static bool Save(
        const std::filesystem::path& path,
        const PerformanceBaselineRecord& record,
        std::string* outError = nullptr);
    [[nodiscard]] static bool Load(
        const std::filesystem::path& path,
        PerformanceBaselineRecord& record,
        std::string* outError = nullptr);
    [[nodiscard]] static nlohmann::json Serialize(
        const PerformanceBaselineRecord& record);
    [[nodiscard]] static nlohmann::json Serialize(
        const PerformanceStatistics& statistics);
    [[nodiscard]] static nlohmann::json Serialize(
        const PerformanceComparison& comparison);
};
} // namespace Prism::Automation
