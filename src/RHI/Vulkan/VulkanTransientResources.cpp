#include "RHI/Vulkan/VulkanTransientResources.h"

#include "Core/Assert.h"
#include "RHI/Vulkan/VulkanContext.h"
#include "RHI/Vulkan/VulkanResources.h"
#include "RHI/Vulkan/VulkanTypeConversions.h"

#include <algorithm>
#include <array>
#include <stdexcept>
#include <unordered_map>
#include <utility>

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

struct MemoryOwner
{
    VulkanContext* context = nullptr;
    VkDeviceMemory memory = VK_NULL_HANDLE;

    ~MemoryOwner()
    {
        if (context != nullptr
            && memory != VK_NULL_HANDLE)
        {
            vkFreeMemory(
                context->GetDevice(),
                memory,
                nullptr);
        }
    }
};

struct PendingImage
{
    const TransientTextureRequest* request = nullptr;
    VkImage image = VK_NULL_HANDLE;
    VkMemoryRequirements requirements{};
};

struct PendingBuffer
{
    const TransientBufferRequest* request = nullptr;
    VkBuffer buffer = VK_NULL_HANDLE;
    VkMemoryRequirements requirements{};
};

struct Slot
{
    std::shared_ptr<MemoryOwner> owner;
    VkDeviceSize bytes = 0;
    std::uint32_t memoryTypeBits = 0;
};

VkImageCreateInfo BuildImageCreateInfo(
    const VulkanContext& context,
    const TextureDescription& description)
{
    VkImageCreateInfo info{
        VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    info.imageType = VK_IMAGE_TYPE_2D;
    info.flags = VK_IMAGE_CREATE_ALIAS_BIT;
    if (description.dimension
        == TextureDimension::TextureCube)
    {
        info.flags |=
            VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
    }
    info.extent = {
        description.width,
        description.height,
        1};
    info.mipLevels = description.mipLevels;
    info.arrayLayers = description.arrayLayers;
    info.format = ToNativeFormat(description.format);
    info.tiling = VK_IMAGE_TILING_OPTIMAL;
    info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    info.usage = ToNativeImageUsage(description.usage);
    info.samples =
        ToNativeSampleCount(description.sampleCount);
    const std::uint32_t graphicsFamily =
        context.GetGraphicsQueueFamilyIndex();
    const std::uint32_t computeFamily =
        context.GetComputeQueueFamilyIndex();
    static thread_local std::array<std::uint32_t, 2>
        queueFamilies{};
    if (graphicsFamily != computeFamily)
    {
        queueFamilies = {
            graphicsFamily,
            computeFamily};
        info.sharingMode =
            VK_SHARING_MODE_CONCURRENT;
        info.queueFamilyIndexCount = 2;
        info.pQueueFamilyIndices =
            queueFamilies.data();
    }
    else
    {
        info.sharingMode =
            VK_SHARING_MODE_EXCLUSIVE;
    }
    return info;
}

VkBufferUsageFlags ToBufferUsage(
    const BufferUsage usage)
{
    VkBufferUsageFlags result = 0;
    if (HasAnyFlag(usage, BufferUsage::Vertex))
        result |= VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
    if (HasAnyFlag(usage, BufferUsage::Index))
        result |= VK_BUFFER_USAGE_INDEX_BUFFER_BIT;
    if (HasAnyFlag(usage, BufferUsage::Constant))
        result |= VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
    if (HasAnyFlag(usage, BufferUsage::Storage))
        result |= VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
    if (HasAnyFlag(
            usage,
            BufferUsage::ShaderResource))
        result |= VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
    if (HasAnyFlag(usage, BufferUsage::CopySource))
        result |= VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    if (HasAnyFlag(usage, BufferUsage::CopyDestination))
        result |= VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    if (HasAnyFlag(usage, BufferUsage::Indirect))
        result |= VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT;
    return result;
}

VkBufferCreateInfo BuildBufferCreateInfo(
    const VulkanContext& context,
    const BufferDescription& description)
{
    VkBufferCreateInfo info{
        VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    info.size = description.size;
    info.usage = ToBufferUsage(description.usage);
    const std::uint32_t graphicsFamily =
        context.GetGraphicsQueueFamilyIndex();
    const std::uint32_t computeFamily =
        context.GetComputeQueueFamilyIndex();
    static thread_local std::array<std::uint32_t, 2>
        queueFamilies{};
    if (graphicsFamily != computeFamily)
    {
        queueFamilies = {
            graphicsFamily,
            computeFamily};
        info.sharingMode =
            VK_SHARING_MODE_CONCURRENT;
        info.queueFamilyIndexCount = 2;
        info.pQueueFamilyIndices =
            queueFamilies.data();
    }
    else
    {
        info.sharingMode =
            VK_SHARING_MODE_EXCLUSIVE;
    }
    return info;
}

VkImageView CreateDefaultView(
    VulkanContext& context,
    const VkImage image,
    const TextureDescription& description)
{
    VkImageViewCreateInfo info{
        VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    info.image = image;
    info.viewType =
        description.dimension
            == TextureDimension::TextureCube
        ? (description.arrayLayers > 6
               ? VK_IMAGE_VIEW_TYPE_CUBE_ARRAY
               : VK_IMAGE_VIEW_TYPE_CUBE)
        : (description.arrayLayers > 1
               ? VK_IMAGE_VIEW_TYPE_2D_ARRAY
               : VK_IMAGE_VIEW_TYPE_2D);
    info.format = ToNativeFormat(description.format);
    info.subresourceRange.aspectMask =
        IsDepthFormat(description.format)
        ? VK_IMAGE_ASPECT_DEPTH_BIT
        : VK_IMAGE_ASPECT_COLOR_BIT;
    info.subresourceRange.levelCount =
        description.mipLevels;
    info.subresourceRange.layerCount =
        description.arrayLayers;
    VkImageView view = VK_NULL_HANDLE;
    CheckVk(
        vkCreateImageView(
            context.GetDevice(),
            &info,
            nullptr,
            &view),
        "Failed to create a Vulkan transient texture view.");
    return view;
}
} // namespace

VulkanTransientTexturePool::VulkanTransientTexturePool(
    VulkanContext& context,
    const std::vector<TransientTextureRequest>& requests)
{
    std::string validationError;
    const bool requestsValid =
        ValidateTransientTextureRequests(
            requests,
            &validationError);
    Core::Check(
        requestsValid,
        validationError);

    m_statistics.poolId = AllocateTransientPoolId();
    m_statistics.textureCount = requests.size();

    std::vector<PendingImage> pendingImages;
    pendingImages.reserve(requests.size());
    std::unordered_map<std::size_t, Slot> slots;
    for (const TransientTextureRequest& request : requests)
    {
        PendingImage pending{};
        pending.request = &request;
        const VkImageCreateInfo imageInfo =
            BuildImageCreateInfo(
                context,
                request.description);
        CheckVk(
            vkCreateImage(
                context.GetDevice(),
                &imageInfo,
                nullptr,
                &pending.image),
            "Failed to create a Vulkan aliased image.");
        vkGetImageMemoryRequirements(
            context.GetDevice(),
            pending.image,
            &pending.requirements);
        Core::Check(
            pending.requirements.size > 0,
            "Vulkan returned invalid transient image memory requirements.");
        m_statistics.logicalBytes +=
            pending.requirements.size;

        auto [slot, inserted] = slots.try_emplace(
            request.allocationIndex);
        if (inserted)
        {
            slot->second.bytes =
                pending.requirements.size;
            slot->second.memoryTypeBits =
                pending.requirements.memoryTypeBits;
        }
        else
        {
            slot->second.bytes = std::max(
                slot->second.bytes,
                pending.requirements.size);
            slot->second.memoryTypeBits &=
                pending.requirements.memoryTypeBits;
            Core::Check(
                slot->second.memoryTypeBits != 0,
                "Aliased Vulkan images do not share a compatible memory type.");
        }
        pendingImages.push_back(pending);
    }

    for (auto& [allocationIndex, slot] : slots)
    {
        (void)allocationIndex;
        slot.owner = std::make_shared<MemoryOwner>();
        slot.owner->context = &context;
        VkMemoryAllocateInfo allocateInfo{
            VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        allocateInfo.allocationSize = slot.bytes;
        allocateInfo.memoryTypeIndex =
            context.FindMemoryType(
                slot.memoryTypeBits,
                VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        CheckVk(
            vkAllocateMemory(
                context.GetDevice(),
                &allocateInfo,
                nullptr,
                &slot.owner->memory),
            "Failed to allocate Vulkan transient image memory.");
        m_statistics.physicalBytes += slot.bytes;
    }

    m_statistics.allocationCount = slots.size();
    m_statistics.aliasedBytes =
        m_statistics.logicalBytes
            - m_statistics.physicalBytes;

    for (const PendingImage& pending : pendingImages)
    {
        const TransientTextureRequest& request =
            *pending.request;
        Slot& slot = slots.at(request.allocationIndex);
        CheckVk(
            vkBindImageMemory(
                context.GetDevice(),
                pending.image,
                slot.owner->memory,
                0),
            "Failed to bind Vulkan aliased image memory.");
        const VkImageView view = CreateDefaultView(
            context,
            pending.image,
            request.description);

        TransientTextureAllocationInfo info{};
        info.poolId = m_statistics.poolId;
        info.allocationIndex =
            request.allocationIndex;
        info.logicalBytes =
            pending.requirements.size;
        info.allocationBytes = slot.bytes;
        info.poolPhysicalBytes =
            m_statistics.physicalBytes;
        m_textures.emplace(
            request.name,
            std::make_shared<VulkanTexture>(
                context,
                request.description,
                pending.image,
                view,
                info,
                slot.owner));
    }
}

GraphicsApi
VulkanTransientTexturePool::GetGraphicsApi() const
{
    return GraphicsApi::Vulkan;
}

std::shared_ptr<ITexture>
VulkanTransientTexturePool::GetTexture(
    const std::string_view name) const
{
    const auto found = m_textures.find(std::string(name));
    Core::Check(
        found != m_textures.end(),
        "A Vulkan transient texture was not found.");
    return found->second;
}

const TransientTexturePoolStatistics&
VulkanTransientTexturePool::GetStatistics() const
{
    return m_statistics;
}

VulkanTransientBufferPool::VulkanTransientBufferPool(
    VulkanContext& context,
    const std::vector<TransientBufferRequest>& requests)
{
    std::string validationError;
    const bool requestsValid =
        ValidateTransientBufferRequests(
            requests,
            &validationError);
    Core::Check(
        requestsValid,
        validationError);

    m_statistics.poolId = AllocateTransientPoolId();
    m_statistics.bufferCount = requests.size();

    std::vector<PendingBuffer> pendingBuffers;
    pendingBuffers.reserve(requests.size());
    std::unordered_map<std::size_t, Slot> slots;
    for (const TransientBufferRequest& request : requests)
    {
        PendingBuffer pending{};
        pending.request = &request;
        const VkBufferCreateInfo bufferInfo =
            BuildBufferCreateInfo(
                context,
                request.description);
        CheckVk(
            vkCreateBuffer(
                context.GetDevice(),
                &bufferInfo,
                nullptr,
                &pending.buffer),
            "Failed to create a Vulkan aliased buffer.");
        vkGetBufferMemoryRequirements(
            context.GetDevice(),
            pending.buffer,
            &pending.requirements);
        Core::Check(
            pending.requirements.size > 0,
            "Vulkan returned invalid transient buffer memory requirements.");
        m_statistics.logicalBytes +=
            pending.requirements.size;

        auto [slot, inserted] = slots.try_emplace(
            request.allocationIndex);
        if (inserted)
        {
            slot->second.bytes =
                pending.requirements.size;
            slot->second.memoryTypeBits =
                pending.requirements.memoryTypeBits;
        }
        else
        {
            slot->second.bytes = std::max(
                slot->second.bytes,
                pending.requirements.size);
            slot->second.memoryTypeBits &=
                pending.requirements.memoryTypeBits;
            Core::Check(
                slot->second.memoryTypeBits != 0,
                "Aliased Vulkan buffers do not share a compatible memory type.");
        }
        pendingBuffers.push_back(pending);
    }

    for (auto& [allocationIndex, slot] : slots)
    {
        (void)allocationIndex;
        slot.owner = std::make_shared<MemoryOwner>();
        slot.owner->context = &context;
        VkMemoryAllocateInfo allocateInfo{
            VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        allocateInfo.allocationSize = slot.bytes;
        allocateInfo.memoryTypeIndex =
            context.FindMemoryType(
                slot.memoryTypeBits,
                VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        CheckVk(
            vkAllocateMemory(
                context.GetDevice(),
                &allocateInfo,
                nullptr,
                &slot.owner->memory),
            "Failed to allocate Vulkan transient buffer memory.");
        m_statistics.physicalBytes += slot.bytes;
    }

    m_statistics.allocationCount = slots.size();
    m_statistics.aliasedBytes =
        m_statistics.logicalBytes
            - m_statistics.physicalBytes;

    for (const PendingBuffer& pending : pendingBuffers)
    {
        const TransientBufferRequest& request =
            *pending.request;
        Slot& slot = slots.at(request.allocationIndex);
        CheckVk(
            vkBindBufferMemory(
                context.GetDevice(),
                pending.buffer,
                slot.owner->memory,
                0),
            "Failed to bind Vulkan aliased buffer memory.");

        TransientBufferAllocationInfo info{};
        info.poolId = m_statistics.poolId;
        info.allocationIndex =
            request.allocationIndex;
        info.logicalBytes =
            pending.requirements.size;
        info.allocationBytes = slot.bytes;
        info.poolPhysicalBytes =
            m_statistics.physicalBytes;
        m_buffers.emplace(
            request.name,
            std::make_shared<VulkanBuffer>(
                context,
                request.description,
                pending.buffer,
                info,
                slot.owner));
    }
}

GraphicsApi
VulkanTransientBufferPool::GetGraphicsApi() const
{
    return GraphicsApi::Vulkan;
}

std::shared_ptr<IBuffer>
VulkanTransientBufferPool::GetBuffer(
    const std::string_view name) const
{
    const auto found = m_buffers.find(std::string(name));
    Core::Check(
        found != m_buffers.end(),
        "A Vulkan transient buffer was not found.");
    return found->second;
}

const TransientBufferPoolStatistics&
VulkanTransientBufferPool::GetStatistics() const
{
    return m_statistics;
}
} // namespace Prism::RHI::Vulkan
