#include "RHI/Vulkan/VulkanResources.h"

#include "Core/Assert.h"
#include "RHI/RayTracing.h"
#include "RHI/Vulkan/VulkanContext.h"
#include "RHI/Vulkan/VulkanTypeConversions.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <stdexcept>
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
        throw std::runtime_error(std::string(message) + " VkResult=" + std::to_string(static_cast<int>(result)));
    }
}

VkBufferUsageFlags ToNativeBufferUsage(const BufferUsage usage)
{
    VkBufferUsageFlags result = 0;
    if (HasAnyFlag(usage, BufferUsage::Vertex)) result |= VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
    if (HasAnyFlag(usage, BufferUsage::Index)) result |= VK_BUFFER_USAGE_INDEX_BUFFER_BIT;
    if (HasAnyFlag(usage, BufferUsage::Constant)) result |= VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
    if (HasAnyFlag(usage, BufferUsage::Storage)) result |= VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
    if (HasAnyFlag(usage, BufferUsage::ShaderResource)) result |= VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
    if (HasAnyFlag(usage, BufferUsage::CopySource)) result |= VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    if (HasAnyFlag(usage, BufferUsage::CopyDestination)) result |= VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    if (HasAnyFlag(usage, BufferUsage::Indirect)) result |= VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT;
    if (HasAnyFlag(
            usage,
            BufferUsage::AccelerationStructureStorage))
    {
        result |=
            VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR
            | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
    }
    if (HasAnyFlag(
            usage,
            BufferUsage::AccelerationStructureBuildInput))
    {
        result |=
            VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR
            | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
    }
    if (HasAnyFlag(
            usage,
            BufferUsage::ShaderBindingTable))
    {
        result |=
            VK_BUFFER_USAGE_SHADER_BINDING_TABLE_BIT_KHR
            | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
    }
    if (HasAnyFlag(
            usage,
            BufferUsage::AccelerationStructureScratch))
    {
        result |=
            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT
            | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
    }
    return result;
}

VkDescriptorType ToNativeDescriptorType(const DescriptorType type)
{
    switch (type)
    {
    case DescriptorType::ConstantBuffer: return VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    case DescriptorType::DynamicConstantBuffer: return VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
    case DescriptorType::SampledTexture: return VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    case DescriptorType::AccelerationStructure: return VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
    case DescriptorType::ReadOnlyStorageBuffer: return VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    case DescriptorType::Sampler: return VK_DESCRIPTOR_TYPE_SAMPLER;
    case DescriptorType::StorageBuffer: return VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    case DescriptorType::StorageTexture: return VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    }
    throw std::invalid_argument("Unsupported Vulkan descriptor type.");
}

VkShaderStageFlags ToNativeShaderStages(const ShaderStageFlags stages)
{
    VkShaderStageFlags result = 0;
    if (HasAnyFlag(stages, ShaderStageFlags::Vertex)) result |= VK_SHADER_STAGE_VERTEX_BIT;
    if (HasAnyFlag(stages, ShaderStageFlags::Hull)) result |= VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT;
    if (HasAnyFlag(stages, ShaderStageFlags::Domain)) result |= VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT;
    if (HasAnyFlag(stages, ShaderStageFlags::Pixel)) result |= VK_SHADER_STAGE_FRAGMENT_BIT;
    if (HasAnyFlag(stages, ShaderStageFlags::Compute)) result |= VK_SHADER_STAGE_COMPUTE_BIT;
    if (HasAnyFlag(stages, ShaderStageFlags::RayGeneration)) result |= VK_SHADER_STAGE_RAYGEN_BIT_KHR;
    if (HasAnyFlag(stages, ShaderStageFlags::AnyHit)) result |= VK_SHADER_STAGE_ANY_HIT_BIT_KHR;
    if (HasAnyFlag(stages, ShaderStageFlags::ClosestHit)) result |= VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR;
    if (HasAnyFlag(stages, ShaderStageFlags::Miss)) result |= VK_SHADER_STAGE_MISS_BIT_KHR;
    if (HasAnyFlag(stages, ShaderStageFlags::Intersection)) result |= VK_SHADER_STAGE_INTERSECTION_BIT_KHR;
    if (HasAnyFlag(stages, ShaderStageFlags::Callable)) result |= VK_SHADER_STAGE_CALLABLE_BIT_KHR;
    return result;
}
} // namespace

VulkanBuffer::VulkanBuffer(
    VulkanContext& context,
    const BufferDescription& description,
    const void* initialData)
    : m_context(&context)
    , m_description(description)
{
    std::string validationError;
    Core::Check(ValidateBufferDescription(description, initialData, &validationError), validationError);

    VkBufferUsageFlags usage = ToNativeBufferUsage(description.usage);
    const bool deviceLocal = description.memoryAccess == MemoryAccess::GpuOnly;
    if (deviceLocal && initialData != nullptr)
    {
        usage |= VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    }

    const VkMemoryPropertyFlags memoryProperties = deviceLocal
                                                       ? VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT
                                                       : VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    const bool requiresDeviceAddress =
        HasAnyFlag(
            description.usage,
            BufferUsage::AccelerationStructureStorage
                | BufferUsage::AccelerationStructureBuildInput
                | BufferUsage::ShaderBindingTable
                | BufferUsage::AccelerationStructureScratch);
    context.CreateBuffer(
        description.size,
        usage,
        memoryProperties,
        m_buffer,
        m_memory,
        requiresDeviceAddress);

    if (!deviceLocal)
    {
        CheckVk(vkMapMemory(context.GetDevice(), m_memory, 0, description.size, 0, &m_mappedData), "Failed to map Vulkan RHI buffer.");
        if (initialData != nullptr)
        {
            std::memcpy(m_mappedData, initialData, description.size);
        }
        return;
    }

    if (initialData != nullptr)
    {
        const VulkanContext::UploadAllocation
            upload = context.AllocateUpload(
            description.size,
            16);
        std::memcpy(
            upload.cpuAddress,
            initialData,
            description.size);
        context.QueueUpload(
            upload,
            [&](const VkCommandBuffer commandBuffer)
        {
            const VkBufferCopy region{
                upload.offset,
                0,
                description.size};
            vkCmdCopyBuffer(
                commandBuffer,
                upload.buffer,
                m_buffer,
                1,
                &region);
        });
    }
}

VulkanBuffer::~VulkanBuffer()
{
    if (m_context == nullptr) return;
    if (m_mappedData != nullptr) vkUnmapMemory(m_context->GetDevice(), m_memory);
    const VkBuffer buffer = std::exchange(
        m_buffer,
        VK_NULL_HANDLE);
    const VkDeviceMemory memory = std::exchange(
        m_memory,
        VK_NULL_HANDLE);
    std::shared_ptr<void> allocationOwner =
        std::move(m_allocationOwner);
    m_context->RetireGpuObject(
        [buffer,
         memory,
         allocationOwner = std::move(
             allocationOwner)](const VkDevice device)
        {
            if (buffer != VK_NULL_HANDLE)
            {
                vkDestroyBuffer(
                    device,
                    buffer,
                    nullptr);
            }
            if (memory != VK_NULL_HANDLE)
            {
                vkFreeMemory(
                    device,
                    memory,
                    nullptr);
            }
        });
}

VulkanBuffer::VulkanBuffer(
    VulkanContext& context,
    const BufferDescription& description,
    const VkBuffer buffer,
    TransientBufferAllocationInfo allocationInfo,
    std::shared_ptr<void> allocationOwner)
    : m_context(&context)
    , m_description(description)
    , m_buffer(buffer)
    , m_transientAllocationInfo(allocationInfo)
    , m_allocationOwner(std::move(allocationOwner))
{
    std::string validationError;
    Core::Check(
        ValidateBufferDescription(
            description,
            &description,
            &validationError),
        validationError);
    Core::Check(
        description.memoryAccess == MemoryAccess::GpuOnly,
        "Vulkan transient buffers must use GPU-only memory.");
    Core::Check(
        buffer != VK_NULL_HANDLE,
        "Vulkan transient buffers require a valid buffer handle.");
    Core::Check(
        allocationInfo.poolId != 0
            && allocationInfo.allocationBytes > 0
            && allocationInfo.poolPhysicalBytes > 0,
        "Vulkan transient buffers require valid allocation metadata.");
    Core::Check(
        m_allocationOwner != nullptr,
        "Vulkan transient buffers require a memory owner.");
}

GraphicsApi VulkanBuffer::GetGraphicsApi() const { return GraphicsApi::Vulkan; }
const BufferDescription& VulkanBuffer::GetDescription() const { return m_description; }
const TransientBufferAllocationInfo*
VulkanBuffer::GetTransientAllocationInfo() const
{
    return m_transientAllocationInfo.has_value()
        ? &*m_transientAllocationInfo
        : nullptr;
}
VkBuffer VulkanBuffer::GetHandle() const { return m_buffer; }

void VulkanBuffer::Update(const void* data, const std::size_t size, const std::size_t offset)
{
    Core::Check(data != nullptr, "Vulkan buffer updates require source data.");
    Core::Check(offset + size <= m_description.size, "Vulkan buffer update exceeds its allocation.");
    Core::Check(m_mappedData != nullptr, "GPU-only Vulkan buffers require an upload command instead of Update().");
    std::memcpy(static_cast<std::byte*>(m_mappedData) + offset, data, size);
}

void VulkanBuffer::Read(
    void* data,
    const std::size_t size,
    const std::size_t offset) const
{
    Core::Check(
        data != nullptr && offset + size <= m_description.size,
        "Vulkan buffer read range is invalid.");
    Core::Check(
        m_description.memoryAccess == MemoryAccess::GpuToCpu
            && m_mappedData != nullptr,
        "Only GPU-to-CPU Vulkan buffers can be read directly.");
    std::memcpy(
        data,
        static_cast<const std::byte*>(m_mappedData) + offset,
        size);
}

VulkanTexture::VulkanTexture(
    VulkanContext& context,
    const TextureDescription& description,
    const TextureInitialData* initialData)
    : m_context(&context)
    , m_description(description)
{
    std::string validationError;
    Core::Check(ValidateTextureDescription(description, &validationError), validationError);
    if (initialData != nullptr)
    {
        Core::Check(description.mipLevels == 1,
                    "Vulkan initial texture uploads currently support one mip level.");
    }

    VkImageCreateInfo imageInfo{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.flags = description.dimension == TextureDimension::TextureCube
                          ? VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT
                          : 0u;
    imageInfo.extent = {description.width, description.height, 1};
    imageInfo.mipLevels = description.mipLevels;
    imageInfo.arrayLayers = description.arrayLayers;
    imageInfo.format = ToNativeFormat(description.format);
    imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    imageInfo.usage = ToNativeImageUsage(description.usage)
                      | (initialData != nullptr ? VK_IMAGE_USAGE_TRANSFER_DST_BIT : 0u);
    imageInfo.samples = ToNativeSampleCount(description.sampleCount);
    std::array<std::uint32_t, 3> queueFamilies{};
    std::uint32_t queueFamilyCount = 0;
    for (const std::uint32_t family : {
             context.GetGraphicsQueueFamilyIndex(),
             context.GetComputeQueueFamilyIndex(),
             context.GetTransferQueueFamilyIndex()})
    {
        if (std::find(
                queueFamilies.begin(),
                queueFamilies.begin()
                    + queueFamilyCount,
                family)
            == queueFamilies.begin()
                + queueFamilyCount)
        {
            queueFamilies[queueFamilyCount++] =
                family;
        }
    }
    const bool crossesQueueFamilies =
        queueFamilyCount > 1;
    imageInfo.sharingMode =
        crossesQueueFamilies
        ? VK_SHARING_MODE_CONCURRENT
        : VK_SHARING_MODE_EXCLUSIVE;
    imageInfo.queueFamilyIndexCount =
        crossesQueueFamilies
        ? queueFamilyCount
        : 0u;
    imageInfo.pQueueFamilyIndices =
        crossesQueueFamilies
        ? queueFamilies.data()
        : nullptr;
    CheckVk(vkCreateImage(context.GetDevice(), &imageInfo, nullptr, &m_image), "Failed to create Vulkan RHI image.");

    VkMemoryRequirements memoryRequirements{};
    vkGetImageMemoryRequirements(context.GetDevice(), m_image, &memoryRequirements);
    VkMemoryAllocateInfo allocateInfo{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    allocateInfo.allocationSize = memoryRequirements.size;
    allocateInfo.memoryTypeIndex = context.FindMemoryType(memoryRequirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    CheckVk(vkAllocateMemory(context.GetDevice(), &allocateInfo, nullptr, &m_memory), "Failed to allocate Vulkan image memory.");
    CheckVk(vkBindImageMemory(context.GetDevice(), m_image, m_memory, 0), "Failed to bind Vulkan image memory.");

    const bool depthTexture = IsDepthFormat(description.format);
    VkImageViewCreateInfo viewInfo{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    viewInfo.image = m_image;
    viewInfo.viewType = description.dimension == TextureDimension::TextureCube
                            ? (description.arrayLayers > 6 ? VK_IMAGE_VIEW_TYPE_CUBE_ARRAY : VK_IMAGE_VIEW_TYPE_CUBE)
                            : (description.arrayLayers > 1 ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_2D);
    viewInfo.format = imageInfo.format;
    viewInfo.subresourceRange.aspectMask = depthTexture ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
    viewInfo.subresourceRange.levelCount = description.mipLevels;
    viewInfo.subresourceRange.layerCount = description.arrayLayers;
    CheckVk(vkCreateImageView(context.GetDevice(), &viewInfo, nullptr, &m_imageView), "Failed to create Vulkan RHI image view.");

    if (initialData == nullptr)
    {
        return;
    }

    Core::Check(initialData->data != nullptr, "Vulkan texture initial data cannot be null.");
    Core::Check(!depthTexture, "Initial upload for depth textures is not supported.");
    Core::Check(
        description.format == Format::Rgba8Unorm || description.format == Format::Rgba8UnormSrgb,
        "Stage 10 Vulkan initial texture uploads currently support RGBA8 formats only.");
    constexpr std::size_t BytesPerPixel = 4;
    const std::size_t minimumRowPitch = static_cast<std::size_t>(description.width) * BytesPerPixel;
    const std::size_t effectiveRowPitch = initialData->rowPitch == 0 ? minimumRowPitch : initialData->rowPitch;
    Core::Check(effectiveRowPitch >= minimumRowPitch && effectiveRowPitch % BytesPerPixel == 0,
                "Vulkan RGBA8 texture row pitch is invalid.");
    Core::Check(initialData->slicePitch >= effectiveRowPitch * description.height,
                "Vulkan texture slice pitch is smaller than the uploaded image.");
    const std::size_t uploadSize = initialData->slicePitch * description.arrayLayers;
    const VulkanContext::UploadAllocation upload =
        context.AllocateUpload(
        uploadSize,
        16);
    std::memcpy(
        upload.cpuAddress,
        initialData->data,
        uploadSize);

    context.QueueUpload(
        upload,
        [&](const VkCommandBuffer commandBuffer)
    {
        VkImageMemoryBarrier toTransfer{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        toTransfer.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toTransfer.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toTransfer.image = m_image;
        toTransfer.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        toTransfer.subresourceRange.levelCount = 1;
        toTransfer.subresourceRange.layerCount = description.arrayLayers;
        toTransfer.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        toTransfer.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        toTransfer.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             0, 0, nullptr, 0, nullptr, 1, &toTransfer);

        VkBufferImageCopy copy{};
        copy.bufferOffset = upload.offset;
        copy.bufferRowLength = initialData->rowPitch == 0
                                   ? 0u
                                   : static_cast<std::uint32_t>(effectiveRowPitch / BytesPerPixel);
        copy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        copy.imageSubresource.layerCount = description.arrayLayers;
        copy.imageExtent = {description.width, description.height, 1};
        vkCmdCopyBufferToImage(commandBuffer, upload.buffer, m_image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);

        VkImageMemoryBarrier toShaderRead = toTransfer;
        toShaderRead.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        toShaderRead.dstAccessMask = 0;
        toShaderRead.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        toShaderRead.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                             0, 0, nullptr, 0, nullptr, 1, &toShaderRead);
    });
}

VulkanTexture::VulkanTexture(
    VulkanContext& context,
    const TextureDescription& description,
    const VkImage externalImage)
    : m_context(&context)
    , m_description(description)
    , m_image(externalImage)
    , m_ownsImage(false)
{
    std::string validationError;
    Core::Check(
        ValidateTextureDescription(
            description,
            &validationError),
        validationError);
    Core::Check(
        externalImage != VK_NULL_HANDLE,
        "External Vulkan textures require a valid image.");
}

VulkanTexture::~VulkanTexture()
{
    if (m_context == nullptr || !m_ownsImage)
    {
        return;
    }
    const VkImageView imageView =
        std::exchange(
            m_imageView,
            VK_NULL_HANDLE);
    const VkImage image = std::exchange(
        m_image,
        VK_NULL_HANDLE);
    const VkDeviceMemory memory =
        std::exchange(
            m_memory,
            VK_NULL_HANDLE);
    std::shared_ptr<void> allocationOwner =
        std::move(m_allocationOwner);
    m_context->RetireGpuObject(
        [imageView,
         image,
         memory,
         allocationOwner = std::move(
             allocationOwner)](
            const VkDevice device)
        {
            if (imageView != VK_NULL_HANDLE)
            {
                vkDestroyImageView(
                    device,
                    imageView,
                    nullptr);
            }
            if (image != VK_NULL_HANDLE)
            {
                vkDestroyImage(
                    device,
                    image,
                    nullptr);
            }
            if (memory != VK_NULL_HANDLE)
            {
                vkFreeMemory(
                    device,
                    memory,
                    nullptr);
            }
        });
}

GraphicsApi VulkanTexture::GetGraphicsApi() const { return GraphicsApi::Vulkan; }
const TextureDescription& VulkanTexture::GetDescription() const { return m_description; }
const TransientTextureAllocationInfo*
VulkanTexture::GetTransientAllocationInfo() const
{
    return m_transientAllocationInfo.has_value()
        ? &*m_transientAllocationInfo
        : nullptr;
}
VkImage VulkanTexture::GetImage() const { return m_image; }
VkImageView VulkanTexture::GetImageView() const { return m_imageView; }

VulkanTexture::VulkanTexture(
    VulkanContext& context,
    const TextureDescription& description,
    const VkImage image,
    const VkImageView imageView,
    TransientTextureAllocationInfo allocationInfo,
    std::shared_ptr<void> allocationOwner)
    : m_context(&context)
    , m_description(description)
    , m_image(image)
    , m_imageView(imageView)
    , m_transientAllocationInfo(allocationInfo)
    , m_allocationOwner(std::move(allocationOwner))
{
    std::string validationError;
    Core::Check(
        ValidateTextureDescription(
            description,
            &validationError),
        validationError);
    Core::Check(
        image != VK_NULL_HANDLE
            && imageView != VK_NULL_HANDLE,
        "Vulkan transient textures require valid image handles.");
    Core::Check(
        allocationInfo.poolId != 0
            && allocationInfo.allocationBytes > 0
            && allocationInfo.poolPhysicalBytes > 0,
        "Vulkan transient textures require valid allocation metadata.");
    Core::Check(
        m_allocationOwner != nullptr,
        "Vulkan transient textures require a memory owner.");
}

VulkanTextureView::VulkanTextureView(
    VulkanContext& context,
    std::shared_ptr<VulkanTexture> texture,
    const TextureViewDescription& description)
    : m_context(&context)
    , m_texture(std::move(texture))
    , m_textureDescription(m_texture->GetDescription())
    , m_description(description)
    , m_image(m_texture->GetImage())
    , m_ownsImageView(true)
{
    std::string validationError;
    Core::Check(ValidateTextureViewDescription(m_textureDescription, description, &validationError),
                validationError);
    const Format viewFormat = description.format == Format::Unknown
                                  ? m_textureDescription.format
                                  : description.format;
    VkImageViewCreateInfo createInfo{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    createInfo.image = m_image;
    const bool cubeSampledView =
        m_textureDescription.dimension
                == TextureDimension::TextureCube
        && description.type
                == TextureViewType::Sampled;
    createInfo.viewType = cubeSampledView
                              ? (description.arrayLayerCount > 6
                                     ? VK_IMAGE_VIEW_TYPE_CUBE_ARRAY
                                     : VK_IMAGE_VIEW_TYPE_CUBE)
                              : (description.arrayLayerCount > 1
                                     ? VK_IMAGE_VIEW_TYPE_2D_ARRAY
                                     : VK_IMAGE_VIEW_TYPE_2D);
    createInfo.format = ToNativeFormat(viewFormat);
    createInfo.subresourceRange.aspectMask = IsDepthFormat(m_textureDescription.format)
                                                  ? VK_IMAGE_ASPECT_DEPTH_BIT
                                                  : VK_IMAGE_ASPECT_COLOR_BIT;
    createInfo.subresourceRange.baseMipLevel = description.baseMipLevel;
    createInfo.subresourceRange.levelCount = description.mipLevelCount;
    createInfo.subresourceRange.baseArrayLayer = description.baseArrayLayer;
    createInfo.subresourceRange.layerCount = description.arrayLayerCount;
    CheckVk(vkCreateImageView(context.GetDevice(), &createInfo, nullptr, &m_imageView),
            "Failed to create Vulkan texture view.");
}

VulkanTextureView::VulkanTextureView(
    VulkanContext& context,
    std::shared_ptr<VulkanTexture> texture,
    const VkImageView externalImageView,
    const TextureViewDescription& description)
    : m_context(&context)
    , m_texture(std::move(texture))
    , m_textureDescription(
          m_texture->GetDescription())
    , m_description(description)
    , m_image(m_texture->GetImage())
    , m_imageView(externalImageView)
{
    std::string validationError;
    Core::Check(
        ValidateTextureViewDescription(
            m_textureDescription,
            description,
            &validationError),
        validationError);
    Core::Check(
        m_image != VK_NULL_HANDLE
            && m_imageView != VK_NULL_HANDLE,
        "External Vulkan texture views require valid image handles.");
}

VulkanTextureView::VulkanTextureView(
    VulkanContext& context,
    const VkImage image,
    const VkImageView imageView,
    const TextureDescription& textureDescription,
    const TextureViewDescription& description)
    : m_context(&context)
    , m_textureDescription(textureDescription)
    , m_description(description)
    , m_image(image)
    , m_imageView(imageView)
{
    std::string validationError;
    Core::Check(ValidateTextureViewDescription(textureDescription, description, &validationError),
                validationError);
    Core::Check(image != VK_NULL_HANDLE && imageView != VK_NULL_HANDLE,
                "External Vulkan texture views require valid image handles.");
}

VulkanTextureView::~VulkanTextureView()
{
    if (m_ownsImageView && m_context != nullptr && m_imageView != VK_NULL_HANDLE)
    {
        const VkImageView imageView =
            std::exchange(
                m_imageView,
                VK_NULL_HANDLE);
        m_context->RetireGpuObject(
            [imageView](const VkDevice device)
            {
                vkDestroyImageView(
                    device,
                    imageView,
                    nullptr);
            });
    }
}

GraphicsApi VulkanTextureView::GetGraphicsApi() const { return GraphicsApi::Vulkan; }
const TextureViewDescription& VulkanTextureView::GetDescription() const { return m_description; }
const ITexture* VulkanTextureView::GetTexture() const { return m_texture.get(); }
const TextureDescription& VulkanTextureView::GetTextureDescription() const { return m_textureDescription; }
VkImage VulkanTextureView::GetImage() const { return m_image; }
VkImageView VulkanTextureView::GetHandle() const { return m_imageView; }

VulkanSampler::VulkanSampler(VulkanContext& context, const SamplerDescription& description)
    : m_context(&context)
    , m_description(description)
{
    VkSamplerCreateInfo createInfo{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    createInfo.magFilter = ToNativeFilter(description.filter);
    createInfo.minFilter = ToNativeFilter(description.filter);
    createInfo.mipmapMode = ToNativeMipmapMode(description.filter);
    createInfo.addressModeU = ToNativeAddressMode(description.addressU);
    createInfo.addressModeV = ToNativeAddressMode(description.addressV);
    createInfo.addressModeW = ToNativeAddressMode(description.addressW);
    createInfo.mipLodBias = description.mipLodBias;
    const bool useAnisotropy = description.filter == Filter::Anisotropic && context.SupportsSamplerAnisotropy();
    createInfo.anisotropyEnable = useAnisotropy ? VK_TRUE : VK_FALSE;
    createInfo.maxAnisotropy = useAnisotropy
                                   ? static_cast<float>(std::clamp(description.maxAnisotropy, 1u, 16u))
                                   : 1.0f;
    createInfo.compareEnable = description.filter == Filter::ComparisonLinear ? VK_TRUE : VK_FALSE;
    createInfo.compareOp = ToNativeCompareOperation(description.comparison);
    createInfo.minLod = description.minLod;
    createInfo.maxLod = description.maxLod;
    createInfo.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK;
    CheckVk(vkCreateSampler(context.GetDevice(), &createInfo, nullptr, &m_sampler), "Failed to create Vulkan sampler.");
}

VulkanSampler::~VulkanSampler()
{
    if (m_context != nullptr && m_sampler != VK_NULL_HANDLE)
    {
        const VkSampler sampler = std::exchange(
            m_sampler,
            VK_NULL_HANDLE);
        m_context->RetireGpuObject(
            [sampler](const VkDevice device)
            {
                vkDestroySampler(
                    device,
                    sampler,
                    nullptr);
            });
    }
}

GraphicsApi VulkanSampler::GetGraphicsApi() const { return GraphicsApi::Vulkan; }
const SamplerDescription& VulkanSampler::GetDescription() const { return m_description; }
VkSampler VulkanSampler::GetHandle() const { return m_sampler; }

VulkanDescriptorSetLayout::VulkanDescriptorSetLayout(
    VulkanContext& context,
    const DescriptorSetLayoutDescription& description)
    : m_context(&context)
    , m_description(description)
{
    std::string validationError;
    Core::Check(ValidateDescriptorSetLayoutDescription(description, &validationError), validationError);
    std::vector<VkDescriptorSetLayoutBinding> bindings;
    bindings.reserve(description.bindings.size());
    for (const DescriptorBindingDescription& binding : description.bindings)
    {
        const VkDescriptorType descriptorType = ToNativeDescriptorType(binding.type);
        bindings.push_back({binding.binding, descriptorType, binding.descriptorCount, ToNativeShaderStages(binding.stages), nullptr});
        m_bindingTypes.emplace(binding.binding, descriptorType);
        if (binding.type == DescriptorType::DynamicConstantBuffer)
        {
            m_dynamicBufferBindings.push_back(binding.binding);
        }
    }
    std::ranges::sort(m_dynamicBufferBindings);

    VkDescriptorSetLayoutCreateInfo createInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    createInfo.bindingCount = static_cast<std::uint32_t>(bindings.size());
    createInfo.pBindings = bindings.data();
    CheckVk(vkCreateDescriptorSetLayout(context.GetDevice(), &createInfo, nullptr, &m_layout), "Failed to create Vulkan descriptor-set layout.");
}

VulkanDescriptorSetLayout::~VulkanDescriptorSetLayout()
{
    if (m_context != nullptr && m_layout != VK_NULL_HANDLE)
    {
        const VkDescriptorSetLayout layout =
            std::exchange(
                m_layout,
                VK_NULL_HANDLE);
        m_context->RetireGpuObject(
            [layout](const VkDevice device)
            {
                vkDestroyDescriptorSetLayout(
                    device,
                    layout,
                    nullptr);
            });
    }
}

GraphicsApi VulkanDescriptorSetLayout::GetGraphicsApi() const { return GraphicsApi::Vulkan; }
const DescriptorSetLayoutDescription& VulkanDescriptorSetLayout::GetDescription() const { return m_description; }
VkDescriptorSetLayout VulkanDescriptorSetLayout::GetHandle() const { return m_layout; }

VkDescriptorType VulkanDescriptorSetLayout::GetBindingType(const std::uint32_t binding) const
{
    const auto found = m_bindingTypes.find(binding);
    if (found == m_bindingTypes.end())
    {
        throw std::invalid_argument(
            "Vulkan descriptor write references undeclared binding " + std::to_string(binding) + ".");
    }
    return found->second;
}

const std::vector<std::uint32_t>& VulkanDescriptorSetLayout::GetDynamicBufferBindings() const
{
    return m_dynamicBufferBindings;
}

VulkanDescriptorSet::VulkanDescriptorSet(
    VulkanContext& context,
    VulkanDescriptorAllocator& allocator,
    std::shared_ptr<VulkanDescriptorSetLayout> layout,
    const VkDescriptorPool descriptorPool,
    const VkDescriptorSet descriptorSet)
    : m_context(&context)
    , m_allocator(&allocator)
    , m_layout(std::move(layout))
    , m_descriptorPool(descriptorPool)
    , m_descriptorSet(descriptorSet)
{
}

VulkanDescriptorSet::~VulkanDescriptorSet()
{
    if (m_allocator != nullptr && m_descriptorSet != VK_NULL_HANDLE)
    {
        m_allocator->Retire(
            m_descriptorPool,
            m_descriptorSet);
    }
}

GraphicsApi VulkanDescriptorSet::GetGraphicsApi() const { return GraphicsApi::Vulkan; }
VkDescriptorSet VulkanDescriptorSet::GetHandle() const { return m_descriptorSet; }

void VulkanDescriptorSet::WriteBuffer(
    const std::uint32_t binding,
    std::shared_ptr<IBuffer> buffer,
    const std::size_t offset,
    const std::size_t range)
{
    auto nativeBuffer = std::dynamic_pointer_cast<VulkanBuffer>(buffer);
    Core::Check(nativeBuffer != nullptr, "Vulkan descriptor sets require Vulkan buffers.");
    const std::size_t effectiveRange = range == 0 ? nativeBuffer->GetDescription().size - offset : range;
    Core::Check(offset + effectiveRange <= nativeBuffer->GetDescription().size, "Vulkan descriptor buffer range is invalid.");
    const VkDescriptorType descriptorType = m_layout->GetBindingType(binding);
    Core::Check(
        descriptorType == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER
            || descriptorType == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC
            || descriptorType == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
        "WriteBuffer requires a buffer descriptor binding.");

    const VkDescriptorBufferInfo bufferInfo{nativeBuffer->GetHandle(), offset, effectiveRange};
    VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    write.dstSet = m_descriptorSet;
    write.dstBinding = binding;
    write.descriptorCount = 1;
    write.descriptorType = descriptorType;
    write.pBufferInfo = &bufferInfo;
    vkUpdateDescriptorSets(m_context->GetDevice(), 1, &write, 0, nullptr);
    m_bufferBindings[binding] = {nativeBuffer, offset, effectiveRange};
    m_boundResources[binding] = std::move(buffer);
}

std::vector<std::uint32_t> VulkanDescriptorSet::BuildDynamicOffsets(
    const std::span<const DynamicBufferOffset> dynamicOffsets) const
{
    const std::vector<std::uint32_t>& bindings = m_layout->GetDynamicBufferBindings();
    Core::Check(dynamicOffsets.size() <= bindings.size(),
                "Too many Vulkan dynamic constant-buffer offsets were supplied.");

    VkPhysicalDeviceProperties properties{};
    vkGetPhysicalDeviceProperties(m_context->GetPhysicalDevice(), &properties);
    const VkDeviceSize alignment = properties.limits.minUniformBufferOffsetAlignment;
    std::vector<std::uint32_t> nativeOffsets;
    nativeOffsets.reserve(bindings.size());
    for (const std::uint32_t binding : bindings)
    {
        const auto supplied = std::ranges::find_if(
            dynamicOffsets,
            [binding](const DynamicBufferOffset& candidate)
            {
                return candidate.binding == binding;
            });
        const std::uint32_t offset = supplied == dynamicOffsets.end() ? 0u : supplied->offset;
        const auto bufferBinding = m_bufferBindings.find(binding);
        Core::Check(bufferBinding != m_bufferBindings.end(),
                    "A Vulkan dynamic constant buffer was not written before binding.");
        Core::Check(alignment == 0 || (bufferBinding->second.offset + offset) % alignment == 0,
                    "A Vulkan dynamic constant-buffer offset does not satisfy device alignment.");
        Core::Check(bufferBinding->second.offset + offset + bufferBinding->second.range
                        <= bufferBinding->second.buffer->GetDescription().size,
                    "A Vulkan dynamic constant-buffer range exceeds its buffer.");
        nativeOffsets.push_back(offset);
    }
    for (std::size_t index = 0; index < dynamicOffsets.size(); ++index)
    {
        const DynamicBufferOffset& offset = dynamicOffsets[index];
        Core::Check(std::ranges::find(bindings, offset.binding) != bindings.end(),
                    "A Vulkan dynamic offset references a non-dynamic binding.");
        Core::Check(std::ranges::find_if(
                        dynamicOffsets.begin(), dynamicOffsets.begin() + index,
                        [offset](const DynamicBufferOffset& candidate)
                        {
                            return candidate.binding == offset.binding;
                        }) == dynamicOffsets.begin() + index,
                    "A Vulkan dynamic binding received more than one offset.");
    }
    return nativeOffsets;
}

void VulkanDescriptorSet::WriteTexture(const std::uint32_t binding, std::shared_ptr<ITexture> texture)
{
    auto nativeTexture = std::dynamic_pointer_cast<VulkanTexture>(texture);
    Core::Check(nativeTexture != nullptr, "Vulkan descriptor sets require Vulkan textures.");
    const VkDescriptorType descriptorType = m_layout->GetBindingType(binding);
    Core::Check(descriptorType == VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
                "WriteTexture currently requires a sampled-image descriptor binding.");
    const VkDescriptorImageInfo imageInfo{VK_NULL_HANDLE, nativeTexture->GetImageView(),
        IsDepthFormat(nativeTexture->GetDescription().format)
            ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    write.dstSet = m_descriptorSet;
    write.dstBinding = binding;
    write.descriptorCount = 1;
    write.descriptorType = descriptorType;
    write.pImageInfo = &imageInfo;
    vkUpdateDescriptorSets(m_context->GetDevice(), 1, &write, 0, nullptr);
    m_boundResources[binding] = std::move(texture);
}

void VulkanDescriptorSet::WriteTextureView(
    const std::uint32_t binding,
    std::shared_ptr<ITextureView> textureView)
{
    auto nativeView = std::dynamic_pointer_cast<VulkanTextureView>(textureView);
    Core::Check(nativeView != nullptr, "Vulkan descriptor sets require Vulkan texture views.");
    const VkDescriptorType descriptorType = m_layout->GetBindingType(binding);
    Core::Check(
        descriptorType == VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE || descriptorType == VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
        "WriteTextureView requires an image descriptor binding.");
    const VkImageLayout imageLayout = descriptorType == VK_DESCRIPTOR_TYPE_STORAGE_IMAGE
                                          ? VK_IMAGE_LAYOUT_GENERAL
                                          : IsDepthFormat(nativeView->GetTextureDescription().format)
                                              ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL
                                              : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    const VkDescriptorImageInfo imageInfo{VK_NULL_HANDLE, nativeView->GetHandle(), imageLayout};
    VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    write.dstSet = m_descriptorSet;
    write.dstBinding = binding;
    write.descriptorCount = 1;
    write.descriptorType = descriptorType;
    write.pImageInfo = &imageInfo;
    vkUpdateDescriptorSets(m_context->GetDevice(), 1, &write, 0, nullptr);
    m_boundResources[binding] = std::move(textureView);
}

void VulkanDescriptorSet::WriteSampler(const std::uint32_t binding, std::shared_ptr<ISampler> sampler)
{
    auto nativeSampler = std::dynamic_pointer_cast<VulkanSampler>(sampler);
    Core::Check(nativeSampler != nullptr, "Vulkan descriptor sets require Vulkan samplers.");
    const VkDescriptorType descriptorType = m_layout->GetBindingType(binding);
    Core::Check(descriptorType == VK_DESCRIPTOR_TYPE_SAMPLER, "WriteSampler requires a sampler descriptor binding.");
    const VkDescriptorImageInfo imageInfo{nativeSampler->GetHandle(), VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED};
    VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    write.dstSet = m_descriptorSet;
    write.dstBinding = binding;
    write.descriptorCount = 1;
    write.descriptorType = descriptorType;
    write.pImageInfo = &imageInfo;
    vkUpdateDescriptorSets(m_context->GetDevice(), 1, &write, 0, nullptr);
    m_boundResources[binding] = std::move(sampler);
}

void VulkanDescriptorSet::WriteAccelerationStructure(
    const std::uint32_t binding,
    std::shared_ptr<IRayTracingAccelerationStructure>
        accelerationStructure)
{
    Core::Check(
        accelerationStructure != nullptr
            && accelerationStructure->GetGraphicsApi()
                == GraphicsApi::Vulkan,
        "Vulkan descriptor sets require a Vulkan acceleration structure.");
    Core::Check(
        accelerationStructure->GetDescription().type
            == AccelerationStructureType::TopLevel,
        "Ray-query descriptors require a top-level acceleration structure.");
    const VkDescriptorType descriptorType =
        m_layout->GetBindingType(binding);
    Core::Check(
        descriptorType
            == VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR,
        "WriteAccelerationStructure requires an acceleration-structure binding.");
    const VkAccelerationStructureKHR nativeHandle =
        reinterpret_cast<VkAccelerationStructureKHR>(
            static_cast<std::uintptr_t>(
                accelerationStructure
                    ->GetNativeHandleBits()));
    const VkWriteDescriptorSetAccelerationStructureKHR
        accelerationWrite{
            VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR,
            nullptr,
            1u,
            &nativeHandle};
    VkWriteDescriptorSet write{
        VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    write.pNext = &accelerationWrite;
    write.dstSet = m_descriptorSet;
    write.dstBinding = binding;
    write.descriptorCount = 1;
    write.descriptorType = descriptorType;
    vkUpdateDescriptorSets(
        m_context->GetDevice(),
        1,
        &write,
        0,
        nullptr);
    m_boundResources[binding] =
        std::move(accelerationStructure);
}

VulkanDescriptorAllocator::VulkanDescriptorAllocator(VulkanContext& context)
    : m_context(&context)
{
    constexpr std::uint32_t InitialPoolCapacity = 256;
    m_pools.push_back({
        CreatePool(InitialPoolCapacity),
        InitialPoolCapacity,
        0});
    m_statistics.poolCount = 1;
    m_statistics.setCapacity =
        InitialPoolCapacity;
}

VkDescriptorPool
VulkanDescriptorAllocator::CreatePool(
    const std::uint32_t capacity)
{
    const std::array<VkDescriptorPoolSize, 7> poolSizes = {
        VkDescriptorPoolSize{
            VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
            capacity * 4},
        VkDescriptorPoolSize{
            VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC,
            capacity * 2},
        VkDescriptorPoolSize{
            VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
            capacity * 8},
        VkDescriptorPoolSize{
            VK_DESCRIPTOR_TYPE_SAMPLER,
            capacity * 4},
        VkDescriptorPoolSize{
            VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
            capacity * 2},
        VkDescriptorPoolSize{
            VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
            capacity * 2},
        VkDescriptorPoolSize{
            VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR,
            capacity},
    };
    VkDescriptorPoolCreateInfo createInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    createInfo.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
    createInfo.maxSets = capacity;
    createInfo.poolSizeCount = static_cast<std::uint32_t>(poolSizes.size());
    createInfo.pPoolSizes = poolSizes.data();
    VkDescriptorPool pool = VK_NULL_HANDLE;
    CheckVk(
        vkCreateDescriptorPool(
            m_context->GetDevice(),
            &createInfo,
            nullptr,
            &pool),
        "Failed to create Vulkan descriptor pool.");
    return pool;
}

VulkanDescriptorAllocator::~VulkanDescriptorAllocator()
{
    if (m_context == nullptr)
    {
        return;
    }
    for (const PoolState& pool : m_pools)
    {
        if (pool.handle != VK_NULL_HANDLE)
        {
            vkDestroyDescriptorPool(
                m_context->GetDevice(),
                pool.handle,
                nullptr);
        }
    }
}

std::shared_ptr<VulkanDescriptorSet> VulkanDescriptorAllocator::Allocate(
    std::shared_ptr<VulkanDescriptorSetLayout> layout)
{
    std::scoped_lock lock(m_mutex);
    const VkDescriptorSetLayout nativeLayout = layout->GetHandle();
    VkDescriptorSet descriptorSet =
        VK_NULL_HANDLE;
    VkDescriptorPool sourcePool =
        VK_NULL_HANDLE;
    for (PoolState& pool : m_pools)
    {
        VkDescriptorSetAllocateInfo allocateInfo{
            VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        allocateInfo.descriptorPool = pool.handle;
        allocateInfo.descriptorSetCount = 1;
        allocateInfo.pSetLayouts = &nativeLayout;
        const VkResult result = vkAllocateDescriptorSets(
            m_context->GetDevice(),
            &allocateInfo,
            &descriptorSet);
        if (result == VK_SUCCESS)
        {
            sourcePool = pool.handle;
            ++pool.allocatedSetCount;
            break;
        }
        if (result != VK_ERROR_OUT_OF_POOL_MEMORY
            && result != VK_ERROR_FRAGMENTED_POOL)
        {
            CheckVk(
                result,
                "Failed to allocate Vulkan descriptor set.");
        }
    }

    if (descriptorSet == VK_NULL_HANDLE)
    {
        constexpr std::uint32_t MaximumPoolCapacity =
            4096;
        const std::uint32_t capacity = std::min(
            m_pools.back().capacity * 2,
            MaximumPoolCapacity);
        sourcePool = CreatePool(capacity);
        m_pools.push_back({sourcePool, capacity, 0});
        ++m_statistics.poolCount;
        m_statistics.setCapacity += capacity;

        VkDescriptorSetAllocateInfo allocateInfo{
            VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        allocateInfo.descriptorPool = sourcePool;
        allocateInfo.descriptorSetCount = 1;
        allocateInfo.pSetLayouts = &nativeLayout;
        CheckVk(
            vkAllocateDescriptorSets(
                m_context->GetDevice(),
                &allocateInfo,
                &descriptorSet),
            "Failed to allocate Vulkan descriptor set from a new pool.");
        ++m_pools.back().allocatedSetCount;
    }

    ++m_statistics.allocatedSetCount;
    m_statistics.setHighWatermark = std::max(
        m_statistics.setHighWatermark,
        m_statistics.allocatedSetCount);
    return std::make_shared<VulkanDescriptorSet>(
        *m_context,
        *this,
        std::move(layout),
        sourcePool,
        descriptorSet);
}

void VulkanDescriptorAllocator::Retire(
    const VkDescriptorPool descriptorPool,
    const VkDescriptorSet descriptorSet)
{
    if (descriptorPool == VK_NULL_HANDLE
        || descriptorSet == VK_NULL_HANDLE)
    {
        return;
    }
    std::scoped_lock lock(m_mutex);
    const std::uint32_t frameIndex =
        m_context->GetCurrentFrameIndex()
        % static_cast<std::uint32_t>(
            m_retiredSets.size());
    m_retiredSets[frameIndex].push_back(
        {descriptorPool, descriptorSet});
    ++m_statistics.pendingReleaseCount;
}

void VulkanDescriptorAllocator::ReclaimFrame(
    const std::uint32_t frameIndex)
{
    std::scoped_lock lock(m_mutex);
    std::vector<RetiredSet>& retired =
        m_retiredSets[
            frameIndex
            % static_cast<std::uint32_t>(
                m_retiredSets.size())];
    for (const RetiredSet& entry : retired)
    {
        CheckVk(
            vkFreeDescriptorSets(
                m_context->GetDevice(),
                entry.pool,
                1,
                &entry.set),
            "Failed to reclaim a Vulkan descriptor set.");
        const auto pool = std::ranges::find(
            m_pools,
            entry.pool,
            &PoolState::handle);
        if (pool != m_pools.end()
            && pool->allocatedSetCount > 0)
        {
            --pool->allocatedSetCount;
        }
        --m_statistics.allocatedSetCount;
        --m_statistics.pendingReleaseCount;
    }
    retired.clear();
}

DescriptorAllocatorStatistics
VulkanDescriptorAllocator::GetStatistics() const
{
    std::scoped_lock lock(m_mutex);
    return m_statistics;
}
} // namespace Prism::RHI::Vulkan
