#include "Renderer/Features/Ocean/WaterBenchmarkReport.h"

#include "Renderer/Features/Ocean/OceanSettings.h"
#include "Renderer/Features/Ocean/OceanStatistics.h"
#include "Renderer/RenderGraph.h"

namespace Prism::Renderer
{
nlohmann::json BuildWaterBenchmarkMetadata(const OceanSettings& settings,
    const OceanStatistics& statistics, const RenderGraph& graph)
{
    const auto& optics = settings.optics;
    nlohmann::json stages = nlohmann::json::array();
    for (const auto& pass : graph.GetPassInfos())
        if (pass.name.starts_with("WaterOptics."))
            stages.push_back({{"name", pass.name}, {"cpuRecordMilliseconds", pass.cpuMilliseconds}});
    return {
        {"waterOpticsActive", statistics.waterOpticsActive},
        {"outputExtent", {statistics.outputWidth, statistics.outputHeight}},
        {"activeViews", statistics.activeRenderedViews},
        {"opticalQuality", statistics.waterOpticsQuality},
        {"spectralQuality", static_cast<std::uint32_t>(settings.quality)},
        {"refraction", {{"enabled", optics.refraction.enabled},
            {"requestedRayMarch", optics.refraction.highPrecision},
            {"effectiveRayMarch", statistics.waterRefractionRayMarchActive},
            {"effectiveSamples", statistics.waterRefractionEffectiveSamples},
            {"extent", {statistics.waterRefractionWidth, statistics.waterRefractionHeight}}}},
        {"caustics", {{"mode", static_cast<std::uint32_t>(GetEffectiveWaterCausticsMode(optics))},
            {"requestedMode", static_cast<std::uint32_t>(optics.caustics.mode)},
            {"resolution", optics.caustics.resolution},
            {"coverageMeters", {optics.caustics.nearCoverageMeters, optics.caustics.middleCoverageMeters}}}},
        {"volumetrics", {{"active", statistics.waterVolumetricsActive},
            {"extent", {statistics.waterVolumetricWidth, statistics.waterVolumetricHeight}},
            {"samples", statistics.waterVolumetricsActive ? optics.volumetrics.sampleCount : 0u},
            {"historyValid", statistics.waterVolumetricHistoryValid}}},
        {"medium", {{"underwater", statistics.waterCameraUnderwater},
            {"meanSeaLevelFallback", statistics.waterMediumFallback}, {"version", statistics.waterMediumVersion}}},
        {"history", {{"opticalVersion", statistics.waterOpticsHistoryVersion},
            {"surfaceVersion", statistics.waterOpticsSurfaceHistoryVersion},
            {"valid", statistics.waterOpticsHistoryValid}}},
        {"distanceMeters", {optics.distance.nearEndMeters, optics.distance.middleEndMeters, optics.distance.farEndMeters}},
        {"waterMemoryMiB", statistics.waterAllocatedMegabytes},
        {"waterCoverage", {{"available", statistics.waterCoverageAvailable}, {"fraction", statistics.waterPixelCoverage}}},
        {"waterDraws", statistics.waterOpticsActive ? statistics.waterVisibilityDrawCount + 1u : 0u},
        {"visibilityDraws", statistics.waterVisibilityDrawCount},
        {"waterDispatches", statistics.waterOpticsDispatchCount},
        {"cpuStages", stages},
        {"timingPolicy", "Latest resolved GPU frame after warm-up; CPU command recording, not GPU execution"}};
}
}
