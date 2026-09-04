#pragma once

#include "RHI/GraphicsResources.h"
#include "RHI/PipelineState.h"
#include "RHI/Rendering.h"

#include <cstdint>
#include <memory>
#include <span>
#include <string_view>

namespace Prism::RHI
{
enum class CommandQueueType
{
    Graphics,
    Compute
};

struct CommandQueueCapabilities
{
    bool graphics = true;
    bool compute = false;
    bool dedicatedCompute = false;
    bool timelineSynchronization = false;
    bool nativeQueueSwitching = false;
    bool independentBatchSubmission = false;
    bool deferredBatchSubmission = false;
    bool nativeParallelCommandRecording = false;
};

struct QueueSyncPoint
{
    CommandQueueType queue = CommandQueueType::Graphics;
    std::uint64_t value = 0;

    [[nodiscard]] bool IsValid() const
    {
        return value != 0;
    }
};

struct DrawIndexedIndirectArguments
{
    std::uint32_t indexCount = 0;
    std::uint32_t instanceCount = 0;
    std::uint32_t firstIndex = 0;
    std::int32_t vertexOffset = 0;
    std::uint32_t firstInstance = 0;
};

static_assert(
    sizeof(DrawIndexedIndirectArguments) == 20,
    "The public indexed-indirect layout must match D3D12 and Vulkan.");

class ICommandContext;

class IParallelCommandRecording
{
public:
    virtual ~IParallelCommandRecording() = default;
    virtual ICommandContext& GetCommandContext() = 0;
    virtual bool Close() = 0;
};

class ICommandContext
{
public:
    virtual ~ICommandContext() = default;

    virtual GraphicsApi GetGraphicsApi() const = 0;
    virtual CommandQueueCapabilities
        GetQueueCapabilities() const
    {
        return {};
    }
    virtual CommandQueueType GetActiveCommandQueue() const
    {
        return CommandQueueType::Graphics;
    }
    [[nodiscard]] virtual bool
        SupportsBufferRangeBarriers() const
    {
        return true;
    }
    virtual bool SwitchCommandQueue(
        const CommandQueueType queue)
    {
        return queue == CommandQueueType::Graphics;
    }
    virtual bool BeginQueueBatch(
        CommandQueueType,
        std::span<const QueueSyncPoint>)
    {
        return false;
    }
    virtual QueueSyncPoint EndQueueBatch()
    {
        return {};
    }
    virtual bool FlushQueueBatches()
    {
        return false;
    }
    virtual bool ResumeGraphicsQueue(
        std::span<const QueueSyncPoint>)
    {
        return false;
    }
    virtual std::unique_ptr<IParallelCommandRecording>
        CreateParallelCommandRecording(
            CommandQueueType)
    {
        return {};
    }
    virtual bool AppendParallelCommandRecording(
        std::unique_ptr<IParallelCommandRecording>)
    {
        return false;
    }
    virtual void BeginDebugLabel(std::string_view)
    {
    }
    virtual void EndDebugLabel()
    {
    }
    virtual void BeginRendering(const RenderingInfo& renderingInfo) = 0;
    virtual void EndRendering() = 0;
    virtual void BindGraphicsPipeline(const IGraphicsPipeline& pipeline) = 0;
    virtual void BindComputePipeline(const IComputePipeline& pipeline) = 0;
    virtual void BindVertexBuffer(const IBuffer& buffer, std::uint32_t slot = 0) = 0;
    virtual void BindIndexBuffer(const IBuffer& buffer, IndexFormat format) = 0;
    virtual void BindDescriptorSet(
        const IDescriptorSet& descriptorSet,
        std::span<const DynamicBufferOffset> dynamicOffsets = {}) = 0;
    virtual void DrawIndexed(
        std::uint32_t indexCount,
        std::uint32_t instanceCount = 1,
        std::uint32_t firstIndex = 0,
        std::int32_t vertexOffset = 0,
        std::uint32_t firstInstance = 0) = 0;
    virtual void DrawIndexedIndirect(
        const IBuffer& argumentBuffer,
        std::size_t argumentOffset = 0,
        std::uint32_t maxDrawCount = 1,
        std::uint32_t stride =
            sizeof(DrawIndexedIndirectArguments),
        const IBuffer* countBuffer = nullptr,
        std::size_t countOffset = 0) = 0;
    virtual void Draw(
        std::uint32_t vertexCount,
        std::uint32_t instanceCount = 1,
        std::uint32_t firstVertex = 0,
        std::uint32_t firstInstance = 0) = 0;
    virtual void Dispatch(
        std::uint32_t groupCountX,
        std::uint32_t groupCountY = 1,
        std::uint32_t groupCountZ = 1) = 0;
    virtual void CopyBuffer(
        const IBuffer& source,
        IBuffer& destination,
        std::size_t size,
        std::size_t sourceOffset = 0,
        std::size_t destinationOffset = 0) = 0;
    virtual void TextureBarrier(const RHI::TextureBarrier& barrier) = 0;
    virtual void BufferBarrier(
        const RHI::BufferBarrier& barrier) = 0;
    virtual void GlobalBarrier(
        const RHI::GlobalBarrier& barrier) = 0;
    virtual void TextureViewBarrier(
        const ITextureView& textureView,
        ResourceState before,
        ResourceState after) = 0;
    virtual void TextureAliasingBarrier(
        const ITexture* before,
        ITexture& after) = 0;
    virtual void BufferAliasingBarrier(
        const IBuffer* before,
        IBuffer& after) = 0;
};

class ScopedDebugLabel
{
public:
    ScopedDebugLabel(
        ICommandContext& commandContext,
        const std::string_view name)
        : m_commandContext(&commandContext)
    {
        m_commandContext->BeginDebugLabel(name);
    }

    ~ScopedDebugLabel()
    {
        if (m_commandContext != nullptr)
        {
            m_commandContext->EndDebugLabel();
        }
    }

    ScopedDebugLabel(const ScopedDebugLabel&) = delete;
    ScopedDebugLabel& operator=(
        const ScopedDebugLabel&) = delete;

private:
    ICommandContext* m_commandContext = nullptr;
};
} // namespace Prism::RHI
