#include "Renderer/Pipeline/PipelineCache.h"

#include "Core/Assert.h"
#include "RHI/IGraphicsDevice.h"
#include "RHI/PipelineState.h"

#include <iomanip>
#include <sstream>
#include <stdexcept>

namespace Prism::Renderer
{
namespace
{
std::uint64_t HashBytes(std::uint64_t hash,
    const std::vector<std::uint8_t>& bytes)
{
    for (const std::uint8_t value : bytes)
    {
        hash ^= value;
        hash *= 1099511628211ull;
    }
    return hash;
}

void HashShader(std::uint64_t& hash, const RHI::ShaderBinary& shader)
{
    hash ^= static_cast<std::uint64_t>(shader.stage) + 0x9e3779b97f4a7c15ull;
    hash *= 1099511628211ull;
    hash = HashBytes(hash, shader.bytecode);
}
}

std::string PipelineCache::SerializeGraphicsIdentity(
    const RHI::GraphicsPipelineDescription& description)
{
    std::uint64_t hash = 1469598103934665603ull;
    HashShader(hash, description.vertexShader);
    HashShader(hash, description.hullShader);
    HashShader(hash, description.domainShader);
    HashShader(hash, description.pixelShader);
    hash ^= static_cast<std::uint64_t>(description.topology);
    hash *= 1099511628211ull;
    hash ^= description.patchControlPointCount;
    hash *= 1099511628211ull;
    if (description.descriptorSetLayout != nullptr)
    {
        for (const RHI::DescriptorBindingDescription& binding :
             description.descriptorSetLayout->GetDescription().bindings)
        {
            hash ^= binding.binding;
            hash *= 1099511628211ull;
            hash ^= static_cast<std::uint64_t>(binding.type);
            hash *= 1099511628211ull;
            hash ^= static_cast<std::uint64_t>(binding.stages);
            hash *= 1099511628211ull;
        }
    }
    std::ostringstream stream;
    stream << std::hex << std::setw(16) << std::setfill('0') << hash;
    return stream.str();
}

std::shared_ptr<RHI::IGraphicsPipeline>
PipelineCache::GetOrCreateGraphics(
    RHI::IGraphicsDevice& device,
    const std::string_view key,
    const RHI::GraphicsPipelineDescription& description)
{
    Core::Check(!key.empty(), "A graphics pipeline cache key cannot be empty.");
    const std::string ownedKey = std::string(key) + "#"
        + SerializeGraphicsIdentity(description);
    if (const auto existing = m_graphicsPipelines.find(ownedKey);
        existing != m_graphicsPipelines.end())
    {
        return existing->second;
    }
    std::shared_ptr<RHI::IGraphicsPipeline> pipeline;
    try
    {
        pipeline =
            device.CreateGraphicsPipeline(description);
    }
    catch (const std::exception& exception)
    {
        throw std::runtime_error(
            "Failed to create cached graphics pipeline '"
            + ownedKey
            + "': "
            + exception.what());
    }
    catch (...)
    {
        throw std::runtime_error(
            "Failed to create cached graphics pipeline '"
            + ownedKey
            + "' because of an unknown backend error.");
    }
    Core::Check(pipeline != nullptr, "Graphics pipeline creation returned null.");
    m_graphicsPipelines.emplace(ownedKey, pipeline);
    return pipeline;
}

std::shared_ptr<RHI::IComputePipeline>
PipelineCache::GetOrCreateCompute(
    RHI::IGraphicsDevice& device,
    const std::string_view key,
    const RHI::ComputePipelineDescription& description)
{
    Core::Check(!key.empty(), "A compute pipeline cache key cannot be empty.");
    const std::string ownedKey(key);
    if (const auto existing = m_computePipelines.find(ownedKey);
        existing != m_computePipelines.end())
    {
        return existing->second;
    }
    std::shared_ptr<RHI::IComputePipeline> pipeline;
    try
    {
        pipeline =
            device.CreateComputePipeline(description);
    }
    catch (const std::exception& exception)
    {
        throw std::runtime_error(
            "Failed to create cached compute pipeline '"
            + ownedKey
            + "': "
            + exception.what());
    }
    catch (...)
    {
        throw std::runtime_error(
            "Failed to create cached compute pipeline '"
            + ownedKey
            + "' because of an unknown backend error.");
    }
    Core::Check(pipeline != nullptr, "Compute pipeline creation returned null.");
    m_computePipelines.emplace(ownedKey, pipeline);
    return pipeline;
}

std::size_t PipelineCache::GetPipelineCount() const
{
    return m_graphicsPipelines.size()
        + m_computePipelines.size();
}

void PipelineCache::Clear()
{
    m_graphicsPipelines.clear();
    m_computePipelines.clear();
}
} // namespace Prism::Renderer
