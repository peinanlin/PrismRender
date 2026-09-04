#include "Renderer/Features/Ocean/WaterCoverageReadback.h"

#include "Asset/ShaderManager.h"
#include "Renderer/Pipeline/PipelineCache.h"
#include "RHI/IGraphicsDevice.h"
#include "RHI/ICommandContext.h"
#include "RHI/ShaderLayoutBuilder.h"

#include <array>
#include <algorithm>
#include <numeric>

namespace Prism::Renderer
{
void WaterCoverageReadback::Initialize(RHI::IGraphicsDevice& device,
    Asset::ShaderManager& shaders, PipelineCache& cache,
    const std::filesystem::path& directory, RHI::ShaderBinaryFormat format,
    std::uint32_t frames)
{
    const auto shader = shaders.LoadShader(directory / "Ocean" / "WaterCoverage.slang",
        "WaterCoverageCS", RHI::ShaderStage::Compute, format);
    const std::array stages{RHI::ShaderLayoutStage{&shader.reflection, RHI::ShaderStage::Compute}};
    m_layout = device.CreateDescriptorSetLayout(RHI::BuildDescriptorSetLayout(stages, {}));
    RHI::ComputePipelineDescription description{};
    description.computeShader = shader;
    description.descriptorSetLayout = m_layout;
    m_pipeline = cache.GetOrCreateCompute(device, "WaterOptics.Coverage", description);
    m_slots.resize(frames);
}

void WaterCoverageReadback::Prepare(RHI::IGraphicsDevice& device,
    std::uint32_t frameIndex, std::uint32_t width, std::uint32_t height,
    const std::shared_ptr<RHI::ITextureView>& mask)
{
    m_frameIndex = frameIndex;
    auto& slot = m_slots.at(frameIndex);
    // The caller has waited for this frame slot's fence.
    if (slot.submitted && slot.width == width && slot.height == height)
    {
        std::vector<std::uint32_t> counts(slot.counts->GetDescription().size / sizeof(std::uint32_t));
        slot.readback->Read(counts.data(), counts.size() * sizeof(std::uint32_t));
        m_coverage = static_cast<float>(std::accumulate(counts.begin(), counts.end(), std::uint64_t{0}))
            / static_cast<float>(std::uint64_t(width) * height);
        m_available = true;
    }
    else m_available = false;
    if (!slot.counts || slot.width != width || slot.height != height)
    {
        slot = {};
        slot.width = width;
        slot.height = height;
        RHI::BufferDescription description{};
        description.size = ((width + 15u) / 16u) * ((height + 15u) / 16u) * sizeof(std::uint32_t);
        description.stride = sizeof(std::uint32_t);
        description.usage = RHI::BufferUsage::Storage | RHI::BufferUsage::CopySource | RHI::BufferUsage::CopyDestination;
        slot.counts = device.CreateBuffer(description);
        description.usage = RHI::BufferUsage::CopyDestination;
        description.memoryAccess = RHI::MemoryAccess::GpuToCpu;
        slot.readback = device.CreateBuffer(description);
        slot.descriptors = device.CreateDescriptorSet(m_layout);
        slot.descriptors->WriteBuffer(32u, slot.counts);
    }
    slot.descriptors->WriteTextureView(16u, mask);
}

void WaterCoverageReadback::AddPasses(RenderGraph& graph, TextureHandle mask) const
{
    const auto& slot = m_slots.at(m_frameIndex);
    auto counts = graph.ImportBuffer("WaterCoverageTiles", *slot.counts,
        slot.submitted ? RHI::ResourceState::CopySource : RHI::ResourceState::UnorderedAccess);
    auto readback = graph.ImportBuffer("WaterCoverageReadback", *slot.readback, RHI::ResourceState::CopyDestination);
    auto parameters = graph.CreatePassParameters();
    parameters.ReadTexture(mask, RHI::ResourceState::ShaderResource);
    counts = parameters.WriteBuffer(counts, RHI::ResourceState::UnorderedAccess);
    graph.AddParameterPass("WaterOptics.Coverage", std::move(parameters),
        [this, &slot](RHI::ICommandContext& context, const RenderGraphPassResources&)
        {
            context.BindComputePipeline(*m_pipeline);
            context.BindDescriptorSet(*slot.descriptors);
            context.Dispatch((slot.width + 15u) / 16u, (slot.height + 15u) / 16u, 1u);
        });
    auto copy = graph.CreatePassParameters();
    copy.ReadBuffer(counts, RHI::ResourceState::CopySource);
    readback = copy.WriteBuffer(readback, RHI::ResourceState::CopyDestination);
    graph.AddParameterPass("WaterOptics.Coverage.Readback", std::move(copy),
        [&slot](RHI::ICommandContext& context, const RenderGraphPassResources&)
        {
            context.CopyBuffer(*slot.counts, *slot.readback, slot.counts->GetDescription().size);
            slot.submitted = true;
        }, RenderGraph::PassOptions{RenderGraph::QueueClass::Graphics, true, false, false});
}

float WaterCoverageReadback::Megabytes() const noexcept
{
    std::size_t bytes = 0u;
    for (const auto& slot : m_slots)
        if (slot.counts) bytes += slot.counts->GetDescription().size * 2u;
    return static_cast<float>(bytes) / (1024.0f * 1024.0f);
}

void WaterCoverageReadback::Retire(std::uint64_t frameSerial, bool immediate)
{
    const auto frames = m_slots.size();
    if (immediate)
        m_retired.clear();
    else if (std::ranges::any_of(m_slots, [](const Slot& slot) { return slot.counts != nullptr; }))
        m_retired.push_back({frameSerial + frames, std::move(m_slots)});
    m_slots.clear();
    m_slots.resize(frames);
    m_available = false;
}

void WaterCoverageReadback::Collect(std::uint64_t frameSerial)
{
    std::erase_if(m_retired, [frameSerial](const RetiredSlots& slots) {
        return slots.releaseFrame <= frameSerial;
    });
}
}
