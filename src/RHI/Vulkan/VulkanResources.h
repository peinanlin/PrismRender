#pragma once

#include "RHI/DeviceCapabilities.h"
#include "RHI/GraphicsResources.h"
#include "RHI/Vulkan/VulkanLoader.h"

#include <array>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <unordered_map>
#include <vector>

namespace Prism::RHI::Vulkan
{
class VulkanContext;

class VulkanBuffer final : public IBuffer
{
public:
    VulkanBuffer(VulkanContext& context, const BufferDescription& description, const void* initialData);
    VulkanBuffer(
        VulkanContext& context,
        const BufferDescription& description,
        VkBuffer buffer,
        TransientBufferAllocationInfo allocationInfo,
        std::shared_ptr<void> allocationOwner);
    ~VulkanBuffer() override;

    GraphicsApi GetGraphicsApi() const override;
    const BufferDescription& GetDescription() const override;
    void Update(const void* data, std::size_t size, std::size_t offset = 0) override;
    void Read(
        void* data,
        std::size_t size,
        std::size_t offset = 0) const override;
    const TransientBufferAllocationInfo*
        GetTransientAllocationInfo() const override;
    VkBuffer GetHandle() const;

private:
    VulkanContext* m_context = nullptr;
    BufferDescription m_description{};
    VkBuffer m_buffer = VK_NULL_HANDLE;
    VkDeviceMemory m_memory = VK_NULL_HANDLE;
    void* m_mappedData = nullptr;
    std::optional<TransientBufferAllocationInfo>
        m_transientAllocationInfo;
    std::shared_ptr<void> m_allocationOwner;
};

class VulkanTexture final : public ITexture
{
public:
    VulkanTexture(
        VulkanContext& context,
        const TextureDescription& description,
        const TextureInitialData* initialData);
    VulkanTexture(
        VulkanContext& context,
        const TextureDescription& description,
        VkImage externalImage);
    VulkanTexture(
        VulkanContext& context,
        const TextureDescription& description,
        VkImage image,
        VkImageView imageView,
        TransientTextureAllocationInfo allocationInfo,
        std::shared_ptr<void> allocationOwner);
    ~VulkanTexture() override;

    GraphicsApi GetGraphicsApi() const override;
    const TextureDescription& GetDescription() const override;
    const TransientTextureAllocationInfo*
        GetTransientAllocationInfo() const override;
    VkImage GetImage() const;
    VkImageView GetImageView() const;

private:
    VulkanContext* m_context = nullptr;
    TextureDescription m_description{};
    VkImage m_image = VK_NULL_HANDLE;
    VkImageView m_imageView = VK_NULL_HANDLE;
    VkDeviceMemory m_memory = VK_NULL_HANDLE;
    std::optional<TransientTextureAllocationInfo>
        m_transientAllocationInfo;
    std::shared_ptr<void> m_allocationOwner;
    bool m_ownsImage = true;
};

class VulkanTextureView final : public ITextureView
{
public:
    VulkanTextureView(
        VulkanContext& context,
        std::shared_ptr<VulkanTexture> texture,
        const TextureViewDescription& description);
    VulkanTextureView(
        VulkanContext& context,
        std::shared_ptr<VulkanTexture> texture,
        VkImageView externalImageView,
        const TextureViewDescription& description);
    VulkanTextureView(
        VulkanContext& context,
        VkImage image,
        VkImageView imageView,
        const TextureDescription& textureDescription,
        const TextureViewDescription& description);
    ~VulkanTextureView() override;

    GraphicsApi GetGraphicsApi() const override;
    const TextureViewDescription& GetDescription() const override;
    const ITexture* GetTexture() const override;
    const TextureDescription& GetTextureDescription() const;
    VkImage GetImage() const;
    VkImageView GetHandle() const;

private:
    VulkanContext* m_context = nullptr;
    std::shared_ptr<VulkanTexture> m_texture;
    TextureDescription m_textureDescription{};
    TextureViewDescription m_description{};
    VkImage m_image = VK_NULL_HANDLE;
    VkImageView m_imageView = VK_NULL_HANDLE;
    bool m_ownsImageView = false;
};

class VulkanSampler final : public ISampler
{
public:
    VulkanSampler(VulkanContext& context, const SamplerDescription& description);
    ~VulkanSampler() override;

    GraphicsApi GetGraphicsApi() const override;
    const SamplerDescription& GetDescription() const override;
    VkSampler GetHandle() const;

private:
    VulkanContext* m_context = nullptr;
    SamplerDescription m_description{};
    VkSampler m_sampler = VK_NULL_HANDLE;
};

class VulkanDescriptorSetLayout final : public IDescriptorSetLayout
{
public:
    VulkanDescriptorSetLayout(VulkanContext& context, const DescriptorSetLayoutDescription& description);
    ~VulkanDescriptorSetLayout() override;

    GraphicsApi GetGraphicsApi() const override;
    const DescriptorSetLayoutDescription& GetDescription() const override;
    VkDescriptorSetLayout GetHandle() const;
    VkDescriptorType GetBindingType(std::uint32_t binding) const;
    const std::vector<std::uint32_t>& GetDynamicBufferBindings() const;

private:
    VulkanContext* m_context = nullptr;
    DescriptorSetLayoutDescription m_description;
    VkDescriptorSetLayout m_layout = VK_NULL_HANDLE;
    std::unordered_map<std::uint32_t, VkDescriptorType> m_bindingTypes;
    std::vector<std::uint32_t> m_dynamicBufferBindings;
};

class VulkanDescriptorAllocator;

class VulkanDescriptorSet final : public IDescriptorSet
{
public:
    VulkanDescriptorSet(
        VulkanContext& context,
        VulkanDescriptorAllocator& allocator,
        std::shared_ptr<VulkanDescriptorSetLayout> layout,
        VkDescriptorPool descriptorPool,
        VkDescriptorSet descriptorSet);
    ~VulkanDescriptorSet() override;

    GraphicsApi GetGraphicsApi() const override;
    void WriteBuffer(
        std::uint32_t binding,
        std::shared_ptr<IBuffer> buffer,
        std::size_t offset = 0,
        std::size_t range = 0) override;
    void WriteTexture(std::uint32_t binding, std::shared_ptr<ITexture> texture) override;
    void WriteTextureView(std::uint32_t binding, std::shared_ptr<ITextureView> textureView) override;
    void WriteSampler(std::uint32_t binding, std::shared_ptr<ISampler> sampler) override;
    void WriteAccelerationStructure(
        std::uint32_t binding,
        std::shared_ptr<IRayTracingAccelerationStructure>
            accelerationStructure) override;

    VkDescriptorSet GetHandle() const;
    std::vector<std::uint32_t> BuildDynamicOffsets(
        std::span<const DynamicBufferOffset> dynamicOffsets) const;

private:
    VulkanContext* m_context = nullptr;
    VulkanDescriptorAllocator* m_allocator = nullptr;
    std::shared_ptr<VulkanDescriptorSetLayout> m_layout;
    VkDescriptorPool m_descriptorPool = VK_NULL_HANDLE;
    VkDescriptorSet m_descriptorSet = VK_NULL_HANDLE;
    std::unordered_map<std::uint32_t, std::shared_ptr<IGraphicsResource>> m_boundResources;
    struct BufferBinding
    {
        std::shared_ptr<VulkanBuffer> buffer;
        std::size_t offset = 0;
        std::size_t range = 0;
    };
    std::unordered_map<std::uint32_t, BufferBinding> m_bufferBindings;
};

class VulkanDescriptorAllocator
{
public:
    explicit VulkanDescriptorAllocator(VulkanContext& context);
    ~VulkanDescriptorAllocator();

    std::shared_ptr<VulkanDescriptorSet> Allocate(std::shared_ptr<VulkanDescriptorSetLayout> layout);
    void Retire(
        VkDescriptorPool descriptorPool,
        VkDescriptorSet descriptorSet);
    void ReclaimFrame(std::uint32_t frameIndex);
    DescriptorAllocatorStatistics GetStatistics() const;

private:
    struct PoolState
    {
        VkDescriptorPool handle = VK_NULL_HANDLE;
        std::uint32_t capacity = 0;
        std::uint32_t allocatedSetCount = 0;
    };

    struct RetiredSet
    {
        VkDescriptorPool pool = VK_NULL_HANDLE;
        VkDescriptorSet set = VK_NULL_HANDLE;
    };

    VkDescriptorPool CreatePool(std::uint32_t capacity);

    VulkanContext* m_context = nullptr;
    mutable std::mutex m_mutex;
    std::vector<PoolState> m_pools;
    std::array<std::vector<RetiredSet>, 2>
        m_retiredSets;
    DescriptorAllocatorStatistics m_statistics;
};
} // namespace Prism::RHI::Vulkan
