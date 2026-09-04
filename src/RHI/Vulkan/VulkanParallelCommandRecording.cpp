#include "RHI/Vulkan/VulkanParallelCommandRecording.h"

#include "Core/Assert.h"
#include "RHI/Vulkan/VulkanContext.h"
#include "RHI/Vulkan/VulkanPipeline.h"
#include "RHI/Vulkan/VulkanResources.h"
#include "RHI/Vulkan/VulkanTypeConversions.h"

#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace Prism::RHI::Vulkan
{
namespace
{
void CheckVk(const VkResult result, const char* message)
{
    if (result != VK_SUCCESS)
    {
        throw std::runtime_error(
            std::string(message)
            + " VkResult="
            + std::to_string(
                static_cast<int>(result)));
    }
}

VkAttachmentLoadOp ToNativeLoadOperation(
    const LoadOperation operation)
{
    switch (operation)
    {
    case LoadOperation::Load:
        return VK_ATTACHMENT_LOAD_OP_LOAD;
    case LoadOperation::Clear:
        return VK_ATTACHMENT_LOAD_OP_CLEAR;
    case LoadOperation::Discard:
        return VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    }
    throw std::invalid_argument(
        "Unsupported Vulkan attachment load operation.");
}

VkAttachmentStoreOp ToNativeStoreOperation(
    const StoreOperation operation)
{
    switch (operation)
    {
    case StoreOperation::Store:
        return VK_ATTACHMENT_STORE_OP_STORE;
    case StoreOperation::Discard:
        return VK_ATTACHMENT_STORE_OP_DONT_CARE;
    }
    throw std::invalid_argument(
        "Unsupported Vulkan attachment store operation.");
}

class VulkanParallelCommandContext final
    : public ICommandContext
{
public:
    VulkanParallelCommandContext(
        VulkanContext& context,
        const VkCommandBuffer commandBuffer,
        const CommandQueueType queue)
        : m_context(&context),
          m_commandBuffer(commandBuffer),
          m_queue(queue)
    {
    }

    GraphicsApi GetGraphicsApi() const override
    {
        return GraphicsApi::Vulkan;
    }

    CommandQueueCapabilities
        GetQueueCapabilities() const override
    {
        return {
            true,
            m_queue == CommandQueueType::Compute,
            false,
            false,
            false,
            false,
            false,
            false};
    }

    CommandQueueType
        GetActiveCommandQueue() const override
    {
        return m_queue;
    }

    void BeginRendering(
        const RenderingInfo& renderingInfo) override
    {
        Core::Check(
            m_queue == CommandQueueType::Graphics,
            "Vulkan rendering scopes require a graphics command buffer.");
        Core::Check(
            !m_renderingInProgress,
            "Nested Vulkan parallel rendering scopes are not supported.");
        std::string validationError;
        Core::Check(
            ValidateRenderingInfo(
                renderingInfo,
                &validationError),
            validationError.c_str());

        std::vector<VkRenderingAttachmentInfo>
            colorAttachments;
        colorAttachments.reserve(
            renderingInfo.colorAttachments.size());
        m_pendingAttachmentTransitions.clear();
        for (const RenderingAttachment& attachment :
             renderingInfo.colorAttachments)
        {
            const auto* view =
                dynamic_cast<const VulkanTextureView*>(
                    attachment.view);
            Core::Check(
                view != nullptr,
                "Vulkan rendering requires Vulkan texture views.");
            TransitionTextureView(
                *view,
                attachment.stateBefore,
                ResourceState::RenderTarget);

            VkRenderingAttachmentInfo native{
                VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
            native.imageView = view->GetHandle();
            native.imageLayout =
                VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            native.loadOp =
                ToNativeLoadOperation(
                    attachment.loadOperation);
            native.storeOp =
                ToNativeStoreOperation(
                    attachment.storeOperation);
            native.clearValue.color = {{
                attachment.clearColor.red,
                attachment.clearColor.green,
                attachment.clearColor.blue,
                attachment.clearColor.alpha}};
            colorAttachments.push_back(native);
            m_pendingAttachmentTransitions.push_back({
                attachment.view,
                ResourceState::RenderTarget,
                attachment.stateAfter});
        }

        VkRenderingAttachmentInfo depthAttachment{
            VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        const VkRenderingAttachmentInfo*
            nativeDepthAttachment = nullptr;
        if (renderingInfo.depthAttachment.has_value())
        {
            const RenderingAttachment& attachment =
                *renderingInfo.depthAttachment;
            const auto* view =
                dynamic_cast<const VulkanTextureView*>(
                    attachment.view);
            Core::Check(
                view != nullptr,
                "Vulkan rendering requires a Vulkan depth texture view.");
            TransitionTextureView(
                *view,
                attachment.stateBefore,
                ResourceState::DepthWrite);
            depthAttachment.imageView =
                view->GetHandle();
            depthAttachment.imageLayout =
                VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
            depthAttachment.loadOp =
                ToNativeLoadOperation(
                    attachment.loadOperation);
            depthAttachment.storeOp =
                ToNativeStoreOperation(
                    attachment.storeOperation);
            depthAttachment.clearValue.depthStencil = {
                attachment.clearDepthStencil.depth,
                attachment.clearDepthStencil.stencil};
            nativeDepthAttachment =
                &depthAttachment;
            m_pendingAttachmentTransitions.push_back({
                attachment.view,
                ResourceState::DepthWrite,
                attachment.stateAfter});
        }

        VkRenderingInfo native{
            VK_STRUCTURE_TYPE_RENDERING_INFO};
        native.renderArea.extent = {
            renderingInfo.width,
            renderingInfo.height};
        native.layerCount = renderingInfo.layerCount;
        native.colorAttachmentCount =
            static_cast<std::uint32_t>(
                colorAttachments.size());
        native.pColorAttachments =
            colorAttachments.data();
        native.pDepthAttachment =
            nativeDepthAttachment;
        vkCmdBeginRendering(
            m_commandBuffer,
            &native);

        VkViewport viewport{};
        viewport.y =
            static_cast<float>(
                renderingInfo.height);
        viewport.width =
            static_cast<float>(
                renderingInfo.width);
        viewport.height =
            -static_cast<float>(
                renderingInfo.height);
        viewport.minDepth = 0.0f;
        viewport.maxDepth = 1.0f;
        vkCmdSetViewport(
            m_commandBuffer,
            0,
            1,
            &viewport);
        const VkRect2D scissor{
            {0, 0},
            {renderingInfo.width,
             renderingInfo.height}};
        vkCmdSetScissor(
            m_commandBuffer,
            0,
            1,
            &scissor);
        m_renderingInProgress = true;
    }

    void EndRendering() override
    {
        Core::Check(
            m_renderingInProgress,
            "No Vulkan parallel rendering scope is active.");
        vkCmdEndRendering(m_commandBuffer);
        m_renderingInProgress = false;
        for (const PendingAttachmentTransition&
                 transition :
             m_pendingAttachmentTransitions)
        {
            if (transition.stateAfter
                != ResourceState::Undefined)
            {
                TransitionTextureView(
                    *transition.view,
                    transition.stateBefore,
                    transition.stateAfter);
            }
        }
        m_pendingAttachmentTransitions.clear();
    }

    void BindGraphicsPipeline(
        const IGraphicsPipeline& pipeline) override
    {
        const auto* nativePipeline =
            dynamic_cast<
                const VulkanGraphicsPipeline*>(
                &pipeline);
        Core::Check(
            nativePipeline != nullptr,
            "Vulkan parallel recording requires a Vulkan graphics pipeline.");
        vkCmdBindPipeline(
            m_commandBuffer,
            VK_PIPELINE_BIND_POINT_GRAPHICS,
            nativePipeline->GetHandle());
        m_activePipelineLayout =
            nativePipeline->GetLayout();
        m_activePipelineBindPoint =
            VK_PIPELINE_BIND_POINT_GRAPHICS;
    }

    void BindComputePipeline(
        const IComputePipeline& pipeline) override
    {
        const auto* nativePipeline =
            dynamic_cast<
                const VulkanComputePipeline*>(
                &pipeline);
        Core::Check(
            nativePipeline != nullptr,
            "Vulkan parallel recording requires a Vulkan compute pipeline.");
        vkCmdBindPipeline(
            m_commandBuffer,
            VK_PIPELINE_BIND_POINT_COMPUTE,
            nativePipeline->GetHandle());
        m_activePipelineLayout =
            nativePipeline->GetLayout();
        m_activePipelineBindPoint =
            VK_PIPELINE_BIND_POINT_COMPUTE;
    }

    void BindVertexBuffer(
        const IBuffer& buffer,
        const std::uint32_t slot) override
    {
        const auto* nativeBuffer =
            dynamic_cast<const VulkanBuffer*>(
                &buffer);
        Core::Check(
            nativeBuffer != nullptr,
            "Vulkan parallel recording requires a Vulkan vertex buffer.");
        const VkBuffer handle =
            nativeBuffer->GetHandle();
        constexpr VkDeviceSize offset = 0;
        vkCmdBindVertexBuffers(
            m_commandBuffer,
            slot,
            1,
            &handle,
            &offset);
    }

    void BindIndexBuffer(
        const IBuffer& buffer,
        const IndexFormat format) override
    {
        const auto* nativeBuffer =
            dynamic_cast<const VulkanBuffer*>(
                &buffer);
        Core::Check(
            nativeBuffer != nullptr,
            "Vulkan parallel recording requires a Vulkan index buffer.");
        vkCmdBindIndexBuffer(
            m_commandBuffer,
            nativeBuffer->GetHandle(),
            0,
            format == IndexFormat::UInt16
                ? VK_INDEX_TYPE_UINT16
                : VK_INDEX_TYPE_UINT32);
    }

    void BindDescriptorSet(
        const IDescriptorSet& descriptorSet,
        const std::span<
            const DynamicBufferOffset>
            dynamicOffsets) override
    {
        const auto* nativeSet =
            dynamic_cast<const VulkanDescriptorSet*>(
                &descriptorSet);
        Core::Check(
            nativeSet != nullptr,
            "Vulkan parallel recording requires a Vulkan descriptor set.");
        Core::Check(
            m_activePipelineLayout
                != VK_NULL_HANDLE,
            "BindPipeline must be called before BindDescriptorSet.");
        const VkDescriptorSet handle =
            nativeSet->GetHandle();
        const std::vector<std::uint32_t>
            nativeOffsets =
                nativeSet->BuildDynamicOffsets(
                    dynamicOffsets);
        vkCmdBindDescriptorSets(
            m_commandBuffer,
            m_activePipelineBindPoint,
            m_activePipelineLayout,
            0,
            1,
            &handle,
            static_cast<std::uint32_t>(
                nativeOffsets.size()),
            nativeOffsets.data());
    }

    void DrawIndexed(
        const std::uint32_t indexCount,
        const std::uint32_t instanceCount,
        const std::uint32_t firstIndex,
        const std::int32_t vertexOffset,
        const std::uint32_t firstInstance) override
    {
        vkCmdDrawIndexed(
            m_commandBuffer,
            indexCount,
            instanceCount,
            firstIndex,
            vertexOffset,
            firstInstance);
    }

    void DrawIndexedIndirect(
        const IBuffer& argumentBuffer,
        const std::size_t argumentOffset,
        const std::uint32_t maxDrawCount,
        const std::uint32_t stride,
        const IBuffer* countBuffer,
        const std::size_t countOffset) override
    {
        const auto* nativeArguments =
            dynamic_cast<const VulkanBuffer*>(
                &argumentBuffer);
        const auto* nativeCount =
            dynamic_cast<const VulkanBuffer*>(
                countBuffer);
        Core::Check(
            nativeArguments != nullptr,
            "Vulkan parallel indirect draws require a Vulkan argument buffer.");
        Core::Check(
            countBuffer == nullptr
                || nativeCount != nullptr,
            "Vulkan parallel indirect counts require a Vulkan count buffer.");
        Core::Check(
            stride
                    >= sizeof(
                        DrawIndexedIndirectArguments)
                && stride % 4u == 0u,
            "Vulkan indexed-indirect strides must be at least 20 bytes and four-byte aligned.");
        Core::Check(
            argumentOffset
                    + static_cast<std::size_t>(
                          maxDrawCount)
                          * stride
                <= nativeArguments
                       ->GetDescription()
                       .size,
            "Vulkan parallel indexed-indirect arguments exceed their buffer.");
        if (nativeCount != nullptr)
        {
            Core::Check(
                m_context->GetCapabilities()
                    .features.drawIndirectCount,
                "This Vulkan device does not support indirect draw counts.");
            vkCmdDrawIndexedIndirectCount(
                m_commandBuffer,
                nativeArguments->GetHandle(),
                argumentOffset,
                nativeCount->GetHandle(),
                countOffset,
                maxDrawCount,
                stride);
            return;
        }
        vkCmdDrawIndexedIndirect(
            m_commandBuffer,
            nativeArguments->GetHandle(),
            argumentOffset,
            maxDrawCount,
            stride);
    }

    void Draw(
        const std::uint32_t vertexCount,
        const std::uint32_t instanceCount,
        const std::uint32_t firstVertex,
        const std::uint32_t firstInstance) override
    {
        vkCmdDraw(
            m_commandBuffer,
            vertexCount,
            instanceCount,
            firstVertex,
            firstInstance);
    }

    void Dispatch(
        const std::uint32_t groupCountX,
        const std::uint32_t groupCountY,
        const std::uint32_t groupCountZ) override
    {
        vkCmdDispatch(
            m_commandBuffer,
            groupCountX,
            groupCountY,
            groupCountZ);
    }

    void CopyBuffer(
        const IBuffer& source,
        IBuffer& destination,
        const std::size_t size,
        const std::size_t sourceOffset,
        const std::size_t destinationOffset) override
    {
        const auto* sourceBuffer =
            dynamic_cast<const VulkanBuffer*>(&source);
        auto* destinationBuffer =
            dynamic_cast<VulkanBuffer*>(&destination);
        Core::Check(
            sourceBuffer != nullptr
                && destinationBuffer != nullptr,
            "Vulkan parallel buffer copies require Vulkan buffers.");
        Core::Check(
            sourceOffset + size
                    <= source.GetDescription().size
                && destinationOffset + size
                    <= destination.GetDescription().size,
            "Vulkan parallel buffer copy range exceeds its resource.");
        const VkBufferCopy region{
            sourceOffset,
            destinationOffset,
            size};
        vkCmdCopyBuffer(
            m_commandBuffer,
            sourceBuffer->GetHandle(),
            destinationBuffer->GetHandle(),
            1,
            &region);
    }

    void TextureBarrier(
        const RHI::TextureBarrier& barrier) override
    {
        auto* nativeTexture =
            dynamic_cast<VulkanTexture*>(
                barrier.texture);
        Core::Check(
            nativeTexture != nullptr,
            "Vulkan parallel barriers require a Vulkan texture.");
        const bool depthResource =
            IsDepthFormat(
                nativeTexture
                    ->GetDescription().format);
        const ResourceStateMapping before =
            ToNativeResourceState(
                barrier.before,
                depthResource);
        const ResourceStateMapping after =
            ToNativeResourceState(
                barrier.after,
                depthResource);

        VkImageMemoryBarrier nativeBarrier{
            VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        nativeBarrier.srcAccessMask =
            before.accessMask;
        nativeBarrier.dstAccessMask =
            after.accessMask;
        nativeBarrier.oldLayout =
            before.imageLayout;
        nativeBarrier.newLayout =
            after.imageLayout;
        nativeBarrier.srcQueueFamilyIndex =
            VK_QUEUE_FAMILY_IGNORED;
        nativeBarrier.dstQueueFamilyIndex =
            VK_QUEUE_FAMILY_IGNORED;
        nativeBarrier.image =
            nativeTexture->GetImage();
        nativeBarrier.subresourceRange.aspectMask =
            depthResource
            ? VK_IMAGE_ASPECT_DEPTH_BIT
            : VK_IMAGE_ASPECT_COLOR_BIT;
        const TextureDescription& description =
            nativeTexture->GetDescription();
        Core::Check(
            barrier.baseMipLevel
                    < description.mipLevels
                && barrier.baseArrayLayer
                    < description.arrayLayers,
            "A Vulkan parallel texture barrier starts outside its subresources.");
        nativeBarrier.subresourceRange.baseMipLevel =
            barrier.baseMipLevel;
        nativeBarrier.subresourceRange.levelCount =
            barrier.mipLevelCount == 0
            ? description.mipLevels
                - barrier.baseMipLevel
            : barrier.mipLevelCount;
        nativeBarrier.subresourceRange.baseArrayLayer =
            barrier.baseArrayLayer;
        nativeBarrier.subresourceRange.layerCount =
            barrier.arrayLayerCount == 0
            ? description.arrayLayers
                - barrier.baseArrayLayer
            : barrier.arrayLayerCount;
        Core::Check(
            nativeBarrier.subresourceRange.baseMipLevel
                    + nativeBarrier.subresourceRange.levelCount
                <= description.mipLevels
                && nativeBarrier.subresourceRange
                        .baseArrayLayer
                        + nativeBarrier.subresourceRange
                              .layerCount
                    <= description.arrayLayers,
            "A Vulkan parallel texture barrier exceeds its subresources.");
        vkCmdPipelineBarrier(
            m_commandBuffer,
            QueueCompatiblePipelineStages(before.pipelineStages, GetActiveCommandQueue()),
            QueueCompatiblePipelineStages(after.pipelineStages, GetActiveCommandQueue()),
            0,
            0,
            nullptr,
            0,
            nullptr,
            1,
            &nativeBarrier);
    }

    void BufferBarrier(
        const RHI::BufferBarrier& barrier) override
    {
        auto* nativeBuffer =
            dynamic_cast<VulkanBuffer*>(
                barrier.buffer);
        Core::Check(
            nativeBuffer != nullptr,
            "Vulkan parallel buffer barriers require a Vulkan buffer.");
        const std::size_t effectiveSize =
            barrier.size == 0
            ? nativeBuffer->GetDescription().size
                  - barrier.offset
            : barrier.size;
        Core::Check(
            barrier.offset + effectiveSize
                <= nativeBuffer
                       ->GetDescription().size,
            "A Vulkan parallel buffer barrier exceeds its resource.");
        const ResourceStateMapping before =
            ToNativeResourceState(
                barrier.before,
                false);
        const ResourceStateMapping after =
            ToNativeResourceState(
                barrier.after,
                false);
        VkBufferMemoryBarrier native{
            VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
        native.srcAccessMask =
            before.accessMask;
        native.dstAccessMask =
            after.accessMask;
        native.srcQueueFamilyIndex =
            VK_QUEUE_FAMILY_IGNORED;
        native.dstQueueFamilyIndex =
            VK_QUEUE_FAMILY_IGNORED;
        native.buffer =
            nativeBuffer->GetHandle();
        native.offset = barrier.offset;
        native.size = effectiveSize;
        vkCmdPipelineBarrier(
            m_commandBuffer,
            QueueCompatiblePipelineStages(before.pipelineStages, GetActiveCommandQueue()),
            QueueCompatiblePipelineStages(after.pipelineStages, GetActiveCommandQueue()),
            0,
            0,
            nullptr,
            1,
            &native,
            0,
            nullptr);
    }

    void GlobalBarrier(
        const RHI::GlobalBarrier& barrier) override
    {
        const ResourceStateMapping before =
            ToNativeResourceState(
                barrier.before,
                false);
        const ResourceStateMapping after =
            ToNativeResourceState(
                barrier.after,
                false);
        VkMemoryBarrier native{
            VK_STRUCTURE_TYPE_MEMORY_BARRIER};
        native.srcAccessMask =
            before.accessMask;
        native.dstAccessMask =
            after.accessMask;
        vkCmdPipelineBarrier(
            m_commandBuffer,
            QueueCompatiblePipelineStages(before.pipelineStages, GetActiveCommandQueue()),
            QueueCompatiblePipelineStages(after.pipelineStages, GetActiveCommandQueue()),
            0,
            1,
            &native,
            0,
            nullptr,
            0,
            nullptr);
    }

    void TextureViewBarrier(
        const ITextureView& textureView,
        const ResourceState before,
        const ResourceState after) override
    {
        TransitionTextureView(
            textureView,
            before,
            after);
    }

    void TextureAliasingBarrier(
        const ITexture* before,
        ITexture& after) override
    {
        Core::Check(
            before == nullptr
                || dynamic_cast<
                       const VulkanTexture*>(
                       before)
                    != nullptr,
            "Vulkan parallel aliasing barriers require Vulkan textures.");
        Core::Check(
            dynamic_cast<VulkanTexture*>(&after)
                != nullptr,
            "Vulkan parallel aliasing barriers require a Vulkan destination texture.");
        VkMemoryBarrier memoryBarrier{
            VK_STRUCTURE_TYPE_MEMORY_BARRIER};
        memoryBarrier.srcAccessMask =
            VK_ACCESS_MEMORY_WRITE_BIT;
        memoryBarrier.dstAccessMask =
            VK_ACCESS_MEMORY_READ_BIT
            | VK_ACCESS_MEMORY_WRITE_BIT;
        vkCmdPipelineBarrier(
            m_commandBuffer,
            VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
            VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
            0,
            1,
            &memoryBarrier,
            0,
            nullptr,
            0,
            nullptr);
    }

    void BufferAliasingBarrier(
        const IBuffer* before,
        IBuffer& after) override
    {
        Core::Check(
            before == nullptr
                || dynamic_cast<
                       const VulkanBuffer*>(
                       before)
                    != nullptr,
            "Vulkan parallel aliasing barriers require Vulkan buffers.");
        Core::Check(
            dynamic_cast<VulkanBuffer*>(&after)
                != nullptr,
            "Vulkan parallel aliasing barriers require a Vulkan destination buffer.");
        VkMemoryBarrier memoryBarrier{
            VK_STRUCTURE_TYPE_MEMORY_BARRIER};
        memoryBarrier.srcAccessMask =
            VK_ACCESS_MEMORY_WRITE_BIT;
        memoryBarrier.dstAccessMask =
            VK_ACCESS_MEMORY_READ_BIT
            | VK_ACCESS_MEMORY_WRITE_BIT;
        vkCmdPipelineBarrier(
            m_commandBuffer,
            VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
            VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
            0,
            1,
            &memoryBarrier,
            0,
            nullptr,
            0,
            nullptr);
    }

private:
    struct PendingAttachmentTransition
    {
        const ITextureView* view = nullptr;
        ResourceState stateBefore =
            ResourceState::Undefined;
        ResourceState stateAfter =
            ResourceState::Undefined;
    };

    void TransitionTextureView(
        const ITextureView& textureView,
        const ResourceState beforeState,
        const ResourceState afterState)
    {
        if (beforeState == afterState)
        {
            return;
        }
        const auto* view =
            dynamic_cast<const VulkanTextureView*>(
                &textureView);
        Core::Check(
            view != nullptr,
            "Vulkan parallel transitions require Vulkan texture views.");
        const bool depthResource =
            IsDepthFormat(
                view->GetTextureDescription().format);
        const ResourceStateMapping before =
            ToNativeResourceState(
                beforeState,
                depthResource);
        const ResourceStateMapping after =
            ToNativeResourceState(
                afterState,
                depthResource);
        const TextureViewDescription& description =
            view->GetDescription();

        VkImageMemoryBarrier barrier{
            VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        barrier.srcAccessMask = before.accessMask;
        barrier.dstAccessMask = after.accessMask;
        barrier.oldLayout = before.imageLayout;
        barrier.newLayout = after.imageLayout;
        barrier.srcQueueFamilyIndex =
            VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex =
            VK_QUEUE_FAMILY_IGNORED;
        barrier.image = view->GetImage();
        barrier.subresourceRange.aspectMask =
            depthResource
            ? VK_IMAGE_ASPECT_DEPTH_BIT
            : VK_IMAGE_ASPECT_COLOR_BIT;
        barrier.subresourceRange.baseMipLevel =
            description.baseMipLevel;
        barrier.subresourceRange.levelCount =
            description.mipLevelCount;
        barrier.subresourceRange.baseArrayLayer =
            description.baseArrayLayer;
        barrier.subresourceRange.layerCount =
            description.arrayLayerCount;
        vkCmdPipelineBarrier(
            m_commandBuffer,
            QueueCompatiblePipelineStages(before.pipelineStages, GetActiveCommandQueue()),
            QueueCompatiblePipelineStages(after.pipelineStages, GetActiveCommandQueue()),
            0,
            0,
            nullptr,
            0,
            nullptr,
            1,
            &barrier);
    }

    VulkanContext* m_context = nullptr;
    VkCommandBuffer m_commandBuffer =
        VK_NULL_HANDLE;
    CommandQueueType m_queue =
        CommandQueueType::Graphics;
    bool m_renderingInProgress = false;
    VkPipelineLayout m_activePipelineLayout =
        VK_NULL_HANDLE;
    VkPipelineBindPoint m_activePipelineBindPoint =
        VK_PIPELINE_BIND_POINT_GRAPHICS;
    std::vector<PendingAttachmentTransition>
        m_pendingAttachmentTransitions;
};
} // namespace

VulkanParallelCommandRecording::
    VulkanParallelCommandRecording(
        VulkanContext& context,
        const CommandQueueType queue)
    : m_context(&context),
      m_queue(queue)
{
    const std::uint32_t queueFamily =
        queue == CommandQueueType::Compute
        ? context.GetComputeQueueFamilyIndex()
        : context.GetGraphicsQueueFamilyIndex();
    VkCommandPoolCreateInfo poolInfo{
        VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    poolInfo.flags =
        VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
    poolInfo.queueFamilyIndex = queueFamily;
    CheckVk(
        vkCreateCommandPool(
            context.GetDevice(),
            &poolInfo,
            nullptr,
            &m_commandPool),
        "Failed to create a Vulkan parallel command pool.");

    try
    {
        VkCommandBufferAllocateInfo allocateInfo{
            VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        allocateInfo.commandPool = m_commandPool;
        allocateInfo.level =
            VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        allocateInfo.commandBufferCount = 1;
        CheckVk(
            vkAllocateCommandBuffers(
                context.GetDevice(),
                &allocateInfo,
                &m_commandBuffer),
            "Failed to allocate a Vulkan parallel command buffer.");
        VkCommandBufferBeginInfo beginInfo{
            VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        beginInfo.flags =
            VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        CheckVk(
            vkBeginCommandBuffer(
                m_commandBuffer,
                &beginInfo),
            "Failed to begin a Vulkan parallel command buffer.");
        m_commandContext =
            std::make_unique<
                VulkanParallelCommandContext>(
                context,
                m_commandBuffer,
                queue);
    }
    catch (...)
    {
        vkDestroyCommandPool(
            context.GetDevice(),
            m_commandPool,
            nullptr);
        m_commandPool = VK_NULL_HANDLE;
        m_commandBuffer = VK_NULL_HANDLE;
        throw;
    }
}

VulkanParallelCommandRecording::
    ~VulkanParallelCommandRecording()
{
    if (m_commandPool != VK_NULL_HANDLE
        && m_context != nullptr)
    {
        vkDestroyCommandPool(
            m_context->GetDevice(),
            m_commandPool,
            nullptr);
    }
}

ICommandContext&
VulkanParallelCommandRecording::
    GetCommandContext()
{
    Core::Check(
        !m_closed && m_commandContext != nullptr,
        "A closed Vulkan parallel command recording cannot be modified.");
    return *m_commandContext;
}

bool VulkanParallelCommandRecording::Close()
{
    if (m_closed)
    {
        return true;
    }
    CheckVk(
        vkEndCommandBuffer(m_commandBuffer),
        "Failed to close a Vulkan parallel command buffer.");
    m_closed = true;
    return true;
}

CommandQueueType
VulkanParallelCommandRecording::GetQueue() const
{
    return m_queue;
}

VkCommandBuffer
VulkanParallelCommandRecording::
    GetCommandBuffer() const
{
    Core::Check(
        m_closed,
        "A Vulkan parallel command buffer must be closed before submission.");
    return m_commandBuffer;
}

VkCommandPool
VulkanParallelCommandRecording::
    ReleaseCommandPool()
{
    Core::Check(
        m_closed
            && m_commandPool != VK_NULL_HANDLE,
        "A Vulkan parallel command pool can only be released once after closing.");
    const VkCommandPool commandPool =
        std::exchange(
            m_commandPool,
            VK_NULL_HANDLE);
    m_commandBuffer = VK_NULL_HANDLE;
    return commandPool;
}
} // namespace Prism::RHI::Vulkan
