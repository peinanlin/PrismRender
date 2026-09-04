#include "RHI/PipelineState.h"

#include <unordered_map>
#include <unordered_set>

namespace Prism::RHI
{
namespace
{
bool IsValidSampleCount(const std::uint32_t sampleCount)
{
    return sampleCount == 1 || sampleCount == 2 || sampleCount == 4 || sampleCount == 8;
}
} // namespace

std::uint32_t GetVertexElementSize(const VertexElementFormat format)
{
    switch (format)
    {
    case VertexElementFormat::Float: return 4;
    case VertexElementFormat::Float2: return 8;
    case VertexElementFormat::Float3: return 12;
    case VertexElementFormat::Float4: return 16;
    case VertexElementFormat::Uint: return 4;
    }
    return 0;
}

bool ValidateGraphicsPipelineDescription(
    const GraphicsPipelineDescription& description,
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

    if (!description.vertexShader.IsValid() || description.vertexShader.stage != ShaderStage::Vertex)
    {
        return fail("Graphics pipelines require valid vertex shader bytecode.");
    }
    const bool hasHullShader = description.hullShader.IsValid();
    const bool hasDomainShader = description.domainShader.IsValid();
    if (hasHullShader != hasDomainShader)
    {
        return fail("Hull and domain shader stages must be provided together.");
    }
    if (hasHullShader
        && (description.hullShader.stage != ShaderStage::Hull
            || description.domainShader.stage != ShaderStage::Domain))
    {
        return fail("Tessellation stages have incorrect shader stage metadata.");
    }
    if (hasHullShader && description.patchControlPointCount == 0u)
    {
        return fail("Tessellation pipelines require a non-zero patch control-point count.");
    }
    if (description.topology == PrimitiveTopology::PatchList && !hasHullShader)
    {
        return fail("Patch-list topology requires hull and domain shader stages.");
    }
    if (hasHullShader && description.topology != PrimitiveTopology::PatchList)
    {
        return fail("Hull and domain shaders require patch-list topology.");
    }
    const bool hasPixelShader = description.pixelShader.IsValid();
    if (!description.colorFormats.empty() && !hasPixelShader)
    {
        return fail("Graphics pipelines with color attachments require pixel shader bytecode.");
    }
    if (hasPixelShader && description.pixelShader.stage != ShaderStage::Pixel)
    {
        return fail("Graphics pipeline pixel shader bytecode has the wrong stage.");
    }
    if (hasPixelShader && description.vertexShader.format != description.pixelShader.format)
    {
        return fail("Graphics pipeline shader stages must use the same binary format.");
    }
    if (hasHullShader
        && (description.vertexShader.format != description.hullShader.format
            || description.vertexShader.format != description.domainShader.format))
    {
        return fail("All graphics shader stages must use the same binary format.");
    }
    if (!IsValidSampleCount(description.sampleCount))
    {
        return fail("Graphics pipeline sample count must be 1, 2, 4, or 8.");
    }
    if (description.colorFormats.size() != description.blendAttachments.size())
    {
        return fail("Every color attachment requires one blend description.");
    }
    for (const Format format : description.colorFormats)
    {
        if (format == Format::Unknown || IsDepthFormat(format))
        {
            return fail("Graphics pipeline color attachments require color formats.");
        }
    }
    const bool usesDepthStencil = description.depthStencil.depthTestEnabled
                                  || description.depthStencil.depthWriteEnabled
                                  || description.depthStencil.stencilEnabled;
    if (usesDepthStencil && !IsDepthFormat(description.depthFormat))
    {
        return fail("Depth-enabled graphics pipelines require a depth format.");
    }
    if (description.colorFormats.empty() && !usesDepthStencil)
    {
        return fail("Graphics pipelines require at least one color or depth attachment.");
    }

    std::unordered_map<std::uint32_t, std::uint32_t> bindingStrides;
    for (const VertexBufferBindingDescription& binding : description.vertexBindings)
    {
        if (binding.stride == 0)
        {
            return fail("Vertex buffer bindings require a non-zero stride.");
        }
        if (!bindingStrides.emplace(binding.binding, binding.stride).second)
        {
            return fail("Vertex buffer binding indices must be unique.");
        }
    }

    std::unordered_set<std::uint32_t> locations;
    for (const VertexAttributeDescription& attribute : description.vertexAttributes)
    {
        if (!locations.emplace(attribute.location).second)
        {
            return fail("Vertex attribute locations must be unique.");
        }
        const auto binding = bindingStrides.find(attribute.binding);
        if (binding == bindingStrides.end())
        {
            return fail("Vertex attributes must reference a declared buffer binding.");
        }
        if (attribute.offset + GetVertexElementSize(attribute.format) > binding->second)
        {
            return fail("Vertex attribute data exceeds its buffer stride.");
        }
    }
    return true;
}

bool ValidateComputePipelineDescription(
    const ComputePipelineDescription& description,
    std::string* outError)
{
    if (!description.computeShader.IsValid() || description.computeShader.stage != ShaderStage::Compute)
    {
        if (outError != nullptr)
        {
            *outError = "Compute pipelines require valid compute shader bytecode.";
        }
        return false;
    }
    return true;
}
} // namespace Prism::RHI
