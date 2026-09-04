#pragma once

#include "RHI/GraphicsApi.h"
#include "RHI/GraphicsTypes.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace Prism::RHI
{
class IRayTracingAccelerationStructure;

struct TransientTextureAllocationInfo
{
    std::uint64_t poolId = 0;
    std::size_t allocationIndex = 0;
    std::uint64_t logicalBytes = 0;
    std::uint64_t allocationBytes = 0;
    std::uint64_t poolPhysicalBytes = 0;
};

struct TransientBufferAllocationInfo
{
    std::uint64_t poolId = 0;
    std::size_t allocationIndex = 0;
    std::uint64_t logicalBytes = 0;
    std::uint64_t allocationBytes = 0;
    std::uint64_t poolPhysicalBytes = 0;
};

enum class BufferUsage : std::uint32_t
{
    None = 0,
    Vertex = 1u << 0u,
    Index = 1u << 1u,
    Constant = 1u << 2u,
    Storage = 1u << 3u,
    CopySource = 1u << 4u,
    CopyDestination = 1u << 5u,
    Indirect = 1u << 6u,
    ShaderResource = 1u << 7u,
    AccelerationStructureStorage = 1u << 8u,
    AccelerationStructureBuildInput = 1u << 9u,
    ShaderBindingTable = 1u << 10u,
    AccelerationStructureScratch = 1u << 11u
};

enum class IndexFormat
{
    UInt16,
    UInt32
};

enum class DescriptorType
{
    ConstantBuffer,
    DynamicConstantBuffer,
    SampledTexture,
    AccelerationStructure,
    ReadOnlyStorageBuffer,
    Sampler,
    StorageBuffer,
    StorageTexture
};

enum class TextureViewType
{
    Sampled,
    RenderTarget,
    DepthStencil,
    Storage
};

enum class ShaderStageFlags : std::uint32_t
{
    None = 0,
    // Keep the original values stable: descriptor/cache data may persist
    // these masks across runs.  Tessellation stages are appended instead of
    // shifting existing vertex/pixel/compute/ray-tracing bits.
    Vertex = 1u << 0u,
    Pixel = 1u << 1u,
    Compute = 1u << 2u,
    RayGeneration = 1u << 3u,
    AnyHit = 1u << 4u,
    ClosestHit = 1u << 5u,
    Miss = 1u << 6u,
    Intersection = 1u << 7u,
    Callable = 1u << 8u,
    Hull = 1u << 9u,
    Domain = 1u << 10u,
    AllGraphics = (1u << 0u) | (1u << 1u) | (1u << 9u) | (1u << 10u),
    AllRayTracing = (1u << 3u) | (1u << 4u)
                    | (1u << 5u) | (1u << 6u)
                    | (1u << 7u) | (1u << 8u),
    All = (1u << 11u) - 1u
};

constexpr BufferUsage operator|(const BufferUsage lhs, const BufferUsage rhs)
{
    return static_cast<BufferUsage>(static_cast<std::uint32_t>(lhs) | static_cast<std::uint32_t>(rhs));
}

constexpr BufferUsage operator&(const BufferUsage lhs, const BufferUsage rhs)
{
    return static_cast<BufferUsage>(static_cast<std::uint32_t>(lhs) & static_cast<std::uint32_t>(rhs));
}

constexpr ShaderStageFlags operator|(const ShaderStageFlags lhs, const ShaderStageFlags rhs)
{
    return static_cast<ShaderStageFlags>(static_cast<std::uint32_t>(lhs) | static_cast<std::uint32_t>(rhs));
}

constexpr ShaderStageFlags operator&(const ShaderStageFlags lhs, const ShaderStageFlags rhs)
{
    return static_cast<ShaderStageFlags>(static_cast<std::uint32_t>(lhs) & static_cast<std::uint32_t>(rhs));
}

constexpr bool HasAnyFlag(const BufferUsage value, const BufferUsage flags)
{
    return (value & flags) != BufferUsage::None;
}

constexpr bool HasAnyFlag(const ShaderStageFlags value, const ShaderStageFlags flags)
{
    return (value & flags) != ShaderStageFlags::None;
}

struct BufferDescription
{
    std::size_t size = 0;
    std::uint32_t stride = 0;
    BufferUsage usage = BufferUsage::None;
    MemoryAccess memoryAccess = MemoryAccess::GpuOnly;
};

struct TextureInitialData
{
    const void* data = nullptr;
    std::size_t rowPitch = 0;
    std::size_t slicePitch = 0;
};

struct TextureViewDescription
{
    TextureViewType type = TextureViewType::Sampled;
    Format format = Format::Unknown;
    std::uint32_t baseMipLevel = 0;
    std::uint32_t mipLevelCount = 1;
    std::uint32_t baseArrayLayer = 0;
    std::uint32_t arrayLayerCount = 1;
};

struct DescriptorBindingDescription
{
    std::uint32_t binding = 0;
    DescriptorType type = DescriptorType::ConstantBuffer;
    std::uint32_t descriptorCount = 1;
    ShaderStageFlags stages = ShaderStageFlags::All;
};

struct DescriptorSetLayoutDescription
{
    std::vector<DescriptorBindingDescription> bindings;
};

struct DynamicBufferOffset
{
    std::uint32_t binding = 0;
    std::uint32_t offset = 0;
};

class IGraphicsResource
{
public:
    virtual ~IGraphicsResource() = default;
    virtual GraphicsApi GetGraphicsApi() const = 0;
    virtual void SetDebugName(std::string_view)
    {
    }
};

class IBuffer : public IGraphicsResource
{
public:
    virtual const BufferDescription& GetDescription() const = 0;
    virtual void Update(const void* data, std::size_t size, std::size_t offset = 0) = 0;
    virtual void Read(
        void* data,
        std::size_t size,
        std::size_t offset = 0) const = 0;
    virtual const TransientBufferAllocationInfo*
        GetTransientAllocationInfo() const
    {
        return nullptr;
    }
};

class ITexture : public IGraphicsResource
{
public:
    virtual const TextureDescription& GetDescription() const = 0;
    virtual const TransientTextureAllocationInfo*
        GetTransientAllocationInfo() const
    {
        return nullptr;
    }
};

class ITextureView : public IGraphicsResource
{
public:
    virtual const TextureViewDescription& GetDescription() const = 0;
    virtual const ITexture* GetTexture() const = 0;
};

class ISampler : public IGraphicsResource
{
public:
    virtual const SamplerDescription& GetDescription() const = 0;
};

class IDescriptorSetLayout : public IGraphicsResource
{
public:
    virtual const DescriptorSetLayoutDescription& GetDescription() const = 0;
};

class IDescriptorSet : public IGraphicsResource
{
public:
    virtual void WriteBuffer(
        std::uint32_t binding,
        std::shared_ptr<IBuffer> buffer,
        std::size_t offset = 0,
        std::size_t range = 0) = 0;
    virtual void WriteTexture(std::uint32_t binding, std::shared_ptr<ITexture> texture) = 0;
    virtual void WriteTextureView(std::uint32_t binding, std::shared_ptr<ITextureView> textureView) = 0;
    virtual void WriteSampler(std::uint32_t binding, std::shared_ptr<ISampler> sampler) = 0;
    virtual void WriteAccelerationStructure(
        std::uint32_t binding,
        std::shared_ptr<IRayTracingAccelerationStructure>
            accelerationStructure) = 0;
};

bool ValidateBufferDescription(const BufferDescription& description, const void* initialData, std::string* outError = nullptr);
bool ValidateTextureViewDescription(
    const TextureDescription& texture,
    const TextureViewDescription& view,
    std::string* outError = nullptr);
bool ValidateDescriptorSetLayoutDescription(const DescriptorSetLayoutDescription& description, std::string* outError = nullptr);
} // namespace Prism::RHI
