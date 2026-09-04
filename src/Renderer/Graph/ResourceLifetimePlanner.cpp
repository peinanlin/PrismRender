#include "Renderer/Graph/ResourceLifetimePlanner.h"

#include <algorithm>
#include <cstdint>
#include <limits>

namespace Prism::Renderer
{
namespace
{
constexpr std::size_t InvalidIndex =
    std::numeric_limits<std::size_t>::max();

std::uint64_t BytesPerPixel(const RHI::Format format)
{
    switch (format)
    {
    case RHI::Format::R8Unorm: return 1;
    case RHI::Format::R16Float: return 2;
    case RHI::Format::R32Float:
    case RHI::Format::R32Typeless:
    case RHI::Format::Rg16Float:
    case RHI::Format::Rgba8Unorm:
    case RHI::Format::Rgba8UnormSrgb:
    case RHI::Format::Bgra8Unorm:
    case RHI::Format::Bgra8UnormSrgb:
    case RHI::Format::D32Float: return 4;
    case RHI::Format::Rgba16Float: return 8;
    case RHI::Format::Rgba32Float: return 16;
    case RHI::Format::Unknown: return 0;
    }
    return 0;
}

std::uint64_t EstimateTextureBytes(
    const RHI::TextureDescription& description)
{
    std::uint64_t bytes = 0;
    std::uint32_t width = description.width;
    std::uint32_t height = description.height;
    for (std::uint32_t mip = 0; mip < description.mipLevels; ++mip)
    {
        bytes += static_cast<std::uint64_t>(width)
            * height
            * description.arrayLayers
            * description.sampleCount
            * BytesPerPixel(description.format);
        width = std::max(1u, width / 2u);
        height = std::max(1u, height / 2u);
    }
    return bytes;
}
} // namespace

void ResourceLifetimePlanner::Build(
    const GraphDescription& description,
    const GraphExecutionState& executionState,
    CompiledGraph& compiledGraph)
{
    compiledGraph.compiledResources.clear();
    const auto ensureResource =
        [&](const std::string& name) -> GraphCompiledResource&
        {
            auto [found, inserted] =
                compiledGraph.compiledResources.try_emplace(name);
            if (inserted)
            {
                found->second.name = name;
            }
            return found->second;
        };

    for (const std::string& name : description.importedResources)
    {
        ensureResource(name).imported = true;
    }
    for (const auto& [name, texture] : executionState.textureResources)
    {
        GraphCompiledResource& resource = ensureResource(name);
        resource.texture = true;
        resource.state = texture.state;
    }
    for (const auto& [name, buffer] : executionState.bufferResources)
    {
        GraphCompiledResource& resource = ensureResource(name);
        resource.buffer = true;
        if (!buffer.states.empty())
        {
            resource.state = buffer.states.front().state;
        }
        resource.estimatedBytes = buffer.description.size;
    }
    for (const auto& [name, transient] : description.transientTextures)
    {
        GraphCompiledResource& resource = ensureResource(name);
        resource.texture = true;
        resource.transient = true;
        resource.state = transient.initialState;
        resource.estimatedBytes = EstimateTextureBytes(transient.description);
        const auto texture = executionState.textureResources.find(name);
        if (texture != executionState.textureResources.end()
            && texture->second.texture != nullptr)
        {
            if (const auto* allocation = texture->second.texture
                    ->GetTransientAllocationInfo();
                allocation != nullptr)
            {
                resource.nativePoolId = allocation->poolId;
                resource.nativeAllocation = allocation->allocationIndex;
                resource.nativeAllocationBytes = allocation->allocationBytes;
            }
        }
    }
    for (const auto& [name, transient] : description.transientBuffers)
    {
        GraphCompiledResource& resource = ensureResource(name);
        resource.buffer = true;
        resource.transient = true;
        resource.state = transient.initialState;
        resource.estimatedBytes = transient.description.size;
        const auto buffer = executionState.bufferResources.find(name);
        if (buffer != executionState.bufferResources.end()
            && buffer->second.buffer != nullptr)
        {
            if (const auto* allocation = buffer->second.buffer
                    ->GetTransientAllocationInfo();
                allocation != nullptr)
            {
                resource.nativePoolId = allocation->poolId;
                resource.nativeAllocation = allocation->allocationIndex;
                resource.nativeAllocationBytes = allocation->allocationBytes;
            }
        }
    }
    for (const GraphRegisteredTexture& registered :
         description.registeredTextures)
    {
        GraphCompiledResource& resource = ensureResource(registered.name);
        resource.currentVersion = registered.currentVersion;
        resource.history = registered.lifetime
            == RenderGraphResourceLifetime::History;
        resource.imported = resource.imported
            || registered.lifetime == RenderGraphResourceLifetime::External
            || registered.lifetime == RenderGraphResourceLifetime::History;
    }
    for (const GraphRegisteredBuffer& registered :
         description.registeredBuffers)
    {
        GraphCompiledResource& resource = ensureResource(registered.name);
        resource.currentVersion = registered.currentVersion;
        resource.history = registered.lifetime
            == RenderGraphResourceLifetime::History;
        resource.imported = resource.imported
            || registered.lifetime == RenderGraphResourceLifetime::External
            || registered.lifetime == RenderGraphResourceLifetime::History;
    }
    for (const GraphPassDeclaration& pass : description.passes)
    {
        for (const std::string& name : pass.reads)
        {
            ensureResource(name);
        }
        for (const std::string& name : pass.writes)
        {
            ensureResource(name);
        }
        if (!pass.active)
        {
            continue;
        }
        const auto recordUse =
            [&](const std::string& name)
            {
                GraphCompiledResource& resource = ensureResource(name);
                resource.active = true;
                resource.firstUse = std::min(
                    resource.firstUse,
                    pass.executionIndex);
                resource.lastUse = resource.lastUse == InvalidIndex
                    ? pass.executionIndex
                    : std::max(resource.lastUse, pass.executionIndex);
            };
        for (const std::string& name : pass.reads)
        {
            recordUse(name);
        }
        for (const std::string& name : pass.writes)
        {
            recordUse(name);
        }
    }
}
} // namespace Prism::Renderer
