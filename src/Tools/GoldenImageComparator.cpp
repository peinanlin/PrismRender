#include "Tools/GoldenImageComparator.h"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <limits>

namespace Prism::Tools
{
namespace
{
#pragma pack(push, 1)
struct BitmapFileHeader
{
    std::uint16_t type = 0;
    std::uint32_t fileSize = 0;
    std::uint16_t reserved0 = 0;
    std::uint16_t reserved1 = 0;
    std::uint32_t pixelOffset = 0;
};

struct BitmapInfoHeader
{
    std::uint32_t headerSize = 0;
    std::int32_t width = 0;
    std::int32_t height = 0;
    std::uint16_t planes = 0;
    std::uint16_t bitsPerPixel = 0;
    std::uint32_t compression = 0;
    std::uint32_t imageSize = 0;
    std::int32_t xPixelsPerMeter = 0;
    std::int32_t yPixelsPerMeter = 0;
    std::uint32_t colorsUsed = 0;
    std::uint32_t importantColors = 0;
};
#pragma pack(pop)

bool Fail(std::string* outError, const char* message)
{
    if (outError != nullptr)
    {
        *outError = message;
    }
    return false;
}
} // namespace

bool LoadBitmapRgba8(
    const std::filesystem::path& path,
    ImageRgba8& outImage,
    std::string* outError)
{
    std::ifstream input(path, std::ios::binary);
    if (!input)
    {
        return Fail(outError, "Failed to open the bitmap.");
    }

    BitmapFileHeader fileHeader{};
    BitmapInfoHeader infoHeader{};
    input.read(reinterpret_cast<char*>(&fileHeader), sizeof(fileHeader));
    input.read(reinterpret_cast<char*>(&infoHeader), sizeof(infoHeader));
    if (!input || fileHeader.type != 0x4D42 || infoHeader.headerSize < sizeof(BitmapInfoHeader))
    {
        return Fail(outError, "The file is not a supported Windows bitmap.");
    }
    if (infoHeader.width <= 0 || infoHeader.height == 0 || infoHeader.planes != 1
        || (infoHeader.bitsPerPixel != 24 && infoHeader.bitsPerPixel != 32)
        || infoHeader.compression != 0)
    {
        return Fail(outError, "Only uncompressed 24-bit and 32-bit bitmaps are supported.");
    }

    const std::uint32_t width = static_cast<std::uint32_t>(infoHeader.width);
    const std::uint32_t height = static_cast<std::uint32_t>(
        infoHeader.height < 0 ? -static_cast<std::int64_t>(infoHeader.height) : infoHeader.height);
    const std::uint32_t bytesPerPixel = infoHeader.bitsPerPixel / 8u;
    const std::size_t rowPitch =
        (static_cast<std::size_t>(width) * bytesPerPixel + 3u) & ~std::size_t(3u);
    if (width > std::numeric_limits<std::size_t>::max() / 4u / height)
    {
        return Fail(outError, "Bitmap dimensions exceed addressable memory.");
    }

    std::vector<std::uint8_t> sourceRow(rowPitch);
    outImage = {};
    outImage.width = width;
    outImage.height = height;
    outImage.pixels.resize(static_cast<std::size_t>(width) * height * 4u);
    input.seekg(static_cast<std::streamoff>(fileHeader.pixelOffset), std::ios::beg);
    const bool topDown = infoHeader.height < 0;
    for (std::uint32_t sourceY = 0; sourceY < height; ++sourceY)
    {
        input.read(reinterpret_cast<char*>(sourceRow.data()), static_cast<std::streamsize>(rowPitch));
        if (!input)
        {
            outImage = {};
            return Fail(outError, "Bitmap pixel data is truncated.");
        }
        const std::uint32_t destinationY = topDown ? sourceY : height - sourceY - 1u;
        std::uint8_t* destination = outImage.pixels.data()
            + static_cast<std::size_t>(destinationY) * width * 4u;
        for (std::uint32_t x = 0; x < width; ++x)
        {
            destination[x * 4u + 0u] = sourceRow[x * bytesPerPixel + 2u];
            destination[x * 4u + 1u] = sourceRow[x * bytesPerPixel + 1u];
            destination[x * 4u + 2u] = sourceRow[x * bytesPerPixel + 0u];
            destination[x * 4u + 3u] = bytesPerPixel == 4u
                                                ? sourceRow[x * bytesPerPixel + 3u]
                                                : 255u;
        }
    }
    return true;
}

GoldenImageMetrics CompareGoldenImages(
    const ImageRgba8& reference,
    const ImageRgba8& candidate,
    const std::uint8_t changedPixelTolerance)
{
    GoldenImageMetrics metrics{};
    metrics.dimensionsMatch = reference.width == candidate.width
        && reference.height == candidate.height
        && reference.pixels.size() == candidate.pixels.size()
        && reference.pixels.size() == static_cast<std::size_t>(reference.width) * reference.height * 4u;
    if (!metrics.dimensionsMatch || reference.pixels.empty())
    {
        return metrics;
    }

    double absoluteError = 0.0;
    double squareError = 0.0;
    std::size_t changedPixels = 0;
    const std::size_t pixelCount = static_cast<std::size_t>(reference.width) * reference.height;
    for (std::size_t pixel = 0; pixel < pixelCount; ++pixel)
    {
        bool changed = false;
        for (std::size_t channel = 0; channel < 3; ++channel)
        {
            const int difference = std::abs(
                static_cast<int>(reference.pixels[pixel * 4u + channel])
                - static_cast<int>(candidate.pixels[pixel * 4u + channel]));
            const double normalized = static_cast<double>(difference) / 255.0;
            absoluteError += normalized;
            squareError += normalized * normalized;
            metrics.maximumChannelError = std::max(metrics.maximumChannelError, normalized);
            changed = changed || difference > changedPixelTolerance;
        }
        changedPixels += changed ? 1u : 0u;
    }

    const double channelCount = static_cast<double>(pixelCount * 3u);
    metrics.meanAbsoluteError = absoluteError / channelCount;
    metrics.rootMeanSquareError = std::sqrt(squareError / channelCount);
    metrics.changedPixelRatio = static_cast<double>(changedPixels) / static_cast<double>(pixelCount);

    constexpr std::uint32_t WindowSize = 8u;
    constexpr double C1 = 0.01 * 0.01;
    constexpr double C2 = 0.03 * 0.03;
    const auto luminance =
        [](const ImageRgba8& image,
           const std::uint32_t x,
           const std::uint32_t y)
        {
            const std::size_t offset =
                (static_cast<std::size_t>(y)
                     * image.width
                 + x)
                * 4u;
            return (
                0.2126 * image.pixels[offset]
                + 0.7152 * image.pixels[offset + 1u]
                + 0.0722 * image.pixels[offset + 2u])
                / 255.0;
        };
    double totalSsim = 0.0;
    std::size_t windowCount = 0;
    for (std::uint32_t originY = 0;
         originY < reference.height;
         originY += WindowSize)
    {
        for (std::uint32_t originX = 0;
             originX < reference.width;
             originX += WindowSize)
        {
            const std::uint32_t endX =
                std::min(reference.width, originX + WindowSize);
            const std::uint32_t endY =
                std::min(reference.height, originY + WindowSize);
            const double count = static_cast<double>(
                (endX - originX) * (endY - originY));
            double referenceMean = 0.0;
            double candidateMean = 0.0;
            for (std::uint32_t y = originY; y < endY; ++y)
            {
                for (std::uint32_t x = originX; x < endX; ++x)
                {
                    referenceMean += luminance(reference, x, y);
                    candidateMean += luminance(candidate, x, y);
                }
            }
            referenceMean /= count;
            candidateMean /= count;
            double referenceVariance = 0.0;
            double candidateVariance = 0.0;
            double covariance = 0.0;
            for (std::uint32_t y = originY; y < endY; ++y)
            {
                for (std::uint32_t x = originX; x < endX; ++x)
                {
                    const double referenceDelta =
                        luminance(reference, x, y)
                        - referenceMean;
                    const double candidateDelta =
                        luminance(candidate, x, y)
                        - candidateMean;
                    referenceVariance +=
                        referenceDelta * referenceDelta;
                    candidateVariance +=
                        candidateDelta * candidateDelta;
                    covariance +=
                        referenceDelta * candidateDelta;
                }
            }
            referenceVariance /= count;
            candidateVariance /= count;
            covariance /= count;
            const double numerator =
                (2.0 * referenceMean * candidateMean + C1)
                * (2.0 * covariance + C2);
            const double denominator =
                (referenceMean * referenceMean
                     + candidateMean * candidateMean + C1)
                * (referenceVariance
                     + candidateVariance + C2);
            totalSsim += denominator > 0.0
                ? std::clamp(numerator / denominator, 0.0, 1.0)
                : 1.0;
            ++windowCount;
        }
    }
    metrics.structuralSimilarity =
        windowCount > 0u
        ? totalSsim / static_cast<double>(windowCount)
        : 0.0;
    return metrics;
}

ImageVisibilityMetrics AnalyzeImageVisibility(
    const ImageRgba8& image,
    const std::uint8_t blackPixelTolerance)
{
    ImageVisibilityMetrics metrics{};
    const std::size_t pixelCount =
        static_cast<std::size_t>(image.width) * image.height;
    metrics.valid = image.width > 0u
        && image.height > 0u
        && image.pixels.size() == pixelCount * 4u;
    if (!metrics.valid)
    {
        return metrics;
    }

    double luminanceSum = 0.0;
    double squaredLuminanceSum = 0.0;
    std::size_t visiblePixelCount = 0u;
    for (std::size_t pixel = 0; pixel < pixelCount; ++pixel)
    {
        const std::size_t offset = pixel * 4u;
        const double luminance = (
            0.2126 * image.pixels[offset]
            + 0.7152 * image.pixels[offset + 1u]
            + 0.0722 * image.pixels[offset + 2u])
            / 255.0;
        luminanceSum += luminance;
        squaredLuminanceSum += luminance * luminance;
        visiblePixelCount +=
            image.pixels[offset] > blackPixelTolerance
                || image.pixels[offset + 1u] > blackPixelTolerance
                || image.pixels[offset + 2u] > blackPixelTolerance
            ? 1u
            : 0u;
    }
    metrics.meanLuminance =
        luminanceSum / static_cast<double>(pixelCount);
    const double variance = std::max(
        0.0,
        squaredLuminanceSum / static_cast<double>(pixelCount)
            - metrics.meanLuminance * metrics.meanLuminance);
    metrics.luminanceStandardDeviation = std::sqrt(variance);
    metrics.visiblePixelRatio =
        static_cast<double>(visiblePixelCount)
        / static_cast<double>(pixelCount);
    return metrics;
}
} // namespace Prism::Tools
