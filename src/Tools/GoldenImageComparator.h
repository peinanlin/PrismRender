#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace Prism::Tools
{
struct ImageRgba8
{
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::vector<std::uint8_t> pixels;
};

struct GoldenImageMetrics
{
    bool dimensionsMatch = false;
    double meanAbsoluteError = 0.0;
    double rootMeanSquareError = 0.0;
    double changedPixelRatio = 0.0;
    double maximumChannelError = 0.0;
    double structuralSimilarity = 0.0;
};

struct ImageVisibilityMetrics
{
    bool valid = false;
    double meanLuminance = 0.0;
    double luminanceStandardDeviation = 0.0;
    double visiblePixelRatio = 0.0;
};

bool LoadBitmapRgba8(
    const std::filesystem::path& path,
    ImageRgba8& outImage,
    std::string* outError = nullptr);

GoldenImageMetrics CompareGoldenImages(
    const ImageRgba8& reference,
    const ImageRgba8& candidate,
    std::uint8_t changedPixelTolerance = 8);

ImageVisibilityMetrics AnalyzeImageVisibility(
    const ImageRgba8& image,
    std::uint8_t blackPixelTolerance = 5);
} // namespace Prism::Tools
