#include "RHI/GraphicsResources.h"

#include <unordered_set>

namespace Prism::RHI
{
bool ValidateBufferDescription(
    const BufferDescription& description,
    const void* initialData,
    std::string* outError)
{
    const auto fail = [&](const char* message)
    {
        if (outError != nullptr)
        {
            *outError = message;
        }
        return false;
    };

    if (description.size == 0)
    {
        return fail("Buffer size must be non-zero.");
    }
    if (description.usage == BufferUsage::None)
    {
        return fail("Buffer usage must be specified.");
    }
    if (description.memoryAccess != MemoryAccess::GpuOnly
        && HasAnyFlag(
            description.usage,
            BufferUsage::Storage
                | BufferUsage::AccelerationStructureStorage
                | BufferUsage::AccelerationStructureScratch))
    {
        return fail(
            "CPU-visible buffers cannot use storage/UAV or acceleration-structure usage; "
            "use a GPU-only buffer or a read-only ShaderResource buffer.");
    }
    if (HasAnyFlag(
            description.usage,
            BufferUsage::Vertex
                | BufferUsage::Index
                | BufferUsage::Storage
                | BufferUsage::ShaderResource)
        && description.stride == 0)
    {
        return fail("Vertex, index, and storage buffers require a non-zero stride.");
    }
    if (description.memoryAccess == MemoryAccess::GpuOnly && initialData == nullptr
        && !HasAnyFlag(
            description.usage,
            BufferUsage::CopyDestination
                | BufferUsage::AccelerationStructureStorage
                | BufferUsage::AccelerationStructureScratch))
    {
        return fail("GPU-only buffers without initial data require CopyDestination usage.");
    }
    return true;
}

bool ValidateTextureViewDescription(
    const TextureDescription& texture,
    const TextureViewDescription& view,
    std::string* outError)
{
    const auto fail = [&](const char* message)
    {
        if (outError != nullptr)
        {
            *outError = message;
        }
        return false;
    };

    if (view.mipLevelCount == 0 || view.arrayLayerCount == 0
        || view.baseMipLevel + view.mipLevelCount > texture.mipLevels
        || view.baseArrayLayer + view.arrayLayerCount > texture.arrayLayers)
    {
        return fail("Texture view subresources exceed the source texture.");
    }

    const Format viewFormat = view.format == Format::Unknown ? texture.format : view.format;
    const bool depthAlias = IsDepthFormat(texture.format) && viewFormat == Format::R32Float;
    if (viewFormat != texture.format && !depthAlias)
    {
        return fail("Texture view format is not compatible with the source texture.");
    }

    switch (view.type)
    {
    case TextureViewType::Sampled:
        if (!HasAnyFlag(texture.usage, TextureUsage::ShaderResource))
        {
            return fail("Sampled views require ShaderResource texture usage.");
        }
        break;
    case TextureViewType::RenderTarget:
        if (!HasAnyFlag(texture.usage, TextureUsage::RenderTarget) || IsDepthFormat(viewFormat))
        {
            return fail("Render-target views require color RenderTarget texture usage.");
        }
        break;
    case TextureViewType::DepthStencil:
        if (!HasAnyFlag(texture.usage, TextureUsage::DepthStencil) || !IsDepthFormat(viewFormat))
        {
            return fail("Depth-stencil views require a depth texture.");
        }
        break;
    case TextureViewType::Storage:
        if (!HasAnyFlag(texture.usage, TextureUsage::UnorderedAccess) || IsDepthFormat(viewFormat))
        {
            return fail("Storage views require color UnorderedAccess texture usage.");
        }
        break;
    }
    return true;
}

bool ValidateDescriptorSetLayoutDescription(
    const DescriptorSetLayoutDescription& description,
    std::string* outError)
{
    const auto fail = [&](const char* message)
    {
        if (outError != nullptr)
        {
            *outError = message;
        }
        return false;
    };

    std::unordered_set<std::uint32_t> occupiedBindings;
    for (const DescriptorBindingDescription& binding : description.bindings)
    {
        if (binding.descriptorCount == 0)
        {
            return fail("Descriptor counts must be non-zero.");
        }
        if (binding.type == DescriptorType::DynamicConstantBuffer && binding.descriptorCount != 1)
        {
            return fail("Dynamic constant-buffer arrays are not supported by the current RHI contract.");
        }
        if (binding.stages == ShaderStageFlags::None)
        {
            return fail("Descriptor bindings require at least one shader stage.");
        }
        if (!occupiedBindings.emplace(binding.binding).second)
        {
            return fail("Descriptor binding indices must be unique within a set.");
        }
    }
    return true;
}
} // namespace Prism::RHI
