#include "Renderer/Features/Ocean/WaterOpticsTransientLayout.h"

#include <algorithm>
#include <cmath>

namespace Prism::Renderer
{
namespace
{
RHI::TextureDescription MakeColorDescription(
    const std::uint32_t width,
    const std::uint32_t height,
    const RHI::Format format,
    const RHI::TextureUsage usage)
{
    RHI::TextureDescription description{};
    description.width = std::max(width, 1u);
    description.height = std::max(height, 1u);
    description.format = format;
    description.usage = usage;
    return description;
}

std::uint32_t ScaledExtent(const std::uint32_t extent, float scale)
{
    if (!std::isfinite(scale))
        scale = 0.5f;
    scale = std::clamp(scale, 0.25f, 1.0f);
    return std::max(1u, static_cast<std::uint32_t>(
        std::ceil(static_cast<float>(std::max(extent, 1u)) * scale)));
}
} // namespace

std::vector<RHI::TransientTextureRequest>
BuildWaterOpticsTransientTextureRequests(
    const WaterOpticsTransientLayoutConfig& config)
{
    constexpr RHI::TextureUsage VisibilityUsage =
        RHI::TextureUsage::RenderTarget
        | RHI::TextureUsage::ShaderResource;
    constexpr RHI::TextureUsage ComputeUsage =
        RHI::TextureUsage::ShaderResource
        | RHI::TextureUsage::UnorderedAccess;

    const std::uint32_t width = std::max(config.width, 1u);
    const std::uint32_t height = std::max(config.height, 1u);
    const std::uint32_t refractionWidth =
        ScaledExtent(width, config.refractionResolutionScale);
    const std::uint32_t refractionHeight =
        ScaledExtent(height, config.refractionResolutionScale);
    const std::uint32_t volumeWidth =
        ScaledExtent(width, config.volumetricResolutionScale);
    const std::uint32_t volumeHeight =
        ScaledExtent(height, config.volumetricResolutionScale);
    const std::uint32_t causticResolution =
        std::clamp(config.causticResolution, 64u, 2048u);

    RHI::TextureDescription compositeDepth{};
    compositeDepth.width = width;
    compositeDepth.height = height;
    compositeDepth.format = RHI::Format::D32Float;
    compositeDepth.usage = RHI::TextureUsage::DepthStencil
        | RHI::TextureUsage::ShaderResource
        | RHI::TextureUsage::CopyDestination;

    return {
        {"WaterGBuffer0", MakeColorDescription(width, height,
            RHI::Format::Rgba16Float, VisibilityUsage), 0u},
        {"WaterGBuffer1", MakeColorDescription(width, height,
            RHI::Format::Rgba8Unorm, VisibilityUsage), 1u},
        {"WaterGBuffer2", MakeColorDescription(width, height,
            RHI::Format::Rgba8Unorm, VisibilityUsage), 2u},
        {"WaterMotion", MakeColorDescription(width, height,
            RHI::Format::Rg16Float, VisibilityUsage), 3u},
        {"WaterCompositeDepth", compositeDepth, 4u},
        {"WaterRefraction", MakeColorDescription(
            refractionWidth, refractionHeight,
            RHI::Format::Rgba16Float, ComputeUsage), 5u},
        {"WaterCompositeScratch", MakeColorDescription(width, height,
            RHI::Format::Rgba16Float, ComputeUsage), 6u},
        {"WaterCausticsNear", MakeColorDescription(
            causticResolution, causticResolution,
            RHI::Format::Rgba16Float, ComputeUsage), 7u},
        {"WaterCausticsMiddle", MakeColorDescription(
            causticResolution, causticResolution,
            RHI::Format::Rgba16Float, ComputeUsage), 8u},
        {"WaterVolumetricCurrent", MakeColorDescription(
            volumeWidth, volumeHeight,
            RHI::Format::Rgba16Float, ComputeUsage), 9u},
        {"WaterVolumetricHistoryA", MakeColorDescription(
            volumeWidth, volumeHeight,
            RHI::Format::Rgba16Float, ComputeUsage), 10u},
        {"WaterVolumetricHistoryB", MakeColorDescription(
            volumeWidth, volumeHeight,
            RHI::Format::Rgba16Float, ComputeUsage), 11u},
        {"WaterVolumetricReconstruction", MakeColorDescription(
            width, height, RHI::Format::Rgba16Float, ComputeUsage), 12u},
    };
}

bool AreWaterOpticsTexturesAliasCompatible(
    const RHI::TextureDescription& left,
    const RHI::TextureDescription& right) noexcept
{
    return left.dimension == right.dimension
        && left.width == right.width
        && left.height == right.height
        && left.arrayLayers == right.arrayLayers
        && left.mipLevels == right.mipLevels
        && left.sampleCount == right.sampleCount
        && left.format == right.format
        && left.usage == right.usage
        && left.memoryAccess == right.memoryAccess;
}
} // namespace Prism::Renderer
