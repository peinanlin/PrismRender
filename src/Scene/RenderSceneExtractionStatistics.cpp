#include "Scene/RenderSceneExtractionStatistics.h"

#include "Scene/RenderObject.h"
#include "Scene/RenderScene.h"

#include <fstream>
#include <stdexcept>
#include <utility>

#include <json.hpp>

namespace Prism::Scene
{
RenderSceneCopyEstimate EstimateRenderSceneCopy(
    const RenderScene& scene)
{
    RenderSceneCopyEstimate estimate{};
    estimate.fixedBytes = sizeof(RenderScene);
    const std::vector<RenderObject>& objects =
        scene.GetRenderObjects();
    estimate.objectCount = objects.size();
    estimate.objectBytes =
        objects.size() * sizeof(RenderObject);
    for (const RenderObject& object : objects)
    {
        estimate.stringBytes += object.name.size();
        estimate.stringBytes +=
            object.quadtreePatch.parentPatchName.size();
    }
    return estimate;
}

void AccumulateRenderSceneCopy(
    const RenderSceneCopyEstimate& estimate,
    const std::uint64_t copyCount,
    RenderSceneExtractionStatistics& statistics)
{
    statistics.estimatedObjectCopies +=
        estimate.objectCount * copyCount;
    statistics.estimatedBytesCopied +=
        estimate.TotalBytes() * copyCount;
}

RenderSceneExtractionStatisticsReport::
RenderSceneExtractionStatisticsReport(
    std::filesystem::path outputPath)
    : m_outputPath(std::move(outputPath))
{
}

bool RenderSceneExtractionStatisticsReport::IsEnabled() const
{
    return !m_outputPath.empty();
}

void RenderSceneExtractionStatisticsReport::AddFrame(
    RenderSceneExtractionStatistics statistics)
{
    if (IsEnabled())
    {
        m_frames.push_back(std::move(statistics));
    }
}

void RenderSceneExtractionStatisticsReport::Write(
    const char* const sceneLabel,
    const bool editorEnabled) const
{
    if (!IsEnabled()) return;

    nlohmann::json frames = nlohmann::json::array();
    RenderSceneExtractionStatistics totals{};
    for (const RenderSceneExtractionStatistics& frame : m_frames)
    {
        totals.sceneDataBuildCount += frame.sceneDataBuildCount;
        totals.sceneDataReuseCount += frame.sceneDataReuseCount;
        totals.sourceObjectVisitCount +=
            frame.sourceObjectVisitCount;
        totals.frameEnvelopeCount += frame.frameEnvelopeCount;
        totals.framePacketSmallObjectCount +=
            frame.framePacketSmallObjectCount;
        totals.publicationFullCopyCount +=
            frame.publicationFullCopyCount;
        totals.gameViewFullCopyCount +=
            frame.gameViewFullCopyCount;
        totals.sceneViewFullCopyCount +=
            frame.sceneViewFullCopyCount;
        totals.estimatedObjectCopies +=
            frame.estimatedObjectCopies;
        totals.estimatedBytesCopied +=
            frame.estimatedBytesCopied;
        totals.publicationCpuMilliseconds +=
            frame.publicationCpuMilliseconds;
        totals.gameViewBuildCpuMilliseconds +=
            frame.gameViewBuildCpuMilliseconds;
        totals.sceneViewBuildCpuMilliseconds +=
            frame.sceneViewBuildCpuMilliseconds;

        frames.push_back({
            {"logicalFrameId", frame.logicalFrameId},
            {"sceneGeneration", frame.sceneGeneration},
            {"sceneDataRevision", frame.sceneDataRevision},
            {"sourceObjectCount", frame.sourceObjectCount},
            {"sourceObjectVisitCount",
                frame.sourceObjectVisitCount},
            {"extractionReason", frame.extractionReason},
            {"conservativeFallback",
                frame.conservativeFallback},
            {"sceneDataBuildCount", frame.sceneDataBuildCount},
            {"sceneDataReuseCount", frame.sceneDataReuseCount},
            {"frameEnvelopeCount", frame.frameEnvelopeCount},
            {"framePacketSmallObjectCount",
                frame.framePacketSmallObjectCount},
            {"copies", {
                {"publication", frame.publicationFullCopyCount},
                {"gameView", frame.gameViewFullCopyCount},
                {"sceneView", frame.sceneViewFullCopyCount},
                {"estimatedObjects", frame.estimatedObjectCopies},
                {"estimatedBytes", frame.estimatedBytesCopied}}},
            {"cpuMilliseconds", {
                {"publication", frame.publicationCpuMilliseconds},
                {"gameViewBuild",
                    frame.gameViewBuildCpuMilliseconds},
                {"sceneViewBuild",
                    frame.sceneViewBuildCpuMilliseconds}}}});
    }

    const nlohmann::json output{
        {"format", "PrismRenderScenePublicationStatistics"},
        {"version", 1},
        {"model", "immutable-frame-packet"},
        {"scene", sceneLabel != nullptr ? sceneLabel : ""},
        {"editorEnabled", editorEnabled},
        {"frameCount", m_frames.size()},
        {"totals", {
            {"sceneDataBuildCount", totals.sceneDataBuildCount},
            {"sceneDataReuseCount", totals.sceneDataReuseCount},
            {"sourceObjectVisitCount",
                totals.sourceObjectVisitCount},
            {"frameEnvelopeCount", totals.frameEnvelopeCount},
            {"framePacketSmallObjectCount",
                totals.framePacketSmallObjectCount},
            {"copies", {
                {"publication", totals.publicationFullCopyCount},
                {"gameView", totals.gameViewFullCopyCount},
                {"sceneView", totals.sceneViewFullCopyCount},
                {"estimatedObjects", totals.estimatedObjectCopies},
                {"estimatedBytes", totals.estimatedBytesCopied}}},
            {"cpuMilliseconds", {
                {"publication", totals.publicationCpuMilliseconds},
                {"gameViewBuild",
                    totals.gameViewBuildCpuMilliseconds},
                {"sceneViewBuild",
                    totals.sceneViewBuildCpuMilliseconds}}}}},
        {"frames", std::move(frames)}};

    if (!m_outputPath.parent_path().empty())
    {
        std::filesystem::create_directories(
            m_outputPath.parent_path());
    }
    std::ofstream stream(
        m_outputPath,
        std::ios::binary | std::ios::trunc);
    if (!stream)
    {
        throw std::runtime_error(
            "Failed to open RenderScene publication statistics report.");
    }
    stream << output.dump(2) << '\n';
    if (!stream)
    {
        throw std::runtime_error(
            "Failed to write RenderScene publication statistics report.");
    }
}
} // namespace Prism::Scene
