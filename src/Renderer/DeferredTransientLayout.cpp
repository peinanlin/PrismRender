#include "Renderer/DeferredTransientLayout.h"

#include <algorithm>

namespace Prism::Renderer
{
const std::array<
    RHI::Format,
    DeferredGBufferCount>&
GetDeferredGBufferFormats()
{
    static constexpr std::array Formats = {
        RHI::Format::Rgba16Float,
        RHI::Format::Rgba16Float,
        RHI::Format::Rgba8Unorm,
        RHI::Format::Rgba16Float};
    return Formats;
}

std::vector<RHI::TransientTextureRequest>
BuildDeferredTransientTextureRequests(
    const std::uint32_t width,
    const std::uint32_t height)
{
    RHI::TextureDescription fullResolution{};
    fullResolution.width = width;
    fullResolution.height = height;
    fullResolution.format = RHI::Format::Rgba16Float;
    fullResolution.usage =
        RHI::TextureUsage::RenderTarget
        | RHI::TextureUsage::ShaderResource
        | RHI::TextureUsage::UnorderedAccess;

    const auto& gbufferFormats =
        GetDeferredGBufferFormats();
    std::array<RHI::TextureDescription, DeferredGBufferCount>
        gbufferDescriptions{};
    for (std::size_t index = 0;
         index < gbufferDescriptions.size();
         ++index)
    {
        gbufferDescriptions[index] = fullResolution;
        gbufferDescriptions[index].format =
            gbufferFormats[index];
    }

    RHI::TextureDescription bloom = fullResolution;
    bloom.width = std::max(1u, (width + 1u) / 2u);
    bloom.height = std::max(1u, (height + 1u) / 2u);

    // Bloom uses half resolution to reduce bandwidth. The current transient
    // allocator only aliases identical descriptions, so these targets own
    // separate slots instead of incorrectly sharing full-resolution GBuffer
    // allocations.
    return {
        {"GBuffer0", gbufferDescriptions[0], 0},
        {"GBuffer1", gbufferDescriptions[1], 1},
        {"GBuffer2", gbufferDescriptions[2], 2},
        {"GBuffer3", gbufferDescriptions[3], 3},
        {"HdrColor", fullResolution, 4},
        {"BloomA", bloom, 5},
        {"BloomB", bloom, 6},
    };
}

std::uint32_t CalculateHiZMipCount(
    std::uint32_t width,
    std::uint32_t height)
{
    std::uint32_t mipCount = 1;
    std::uint32_t dimension =
        std::max(width, height);
    while (dimension > 1)
    {
        dimension = std::max(1u, dimension / 2u);
        ++mipCount;
    }
    return mipCount;
}
} // namespace Prism::Renderer
