#include "RHI/DeferredCommandContext.h"

#include "Core/Assert.h"

#include <string>

namespace Prism::RHI
{
DeferredCommandContext::DeferredCommandContext(
    const GraphicsApi graphicsApi,
    const CommandQueueType queue,
    const bool supportsBufferRangeBarriers)
    : m_graphicsApi(graphicsApi),
      m_queue(queue),
      m_supportsBufferRangeBarriers(
          supportsBufferRangeBarriers)
{
}

GraphicsApi DeferredCommandContext::GetGraphicsApi() const
{
    return m_graphicsApi;
}

CommandQueueType
DeferredCommandContext::GetActiveCommandQueue() const
{
    return m_queue;
}

bool DeferredCommandContext::
SupportsBufferRangeBarriers() const
{
    return m_supportsBufferRangeBarriers;
}

void DeferredCommandContext::BeginDebugLabel(
    const std::string_view name)
{
    const std::string label(name);
    m_commands.emplace_back(
        [label](ICommandContext& target)
        {
            target.BeginDebugLabel(label);
        });
    ++m_debugLabelCommandCount;
}

void DeferredCommandContext::EndDebugLabel()
{
    m_commands.emplace_back(
        [](ICommandContext& target)
        {
            target.EndDebugLabel();
        });
    ++m_debugLabelCommandCount;
}

void DeferredCommandContext::BeginRendering(
    const RenderingInfo& renderingInfo)
{
    Core::Check(
        !m_rendering,
        "Deferred command streams do not support nested rendering scopes.");
    m_commands.emplace_back(
        [renderingInfo](ICommandContext& target)
        {
            target.BeginRendering(renderingInfo);
        });
    m_rendering = true;
}

void DeferredCommandContext::EndRendering()
{
    Core::Check(
        m_rendering,
        "No deferred rendering scope is active.");
    m_commands.emplace_back(
        [](ICommandContext& target)
        {
            target.EndRendering();
        });
    m_rendering = false;
}

void DeferredCommandContext::BindGraphicsPipeline(
    const IGraphicsPipeline& pipeline)
{
    m_commands.emplace_back(
        [&pipeline](ICommandContext& target)
        {
            target.BindGraphicsPipeline(pipeline);
        });
}

void DeferredCommandContext::BindComputePipeline(
    const IComputePipeline& pipeline)
{
    m_commands.emplace_back(
        [&pipeline](ICommandContext& target)
        {
            target.BindComputePipeline(pipeline);
        });
}

void DeferredCommandContext::BindVertexBuffer(
    const IBuffer& buffer,
    const std::uint32_t slot)
{
    m_commands.emplace_back(
        [&buffer, slot](ICommandContext& target)
        {
            target.BindVertexBuffer(buffer, slot);
        });
}

void DeferredCommandContext::BindIndexBuffer(
    const IBuffer& buffer,
    const IndexFormat format)
{
    m_commands.emplace_back(
        [&buffer, format](ICommandContext& target)
        {
            target.BindIndexBuffer(buffer, format);
        });
}

void DeferredCommandContext::BindDescriptorSet(
    const IDescriptorSet& descriptorSet,
    const std::span<
        const DynamicBufferOffset> dynamicOffsets)
{
    std::vector<DynamicBufferOffset> offsets(
        dynamicOffsets.begin(),
        dynamicOffsets.end());
    m_commands.emplace_back(
        [&descriptorSet,
         offsets = std::move(offsets)](
            ICommandContext& target)
        {
            target.BindDescriptorSet(
                descriptorSet,
                offsets);
        });
}

void DeferredCommandContext::DrawIndexed(
    const std::uint32_t indexCount,
    const std::uint32_t instanceCount,
    const std::uint32_t firstIndex,
    const std::int32_t vertexOffset,
    const std::uint32_t firstInstance)
{
    m_commands.emplace_back(
        [=](ICommandContext& target)
        {
            target.DrawIndexed(
                indexCount,
                instanceCount,
                firstIndex,
                vertexOffset,
                firstInstance);
        });
}

void DeferredCommandContext::DrawIndexedIndirect(
    const IBuffer& argumentBuffer,
    const std::size_t argumentOffset,
    const std::uint32_t maxDrawCount,
    const std::uint32_t stride,
    const IBuffer* countBuffer,
    const std::size_t countOffset)
{
    m_commands.emplace_back(
        [&argumentBuffer,
         argumentOffset,
         maxDrawCount,
         stride,
         countBuffer,
         countOffset](ICommandContext& target)
        {
            target.DrawIndexedIndirect(
                argumentBuffer,
                argumentOffset,
                maxDrawCount,
                stride,
                countBuffer,
                countOffset);
        });
}

void DeferredCommandContext::Draw(
    const std::uint32_t vertexCount,
    const std::uint32_t instanceCount,
    const std::uint32_t firstVertex,
    const std::uint32_t firstInstance)
{
    m_commands.emplace_back(
        [=](ICommandContext& target)
        {
            target.Draw(
                vertexCount,
                instanceCount,
                firstVertex,
                firstInstance);
        });
}

void DeferredCommandContext::Dispatch(
    const std::uint32_t groupCountX,
    const std::uint32_t groupCountY,
    const std::uint32_t groupCountZ)
{
    m_commands.emplace_back(
        [=](ICommandContext& target)
        {
            target.Dispatch(
                groupCountX,
                groupCountY,
                groupCountZ);
        });
}

void DeferredCommandContext::TextureBarrier(
    const RHI::TextureBarrier& barrier)
{
    m_commands.emplace_back(
        [barrier](ICommandContext& target)
        {
            target.TextureBarrier(barrier);
        });
}

void DeferredCommandContext::BufferBarrier(
    const RHI::BufferBarrier& barrier)
{
    m_commands.emplace_back(
        [barrier](ICommandContext& target)
        {
            target.BufferBarrier(barrier);
        });
}

void DeferredCommandContext::CopyBuffer(
    const IBuffer& source,
    IBuffer& destination,
    const std::size_t size,
    const std::size_t sourceOffset,
    const std::size_t destinationOffset)
{
    m_commands.emplace_back(
        [&source,
         &destination,
         size,
         sourceOffset,
         destinationOffset](ICommandContext& target)
        {
            target.CopyBuffer(
                source,
                destination,
                size,
                sourceOffset,
                destinationOffset);
        });
}

void DeferredCommandContext::GlobalBarrier(
    const RHI::GlobalBarrier& barrier)
{
    m_commands.emplace_back(
        [barrier](ICommandContext& target)
        {
            target.GlobalBarrier(barrier);
        });
}

void DeferredCommandContext::TextureViewBarrier(
    const ITextureView& textureView,
    const ResourceState before,
    const ResourceState after)
{
    m_commands.emplace_back(
        [&textureView, before, after](
            ICommandContext& target)
        {
            target.TextureViewBarrier(
                textureView,
                before,
                after);
        });
}

void DeferredCommandContext::TextureAliasingBarrier(
    const ITexture* before,
    ITexture& after)
{
    m_commands.emplace_back(
        [before, &after](ICommandContext& target)
        {
            target.TextureAliasingBarrier(
                before,
                after);
        });
}

void DeferredCommandContext::BufferAliasingBarrier(
    const IBuffer* before,
    IBuffer& after)
{
    m_commands.emplace_back(
        [before, &after](ICommandContext& target)
        {
            target.BufferAliasingBarrier(
                before,
                after);
        });
}

void DeferredCommandContext::Replay(
    ICommandContext& target) const
{
    Core::Check(
        !m_rendering,
        "A deferred command stream ended inside a rendering scope.");
    Core::Check(
        target.GetGraphicsApi() == m_graphicsApi,
        "Deferred commands cannot replay on a different graphics API.");
    Core::Check(
        target.GetActiveCommandQueue() == m_queue,
        "Deferred commands cannot replay on a different command queue.");
    for (const Command& command : m_commands)
    {
        command(target);
    }
}

std::size_t
DeferredCommandContext::GetCommandCount() const
{
    // Debug labels are replayable commands, but they are diagnostics rather
    // than recorded GPU work and must not inflate RDG command statistics.
    return m_commands.size()
           - m_debugLabelCommandCount;
}
} // namespace Prism::RHI
