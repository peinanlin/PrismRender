#include "RHI/Vulkan/VulkanContext.h"

#include "Core/Assert.h"

#include <algorithm>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>

namespace Prism::RHI::Vulkan
{
namespace
{
constexpr VkDeviceSize UploadPageSize =
    16ull * 1024ull * 1024ull;

VkDeviceSize AlignUp(
    const VkDeviceSize value,
    const VkDeviceSize alignment)
{
    Core::Check(
        alignment > 0,
        "Upload allocation alignment must be positive.");
    return ((value + alignment - 1) / alignment) * alignment;
}

void CheckVk(const VkResult result, const char* message)
{
    if (result != VK_SUCCESS)
    {
        throw std::runtime_error(
            std::string(message) + " VkResult="
            + std::to_string(static_cast<int>(result)));
    }
}
}

VulkanContext::UploadAllocation VulkanContext::AllocateUpload(
    const std::uint64_t byteCount,
    const std::uint64_t alignment)
{
    Core::Check(
        byteCount > 0,
        "Vulkan upload allocations must contain bytes.");
    std::scoped_lock lock(m_uploadMutex);
    std::uint64_t completedTicket = 0;
    if (m_uploadTimelineSemaphore != VK_NULL_HANDLE)
    {
        CheckVk(
            vkGetSemaphoreCounterValue(
                m_device,
                m_uploadTimelineSemaphore,
                &completedTicket),
            "Failed to query the Vulkan upload timeline.");
    }

    const auto allocateFromPage =
        [&](UploadPage& page,
            const std::uint32_t pageIndex,
            const bool resetPage)
            -> std::optional<UploadAllocation>
        {
            if (resetPage)
            {
                page.cursor = 0;
                page.lastTicket = 0;
            }
            const VkDeviceSize offset =
                AlignUp(page.cursor, alignment);
            if (offset > page.capacity
                || byteCount > page.capacity - offset)
            {
                return std::nullopt;
            }
            page.cursor = offset + byteCount;
            page.pending = true;
            return UploadAllocation{
                page.buffer,
                page.memory,
                offset,
                byteCount,
                page.cpuAddress + offset,
                pageIndex};
        };

    for (std::uint32_t pageIndex = 0;
         pageIndex < m_uploadPages.size();
         ++pageIndex)
    {
        UploadPage& page = m_uploadPages[pageIndex];
        if (!page.pending)
        {
            continue;
        }
        if (auto allocation = allocateFromPage(
                page,
                pageIndex,
                false))
        {
            return *allocation;
        }
    }
    for (std::uint32_t pageIndex = 0;
         pageIndex < m_uploadPages.size();
         ++pageIndex)
    {
        UploadPage& page = m_uploadPages[pageIndex];
        if (page.pending || page.lastTicket > completedTicket)
        {
            continue;
        }
        if (auto allocation = allocateFromPage(
                page,
                pageIndex,
                true))
        {
            return *allocation;
        }
    }

    UploadPage page{};
    page.capacity = std::max<VkDeviceSize>(
        UploadPageSize,
        AlignUp(byteCount, 64ull * 1024ull));
    CreateBuffer(
        page.capacity,
        VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT
            | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
        page.buffer,
        page.memory);
    void* mapped = nullptr;
    CheckVk(
        vkMapMemory(
            m_device,
            page.memory,
            0,
            page.capacity,
            0,
            &mapped),
        "Failed to map a persistent Vulkan upload page.");
    page.cpuAddress = static_cast<std::byte*>(mapped);
    const VkDeviceSize capacity = page.capacity;
    m_uploadPages.push_back(std::move(page));
    m_uploadQueueStatistics.stagingPageCount =
        static_cast<std::uint32_t>(m_uploadPages.size());
    m_uploadQueueStatistics.stagingCapacityBytes += capacity;
    const std::uint32_t pageIndex =
        static_cast<std::uint32_t>(m_uploadPages.size() - 1);
    return *allocateFromPage(
        m_uploadPages.back(),
        pageIndex,
        false);
}

void VulkanContext::QueueUpload(
    const UploadAllocation& allocation,
    const std::function<void(VkCommandBuffer)>& recordCommands)
{
    Core::Check(
        allocation.buffer != VK_NULL_HANDLE
            && allocation.memory != VK_NULL_HANDLE
            && allocation.size > 0
            && allocation.cpuAddress != nullptr
            && static_cast<bool>(recordCommands),
        "Vulkan upload batches require staging storage, bytes, and commands.");
    std::scoped_lock lock(m_uploadMutex);
    Core::Check(
        allocation.pageIndex < m_uploadPages.size(),
        "Vulkan upload allocation references an invalid staging page.");
    if (m_pendingUploadCommandBuffer == VK_NULL_HANDLE)
    {
        m_pendingUploadTicket.value = m_nextUploadTicket++;
        VkCommandPoolCreateInfo poolInfo{
            VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        poolInfo.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
        poolInfo.queueFamilyIndex = m_queueFamilies.transfer;
        CheckVk(
            vkCreateCommandPool(
                m_device,
                &poolInfo,
                nullptr,
                &m_pendingUploadCommandPool),
            "Failed to create the Vulkan upload command pool.");
        VkCommandBufferAllocateInfo allocateInfo{
            VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        allocateInfo.commandPool = m_pendingUploadCommandPool;
        allocateInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        allocateInfo.commandBufferCount = 1;
        CheckVk(
            vkAllocateCommandBuffers(
                m_device,
                &allocateInfo,
                &m_pendingUploadCommandBuffer),
            "Failed to allocate the Vulkan upload command buffer.");
        VkCommandBufferBeginInfo beginInfo{
            VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        beginInfo.flags =
            VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        CheckVk(
            vkBeginCommandBuffer(
                m_pendingUploadCommandBuffer,
                &beginInfo),
            "Failed to begin the Vulkan upload command buffer.");
    }

    recordCommands(m_pendingUploadCommandBuffer);
    ++m_uploadQueueStatistics.pendingOperationCount;
    m_uploadQueueStatistics.pendingBytes += allocation.size;
    std::uint64_t stagingUsage = 0;
    for (const UploadPage& page : m_uploadPages)
    {
        stagingUsage += page.cursor;
    }
    m_uploadQueueStatistics.stagingHighWatermarkBytes =
        std::max(
            m_uploadQueueStatistics.stagingHighWatermarkBytes,
            stagingUsage);
    m_uploadQueueStatistics.pendingTicket =
        m_pendingUploadTicket.value;
}

void VulkanContext::FlushPendingUploads(
    const bool waitForCompletion)
{
    std::scoped_lock lock(m_uploadMutex);
    if (m_pendingUploadCommandBuffer == VK_NULL_HANDLE)
    {
        return;
    }
    CheckVk(
        vkEndCommandBuffer(m_pendingUploadCommandBuffer),
        "Failed to end the Vulkan upload command buffer.");

    VkFence fence = VK_NULL_HANDLE;
    if (waitForCompletion)
    {
        const VkFenceCreateInfo fenceInfo{
            VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        CheckVk(
            vkCreateFence(
                m_device,
                &fenceInfo,
                nullptr,
                &fence),
            "Failed to create the Vulkan upload fence.");
    }
    VkSubmitInfo submitInfo{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    const UploadTicket submittedTicket = m_pendingUploadTicket;
    Core::Check(
        submittedTicket.IsValid()
            && m_uploadTimelineSemaphore != VK_NULL_HANDLE,
        "Vulkan upload submission requires a valid ticket and timeline semaphore.");
    VkTimelineSemaphoreSubmitInfo timelineInfo{
        VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO};
    timelineInfo.signalSemaphoreValueCount = 1;
    timelineInfo.pSignalSemaphoreValues = &submittedTicket.value;
    submitInfo.pNext = &timelineInfo;
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers = &m_pendingUploadCommandBuffer;
    submitInfo.signalSemaphoreCount = 1;
    submitInfo.pSignalSemaphores = &m_uploadTimelineSemaphore;
    CheckVk(
        vkQueueSubmit(m_transferQueue, 1, &submitInfo, fence),
        "Failed to submit the Vulkan transfer-upload batch.");
    for (UploadPage& page : m_uploadPages)
    {
        if (page.pending)
        {
            page.pending = false;
            page.lastTicket = submittedTicket.value;
        }
    }

    const std::uint64_t batchBytes =
        m_uploadQueueStatistics.pendingBytes;
    m_uploadQueueStatistics.uploadedBytes += batchBytes;
    ++m_uploadQueueStatistics.submittedBatchCount;
    m_uploadQueueStatistics.maximumBatchBytes =
        std::max(
            m_uploadQueueStatistics.maximumBatchBytes,
            batchBytes);
    m_uploadQueueStatistics.pendingOperationCount = 0;
    m_uploadQueueStatistics.pendingBytes = 0;
    m_uploadQueueStatistics.pendingTicket = 0;
    m_uploadQueueStatistics.lastSubmittedTicket =
        submittedTicket.value;
    m_pendingUploadTicket = {};

    if (waitForCompletion)
    {
        ++m_uploadQueueStatistics.synchronousFlushCount;
        CheckVk(
            vkWaitForFences(
                m_device,
                1,
                &fence,
                VK_TRUE,
                UINT64_MAX),
            "Failed to wait for the Vulkan upload batch.");
        vkDestroyFence(m_device, fence, nullptr);
        for (const RetiredUpload& upload : m_pendingUploads)
        {
            vkDestroyBuffer(m_device, upload.buffer, nullptr);
            vkFreeMemory(m_device, upload.memory, nullptr);
        }
        vkDestroyCommandPool(
            m_device,
            m_pendingUploadCommandPool,
            nullptr);
    }
    else
    {
        FrameContext& frame = m_frames[m_currentFrame];
        frame.pendingUploadWaitValue = std::max(
            frame.pendingUploadWaitValue,
            submittedTicket.value);
        frame.retiredUploads.insert(
            frame.retiredUploads.end(),
            m_pendingUploads.begin(),
            m_pendingUploads.end());
        frame.retiredCommandPools.push_back(
            m_pendingUploadCommandPool);
    }

    m_pendingUploads.clear();
    m_pendingUploadCommandPool = VK_NULL_HANDLE;
    m_pendingUploadCommandBuffer = VK_NULL_HANDLE;
}
} // namespace Prism::RHI::Vulkan
