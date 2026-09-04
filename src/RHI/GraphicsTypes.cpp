#include "RHI/GraphicsTypes.h"

namespace Prism::RHI
{
bool IsDepthFormat(const Format format)
{
    return format == Format::D32Float;
}

bool ValidateTextureDescription(const TextureDescription& description, std::string* outError)
{
    auto fail = [&](const char* message)
    {
        if (outError != nullptr)
        {
            *outError = message;
        }
        return false;
    };

    if (description.width == 0 || description.height == 0 || description.arrayLayers == 0
        || description.mipLevels == 0 || description.sampleCount == 0)
    {
        return fail("Texture dimensions, layers, mip levels, and sample count must be non-zero.");
    }
    if (description.format == Format::Unknown)
    {
        return fail("Texture format must be specified.");
    }
    if (description.dimension == TextureDimension::TextureCube && description.arrayLayers % 6u != 0u)
    {
        return fail("Cube texture array layers must be a multiple of six.");
    }
    if (IsDepthFormat(description.format) && !HasAnyFlag(description.usage, TextureUsage::DepthStencil))
    {
        return fail("Depth formats require DepthStencil usage.");
    }
    if (!IsDepthFormat(description.format) && HasAnyFlag(description.usage, TextureUsage::DepthStencil))
    {
        return fail("DepthStencil usage requires a depth format.");
    }
    if (HasAnyFlag(description.usage, TextureUsage::DepthStencil)
        && HasAnyFlag(description.usage, TextureUsage::RenderTarget | TextureUsage::UnorderedAccess))
    {
        return fail("Depth textures cannot also be render targets or unordered-access textures.");
    }
    if (description.sampleCount > 1 && (description.mipLevels > 1
        || HasAnyFlag(description.usage, TextureUsage::UnorderedAccess)))
    {
        return fail("Multisampled textures cannot have mip chains or unordered-access usage.");
    }
    if (description.memoryAccess != MemoryAccess::GpuOnly
        && HasAnyFlag(description.usage, TextureUsage::RenderTarget | TextureUsage::DepthStencil | TextureUsage::UnorderedAccess))
    {
        return fail("CPU-visible textures cannot be render targets, depth targets, or unordered-access textures.");
    }
    return true;
}
} // namespace Prism::RHI
