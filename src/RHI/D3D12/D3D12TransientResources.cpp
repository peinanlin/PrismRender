#include "RHI/D3D12/D3D12TransientResources.h"

#include "Core/Assert.h"
#include "RHI/D3D12/D3D12Resources.h"
#include "RHI/D3D12/D3D12TypeConversions.h"
#include "RHI/D3D12/D3D12Context.h"

#include <algorithm>
#include <unordered_map>
#include <utility>
#include <wrl/client.h>

namespace Prism::RHI::D3D12
{
namespace
{
struct HeapOwner
{
    Microsoft::WRL::ComPtr<ID3D12Heap> heap;
};

struct Slot
{
    std::shared_ptr<HeapOwner> owner;
    std::uint64_t bytes = 0;
};

D3D12_CLEAR_VALUE BuildClearValue(
    const TextureDescription& description)
{
    D3D12_CLEAR_VALUE clearValue{};
    if (IsDepthFormat(description.format))
    {
        clearValue.Format = DXGI_FORMAT_D32_FLOAT;
        clearValue.DepthStencil.Depth = 1.0f;
    }
    else
    {
        clearValue.Format = ToNativeFormat(description.format);
        clearValue.Color[3] = 1.0f;
    }
    return clearValue;
}

D3D12_RESOURCE_DESC BuildBufferDescription(
    const BufferDescription& description)
{
    const std::uint64_t allocationSize =
        HasAnyFlag(
            description.usage,
            BufferUsage::Constant)
        ? (description.size
              + D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT
              - 1u)
            & ~(static_cast<std::uint64_t>(
                    D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT)
                - 1u)
        : description.size;
    D3D12_RESOURCE_DESC native{};
    native.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    native.Width = allocationSize;
    native.Height = 1;
    native.DepthOrArraySize = 1;
    native.MipLevels = 1;
    native.Format = DXGI_FORMAT_UNKNOWN;
    native.SampleDesc.Count = 1;
    native.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    if (HasAnyFlag(
            description.usage,
            BufferUsage::Storage))
    {
        native.Flags =
            D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    }
    return native;
}
} // namespace

D3D12TransientTexturePool::D3D12TransientTexturePool(
    D3D12Context& context,
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

    std::unordered_map<std::size_t, Slot> slots;
    for (const TransientTextureRequest& request : requests)
    {
        D3D12_RESOURCE_DESC nativeDescription =
            ToNativeTextureDescription(request.description);
        if (IsDepthFormat(request.description.format)
            && HasAnyFlag(
                request.description.usage,
                TextureUsage::ShaderResource))
        {
            nativeDescription.Format =
                DXGI_FORMAT_R32_TYPELESS;
        }

        const D3D12_RESOURCE_ALLOCATION_INFO allocation =
            context.GetDevice()->GetResourceAllocationInfo(
                0,
                1,
                &nativeDescription);
        Core::Check(
            allocation.SizeInBytes > 0
                && allocation.Alignment > 0,
            "D3D12 returned invalid transient texture allocation information.");
        m_statistics.logicalBytes += allocation.SizeInBytes;

        auto [slot, inserted] = slots.try_emplace(
            request.allocationIndex);
        if (inserted)
        {
            slot->second.owner =
                std::make_shared<HeapOwner>();
            slot->second.bytes = allocation.SizeInBytes;
            D3D12_HEAP_DESC heapDescription{};
            heapDescription.SizeInBytes =
                allocation.SizeInBytes;
            heapDescription.Alignment =
                allocation.Alignment;
            heapDescription.Properties.Type =
                D3D12_HEAP_TYPE_DEFAULT;
            heapDescription.Properties.CreationNodeMask = 1;
            heapDescription.Properties.VisibleNodeMask = 1;
            heapDescription.Flags =
                D3D12_HEAP_FLAG_NONE;
            Core::ThrowIfFailed(
                context.GetDevice()->CreateHeap(
                    &heapDescription,
                    IID_PPV_ARGS(
                        &slot->second.owner->heap)),
                "Failed to create a D3D12 transient texture heap.");
            m_statistics.physicalBytes +=
                allocation.SizeInBytes;
        }
        else
        {
            Core::Check(
                slot->second.bytes == allocation.SizeInBytes,
                "Aliased D3D12 textures require identical allocation sizes.");
        }

        const bool optimizedClear =
            HasAnyFlag(
                request.description.usage,
                TextureUsage::RenderTarget
                    | TextureUsage::DepthStencil);
        const D3D12_CLEAR_VALUE clearValue =
            BuildClearValue(request.description);
        Microsoft::WRL::ComPtr<ID3D12Resource> resource;
        Core::ThrowIfFailed(
            context.GetDevice()->CreatePlacedResource(
                slot->second.owner->heap.Get(),
                0,
                &nativeDescription,
                D3D12_RESOURCE_STATE_COMMON,
                optimizedClear ? &clearValue : nullptr,
                IID_PPV_ARGS(&resource)),
            "Failed to create a D3D12 placed transient texture.");

        TransientTextureAllocationInfo info{};
        info.poolId = m_statistics.poolId;
        info.allocationIndex =
            request.allocationIndex;
        info.logicalBytes = allocation.SizeInBytes;
        info.allocationBytes = slot->second.bytes;
        info.poolPhysicalBytes =
            m_statistics.physicalBytes;
        m_textures.emplace(
            request.name,
            std::make_shared<D3D12Texture>(
                context,
                resource.Get(),
                request.description,
                info,
                slot->second.owner));
    }

    m_statistics.allocationCount = slots.size();
    m_statistics.aliasedBytes =
        m_statistics.logicalBytes
            - m_statistics.physicalBytes;
    for (auto& [name, texture] : m_textures)
    {
        (void)name;
        auto native =
            std::dynamic_pointer_cast<D3D12Texture>(
                texture);
        TransientTextureAllocationInfo info =
            *native->GetTransientAllocationInfo();
        info.poolPhysicalBytes =
            m_statistics.physicalBytes;
        texture = std::make_shared<D3D12Texture>(
            context,
            native->GetResource(),
            native->GetDescription(),
            info,
            slots.at(info.allocationIndex).owner);
    }
}

GraphicsApi
D3D12TransientTexturePool::GetGraphicsApi() const
{
    return GraphicsApi::Direct3D12;
}

std::shared_ptr<ITexture>
D3D12TransientTexturePool::GetTexture(
    const std::string_view name) const
{
    const auto found = m_textures.find(std::string(name));
    Core::Check(
        found != m_textures.end(),
        "A D3D12 transient texture was not found.");
    return found->second;
}

const TransientTexturePoolStatistics&
D3D12TransientTexturePool::GetStatistics() const
{
    return m_statistics;
}

D3D12TransientBufferPool::D3D12TransientBufferPool(
    D3D12Context& context,
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

    std::unordered_map<std::size_t, Slot> slots;
    for (const TransientBufferRequest& request : requests)
    {
        const D3D12_RESOURCE_DESC nativeDescription =
            BuildBufferDescription(request.description);
        const D3D12_RESOURCE_ALLOCATION_INFO allocation =
            context.GetDevice()->GetResourceAllocationInfo(
                0,
                1,
                &nativeDescription);
        Core::Check(
            allocation.SizeInBytes > 0
                && allocation.Alignment > 0,
            "D3D12 returned invalid transient buffer allocation information.");
        m_statistics.logicalBytes += allocation.SizeInBytes;

        auto [slot, inserted] = slots.try_emplace(
            request.allocationIndex);
        if (inserted)
        {
            slot->second.owner =
                std::make_shared<HeapOwner>();
            slot->second.bytes = allocation.SizeInBytes;
            D3D12_HEAP_DESC heapDescription{};
            heapDescription.SizeInBytes =
                allocation.SizeInBytes;
            heapDescription.Alignment =
                allocation.Alignment;
            heapDescription.Properties.Type =
                D3D12_HEAP_TYPE_DEFAULT;
            heapDescription.Properties.CreationNodeMask = 1;
            heapDescription.Properties.VisibleNodeMask = 1;
            heapDescription.Flags =
                D3D12_HEAP_FLAG_ALLOW_ONLY_BUFFERS;
            Core::ThrowIfFailed(
                context.GetDevice()->CreateHeap(
                    &heapDescription,
                    IID_PPV_ARGS(
                        &slot->second.owner->heap)),
                "Failed to create a D3D12 transient buffer heap.");
            m_statistics.physicalBytes +=
                allocation.SizeInBytes;
        }
        else
        {
            Core::Check(
                slot->second.bytes == allocation.SizeInBytes,
                "Aliased D3D12 buffers require identical allocation sizes.");
        }

        Microsoft::WRL::ComPtr<ID3D12Resource> resource;
        Core::ThrowIfFailed(
            context.GetDevice()->CreatePlacedResource(
                slot->second.owner->heap.Get(),
                0,
                &nativeDescription,
                D3D12_RESOURCE_STATE_COMMON,
                nullptr,
                IID_PPV_ARGS(&resource)),
            "Failed to create a D3D12 placed transient buffer.");

        TransientBufferAllocationInfo info{};
        info.poolId = m_statistics.poolId;
        info.allocationIndex =
            request.allocationIndex;
        info.logicalBytes = allocation.SizeInBytes;
        info.allocationBytes = slot->second.bytes;
        info.poolPhysicalBytes =
            m_statistics.physicalBytes;
        m_buffers.emplace(
            request.name,
            std::make_shared<D3D12Buffer>(
                context,
                resource.Get(),
                request.description,
                info,
                slot->second.owner));
    }

    m_statistics.allocationCount = slots.size();
    m_statistics.aliasedBytes =
        m_statistics.logicalBytes
            - m_statistics.physicalBytes;
    for (auto& [name, buffer] : m_buffers)
    {
        (void)name;
        auto native =
            std::dynamic_pointer_cast<D3D12Buffer>(
                buffer);
        TransientBufferAllocationInfo info =
            *native->GetTransientAllocationInfo();
        info.poolPhysicalBytes =
            m_statistics.physicalBytes;
        buffer = std::make_shared<D3D12Buffer>(
            context,
            native->GetResource(),
            native->GetDescription(),
            info,
            slots.at(info.allocationIndex).owner);
    }
}

GraphicsApi
D3D12TransientBufferPool::GetGraphicsApi() const
{
    return GraphicsApi::Direct3D12;
}

std::shared_ptr<IBuffer>
D3D12TransientBufferPool::GetBuffer(
    const std::string_view name) const
{
    const auto found = m_buffers.find(std::string(name));
    Core::Check(
        found != m_buffers.end(),
        "A D3D12 transient buffer was not found.");
    return found->second;
}

const TransientBufferPoolStatistics&
D3D12TransientBufferPool::GetStatistics() const
{
    return m_statistics;
}
} // namespace Prism::RHI::D3D12
