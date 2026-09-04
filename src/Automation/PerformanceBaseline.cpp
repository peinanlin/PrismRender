#include "Automation/PerformanceBaseline.h"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <numeric>
#include <stdexcept>

namespace Prism::Automation
{
namespace
{
using json = nlohmann::json;

double Percentile(
    const std::vector<double>& sorted,
    const double percentile)
{
    if (sorted.empty())
    {
        return 0.0;
    }
    const double position =
        percentile * static_cast<double>(sorted.size() - 1);
    const std::size_t lower = static_cast<std::size_t>(std::floor(position));
    const std::size_t upper = static_cast<std::size_t>(std::ceil(position));
    const double fraction = position - static_cast<double>(lower);
    return sorted[lower] * (1.0 - fraction) + sorted[upper] * fraction;
}

double RegressionPercent(const double baseline, const double current)
{
    if (baseline <= 0.0)
    {
        return current <= 0.0 ? 0.0 : 100.0;
    }
    return (current - baseline) * 100.0 / baseline;
}

bool SameIdentity(
    const PerformanceIdentity& left,
    const PerformanceIdentity& right)
{
    return left.api == right.api
        && left.stage == right.stage
        && left.width == right.width
        && left.height == right.height
        && left.worldHash == right.worldHash
        && left.assetManifestHash == right.assetManifestHash
        && left.adapterVendorId == right.adapterVendorId
        && left.adapterDeviceId == right.adapterDeviceId
        && left.dedicatedVideoMemoryBytes
            == right.dedicatedVideoMemoryBytes
        && left.sharedSystemMemoryBytes
            == right.sharedSystemMemoryBytes
        && left.driverVersionRaw == right.driverVersionRaw
        && left.apiVersion == right.apiVersion
        && left.cpuName == right.cpuName
        && left.buildConfiguration == right.buildConfiguration
        && left.compiler == right.compiler
        && left.architecture == right.architecture;
}
} // namespace

PerformanceStatistics PerformanceBaseline::ComputeStatistics(
    const std::vector<double>& samplesMilliseconds)
{
    if (samplesMilliseconds.empty())
    {
        throw std::invalid_argument(
            "Performance statistics require at least one sample.");
    }
    std::vector<double> sorted = samplesMilliseconds;
    std::ranges::sort(sorted);
    PerformanceStatistics statistics{};
    statistics.minimumMilliseconds = sorted.front();
    statistics.maximumMilliseconds = sorted.back();
    statistics.meanMilliseconds =
        std::accumulate(sorted.begin(), sorted.end(), 0.0)
        / static_cast<double>(sorted.size());
    statistics.medianMilliseconds = Percentile(sorted, 0.5);
    statistics.p95Milliseconds = Percentile(sorted, 0.95);
    return statistics;
}

PerformanceComparison PerformanceBaseline::Compare(
    const PerformanceBaselineRecord& baseline,
    const PerformanceBaselineRecord& current,
    const double maximumRegressionPercent)
{
    if (maximumRegressionPercent < 0.0)
    {
        throw std::invalid_argument(
            "Performance regression tolerance cannot be negative.");
    }
    PerformanceComparison comparison{};
    comparison.identityMatches =
        SameIdentity(baseline.identity, current.identity);
    comparison.maximumRegressionPercent = maximumRegressionPercent;
    comparison.medianRegressionPercent = RegressionPercent(
        baseline.statistics.medianMilliseconds,
        current.statistics.medianMilliseconds);
    comparison.p95RegressionPercent = RegressionPercent(
        baseline.statistics.p95Milliseconds,
        current.statistics.p95Milliseconds);
    comparison.passed = comparison.identityMatches
        && comparison.medianRegressionPercent <= maximumRegressionPercent
        && comparison.p95RegressionPercent <= maximumRegressionPercent;
    return comparison;
}

bool PerformanceBaseline::Save(
    const std::filesystem::path& path,
    const PerformanceBaselineRecord& record,
    std::string* outError)
{
    try
    {
        if (!path.parent_path().empty())
        {
            std::filesystem::create_directories(path.parent_path());
        }
        const std::filesystem::path temporary =
            path.parent_path() / (path.filename().string() + ".tmp");
        {
            std::ofstream output(
                temporary,
                std::ios::binary | std::ios::trunc);
            if (!output)
            {
                throw std::runtime_error(
                    "Could not create the performance baseline.");
            }
            output << std::setw(2) << Serialize(record) << '\n';
            if (!output)
            {
                throw std::runtime_error(
                    "Could not write the performance baseline.");
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
        if (outError != nullptr)
        {
            *outError = exception.what();
        }
        return false;
    }
}

bool PerformanceBaseline::Load(
    const std::filesystem::path& path,
    PerformanceBaselineRecord& record,
    std::string* outError)
{
    try
    {
        std::ifstream input(path, std::ios::binary);
        if (!input)
        {
            throw std::runtime_error(
                "Could not open the performance baseline.");
        }
        json document;
        input >> document;
        const std::uint32_t version = document.value("version", 0u);
        if (document.value("format", std::string{})
                != "PrismPerformanceBaseline"
            || (version != 1u && version != CurrentVersion))
        {
            throw std::runtime_error(
                "The performance baseline format is invalid.");
        }
        const json& identity = document.at("identity");
        record.identity.api = identity.at("api").get<std::string>();
        record.identity.stage = identity.at("stage").get<std::string>();
        record.identity.width = identity.at("width").get<std::uint32_t>();
        record.identity.height = identity.at("height").get<std::uint32_t>();
        record.identity.worldHash =
            identity.value("worldHash", std::string{});
        record.identity.assetManifestHash =
            identity.value("assetManifestHash", std::string{});
        record.identity.adapterName =
            identity.value("adapterName", std::string{});
        record.identity.adapterVendorId =
            identity.value("adapterVendorId", 0u);
        record.identity.adapterDeviceId =
            identity.value("adapterDeviceId", 0u);
        record.identity.dedicatedVideoMemoryBytes =
            identity.value("dedicatedVideoMemoryBytes", 0ull);
        record.identity.sharedSystemMemoryBytes =
            identity.value("sharedSystemMemoryBytes", 0ull);
        record.identity.driverVersionRaw =
            identity.value("driverVersionRaw", 0ull);
        record.identity.driverVersion =
            identity.value("driverVersion", std::string{});
        record.identity.apiVersion =
            identity.value("apiVersion", std::string{});
        record.identity.cpuName =
            identity.value("cpuName", std::string{});
        record.identity.buildConfiguration =
            identity.value("buildConfiguration", std::string{});
        record.identity.compiler =
            identity.value("compiler", std::string{});
        record.identity.architecture =
            identity.value("architecture", std::string{});
        record.identity.shaderRevision =
            identity.value("shaderRevision", std::string{});
        record.identity.shaderFileCount =
            identity.value("shaderFileCount", 0u);
        record.identity.executableHash =
            identity.value("executableHash", std::string{});
        const json& statistics = document.at("statistics");
        record.statistics.minimumMilliseconds =
            statistics.at("minimumMilliseconds").get<double>();
        record.statistics.maximumMilliseconds =
            statistics.at("maximumMilliseconds").get<double>();
        record.statistics.meanMilliseconds =
            statistics.at("meanMilliseconds").get<double>();
        record.statistics.medianMilliseconds =
            statistics.at("medianMilliseconds").get<double>();
        record.statistics.p95Milliseconds =
            statistics.at("p95Milliseconds").get<double>();
        record.sampleCount = document.at("sampleCount").get<std::size_t>();
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

nlohmann::json PerformanceBaseline::Serialize(
    const PerformanceBaselineRecord& record)
{
    return {
        {"format", "PrismPerformanceBaseline"},
        {"version", CurrentVersion},
        {"metric", "rendererProcessWallMilliseconds"},
        {"identity", {
            {"api", record.identity.api},
            {"stage", record.identity.stage},
            {"width", record.identity.width},
            {"height", record.identity.height},
            {"worldHash", record.identity.worldHash},
            {"assetManifestHash", record.identity.assetManifestHash},
            {"adapterName", record.identity.adapterName},
            {"adapterVendorId", record.identity.adapterVendorId},
            {"adapterDeviceId", record.identity.adapterDeviceId},
            {"dedicatedVideoMemoryBytes",
             record.identity.dedicatedVideoMemoryBytes},
            {"sharedSystemMemoryBytes",
             record.identity.sharedSystemMemoryBytes},
            {"driverVersionRaw", record.identity.driverVersionRaw},
            {"driverVersion", record.identity.driverVersion},
            {"apiVersion", record.identity.apiVersion},
            {"cpuName", record.identity.cpuName},
            {"buildConfiguration", record.identity.buildConfiguration},
            {"compiler", record.identity.compiler},
            {"architecture", record.identity.architecture},
            {"shaderRevision", record.identity.shaderRevision},
            {"shaderFileCount", record.identity.shaderFileCount},
            {"executableHash", record.identity.executableHash}}},
        {"sampleCount", record.sampleCount},
        {"statistics", Serialize(record.statistics)}};
}

nlohmann::json PerformanceBaseline::Serialize(
    const PerformanceStatistics& statistics)
{
    return {
        {"minimumMilliseconds", statistics.minimumMilliseconds},
        {"maximumMilliseconds", statistics.maximumMilliseconds},
        {"meanMilliseconds", statistics.meanMilliseconds},
        {"medianMilliseconds", statistics.medianMilliseconds},
        {"p95Milliseconds", statistics.p95Milliseconds}};
}

nlohmann::json PerformanceBaseline::Serialize(
    const PerformanceComparison& comparison)
{
    return {
        {"identityMatches", comparison.identityMatches},
        {"passed", comparison.passed},
        {"maximumRegressionPercent", comparison.maximumRegressionPercent},
        {"medianRegressionPercent", comparison.medianRegressionPercent},
        {"p95RegressionPercent", comparison.p95RegressionPercent}};
}
} // namespace Prism::Automation
