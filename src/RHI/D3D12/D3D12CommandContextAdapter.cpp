#include "RHI/D3D12/D3D12CommandContextAdapter.h"

#include "Core/Assert.h"
#include "RHI/D3D12/D3D12Debug.h"
#include "RHI/D3D12/D3D12PipelineView.h"
#include "RHI/D3D12/D3D12Resources.h"
#include "RHI/D3D12/D3D12TypeConversions.h"
#include "RHI/D3D12/D3D12Context.h"

#include <algorithm>
#include <stdexcept>
#include <string>

namespace Prism::RHI::D3D12
{
namespace
{
constexpr UINT PixEventUnicodeVersion = 0;

D3D12_RESOURCE_STATES ToQueueCompatibleResourceStates(
    const ResourceState state,
    const CommandQueueType queue)
{
    if (queue != CommandQueueType::Compute
        || !HasAnyFlag(state, ResourceState::ShaderResource))
    {
        return ToNativeResourceStates(state);
    }

    const ResourceState withoutShaderResource =
        static_cast<ResourceState>(
            static_cast<std::uint32_t>(state)
            & ~static_cast<std::uint32_t>(
                ResourceState::ShaderResource));
    return ToNativeResourceStates(withoutShaderResource)
        | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
}
}

D3D12CommandContextAdapter::D3D12CommandContextAdapter(D3D12Context& context)
    : m_context(&context)
{
}

D3D12CommandContextAdapter::D3D12CommandContextAdapter(
    D3D12Context& context,
    ID3D12GraphicsCommandList* commandList,
    const CommandQueueType queue)
    : m_context(&context),
      m_commandList(commandList),
      m_queue(queue)
{
    Core::Check(
        m_commandList != nullptr,
        "An explicit D3D12 command context requires a command list.");
}

GraphicsApi D3D12CommandContextAdapter::GetGraphicsApi() const
{
    return GraphicsApi::Direct3D12;
}

CommandQueueCapabilities
D3D12CommandContextAdapter::GetQueueCapabilities() const
{
    return {
        true,
        m_context->SupportsNativeComputeQueue()
            && m_context->IsComputeQueueValidated(),
        m_context->SupportsNativeComputeQueue()
            && m_context->IsComputeQueueValidated(),
        m_context->SupportsNativeComputeQueue()
            && m_context->IsComputeQueueValidated(),
        m_context->SupportsNativeComputeQueue()
            && m_context->IsComputeQueueValidated(),
        m_context->SupportsNativeComputeQueue()
            && m_context->IsComputeQueueValidated(),
        m_context->SupportsNativeComputeQueue()
            && m_context->IsComputeQueueValidated(),
        true};
}

CommandQueueType
D3D12CommandContextAdapter::GetActiveCommandQueue() const
{
    return m_commandList != nullptr
        ? m_queue
        : m_context->GetActiveCommandQueue();
}

void D3D12CommandContextAdapter::BeginDebugLabel(
    const std::string_view name)
{
    if (name.empty())
    {
        return;
    }

    // Metadata 0 identifies the legacy PIX marker payload as UTF-16.
    // Include the terminator for compatibility with tools that expect it.
    std::wstring wideName = Utf8ToWide(name);
    wideName.push_back(L'\0');
    GetCommandList()->BeginEvent(
        PixEventUnicodeVersion,
        wideName.data(),
        static_cast<UINT>(
            wideName.size() * sizeof(wchar_t)));
}

void D3D12CommandContextAdapter::EndDebugLabel()
{
    GetCommandList()->EndEvent();
}

bool D3D12CommandContextAdapter::SwitchCommandQueue(
    const CommandQueueType queue)
{
    Core::Check(
        !m_renderingInProgress,
        "D3D12 command queues cannot switch inside a rendering scope.");
    m_computePipelineActive = false;
    return m_context->SwitchCommandQueue(queue);
}

bool D3D12CommandContextAdapter::BeginQueueBatch(
    const CommandQueueType queue,
    const std::span<const QueueSyncPoint> waits)
{
    Core::Check(
        !m_renderingInProgress,
        "D3D12 queue batches cannot begin inside a rendering scope.");
    m_computePipelineActive = false;
    return m_context->BeginQueueBatch(queue, waits);
}

QueueSyncPoint
D3D12CommandContextAdapter::EndQueueBatch()
{
    Core::Check(
        !m_renderingInProgress,
        "D3D12 queue batches cannot end inside a rendering scope.");
    m_computePipelineActive = false;
    return m_context->EndQueueBatch();
}

bool D3D12CommandContextAdapter::FlushQueueBatches()
{
    Core::Check(
        !m_renderingInProgress,
        "D3D12 queue batches cannot submit inside a rendering scope.");
    m_computePipelineActive = false;
    return m_context->FlushQueueBatches();
}

bool D3D12CommandContextAdapter::ResumeGraphicsQueue(
    const std::span<const QueueSyncPoint> waits)
{
    Core::Check(
        !m_renderingInProgress,
        "D3D12 graphics continuation cannot begin inside a rendering scope.");
    m_computePipelineActive = false;
    return m_context->ResumeGraphicsQueue(waits);
}

std::unique_ptr<IParallelCommandRecording>
D3D12CommandContextAdapter::
    CreateParallelCommandRecording(
        const CommandQueueType queue)
{
    Core::Check(
        m_commandList == nullptr,
        "Nested D3D12 parallel command recording is not supported.");
    return m_context
        ->CreateParallelCommandRecording(queue);
}

bool D3D12CommandContextAdapter::
    AppendParallelCommandRecording(
        std::unique_ptr<IParallelCommandRecording>
            recording)
{
    Core::Check(
        m_commandList == nullptr
            && !m_renderingInProgress,
        "D3D12 parallel recordings can only be appended by the primary queue-batch context.");
    return m_context
        ->AppendParallelCommandRecording(
            std::move(recording));
}

void D3D12CommandContextAdapter::BeginRendering(const RenderingInfo& renderingInfo)
{
    Core::Check(!m_renderingInProgress, "Nested D3D12 rendering scopes are not supported.");
    std::string validationError;
    Core::Check(ValidateRenderingInfo(renderingInfo, &validationError), validationError.c_str());
    std::vector<D3D12_CPU_DESCRIPTOR_HANDLE> renderTargets;
    renderTargets.reserve(renderingInfo.colorAttachments.size());
    m_pendingAttachmentTransitions.clear();
    for (const RenderingAttachment& attachment : renderingInfo.colorAttachments)
    {
        const auto* view = dynamic_cast<const D3D12TextureView*>(attachment.view);
        Core::Check(view != nullptr, "D3D12 rendering requires D3D12 texture views.");
        TransitionResource(view->GetResource(), view->GetSubresource(), attachment.stateBefore,
                           ResourceState::RenderTarget);
        renderTargets.push_back(view->GetCpuHandle());
        if (attachment.loadOperation == LoadOperation::Clear)
        {
            const float clearColor[] = {attachment.clearColor.red, attachment.clearColor.green,
                                        attachment.clearColor.blue, attachment.clearColor.alpha};
            GetCommandList()->ClearRenderTargetView(view->GetCpuHandle(), clearColor, 0, nullptr);
        }
        m_pendingAttachmentTransitions.push_back({view->GetResource(), view->GetSubresource(),
                                                   ResourceState::RenderTarget, attachment.stateAfter});
    }

    const D3D12_CPU_DESCRIPTOR_HANDLE* depthHandle = nullptr;
    D3D12_CPU_DESCRIPTOR_HANDLE nativeDepthHandle{};
    if (renderingInfo.depthAttachment.has_value())
    {
        const RenderingAttachment& attachment = *renderingInfo.depthAttachment;
        const auto* view = dynamic_cast<const D3D12TextureView*>(attachment.view);
        Core::Check(view != nullptr, "D3D12 rendering requires a D3D12 depth texture view.");
        TransitionResource(view->GetResource(), view->GetSubresource(), attachment.stateBefore,
                           ResourceState::DepthWrite);
        nativeDepthHandle = view->GetCpuHandle();
        depthHandle = &nativeDepthHandle;
        if (attachment.loadOperation == LoadOperation::Clear)
        {
            GetCommandList()->ClearDepthStencilView(
                nativeDepthHandle, D3D12_CLEAR_FLAG_DEPTH, attachment.clearDepthStencil.depth,
                static_cast<UINT8>(attachment.clearDepthStencil.stencil), 0, nullptr);
        }
        m_pendingAttachmentTransitions.push_back({view->GetResource(), view->GetSubresource(),
                                                   ResourceState::DepthWrite, attachment.stateAfter});
    }

    GetCommandList()->OMSetRenderTargets(
        static_cast<UINT>(renderTargets.size()), renderTargets.data(), FALSE, depthHandle);
    const D3D12_VIEWPORT viewport{0.0f, 0.0f, static_cast<float>(renderingInfo.width),
                                  static_cast<float>(renderingInfo.height), 0.0f, 1.0f};
    const D3D12_RECT scissor{0, 0, static_cast<LONG>(renderingInfo.width),
                             static_cast<LONG>(renderingInfo.height)};
    GetCommandList()->RSSetViewports(1, &viewport);
    GetCommandList()->RSSetScissorRects(1, &scissor);
    m_renderingInProgress = true;
}

void D3D12CommandContextAdapter::EndRendering()
{
    Core::Check(m_renderingInProgress, "No D3D12 rendering scope is active.");
    for (const PendingAttachmentTransition& transition : m_pendingAttachmentTransitions)
    {
        if (transition.stateAfter != ResourceState::Undefined)
        {
            TransitionResource(transition.resource, transition.subresource,
                               transition.stateBefore, transition.stateAfter);
        }
    }
    m_pendingAttachmentTransitions.clear();
    m_renderingInProgress = false;
}

void D3D12CommandContextAdapter::BindGraphicsPipeline(const IGraphicsPipeline& pipeline)
{
    const auto* nativePipeline = dynamic_cast<const D3D12GraphicsPipelineBinding*>(&pipeline);
    Core::Check(nativePipeline != nullptr, "The legacy D3D12 adapter requires a D3D12 graphics pipeline view.");
    ID3D12GraphicsCommandList* commandList = GetCommandList();
    commandList->SetGraphicsRootSignature(nativePipeline->GetRootSignature());
    commandList->SetPipelineState(nativePipeline->GetPipelineState());
    commandList->IASetPrimitiveTopology(nativePipeline->GetPrimitiveTopology());
    m_computePipelineActive = false;
}

void D3D12CommandContextAdapter::BindComputePipeline(const IComputePipeline& pipeline)
{
    const auto* nativePipeline = dynamic_cast<const D3D12ComputePipelineBinding*>(&pipeline);
    Core::Check(nativePipeline != nullptr, "The legacy D3D12 adapter requires a D3D12 compute pipeline view.");
    ID3D12GraphicsCommandList* commandList = GetCommandList();
    commandList->SetComputeRootSignature(nativePipeline->GetRootSignature());
    commandList->SetPipelineState(nativePipeline->GetPipelineState());
    m_computePipelineActive = true;
}

void D3D12CommandContextAdapter::BindVertexBuffer(const IBuffer& buffer, const std::uint32_t slot)
{
    const auto* nativeBuffer = dynamic_cast<const D3D12Buffer*>(&buffer);
    Core::Check(nativeBuffer != nullptr, "D3D12 vertex binding requires a D3D12 buffer.");
    const D3D12_VERTEX_BUFFER_VIEW view{
        nativeBuffer->GetGpuVirtualAddress(),
        static_cast<UINT>(nativeBuffer->GetDescription().size),
        nativeBuffer->GetDescription().stride};
    GetCommandList()->IASetVertexBuffers(slot, 1, &view);
}

void D3D12CommandContextAdapter::BindIndexBuffer(const IBuffer& buffer, const IndexFormat format)
{
    const auto* nativeBuffer = dynamic_cast<const D3D12Buffer*>(&buffer);
    Core::Check(nativeBuffer != nullptr, "D3D12 index binding requires a D3D12 buffer.");
    const D3D12_INDEX_BUFFER_VIEW view{
        nativeBuffer->GetGpuVirtualAddress(),
        static_cast<UINT>(nativeBuffer->GetDescription().size),
        format == IndexFormat::UInt16 ? DXGI_FORMAT_R16_UINT : DXGI_FORMAT_R32_UINT};
    GetCommandList()->IASetIndexBuffer(&view);
}

void D3D12CommandContextAdapter::BindDescriptorSet(
    const IDescriptorSet& descriptorSet,
    const std::span<const DynamicBufferOffset> dynamicOffsets)
{
    const auto* nativeSet = dynamic_cast<const D3D12DescriptorSet*>(&descriptorSet);
    Core::Check(nativeSet != nullptr, "D3D12 descriptor binding requires a D3D12 descriptor set.");
    std::vector<ID3D12DescriptorHeap*> heaps;
    if (nativeSet->HasResourceTable()) heaps.push_back(m_context->GetShaderVisibleSrvHeap());
    if (nativeSet->HasSamplerTable()) heaps.push_back(m_context->GetShaderVisibleSamplerHeap());
    if (!heaps.empty()) GetCommandList()->SetDescriptorHeaps(static_cast<UINT>(heaps.size()), heaps.data());
    std::uint32_t rootParameter = 0;
    if (nativeSet->HasResourceTable())
    {
        if (m_computePipelineActive)
            GetCommandList()->SetComputeRootDescriptorTable(rootParameter, nativeSet->GetResourceTable());
        else
            GetCommandList()->SetGraphicsRootDescriptorTable(rootParameter, nativeSet->GetResourceTable());
        ++rootParameter;
    }
    if (nativeSet->HasSamplerTable())
    {
        if (m_computePipelineActive)
            GetCommandList()->SetComputeRootDescriptorTable(rootParameter, nativeSet->GetSamplerTable());
        else
            GetCommandList()->SetGraphicsRootDescriptorTable(rootParameter, nativeSet->GetSamplerTable());
        ++rootParameter;
    }

    const std::vector<std::uint32_t>& dynamicBindings = nativeSet->GetLayout().GetDynamicBufferBindings();
    Core::Check(dynamicOffsets.size() <= dynamicBindings.size(),
                "Too many D3D12 dynamic constant-buffer offsets were supplied.");
    for (std::size_t index = 0; index < dynamicOffsets.size(); ++index)
    {
        const DynamicBufferOffset& offset = dynamicOffsets[index];
        Core::Check(std::ranges::find(dynamicBindings, offset.binding) != dynamicBindings.end(),
                    "A D3D12 dynamic offset references a non-dynamic binding.");
        Core::Check(std::ranges::find_if(
                        dynamicOffsets.begin(), dynamicOffsets.begin() + index,
                        [offset](const DynamicBufferOffset& candidate)
                        {
                            return candidate.binding == offset.binding;
                        }) == dynamicOffsets.begin() + index,
                    "A D3D12 dynamic binding received more than one offset.");
    }
    for (const std::uint32_t binding : dynamicBindings)
    {
        const auto found = std::ranges::find_if(
            dynamicOffsets,
            [binding](const DynamicBufferOffset& candidate)
            {
                return candidate.binding == binding;
            });
        const std::uint32_t offset = found == dynamicOffsets.end() ? 0u : found->offset;
        const D3D12_GPU_VIRTUAL_ADDRESS address = nativeSet->GetDynamicBufferGpuAddress(binding, offset);
        if (m_computePipelineActive)
            GetCommandList()->SetComputeRootConstantBufferView(rootParameter, address);
        else
            GetCommandList()->SetGraphicsRootConstantBufferView(rootParameter, address);
        ++rootParameter;
    }
}

void D3D12CommandContextAdapter::DrawIndexed(
    const std::uint32_t indexCount,
    const std::uint32_t instanceCount,
    const std::uint32_t firstIndex,
    const std::int32_t vertexOffset,
    const std::uint32_t firstInstance)
{
    GetCommandList()->DrawIndexedInstanced(
        indexCount, instanceCount, firstIndex, vertexOffset, firstInstance);
}

void D3D12CommandContextAdapter::DrawIndexedIndirect(
    const IBuffer& argumentBuffer,
    const std::size_t argumentOffset,
    const std::uint32_t maxDrawCount,
    const std::uint32_t stride,
    const IBuffer* countBuffer,
    const std::size_t countOffset)
{
    const auto* nativeArguments =
        dynamic_cast<const D3D12Buffer*>(
            &argumentBuffer);
    const auto* nativeCount =
        dynamic_cast<const D3D12Buffer*>(
            countBuffer);
    Core::Check(
        nativeArguments != nullptr,
        "D3D12 indirect draws require a D3D12 argument buffer.");
    Core::Check(
        countBuffer == nullptr
            || nativeCount != nullptr,
        "D3D12 indirect draw counts require a D3D12 count buffer.");
    Core::Check(
        HasAnyFlag(
            nativeArguments->GetDescription().usage,
            BufferUsage::Indirect),
        "D3D12 indirect argument buffers require Indirect usage.");
    Core::Check(
        stride
                >= sizeof(
                    DrawIndexedIndirectArguments)
            && stride % 4u == 0u,
        "D3D12 indexed-indirect strides must be at least 20 bytes and four-byte aligned.");
    Core::Check(
        argumentOffset % 4u == 0u
            && argumentOffset
                    + static_cast<std::size_t>(
                          maxDrawCount)
                          * stride
                <= nativeArguments
                       ->GetDescription()
                       .size,
        "D3D12 indexed-indirect arguments exceed their buffer.");
    Core::Check(
        nativeCount == nullptr
            || (countOffset % 4u == 0u
                && countOffset
                        + sizeof(std::uint32_t)
                    <= nativeCount
                           ->GetDescription()
                           .size),
        "D3D12 indirect count arguments exceed their buffer.");

    GetCommandList()->ExecuteIndirect(
        m_context->GetDrawIndexedCommandSignature(
            stride),
        maxDrawCount,
        nativeArguments->GetResource(),
        argumentOffset,
        nativeCount != nullptr
            ? nativeCount->GetResource()
            : nullptr,
        countOffset);
}

void D3D12CommandContextAdapter::Draw(
    const std::uint32_t vertexCount,
    const std::uint32_t instanceCount,
    const std::uint32_t firstVertex,
    const std::uint32_t firstInstance)
{
    GetCommandList()->DrawInstanced(vertexCount, instanceCount, firstVertex, firstInstance);
}

void D3D12CommandContextAdapter::Dispatch(
    const std::uint32_t groupCountX,
    const std::uint32_t groupCountY,
    const std::uint32_t groupCountZ)
{
    GetCommandList()->Dispatch(groupCountX, groupCountY, groupCountZ);
}

void D3D12CommandContextAdapter::CopyBuffer(
    const IBuffer& source,
    IBuffer& destination,
    const std::size_t size,
    const std::size_t sourceOffset,
    const std::size_t destinationOffset)
{
    const auto* sourceBuffer =
        dynamic_cast<const D3D12Buffer*>(&source);
    auto* destinationBuffer =
        dynamic_cast<D3D12Buffer*>(&destination);
    Core::Check(
        sourceBuffer != nullptr && destinationBuffer != nullptr,
        "D3D12 buffer copies require D3D12 buffers.");
    Core::Check(
        sourceOffset + size <= source.GetDescription().size
            && destinationOffset + size
                <= destination.GetDescription().size,
        "D3D12 buffer copy range exceeds its resource.");
    GetCommandList()->CopyBufferRegion(
        destinationBuffer->GetResource(),
        destinationOffset,
        sourceBuffer->GetResource(),
        sourceOffset,
        size);
}

void D3D12CommandContextAdapter::TextureBarrier(const RHI::TextureBarrier& barrier)
{
    auto* texture = dynamic_cast<D3D12Texture*>(barrier.texture);
    Core::Check(texture != nullptr, "D3D12 texture barriers require a D3D12 texture.");
    if (barrier.before == barrier.after)
    {
        Core::Check(
            HasAnyFlag(
                barrier.before,
                ResourceState::UnorderedAccess),
            "Equal-state D3D12 texture barriers are only valid for UAV ordering.");
        D3D12_RESOURCE_BARRIER native{};
        native.Type =
            D3D12_RESOURCE_BARRIER_TYPE_UAV;
        native.UAV.pResource =
            texture->GetResource();
        GetCommandList()->ResourceBarrier(
            1,
            &native);
        return;
    }
    const TextureDescription& description =
        texture->GetDescription();
    Core::Check(
        barrier.baseMipLevel
                < description.mipLevels
            && barrier.baseArrayLayer
                < description.arrayLayers,
        "A D3D12 texture barrier starts outside its subresources.");
    const std::uint32_t mipCount =
        barrier.mipLevelCount == 0
        ? description.mipLevels
            - barrier.baseMipLevel
        : barrier.mipLevelCount;
    const std::uint32_t layerCount =
        barrier.arrayLayerCount == 0
        ? description.arrayLayers
            - barrier.baseArrayLayer
        : barrier.arrayLayerCount;
    Core::Check(
        barrier.baseMipLevel + mipCount
                <= description.mipLevels
            && barrier.baseArrayLayer + layerCount
                <= description.arrayLayers,
        "A D3D12 texture barrier exceeds its subresources.");
    const bool wholeResource =
        barrier.baseMipLevel == 0
        && mipCount == description.mipLevels
        && barrier.baseArrayLayer == 0
        && layerCount == description.arrayLayers;
    if (wholeResource)
    {
        TransitionResource(
            texture->GetResource(),
            D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
            barrier.before,
            barrier.after);
        return;
    }
    for (std::uint32_t layer = 0;
         layer < layerCount;
         ++layer)
    {
        for (std::uint32_t mip = 0;
             mip < mipCount;
             ++mip)
        {
            const std::uint32_t subresource =
                barrier.baseMipLevel + mip
                + (barrier.baseArrayLayer + layer)
                    * description.mipLevels;
            TransitionResource(
                texture->GetResource(),
                subresource,
                barrier.before,
                barrier.after);
        }
    }
}

void D3D12CommandContextAdapter::BufferBarrier(
    const RHI::BufferBarrier& barrier)
{
    auto* buffer =
        dynamic_cast<D3D12Buffer*>(barrier.buffer);
    Core::Check(
        buffer != nullptr,
        "D3D12 buffer barriers require a D3D12 buffer.");
    const std::size_t effectiveSize =
        barrier.size == 0
        ? buffer->GetDescription().size
              - barrier.offset
        : barrier.size;
    Core::Check(
        barrier.offset + effectiveSize
            <= buffer->GetDescription().size,
        "A D3D12 buffer barrier exceeds its resource.");
    // Upload/readback heaps have fixed native states. Graph queue handoffs
    // must not transition them through COMMON like device-local resources.
    if (buffer->GetDescription().memoryAccess != MemoryAccess::GpuOnly)
        return;
    if (barrier.before == barrier.after)
    {
        Core::Check(
            HasAnyFlag(
                barrier.before,
                ResourceState::UnorderedAccess),
            "Equal-state D3D12 buffer barriers are only valid for UAV ordering.");
        D3D12_RESOURCE_BARRIER native{};
        native.Type =
            D3D12_RESOURCE_BARRIER_TYPE_UAV;
        native.UAV.pResource =
            buffer->GetResource();
        GetCommandList()->ResourceBarrier(
            1,
            &native);
        return;
    }
    TransitionResource(
        buffer->GetResource(),
        D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
        barrier.before,
        barrier.after);
}

void D3D12CommandContextAdapter::GlobalBarrier(
    const RHI::GlobalBarrier& barrier)
{
    Core::Check(
        HasAnyFlag(
            barrier.before,
            ResourceState::UnorderedAccess)
            && HasAnyFlag(
                barrier.after,
                ResourceState::UnorderedAccess),
        "D3D12 global memory barriers currently require UAV access on both sides.");
    D3D12_RESOURCE_BARRIER native{};
    native.Type =
        D3D12_RESOURCE_BARRIER_TYPE_UAV;
    native.UAV.pResource = nullptr;
    GetCommandList()->ResourceBarrier(
        1,
        &native);
}

void D3D12CommandContextAdapter::TextureViewBarrier(
    const ITextureView& textureView,
    const ResourceState before,
    const ResourceState after)
{
    const auto* view =
        dynamic_cast<const D3D12TextureView*>(&textureView);
    Core::Check(
        view != nullptr,
        "D3D12 texture-view barriers require a D3D12 texture view.");
    TransitionResource(
        view->GetResource(),
        view->GetSubresource(),
        before,
        after);
}

void D3D12CommandContextAdapter::TextureAliasingBarrier(
    const ITexture* before,
    ITexture& after)
{
    const auto* beforeTexture =
        dynamic_cast<const D3D12Texture*>(before);
    auto* afterTexture =
        dynamic_cast<D3D12Texture*>(&after);
    Core::Check(
        before == nullptr || beforeTexture != nullptr,
        "D3D12 aliasing barriers require D3D12 textures.");
    Core::Check(
        afterTexture != nullptr,
        "D3D12 aliasing barriers require a D3D12 destination texture.");

    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type =
        D3D12_RESOURCE_BARRIER_TYPE_ALIASING;
    barrier.Aliasing.pResourceBefore =
        beforeTexture != nullptr
        ? beforeTexture->GetResource()
        : nullptr;
    barrier.Aliasing.pResourceAfter =
        afterTexture->GetResource();
    GetCommandList()->ResourceBarrier(
        1,
        &barrier);
}

void D3D12CommandContextAdapter::BufferAliasingBarrier(
    const IBuffer* before,
    IBuffer& after)
{
    const auto* beforeBuffer =
        dynamic_cast<const D3D12Buffer*>(before);
    auto* afterBuffer =
        dynamic_cast<D3D12Buffer*>(&after);
    Core::Check(
        before == nullptr || beforeBuffer != nullptr,
        "D3D12 aliasing barriers require D3D12 buffers.");
    Core::Check(
        afterBuffer != nullptr,
        "D3D12 aliasing barriers require a D3D12 destination buffer.");

    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type =
        D3D12_RESOURCE_BARRIER_TYPE_ALIASING;
    barrier.Aliasing.pResourceBefore =
        beforeBuffer != nullptr
        ? beforeBuffer->GetResource()
        : nullptr;
    barrier.Aliasing.pResourceAfter =
        afterBuffer->GetResource();
    GetCommandList()->ResourceBarrier(
        1,
        &barrier);
}

void D3D12CommandContextAdapter::TransitionResource(
    ID3D12Resource* resource,
    const std::uint32_t subresource,
    const ResourceState before,
    const ResourceState after)
{
    if (before == after)
    {
        return;
    }
    Core::Check(resource != nullptr, "D3D12 transitions require a resource.");
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = resource;
    barrier.Transition.Subresource = subresource;
    const CommandQueueType queue =
        GetActiveCommandQueue();
    barrier.Transition.StateBefore =
        ToQueueCompatibleResourceStates(before, queue);
    barrier.Transition.StateAfter =
        ToQueueCompatibleResourceStates(after, queue);
    if (queue == CommandQueueType::Compute)
    {
        constexpr D3D12_RESOURCE_STATES
            ComputeStates =
                D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE
                | D3D12_RESOURCE_STATE_UNORDERED_ACCESS
                | D3D12_RESOURCE_STATE_COPY_SOURCE
                | D3D12_RESOURCE_STATE_COPY_DEST
                | D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT;
        const auto invalidBefore =
            static_cast<D3D12_RESOURCE_STATES>(
                barrier.Transition.StateBefore
                & ~ComputeStates);
        const auto invalidAfter =
            static_cast<D3D12_RESOURCE_STATES>(
                barrier.Transition.StateAfter
                & ~ComputeStates);
        if (invalidBefore != 0)
        {
            // Cross-queue handoff barriers are emitted on the producer
            // queue. The compute consumer therefore observes COMMON even
            // though the logical RDG state still names the producer usage.
            barrier.Transition.StateBefore =
                D3D12_RESOURCE_STATE_COMMON;
        }
        if (invalidAfter != 0)
        {
            throw std::runtime_error(
                "D3D12 compute barrier contains queue-incompatible states: before="
                + std::to_string(
                    static_cast<std::uint32_t>(
                        before))
                + ", after="
                + std::to_string(
                    static_cast<std::uint32_t>(
                        after))
                + ", nativeAfter="
                + std::to_string(
                    static_cast<std::uint32_t>(
                        barrier.Transition.StateAfter)));
        }
    }
    if (barrier.Transition.StateBefore != barrier.Transition.StateAfter)
        GetCommandList()->ResourceBarrier(1, &barrier);
}

ID3D12GraphicsCommandList*
D3D12CommandContextAdapter::GetCommandList() const
{
    return m_commandList != nullptr
        ? m_commandList
        : m_context->GetCommandList();
}
} // namespace Prism::RHI::D3D12
