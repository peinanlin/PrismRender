#pragma once

#include "RHI/ICommandContext.h"

#include <cstddef>
#include <functional>
#include <vector>

namespace Prism::RHI
{
class DeferredCommandContext final : public ICommandContext
{
public:
    DeferredCommandContext(
        GraphicsApi graphicsApi,
        CommandQueueType queue,
        bool supportsBufferRangeBarriers = true);

    GraphicsApi GetGraphicsApi() const override;
    CommandQueueType GetActiveCommandQueue() const override;
    [[nodiscard]] bool
        SupportsBufferRangeBarriers() const override;

    void BeginDebugLabel(
        std::string_view name) override;
    void EndDebugLabel() override;

    void BeginRendering(
        const RenderingInfo& renderingInfo) override;
    void EndRendering() override;
    void BindGraphicsPipeline(
        const IGraphicsPipeline& pipeline) override;
    void BindComputePipeline(
        const IComputePipeline& pipeline) override;
    void BindVertexBuffer(
        const IBuffer& buffer,
        std::uint32_t slot) override;
    void BindIndexBuffer(
        const IBuffer& buffer,
        IndexFormat format) override;
    void BindDescriptorSet(
        const IDescriptorSet& descriptorSet,
        std::span<const DynamicBufferOffset>
            dynamicOffsets) override;
    void DrawIndexed(
        std::uint32_t indexCount,
        std::uint32_t instanceCount,
        std::uint32_t firstIndex,
        std::int32_t vertexOffset,
        std::uint32_t firstInstance) override;
    void DrawIndexedIndirect(
        const IBuffer& argumentBuffer,
        std::size_t argumentOffset,
        std::uint32_t maxDrawCount,
        std::uint32_t stride,
        const IBuffer* countBuffer,
        std::size_t countOffset) override;
    void Draw(
        std::uint32_t vertexCount,
        std::uint32_t instanceCount,
        std::uint32_t firstVertex,
        std::uint32_t firstInstance) override;
    void Dispatch(
        std::uint32_t groupCountX,
        std::uint32_t groupCountY,
        std::uint32_t groupCountZ) override;
    void CopyBuffer(
        const IBuffer& source,
        IBuffer& destination,
        std::size_t size,
        std::size_t sourceOffset,
        std::size_t destinationOffset) override;
    void TextureBarrier(
        const RHI::TextureBarrier& barrier) override;
    void BufferBarrier(
        const RHI::BufferBarrier& barrier) override;
    void GlobalBarrier(
        const RHI::GlobalBarrier& barrier) override;
    void TextureViewBarrier(
        const ITextureView& textureView,
        ResourceState before,
        ResourceState after) override;
    void TextureAliasingBarrier(
        const ITexture* before,
        ITexture& after) override;
    void BufferAliasingBarrier(
        const IBuffer* before,
        IBuffer& after) override;

    void Replay(ICommandContext& target) const;
    [[nodiscard]] std::size_t GetCommandCount() const;

private:
    using Command =
        std::function<void(ICommandContext&)>;

    GraphicsApi m_graphicsApi;
    CommandQueueType m_queue;
    bool m_supportsBufferRangeBarriers = true;
    std::vector<Command> m_commands;
    std::size_t m_debugLabelCommandCount = 0;
    bool m_rendering = false;
};
} // namespace Prism::RHI
