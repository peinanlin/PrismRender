#include "Renderer/GpuTimingReport.h"

#include "RHI/GraphicsApi.h"

#include <algorithm>
#include <fstream>
#include <stdexcept>
#include <vector>

namespace Prism::Renderer
{
namespace
{
struct Interval
{
    double begin = 0.0;
    double end = 0.0;
};

std::string_view ToString(const RHI::CommandQueueType queue)
{
    return queue == RHI::CommandQueueType::Compute
        ? "Compute"
        : "Graphics";
}

std::vector<Interval> MergeIntervals(
    std::vector<Interval> intervals)
{
    std::ranges::sort(
        intervals,
        {},
        &Interval::begin);
    std::vector<Interval> merged;
    for (const Interval interval : intervals)
    {
        if (interval.end < interval.begin)
        {
            continue;
        }
        if (merged.empty()
            || interval.begin > merged.back().end)
        {
            merged.push_back(interval);
            continue;
        }
        merged.back().end =
            std::max(merged.back().end, interval.end);
    }
    return merged;
}

double SumIntervals(const std::vector<Interval>& intervals)
{
    double result = 0.0;
    for (const Interval interval : intervals)
    {
        result += std::max(0.0, interval.end - interval.begin);
    }
    return result;
}

double IntersectIntervals(
    const std::vector<Interval>& lhs,
    const std::vector<Interval>& rhs)
{
    double result = 0.0;
    std::size_t lhsIndex = 0;
    std::size_t rhsIndex = 0;
    while (lhsIndex < lhs.size()
           && rhsIndex < rhs.size())
    {
        result += std::max(
            0.0,
            std::min(lhs[lhsIndex].end, rhs[rhsIndex].end)
                - std::max(
                    lhs[lhsIndex].begin,
                    rhs[rhsIndex].begin));
        if (lhs[lhsIndex].end < rhs[rhsIndex].end)
        {
            ++lhsIndex;
        }
        else
        {
            ++rhsIndex;
        }
    }
    return result;
}
} // namespace

nlohmann::json BuildGpuTimingReport(
    const std::span<const RHI::GpuProfiler::Timing> timings,
    const RHI::GraphicsApi graphicsApi,
    const RHI::GpuProfiler::TimelineMetadata& timelineMetadata,
    const nlohmann::json& captureMetadata)
{
    nlohmann::json passes = nlohmann::json::array();
    std::vector<Interval> graphicsIntervals;
    std::vector<Interval> computeIntervals;
    double frameGpuMilliseconds = 0.0;
    bool timelineAvailable = false;
    for (const RHI::GpuProfiler::Timing& timing : timings)
    {
        passes.push_back({
            {"name", timing.name},
            {"gpuMilliseconds", timing.milliseconds},
            {"queue", std::string(ToString(timing.queue))},
            {"startMilliseconds", timing.startMilliseconds},
            {"endMilliseconds", timing.endMilliseconds},
            {"calibrated", timing.calibrated}});
        if (timing.name == "Renderer")
        {
            frameGpuMilliseconds =
                timing.milliseconds;
            continue;
        }
        if (!timing.calibrated
            || timing.endMilliseconds < timing.startMilliseconds)
        {
            continue;
        }
        timelineAvailable = true;
        auto& intervals =
            timing.queue == RHI::CommandQueueType::Compute
                ? computeIntervals
                : graphicsIntervals;
        intervals.push_back({
            timing.startMilliseconds,
            timing.endMilliseconds});
    }
    graphicsIntervals =
        MergeIntervals(std::move(graphicsIntervals));
    computeIntervals =
        MergeIntervals(std::move(computeIntervals));
    const double graphicsBusyMilliseconds =
        SumIntervals(graphicsIntervals);
    const double computeBusyMilliseconds =
        SumIntervals(computeIntervals);
    const double overlapMilliseconds =
        IntersectIntervals(
            graphicsIntervals,
            computeIntervals);
    const double overlapRatio =
        frameGpuMilliseconds > 0.0
            ? overlapMilliseconds / frameGpuMilliseconds
            : 0.0;
    const double computeOverlapRatio =
        computeBusyMilliseconds > 0.0
            ? overlapMilliseconds / computeBusyMilliseconds
            : 0.0;

    return {
        {"format", "PrismGpuTimingReport"},
        {"version", 2},
        {"capture", captureMetadata},
        {"graphicsApi", std::string(RHI::ToString(graphicsApi))},
        {"timeline", {
            {"available", timelineAvailable},
            {"crossQueueCalibrated",
             timelineMetadata.crossQueueCalibrated},
            {"calibrationMethod",
             timelineMetadata.calibrationMethod},
            {"graphicsTimestampFrequency",
             timelineMetadata.graphicsTimestampFrequency},
            {"computeTimestampFrequency",
             timelineMetadata.computeTimestampFrequency},
            {"timestampValidBits",
             timelineMetadata.timestampValidBits},
            {"frameGpuMilliseconds",
             frameGpuMilliseconds},
            {"graphicsBusyMilliseconds",
             graphicsBusyMilliseconds},
            {"computeBusyMilliseconds",
             computeBusyMilliseconds},
            {"overlapMilliseconds",
             overlapMilliseconds},
            {"overlapRatio", overlapRatio},
            {"computeOverlapRatio",
             computeOverlapRatio}}},
        {"passes", std::move(passes)}};
}

bool WriteGpuTimingReport(
    const std::filesystem::path& path,
    const std::span<const RHI::GpuProfiler::Timing> timings,
    const RHI::GraphicsApi graphicsApi,
    std::string* outError,
    const RHI::GpuProfiler::TimelineMetadata& timelineMetadata,
    const nlohmann::json& captureMetadata)
{
    try
    {
        if (path.empty())
        {
            return true;
        }
        if (!path.parent_path().empty())
        {
            std::filesystem::create_directories(path.parent_path());
        }
        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        if (!output)
        {
            throw std::runtime_error(
                "Could not open the GPU timing report for writing.");
        }
        output << BuildGpuTimingReport(
            timings,
            graphicsApi,
            timelineMetadata, captureMetadata).dump(2) << '\n';
        if (!output)
        {
            throw std::runtime_error(
                "Failed while writing the GPU timing report.");
        }
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
} // namespace Prism::Renderer
