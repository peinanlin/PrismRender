#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace Prism::Scene
{
class RenderScene;

struct RenderSceneCopyEstimate
{
    std::uint64_t objectCount = 0;
    std::uint64_t fixedBytes = 0;
    std::uint64_t objectBytes = 0;
    std::uint64_t stringBytes = 0;

    [[nodiscard]] std::uint64_t TotalBytes() const
    {
        return fixedBytes + objectBytes + stringBytes;
    }
};

// Counts the CPU values duplicated by a RenderScene copy. Referenced GPU
// assets are shared_ptr bindings, so their allocations are intentionally not
// included in this estimate.
[[nodiscard]] RenderSceneCopyEstimate EstimateRenderSceneCopy(
    const RenderScene& scene);

struct RenderSceneExtractionStatistics
{
    std::uint64_t logicalFrameId = 0;
    std::uint64_t sceneGeneration = 0;
    std::uint64_t sceneDataRevision = 0;
    std::uint64_t sourceObjectCount = 0;
    std::uint64_t sourceObjectVisitCount = 0;
    std::string extractionReason;
    bool conservativeFallback = false;

    std::uint64_t sceneDataBuildCount = 0;
    std::uint64_t sceneDataReuseCount = 0;
    std::uint64_t frameEnvelopeCount = 0;
    // Packet allocation remains separate from full-scene copies.
    std::uint64_t framePacketSmallObjectCount = 0;

    std::uint64_t publicationFullCopyCount = 0;
    std::uint64_t gameViewFullCopyCount = 0;
    std::uint64_t sceneViewFullCopyCount = 0;
    std::uint64_t estimatedObjectCopies = 0;
    std::uint64_t estimatedBytesCopied = 0;

    double publicationCpuMilliseconds = 0.0;
    double gameViewBuildCpuMilliseconds = 0.0;
    double sceneViewBuildCpuMilliseconds = 0.0;
};

void AccumulateRenderSceneCopy(
    const RenderSceneCopyEstimate& estimate,
    std::uint64_t copyCount,
    RenderSceneExtractionStatistics& statistics);

class RenderSceneExtractionStatisticsReport
{
public:
    explicit RenderSceneExtractionStatisticsReport(
        std::filesystem::path outputPath);

    [[nodiscard]] bool IsEnabled() const;
    void AddFrame(RenderSceneExtractionStatistics statistics);
    void Write(
        const char* sceneLabel,
        bool editorEnabled) const;

private:
    std::filesystem::path m_outputPath;
    std::vector<RenderSceneExtractionStatistics> m_frames;
};
} // namespace Prism::Scene
