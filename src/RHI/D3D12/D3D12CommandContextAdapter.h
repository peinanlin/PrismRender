#pragma once

#include "RHI/ICommandContext.h"

#include <d3d12.h>
#include <vector>

namespace Prism::RHI
{
class D3D12Context;
}

namespace Prism::RHI::D3D12
{
class D3D12CommandContextAdapter final : public ICommandContext
{
public:
    explicit D3D12CommandContextAdapter(D3D12Context& context);
    D3D12CommandContextAdapter(
        D3D12Context& context,
        ID3D12GraphicsCommandList* commandList,
        CommandQueueType queue);

    GraphicsApi GetGraphicsApi() const override;
    CommandQueueCapabilities
        GetQueueCapabilities() const override;
    CommandQueueType GetActiveCommandQueue() const override;
    [[nodiscard]] bool
        SupportsBufferRangeBarriers() const override
    {
        return false;
    }
    void BeginDebugLabel(
        std::string_view name) override;
    void EndDebugLabel() override;
    bool SwitchCommandQueue(CommandQueueType queue) override;
    bool BeginQueueBatch(
        CommandQueueType queue,
        std::span<const QueueSyncPoint> waits) override;
    QueueSyncPoint EndQueueBatch() override;
    bool FlushQueueBatches() override;
    bool ResumeGraphicsQueue(
        std::span<const QueueSyncPoint> waits) override;
    std::unique_ptr<IParallelCommandRecording>
        CreateParallelCommandRecording(
            CommandQueueType queue) override;
    bool AppendParallelCommandRecording(
        std::unique_ptr<IParallelCommandRecording>
            recording) override;
    void BeginRendering(const RenderingInfo& renderingInfo) override;
    void EndRendering() override;
    void BindGraphicsPipeline(const IGraphicsPipeline& pipeline) override;
    void BindComputePipeline(const IComputePipeline& pipeline) override;
    void BindVertexBuffer(const IBuffer& buffer, std::uint32_t slot = 0) override;
    void BindIndexBuffer(const IBuffer& buffer, IndexFormat format) override;
    void BindDescriptorSet(
        const IDescriptorSet& descriptorSet,
        std::span<const DynamicBufferOffset> dynamicOffsets = {}) override;
    void DrawIndexed(
        std::uint32_t indexCount,
        std::uint32_t instanceCount = 1,
        std::uint32_t firstIndex = 0,
        std::int32_t vertexOffset = 0,
        std::uint32_t firstInstance = 0) override;
    void DrawIndexedIndirect(
        const IBuffer& argumentBuffer,
        std::size_t argumentOffset = 0,
        std::uint32_t maxDrawCount = 1,
        std::uint32_t stride =
            sizeof(DrawIndexedIndirectArguments),
        const IBuffer* countBuffer = nullptr,
        std::size_t countOffset = 0) override;
    void Draw(
        std::uint32_t vertexCount,
        std::uint32_t instanceCount = 1,
        std::uint32_t firstVertex = 0,
        std::uint32_t firstInstance = 0) override;
    void Dispatch(
        std::uint32_t groupCountX,
        std::uint32_t groupCountY = 1,
        std::uint32_t groupCountZ = 1) override;
    void CopyBuffer(
        const IBuffer& source,
        IBuffer& destination,
        std::size_t size,
        std::size_t sourceOffset = 0,
        std::size_t destinationOffset = 0) override;
    void TextureBarrier(const RHI::TextureBarrier& barrier) override;
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

private:
    struct PendingAttachmentTransition
    {
        ID3D12Resource* resource = nullptr;
        std::uint32_t subresource = 0;
        ResourceState stateBefore = ResourceState::Undefined;
        ResourceState stateAfter = ResourceState::Undefined;
    };

    void TransitionResource(
        ID3D12Resource* resource,
        std::uint32_t subresource,
        ResourceState before,
        ResourceState after);
    [[nodiscard]] ID3D12GraphicsCommandList*
        GetCommandList() const;

    D3D12Context* m_context = nullptr;
    ID3D12GraphicsCommandList* m_commandList =
        nullptr;
    CommandQueueType m_queue =
        CommandQueueType::Graphics;
    std::vector<PendingAttachmentTransition> m_pendingAttachmentTransitions;
    bool m_renderingInProgress = false;
    bool m_computePipelineActive = false;
};
} // namespace Prism::RHI::D3D12
