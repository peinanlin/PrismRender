#include "RHI/D3D12/D3D12Resources.h"

#include "Core/Assert.h"
#include "RHI/D3D12/D3D12Debug.h"
#include "RHI/RayTracing.h"
#include "RHI/D3D12/D3D12TypeConversions.h"

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <stdexcept>

namespace Prism::RHI::D3D12
{
namespace
{
constexpr std::size_t ConstantBufferAlignment = D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT;

std::size_t AlignUp(const std::size_t value, const std::size_t alignment)
{
    return (value + alignment - 1u) & ~(alignment - 1u);
}

D3D12_RESOURCE_DESC CreateBufferResourceDescription(
    const std::size_t size,
    const BufferUsage usage = BufferUsage::None)
{
    D3D12_RESOURCE_DESC description{};
    description.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    description.Width = size;
    description.Height = 1;
    description.DepthOrArraySize = 1;
    description.MipLevels = 1;
    description.Format = DXGI_FORMAT_UNKNOWN;
    description.SampleDesc.Count = 1;
    description.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    if (HasAnyFlag(usage, BufferUsage::Storage)
        || HasAnyFlag(
            usage,
            BufferUsage::AccelerationStructureStorage))
    {
        description.Flags =
            D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    }
    return description;
}

D3D12_HEAP_PROPERTIES CreateHeapProperties(const D3D12_HEAP_TYPE type)
{
    D3D12_HEAP_PROPERTIES properties{};
    properties.Type = type;
    properties.CreationNodeMask = 1;
    properties.VisibleNodeMask = 1;
    return properties;
}

D3D12_RESOURCE_STATES GetBufferReadyState(const BufferUsage usage)
{
    if (HasAnyFlag(
            usage,
            BufferUsage::AccelerationStructureStorage))
    {
        return D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE;
    }
    if (HasAnyFlag(usage, BufferUsage::Storage))
    {
        return D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    }
    ResourceState state = ResourceState::Common;
    if (HasAnyFlag(usage, BufferUsage::Vertex)) state = state | ResourceState::VertexBuffer;
    if (HasAnyFlag(usage, BufferUsage::Index)) state = state | ResourceState::IndexBuffer;
    if (HasAnyFlag(usage, BufferUsage::Constant)) state = state | ResourceState::ConstantBuffer;
    if (HasAnyFlag(usage, BufferUsage::Indirect)) state = state | ResourceState::IndirectArgument;
    if (HasAnyFlag(usage, BufferUsage::ShaderResource)) state = state | ResourceState::ShaderResource;
    if (HasAnyFlag(
            usage,
            BufferUsage::AccelerationStructureBuildInput
                | BufferUsage::ShaderBindingTable))
    {
        state = state | ResourceState::ShaderResource;
    }
    return ToNativeResourceStates(state);
}

D3D12_DESCRIPTOR_HEAP_TYPE GetViewHeapType(const TextureViewType type)
{
    switch (type)
    {
    case TextureViewType::RenderTarget: return D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    case TextureViewType::DepthStencil: return D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
    case TextureViewType::Sampled:
    case TextureViewType::Storage: return D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    }
    throw std::invalid_argument("Unsupported D3D12 texture-view type.");
}
} // namespace

D3D12Buffer::D3D12Buffer(
    D3D12Context& context,
    const BufferDescription& description,
    const void* initialData)
    : m_context(&context)
    , m_description(description)
{
    std::string validationError;
    Core::Check(ValidateBufferDescription(description, initialData, &validationError), validationError);
    const bool constantBuffer = HasAnyFlag(description.usage, BufferUsage::Constant);
    const std::size_t allocationSize = constantBuffer
                                           ? AlignUp(description.size, ConstantBufferAlignment)
                                           : description.size;
    const D3D12_RESOURCE_DESC resourceDescription =
        CreateBufferResourceDescription(
            allocationSize,
            description.usage);
    const D3D12_HEAP_PROPERTIES heapProperties = CreateHeapProperties(ToNativeHeapType(description.memoryAccess));
    const bool cpuVisible = description.memoryAccess != MemoryAccess::GpuOnly;
    const D3D12_RESOURCE_STATES initialState = cpuVisible
                                                   ? (description.memoryAccess == MemoryAccess::GpuToCpu
                                                          ? D3D12_RESOURCE_STATE_COPY_DEST
                                                          : D3D12_RESOURCE_STATE_GENERIC_READ)
                                                   : (initialData != nullptr
                                                          ? D3D12_RESOURCE_STATE_COPY_DEST
                                                          : GetBufferReadyState(description.usage));
    Core::ThrowIfFailed(
        context.GetDevice()->CreateCommittedResource(
            &heapProperties,
            D3D12_HEAP_FLAG_NONE,
            &resourceDescription,
            initialState,
            nullptr,
            IID_PPV_ARGS(&m_resource)),
        "Failed to create a D3D12 RHI buffer.");

    if (cpuVisible)
    {
        D3D12_RANGE readRange{0, 0};
        Core::ThrowIfFailed(m_resource->Map(0, &readRange, &m_mappedData), "Failed to map a D3D12 RHI buffer.");
        if (initialData != nullptr)
        {
            std::memcpy(m_mappedData, initialData, description.size);
        }
        return;
    }
    if (initialData == nullptr)
    {
        return;
    }

    const D3D12Context::UploadAllocation upload =
        context.AllocateUpload(
            description.size,
            16);
    std::memcpy(
        upload.cpuAddress,
        initialData,
        description.size);
    context.QueueUpload(
        upload,
        [&](ID3D12GraphicsCommandList* commandList)
    {
        commandList->CopyBufferRegion(
            m_resource.Get(),
            0,
            upload.resource.Get(),
            upload.offset,
            description.size);
    },
        [&](ID3D12GraphicsCommandList* commandList)
    {
        D3D12_RESOURCE_BARRIER barrier{};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = m_resource.Get();
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
        barrier.Transition.StateAfter = GetBufferReadyState(description.usage);
        commandList->ResourceBarrier(1, &barrier);
    });
}

D3D12Buffer::~D3D12Buffer()
{
    if (m_mappedData != nullptr && m_resource != nullptr)
    {
        m_resource->Unmap(0, nullptr);
    }
    if (m_context != nullptr
        && m_resource != nullptr)
    {
        m_context->RetireResource(
            std::move(m_resource),
            std::move(m_allocationOwner));
    }
}

D3D12Buffer::D3D12Buffer(ID3D12Resource* resource, const BufferDescription& description)
    : m_description(description)
    , m_resource(resource)
{
    Core::Check(resource != nullptr, "External D3D12 buffers require a valid resource.");
    std::string validationError;
    Core::Check(ValidateBufferDescription(description, nullptr, &validationError), validationError);
    Core::Check(resource->GetDesc().Dimension == D3D12_RESOURCE_DIMENSION_BUFFER,
                "External D3D12 buffer wrappers require a buffer resource.");
    Core::Check(resource->GetDesc().Width >= description.size,
                "External D3D12 buffer resource is smaller than its public description.");
}

D3D12Buffer::D3D12Buffer(
    D3D12Context& context,
    ID3D12Resource* resource,
    const BufferDescription& description,
    TransientBufferAllocationInfo allocationInfo,
    std::shared_ptr<void> allocationOwner)
    : D3D12Buffer(resource, description)
{
    m_context = &context;
    Core::Check(
        allocationInfo.poolId != 0
            && allocationInfo.allocationBytes > 0
            && allocationInfo.poolPhysicalBytes > 0,
        "D3D12 transient buffers require valid allocation metadata.");
    Core::Check(
        allocationOwner != nullptr,
        "D3D12 transient buffers require a heap owner.");
    m_transientAllocationInfo = allocationInfo;
    m_allocationOwner = std::move(allocationOwner);
}

GraphicsApi D3D12Buffer::GetGraphicsApi() const { return GraphicsApi::Direct3D12; }
void D3D12Buffer::SetDebugName(
    const std::string_view name)
{
    SetD3D12DebugName(m_resource.Get(), name);
}
const BufferDescription& D3D12Buffer::GetDescription() const { return m_description; }
const TransientBufferAllocationInfo*
D3D12Buffer::GetTransientAllocationInfo() const
{
    return m_transientAllocationInfo.has_value()
        ? &*m_transientAllocationInfo
        : nullptr;
}
ID3D12Resource* D3D12Buffer::GetResource() const { return m_resource.Get(); }
D3D12_GPU_VIRTUAL_ADDRESS D3D12Buffer::GetGpuVirtualAddress() const { return m_resource->GetGPUVirtualAddress(); }

void D3D12Buffer::Update(const void* data, const std::size_t size, const std::size_t offset)
{
    Core::Check(data != nullptr && offset + size <= m_description.size, "D3D12 buffer update range is invalid.");
    Core::Check(m_mappedData != nullptr, "GPU-only D3D12 buffers require an upload command instead of Update().");
    std::memcpy(static_cast<std::byte*>(m_mappedData) + offset, data, size);
}

void D3D12Buffer::Read(
    void* data,
    const std::size_t size,
    const std::size_t offset) const
{
    Core::Check(
        data != nullptr && offset + size <= m_description.size,
        "D3D12 buffer read range is invalid.");
    Core::Check(
        m_description.memoryAccess == MemoryAccess::GpuToCpu
            && m_mappedData != nullptr,
        "Only GPU-to-CPU D3D12 buffers can be read directly.");
    std::memcpy(
        data,
        static_cast<const std::byte*>(m_mappedData) + offset,
        size);
}

D3D12Texture::D3D12Texture(
    D3D12Context& context,
    const TextureDescription& description,
    const TextureInitialData* initialData)
    : m_context(&context)
    , m_description(description)
{
    std::string validationError;
    Core::Check(ValidateTextureDescription(description, &validationError), validationError);
    Core::Check(description.memoryAccess == MemoryAccess::GpuOnly,
                "The D3D12 RHI currently keeps textures in GPU-only heaps.");
    D3D12_RESOURCE_DESC nativeDescription = ToNativeTextureDescription(description);
    if (IsDepthFormat(description.format) && HasAnyFlag(description.usage, TextureUsage::ShaderResource))
    {
        nativeDescription.Format = DXGI_FORMAT_R32_TYPELESS;
    }
    const D3D12_HEAP_PROPERTIES heap = CreateHeapProperties(D3D12_HEAP_TYPE_DEFAULT);
    D3D12_CLEAR_VALUE clearValue{};
    const D3D12_CLEAR_VALUE* clearValuePointer = nullptr;
    if (HasAnyFlag(description.usage, TextureUsage::DepthStencil))
    {
        clearValue.Format = DXGI_FORMAT_D32_FLOAT;
        clearValue.DepthStencil.Depth = 1.0f;
        clearValuePointer = &clearValue;
    }
    else if (HasAnyFlag(description.usage, TextureUsage::RenderTarget))
    {
        clearValue.Format = ToNativeFormat(description.format);
        clearValuePointer = &clearValue;
    }
    const D3D12_RESOURCE_STATES initialState = initialData != nullptr
                                                    ? D3D12_RESOURCE_STATE_COPY_DEST
                                                    : D3D12_RESOURCE_STATE_COMMON;
    Core::ThrowIfFailed(
        context.GetDevice()->CreateCommittedResource(
            &heap,
            D3D12_HEAP_FLAG_NONE,
            &nativeDescription,
            initialState,
            clearValuePointer,
            IID_PPV_ARGS(&m_resource)),
        "Failed to create a D3D12 RHI texture.");

    if (initialData == nullptr)
    {
        return;
    }
    Core::Check(description.mipLevels == 1,
                "D3D12 initial texture uploads currently support one mip level.");
    Core::Check(description.format == Format::Rgba8Unorm || description.format == Format::Rgba8UnormSrgb,
                "D3D12 initial texture uploads currently support RGBA8 formats.");
    Core::Check(initialData->data != nullptr, "D3D12 texture initial data cannot be null.");

    std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> footprints(description.arrayLayers);
    std::vector<UINT> rowCounts(description.arrayLayers);
    std::vector<UINT64> rowSizes(description.arrayLayers);
    UINT64 uploadSize = 0;
    context.GetDevice()->GetCopyableFootprints(
        &nativeDescription,
        0,
        description.arrayLayers,
        0,
        footprints.data(),
        rowCounts.data(),
        rowSizes.data(),
        &uploadSize);
    const D3D12Context::UploadAllocation upload =
        context.AllocateUpload(
            uploadSize,
            D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT);
    std::byte* mapped = upload.cpuAddress;
    const std::size_t sourceRowPitch = initialData->rowPitch == 0
                                           ? static_cast<std::size_t>(description.width) * 4u
                                           : initialData->rowPitch;
    Core::Check(initialData->slicePitch >= sourceRowPitch * description.height,
                "D3D12 texture upload slice pitch is too small.");
    for (std::uint32_t layer = 0; layer < description.arrayLayers; ++layer)
    {
        const D3D12_PLACED_SUBRESOURCE_FOOTPRINT& footprint = footprints[layer];
        const auto* sourceLayer = static_cast<const std::byte*>(initialData->data)
            + static_cast<std::size_t>(layer) * initialData->slicePitch;
        for (std::uint32_t row = 0; row < description.height; ++row)
        {
            std::memcpy(mapped + footprint.Offset
                            + static_cast<std::size_t>(row) * footprint.Footprint.RowPitch,
                        sourceLayer + row * sourceRowPitch,
                        static_cast<std::size_t>(rowSizes[layer]));
        }
    }
    context.QueueUpload(
        upload,
        [&](ID3D12GraphicsCommandList* commandList)
    {
        for (std::uint32_t layer = 0; layer < description.arrayLayers; ++layer)
        {
            D3D12_TEXTURE_COPY_LOCATION destination{};
            destination.pResource = m_resource.Get();
            destination.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            destination.SubresourceIndex = layer;
            D3D12_TEXTURE_COPY_LOCATION source{};
            source.pResource = upload.resource.Get();
            source.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            source.PlacedFootprint = footprints[layer];
            source.PlacedFootprint.Offset +=
                upload.offset;
            commandList->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);
        }
    },
        [&](ID3D12GraphicsCommandList* commandList)
    {
        D3D12_RESOURCE_BARRIER barrier{};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = m_resource.Get();
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
        barrier.Transition.StateAfter = ToNativeResourceStates(ResourceState::ShaderResource);
        commandList->ResourceBarrier(1, &barrier);
    });
}

D3D12Texture::~D3D12Texture()
{
    if (m_context != nullptr
        && m_resource != nullptr)
    {
        m_context->RetireResource(
            std::move(m_resource),
            std::move(m_allocationOwner));
    }
}

GraphicsApi D3D12Texture::GetGraphicsApi() const { return GraphicsApi::Direct3D12; }
void D3D12Texture::SetDebugName(
    const std::string_view name)
{
    SetD3D12DebugName(m_resource.Get(), name);
}
const TextureDescription& D3D12Texture::GetDescription() const { return m_description; }
const TransientTextureAllocationInfo*
D3D12Texture::GetTransientAllocationInfo() const
{
    return m_transientAllocationInfo.has_value()
        ? &*m_transientAllocationInfo
        : nullptr;
}
ID3D12Resource* D3D12Texture::GetResource() const { return m_resource.Get(); }

D3D12Texture::D3D12Texture(ID3D12Resource* resource, const TextureDescription& description)
    : m_description(description)
    , m_resource(resource)
{
    Core::Check(resource != nullptr, "External D3D12 textures require a valid resource.");
    std::string validationError;
    Core::Check(ValidateTextureDescription(description, &validationError), validationError);
    Core::Check(resource->GetDesc().Dimension != D3D12_RESOURCE_DIMENSION_BUFFER,
                "External D3D12 texture wrappers require a texture resource.");
}

D3D12Texture::D3D12Texture(
    D3D12Context& context,
    ID3D12Resource* resource,
    const TextureDescription& description,
    TransientTextureAllocationInfo allocationInfo,
    std::shared_ptr<void> allocationOwner)
    : D3D12Texture(resource, description)
{
    m_context = &context;
    Core::Check(
        allocationInfo.poolId != 0
            && allocationInfo.allocationBytes > 0
            && allocationInfo.poolPhysicalBytes > 0,
        "D3D12 transient textures require valid allocation metadata.");
    Core::Check(
        allocationOwner != nullptr,
        "D3D12 transient textures require a heap owner.");
    m_transientAllocationInfo = allocationInfo;
    m_allocationOwner = std::move(allocationOwner);
}

D3D12TextureView::D3D12TextureView(
    D3D12Context& context,
    std::shared_ptr<D3D12Texture> texture,
    const TextureViewDescription& description)
    : m_texture(std::move(texture))
    , m_resource(m_texture->GetResource())
    , m_textureDescription(m_texture->GetDescription())
    , m_description(description)
{
    std::string validationError;
    Core::Check(ValidateTextureViewDescription(m_textureDescription, description, &validationError),
                validationError);
    D3D12_DESCRIPTOR_HEAP_DESC heapDescription{};
    heapDescription.Type = GetViewHeapType(description.type);
    heapDescription.NumDescriptors = 1;
    Core::ThrowIfFailed(
        context.GetDevice()->CreateDescriptorHeap(&heapDescription, IID_PPV_ARGS(&m_descriptorHeap)),
        "Failed to create a D3D12 texture-view descriptor heap.");
    m_cpuHandle = m_descriptorHeap->GetCPUDescriptorHandleForHeapStart();
    const Format viewFormat = description.format == Format::Unknown ? m_textureDescription.format : description.format;
    const bool arrayView = m_textureDescription.arrayLayers > 1;
    switch (description.type)
    {
    case TextureViewType::RenderTarget:
    {
        D3D12_RENDER_TARGET_VIEW_DESC native{};
        native.Format = ToNativeFormat(viewFormat);
        native.ViewDimension = arrayView ? D3D12_RTV_DIMENSION_TEXTURE2DARRAY : D3D12_RTV_DIMENSION_TEXTURE2D;
        if (arrayView)
        {
            native.Texture2DArray.MipSlice = description.baseMipLevel;
            native.Texture2DArray.FirstArraySlice = description.baseArrayLayer;
            native.Texture2DArray.ArraySize = description.arrayLayerCount;
        }
        else native.Texture2D.MipSlice = description.baseMipLevel;
        context.GetDevice()->CreateRenderTargetView(m_resource.Get(), &native, m_cpuHandle);
        break;
    }
    case TextureViewType::DepthStencil:
    {
        D3D12_DEPTH_STENCIL_VIEW_DESC native{};
        native.Format = DXGI_FORMAT_D32_FLOAT;
        native.ViewDimension = arrayView ? D3D12_DSV_DIMENSION_TEXTURE2DARRAY : D3D12_DSV_DIMENSION_TEXTURE2D;
        if (arrayView)
        {
            native.Texture2DArray.MipSlice = description.baseMipLevel;
            native.Texture2DArray.FirstArraySlice = description.baseArrayLayer;
            native.Texture2DArray.ArraySize = description.arrayLayerCount;
        }
        else native.Texture2D.MipSlice = description.baseMipLevel;
        context.GetDevice()->CreateDepthStencilView(m_resource.Get(), &native, m_cpuHandle);
        break;
    }
    case TextureViewType::Sampled:
    {
        const auto native = ToNativeSampledTextureViewDescription(m_textureDescription, description);
        context.GetDevice()->CreateShaderResourceView(m_resource.Get(), &native, m_cpuHandle);
        break;
    }
    case TextureViewType::Storage:
    {
        D3D12_UNORDERED_ACCESS_VIEW_DESC native{};
        native.Format = ToNativeFormat(viewFormat);
        native.ViewDimension = arrayView ? D3D12_UAV_DIMENSION_TEXTURE2DARRAY : D3D12_UAV_DIMENSION_TEXTURE2D;
        if (arrayView)
        {
            native.Texture2DArray.MipSlice = description.baseMipLevel;
            native.Texture2DArray.FirstArraySlice = description.baseArrayLayer;
            native.Texture2DArray.ArraySize = description.arrayLayerCount;
        }
        else native.Texture2D.MipSlice = description.baseMipLevel;
        context.GetDevice()->CreateUnorderedAccessView(m_resource.Get(), nullptr, &native, m_cpuHandle);
        break;
    }
    }
}

D3D12TextureView::D3D12TextureView(
    std::shared_ptr<D3D12Texture> texture,
    const D3D12_CPU_DESCRIPTOR_HANDLE descriptor,
    const TextureViewDescription& description)
    : m_texture(std::move(texture))
    , m_resource(m_texture != nullptr ? m_texture->GetResource() : nullptr)
    , m_textureDescription(m_texture != nullptr ? m_texture->GetDescription() : TextureDescription{})
    , m_description(description)
    , m_cpuHandle(descriptor)
{
    Core::Check(m_texture != nullptr && descriptor.ptr != 0,
                "External D3D12 texture views require a public texture and descriptor.");
    std::string validationError;
    Core::Check(ValidateTextureViewDescription(m_textureDescription, description, &validationError),
                validationError);
}

D3D12TextureView::D3D12TextureView(
    ID3D12Resource* resource,
    const D3D12_CPU_DESCRIPTOR_HANDLE descriptor,
    const TextureDescription& textureDescription,
    const TextureViewDescription& description)
    : m_resource(resource)
    , m_textureDescription(textureDescription)
    , m_description(description)
    , m_cpuHandle(descriptor)
{
    Core::Check(resource != nullptr && descriptor.ptr != 0, "External D3D12 texture views require valid handles.");
    std::string validationError;
    Core::Check(ValidateTextureViewDescription(textureDescription, description, &validationError),
                validationError);
}

GraphicsApi D3D12TextureView::GetGraphicsApi() const { return GraphicsApi::Direct3D12; }
const TextureViewDescription& D3D12TextureView::GetDescription() const { return m_description; }
const ITexture* D3D12TextureView::GetTexture() const { return m_texture.get(); }
const TextureDescription& D3D12TextureView::GetTextureDescription() const { return m_textureDescription; }
ID3D12Resource* D3D12TextureView::GetResource() const { return m_resource.Get(); }
D3D12_CPU_DESCRIPTOR_HANDLE D3D12TextureView::GetCpuHandle() const { return m_cpuHandle; }
std::uint32_t D3D12TextureView::GetSubresource() const
{
    return m_description.baseMipLevel + m_description.baseArrayLayer * m_textureDescription.mipLevels;
}

D3D12Sampler::D3D12Sampler(const SamplerDescription& description) : m_description(description) {}
GraphicsApi D3D12Sampler::GetGraphicsApi() const { return GraphicsApi::Direct3D12; }
const SamplerDescription& D3D12Sampler::GetDescription() const { return m_description; }

D3D12DescriptorSetLayout::D3D12DescriptorSetLayout(const DescriptorSetLayoutDescription& description)
    : m_description(description)
{
    std::string validationError;
    Core::Check(ValidateDescriptorSetLayoutDescription(description, &validationError), validationError);
    for (const DescriptorBindingDescription& binding : description.bindings)
    {
        Core::Check(binding.descriptorCount == 1,
                    "D3D12 public descriptor arrays require the later bindless descriptor stage.");
        const bool sampler = binding.type == DescriptorType::Sampler;
        const bool dynamic = binding.type == DescriptorType::DynamicConstantBuffer;
        const std::uint32_t offset = dynamic
                                         ? 0
                                         : (sampler ? m_samplerDescriptorCount++ : m_resourceDescriptorCount++);
        m_locations.emplace(binding.binding, BindingLocation{binding.type, sampler, dynamic, offset});
        if (dynamic)
        {
            m_dynamicBufferBindings.push_back(binding.binding);
        }
    }
    std::ranges::sort(m_dynamicBufferBindings);
}

GraphicsApi D3D12DescriptorSetLayout::GetGraphicsApi() const { return GraphicsApi::Direct3D12; }
const DescriptorSetLayoutDescription& D3D12DescriptorSetLayout::GetDescription() const { return m_description; }
const D3D12DescriptorSetLayout::BindingLocation& D3D12DescriptorSetLayout::GetBindingLocation(
    const std::uint32_t binding) const
{
    const auto found = m_locations.find(binding);
    if (found == m_locations.end())
    {
        throw std::invalid_argument(
            "D3D12 descriptor write uses unknown binding "
            + std::to_string(binding) + ".");
    }
    return found->second;
}
std::uint32_t D3D12DescriptorSetLayout::GetResourceDescriptorCount() const { return m_resourceDescriptorCount; }
std::uint32_t D3D12DescriptorSetLayout::GetSamplerDescriptorCount() const { return m_samplerDescriptorCount; }
const std::vector<std::uint32_t>& D3D12DescriptorSetLayout::GetDynamicBufferBindings() const
{
    return m_dynamicBufferBindings;
}

std::shared_ptr<const D3D12SamplerTable> D3D12DescriptorSetLayout::AcquireSamplerTable(
    D3D12Context& context, std::span<const D3D12_SAMPLER_DESC> descriptions)
{
    Core::Check(descriptions.size() == m_samplerDescriptorCount, "Sampler table does not match its layout.");
    return m_samplerTables.Acquire(context, descriptions);
}

D3D12DescriptorSet::D3D12DescriptorSet(
    D3D12Context& context,
    std::shared_ptr<D3D12DescriptorSetLayout> layout)
    : m_context(&context)
    , m_layout(std::move(layout))
    , m_samplerDescriptions(m_layout->GetSamplerDescriptorCount(), ToNativeSamplerDescription({}))
{
    m_resourceDescriptorSize = context.GetDevice()->GetDescriptorHandleIncrementSize(
        D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    if (m_layout->GetResourceDescriptorCount() > 0)
        m_resourceTable = context.AllocateShaderVisibleSrvRange(m_layout->GetResourceDescriptorCount());
}

D3D12DescriptorSet::~D3D12DescriptorSet()
{
    if (m_context == nullptr || m_layout == nullptr)
    {
        return;
    }
    m_context->RetireShaderVisibleSrvRange(
        m_resourceTable.index,
        m_layout->GetResourceDescriptorCount());
}

GraphicsApi D3D12DescriptorSet::GetGraphicsApi() const { return GraphicsApi::Direct3D12; }

D3D12_CPU_DESCRIPTOR_HANDLE D3D12DescriptorSet::GetCpuHandle(
    const D3D12DescriptorSetLayout::BindingLocation& location) const
{
    Core::Check(!location.sampler, "Shared sampler tables cannot be written in place.");
    D3D12_CPU_DESCRIPTOR_HANDLE handle = m_resourceTable.cpuHandle;
    handle.ptr += static_cast<SIZE_T>(location.offset) * m_resourceDescriptorSize;
    return handle;
}

void D3D12DescriptorSet::WriteBuffer(
    const std::uint32_t binding,
    std::shared_ptr<IBuffer> buffer,
    const std::size_t offset,
    const std::size_t range)
{
    Core::Check(buffer != nullptr,
        "D3D12 descriptor buffer binding " + std::to_string(binding)
            + " received a null buffer.");
    auto nativeBuffer = std::dynamic_pointer_cast<D3D12Buffer>(buffer);
    Core::Check(nativeBuffer != nullptr,
        "D3D12 descriptor buffer binding " + std::to_string(binding)
            + " received a buffer created by another graphics API.");
    const auto& location = m_layout->GetBindingLocation(binding);
    Core::Check(location.type == DescriptorType::ConstantBuffer
                    || location.type == DescriptorType::DynamicConstantBuffer
                    || location.type == DescriptorType::StorageBuffer
                    || location.type
                        == DescriptorType::ReadOnlyStorageBuffer,
                "WriteBuffer requires a D3D12 buffer binding.");
    const std::size_t effectiveRange = range == 0 ? nativeBuffer->GetDescription().size - offset : range;
    Core::Check(offset + effectiveRange <= nativeBuffer->GetDescription().size, "D3D12 buffer descriptor range is invalid.");
    if (location.type
            == DescriptorType::StorageBuffer
        || location.type
            == DescriptorType::ReadOnlyStorageBuffer)
    {
        Core::Check(
            nativeBuffer->GetDescription().stride > 0
                && offset
                        % nativeBuffer
                              ->GetDescription()
                              .stride
                    == 0
                && effectiveRange
                        % nativeBuffer
                              ->GetDescription()
                              .stride
                    == 0,
            "D3D12 structured buffer ranges must be stride aligned.");
    }
    if (location.type == DescriptorType::DynamicConstantBuffer)
    {
        Core::Check(offset % ConstantBufferAlignment == 0,
                    "D3D12 dynamic constant-buffer base offsets must be 256-byte aligned.");
        m_dynamicBuffers[binding] = {nativeBuffer, offset, effectiveRange};
        m_boundResources[binding] = std::move(buffer);
        return;
    }

    const D3D12_CPU_DESCRIPTOR_HANDLE destination = GetCpuHandle(location);
    if (location.type == DescriptorType::ConstantBuffer)
    {
        Core::Check(offset % ConstantBufferAlignment == 0, "D3D12 constant-buffer offsets must be 256-byte aligned.");
        D3D12_CONSTANT_BUFFER_VIEW_DESC native{};
        native.BufferLocation = nativeBuffer->GetGpuVirtualAddress() + offset;
        native.SizeInBytes = static_cast<UINT>(AlignUp(effectiveRange, ConstantBufferAlignment));
        m_context->GetDevice()->CreateConstantBufferView(&native, destination);
    }
    else if (
        location.type
        == DescriptorType::ReadOnlyStorageBuffer)
    {
        Core::Check(
            nativeBuffer->GetDescription().stride > 0,
            "D3D12 structured SRVs require a non-zero stride.");
        D3D12_SHADER_RESOURCE_VIEW_DESC native{};
        native.ViewDimension =
            D3D12_SRV_DIMENSION_BUFFER;
        native.Format = DXGI_FORMAT_UNKNOWN;
        native.Shader4ComponentMapping =
            D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        native.Buffer.FirstElement =
            offset
            / nativeBuffer
                  ->GetDescription()
                  .stride;
        native.Buffer.NumElements =
            static_cast<UINT>(
                effectiveRange
                / nativeBuffer
                      ->GetDescription()
                      .stride);
        native.Buffer.StructureByteStride =
            nativeBuffer
                ->GetDescription()
                .stride;
        m_context->GetDevice()
            ->CreateShaderResourceView(
                nativeBuffer->GetResource(),
                &native,
                destination);
    }
    else
    {
        D3D12_UNORDERED_ACCESS_VIEW_DESC native{};
        native.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
        native.Format = DXGI_FORMAT_UNKNOWN;
        native.Buffer.FirstElement = offset / nativeBuffer->GetDescription().stride;
        native.Buffer.NumElements = static_cast<UINT>(effectiveRange / nativeBuffer->GetDescription().stride);
        native.Buffer.StructureByteStride = nativeBuffer->GetDescription().stride;
        m_context->GetDevice()->CreateUnorderedAccessView(nativeBuffer->GetResource(), nullptr, &native, destination);
    }
    m_boundResources[binding] = std::move(buffer);
}

void D3D12DescriptorSet::WriteTexture(const std::uint32_t binding, std::shared_ptr<ITexture> texture)
{
    auto nativeTexture = std::dynamic_pointer_cast<D3D12Texture>(texture);
    Core::Check(nativeTexture != nullptr, "D3D12 descriptor sets require D3D12 textures.");
    TextureViewDescription description{};
    description.type = TextureViewType::Sampled;
    description.mipLevelCount = nativeTexture->GetDescription().mipLevels;
    description.arrayLayerCount = nativeTexture->GetDescription().arrayLayers;
    const auto& location = m_layout->GetBindingLocation(binding);
    Core::Check(location.type == DescriptorType::SampledTexture || location.type == DescriptorType::StorageTexture,
                "WriteTexture requires a D3D12 image binding.");
    const auto native = ToNativeSampledTextureViewDescription(nativeTexture->GetDescription(), description);
    // The descriptor set already owns the destination slot. A temporary view would
    // allocate and retain an entire CPU descriptor heap for every material binding.
    m_context->GetDevice()->CreateShaderResourceView(
        nativeTexture->GetResource(), &native, GetCpuHandle(location));
    m_boundResources[binding] = std::move(texture);
}

void D3D12DescriptorSet::WriteTextureView(
    const std::uint32_t binding,
    std::shared_ptr<ITextureView> textureView)
{
    auto nativeView = std::dynamic_pointer_cast<D3D12TextureView>(textureView);
    Core::Check(nativeView != nullptr, "D3D12 descriptor sets require D3D12 texture views.");
    const auto& location = m_layout->GetBindingLocation(binding);
    Core::Check(location.type == DescriptorType::SampledTexture || location.type == DescriptorType::StorageTexture,
                "WriteTextureView requires a D3D12 image binding.");
    m_context->GetDevice()->CopyDescriptorsSimple(
        1, GetCpuHandle(location), nativeView->GetCpuHandle(), D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    m_boundResources[binding] = std::move(textureView);
}

void D3D12DescriptorSet::WriteSampler(const std::uint32_t binding, std::shared_ptr<ISampler> sampler)
{
    auto nativeSampler = std::dynamic_pointer_cast<D3D12Sampler>(sampler);
    Core::Check(nativeSampler != nullptr, "D3D12 descriptor sets require D3D12 samplers.");
    const auto& location = m_layout->GetBindingLocation(binding);
    Core::Check(location.type == DescriptorType::Sampler, "WriteSampler requires a D3D12 sampler binding.");
    const D3D12_SAMPLER_DESC native = ToNativeSamplerDescription(nativeSampler->GetDescription());
    std::lock_guard lock(m_samplerMutex);
    m_samplerDescriptions[location.offset] = native;
    if (m_samplerTable && !m_samplerTable->Matches(*m_context, m_samplerDescriptions))
        m_samplerTable.reset();
    m_boundResources[binding] = std::move(sampler);
}

void D3D12DescriptorSet::WriteAccelerationStructure(
    const std::uint32_t binding,
    std::shared_ptr<IRayTracingAccelerationStructure>
        accelerationStructure)
{
    Core::Check(
        accelerationStructure != nullptr
            && accelerationStructure->GetGraphicsApi()
                == GraphicsApi::Direct3D12,
        "D3D12 descriptor sets require a D3D12 acceleration structure.");
    Core::Check(
        accelerationStructure->GetDescription().type
            == AccelerationStructureType::TopLevel,
        "Ray-query descriptors require a top-level acceleration structure.");
    const auto& location =
        m_layout->GetBindingLocation(binding);
    Core::Check(
        location.type
            == DescriptorType::AccelerationStructure,
        "WriteAccelerationStructure requires an acceleration-structure binding.");
    D3D12_SHADER_RESOURCE_VIEW_DESC native{};
    native.ViewDimension =
        D3D12_SRV_DIMENSION_RAYTRACING_ACCELERATION_STRUCTURE;
    native.Shader4ComponentMapping =
        D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    native.RaytracingAccelerationStructure.Location =
        accelerationStructure->GetDeviceAddress();
    m_context->GetDevice()->CreateShaderResourceView(
        nullptr,
        &native,
        GetCpuHandle(location));
    m_boundResources[binding] =
        std::move(accelerationStructure);
}

bool D3D12DescriptorSet::HasResourceTable() const { return m_layout->GetResourceDescriptorCount() > 0; }
bool D3D12DescriptorSet::HasSamplerTable() const { return m_layout->GetSamplerDescriptorCount() > 0; }
D3D12_GPU_DESCRIPTOR_HANDLE D3D12DescriptorSet::GetResourceTable() const { return m_resourceTable.gpuHandle; }
D3D12_GPU_DESCRIPTOR_HANDLE D3D12DescriptorSet::GetSamplerTable() const
{
    // Reflection can retain unused sampler bindings (e.g. TransmittanceCS).
    // Give those slots valid default descriptors and intern only the final
    // table, not intermediate configurations from a sequence of writes.
    // Parallel recording may bind the same set, so publication is synchronized.
    std::lock_guard lock(m_samplerMutex);
    if (!m_samplerTable)
        m_samplerTable = m_layout->AcquireSamplerTable(*m_context, m_samplerDescriptions);
    return m_samplerTable->GetGpuHandle();
}
const D3D12DescriptorSetLayout& D3D12DescriptorSet::GetLayout() const { return *m_layout; }

D3D12_GPU_VIRTUAL_ADDRESS D3D12DescriptorSet::GetDynamicBufferGpuAddress(
    const std::uint32_t binding,
    const std::uint32_t dynamicOffset) const
{
    const auto found = m_dynamicBuffers.find(binding);
    Core::Check(found != m_dynamicBuffers.end(), "A D3D12 dynamic constant buffer was not written before binding.");
    const DynamicBufferBinding& value = found->second;
    Core::Check(dynamicOffset % ConstantBufferAlignment == 0,
                "D3D12 dynamic constant-buffer offsets must be 256-byte aligned.");
    Core::Check(value.offset + dynamicOffset + value.range <= value.buffer->GetDescription().size,
                "D3D12 dynamic constant-buffer range exceeds its buffer.");
    return value.buffer->GetGpuVirtualAddress() + value.offset + dynamicOffset;
}
} // namespace Prism::RHI::D3D12
