#pragma once

#include "RHI/GraphicsResources.h"
#include "RHI/D3D12/D3D12Context.h"
#include "RHI/D3D12/D3D12SamplerTable.h"

#include <d3d12.h>
#include <wrl/client.h>

#include <memory>
#include <optional>
#include <span>
#include <unordered_map>
#include <vector>

namespace Prism::RHI::D3D12
{
class D3D12Buffer final : public IBuffer
{
public:
    D3D12Buffer(D3D12Context& context, const BufferDescription& description, const void* initialData);
    D3D12Buffer(ID3D12Resource* resource, const BufferDescription& description);
    D3D12Buffer(
        D3D12Context& context,
        ID3D12Resource* resource,
        const BufferDescription& description,
        TransientBufferAllocationInfo allocationInfo,
        std::shared_ptr<void> allocationOwner);
    ~D3D12Buffer() override;

    GraphicsApi GetGraphicsApi() const override;
    void SetDebugName(
        std::string_view name) override;
    const BufferDescription& GetDescription() const override;
    void Update(const void* data, std::size_t size, std::size_t offset = 0) override;
    void Read(
        void* data,
        std::size_t size,
        std::size_t offset = 0) const override;
    const TransientBufferAllocationInfo*
        GetTransientAllocationInfo() const override;
    ID3D12Resource* GetResource() const;
    D3D12_GPU_VIRTUAL_ADDRESS GetGpuVirtualAddress() const;

private:
    D3D12Context* m_context = nullptr;
    BufferDescription m_description{};
    Microsoft::WRL::ComPtr<ID3D12Resource> m_resource;
    void* m_mappedData = nullptr;
    std::optional<TransientBufferAllocationInfo>
        m_transientAllocationInfo;
    std::shared_ptr<void> m_allocationOwner;
};

class D3D12Texture final : public ITexture
{
public:
    D3D12Texture(D3D12Context& context, const TextureDescription& description, const TextureInitialData* initialData);
    D3D12Texture(ID3D12Resource* resource, const TextureDescription& description);
    D3D12Texture(
        D3D12Context& context,
        ID3D12Resource* resource,
        const TextureDescription& description,
        TransientTextureAllocationInfo allocationInfo,
        std::shared_ptr<void> allocationOwner);
    ~D3D12Texture() override;

    GraphicsApi GetGraphicsApi() const override;
    void SetDebugName(
        std::string_view name) override;
    const TextureDescription& GetDescription() const override;
    const TransientTextureAllocationInfo*
        GetTransientAllocationInfo() const override;
    ID3D12Resource* GetResource() const;

private:
    D3D12Context* m_context = nullptr;
    TextureDescription m_description{};
    Microsoft::WRL::ComPtr<ID3D12Resource> m_resource;
    std::optional<TransientTextureAllocationInfo>
        m_transientAllocationInfo;
    std::shared_ptr<void> m_allocationOwner;
};

class D3D12TextureView final : public ITextureView
{
public:
    D3D12TextureView(
        D3D12Context& context,
        std::shared_ptr<D3D12Texture> texture,
        const TextureViewDescription& description);
    D3D12TextureView(
        ID3D12Resource* resource,
        D3D12_CPU_DESCRIPTOR_HANDLE descriptor,
        const TextureDescription& textureDescription,
        const TextureViewDescription& description);
    D3D12TextureView(
        std::shared_ptr<D3D12Texture> texture,
        D3D12_CPU_DESCRIPTOR_HANDLE descriptor,
        const TextureViewDescription& description);

    GraphicsApi GetGraphicsApi() const override;
    const TextureViewDescription& GetDescription() const override;
    const ITexture* GetTexture() const override;
    const TextureDescription& GetTextureDescription() const;
    ID3D12Resource* GetResource() const;
    D3D12_CPU_DESCRIPTOR_HANDLE GetCpuHandle() const;
    std::uint32_t GetSubresource() const;

private:
    std::shared_ptr<D3D12Texture> m_texture;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_resource;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> m_descriptorHeap;
    TextureDescription m_textureDescription{};
    TextureViewDescription m_description{};
    D3D12_CPU_DESCRIPTOR_HANDLE m_cpuHandle{};
};

class D3D12Sampler final : public ISampler
{
public:
    explicit D3D12Sampler(const SamplerDescription& description);

    GraphicsApi GetGraphicsApi() const override;
    const SamplerDescription& GetDescription() const override;

private:
    SamplerDescription m_description{};
};

class D3D12DescriptorSetLayout final : public IDescriptorSetLayout
{
public:
    struct BindingLocation
    {
        DescriptorType type = DescriptorType::ConstantBuffer;
        bool sampler = false;
        bool dynamic = false;
        std::uint32_t offset = 0;
    };

    explicit D3D12DescriptorSetLayout(const DescriptorSetLayoutDescription& description);

    GraphicsApi GetGraphicsApi() const override;
    const DescriptorSetLayoutDescription& GetDescription() const override;
    const BindingLocation& GetBindingLocation(std::uint32_t binding) const;
    std::uint32_t GetResourceDescriptorCount() const;
    std::uint32_t GetSamplerDescriptorCount() const;
    const std::vector<std::uint32_t>& GetDynamicBufferBindings() const;
    std::shared_ptr<const D3D12SamplerTable> AcquireSamplerTable(
        D3D12Context& context, std::span<const D3D12_SAMPLER_DESC> descriptions);

private:
    DescriptorSetLayoutDescription m_description;
    std::unordered_map<std::uint32_t, BindingLocation> m_locations;
    std::uint32_t m_resourceDescriptorCount = 0;
    std::uint32_t m_samplerDescriptorCount = 0;
    std::vector<std::uint32_t> m_dynamicBufferBindings;
    D3D12SamplerTableCache m_samplerTables;
};

class D3D12DescriptorSet final : public IDescriptorSet
{
public:
    D3D12DescriptorSet(D3D12Context& context, std::shared_ptr<D3D12DescriptorSetLayout> layout);
    ~D3D12DescriptorSet() override;

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

    bool HasResourceTable() const;
    bool HasSamplerTable() const;
    D3D12_GPU_DESCRIPTOR_HANDLE GetResourceTable() const;
    D3D12_GPU_DESCRIPTOR_HANDLE GetSamplerTable() const;
    const D3D12DescriptorSetLayout& GetLayout() const;
    D3D12_GPU_VIRTUAL_ADDRESS GetDynamicBufferGpuAddress(
        std::uint32_t binding,
        std::uint32_t dynamicOffset) const;

private:
    D3D12_CPU_DESCRIPTOR_HANDLE GetCpuHandle(const D3D12DescriptorSetLayout::BindingLocation& location) const;

    D3D12Context* m_context = nullptr;
    std::shared_ptr<D3D12DescriptorSetLayout> m_layout;
    D3D12Context::ShaderVisibleDescriptor m_resourceTable{};
    mutable std::shared_ptr<const D3D12SamplerTable> m_samplerTable;
    mutable std::mutex m_samplerMutex;
    std::vector<D3D12_SAMPLER_DESC> m_samplerDescriptions;
    std::uint32_t m_resourceDescriptorSize = 0;
    std::unordered_map<std::uint32_t, std::shared_ptr<IGraphicsResource>> m_boundResources;
    struct DynamicBufferBinding
    {
        std::shared_ptr<D3D12Buffer> buffer;
        std::size_t offset = 0;
        std::size_t range = 0;
    };
    std::unordered_map<std::uint32_t, DynamicBufferBinding> m_dynamicBuffers;
};
} // namespace Prism::RHI::D3D12
