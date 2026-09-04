#pragma once

#include <cstdint>
#include <string>

namespace Prism::RHI
{
enum class Format
{
    Unknown,
    R8Unorm,
    R16Float,
    R32Float,
    R32Typeless,
    Rg16Float,
    Rgba8Unorm,
    Rgba8UnormSrgb,
    Bgra8Unorm,
    Bgra8UnormSrgb,
    Rgba16Float,
    Rgba32Float,
    D32Float
};

enum class TextureDimension
{
    Texture2D,
    TextureCube
};

enum class TextureUsage : std::uint32_t
{
    None = 0,
    ShaderResource = 1u << 0u,
    RenderTarget = 1u << 1u,
    DepthStencil = 1u << 2u,
    UnorderedAccess = 1u << 3u,
    CopySource = 1u << 4u,
    CopyDestination = 1u << 5u
};

enum class ResourceState : std::uint32_t
{
    Undefined = 0,
    Common = 1u << 0u,
    Present = 1u << 1u,
    RenderTarget = 1u << 2u,
    DepthWrite = 1u << 3u,
    DepthRead = 1u << 4u,
    ShaderResource = 1u << 5u,
    UnorderedAccess = 1u << 6u,
    CopySource = 1u << 7u,
    CopyDestination = 1u << 8u,
    VertexBuffer = 1u << 9u,
    IndexBuffer = 1u << 10u,
    ConstantBuffer = 1u << 11u,
    IndirectArgument = 1u << 12u,
    AccelerationStructure = 1u << 13u
};

enum class MemoryAccess
{
    GpuOnly,
    CpuToGpu,
    GpuToCpu
};

enum class Filter
{
    Nearest,
    Linear,
    Anisotropic,
    ComparisonLinear
};

enum class AddressMode
{
    Repeat,
    MirroredRepeat,
    ClampToEdge,
    ClampToBorder
};

enum class CompareOperation
{
    Never,
    Less,
    Equal,
    LessEqual,
    Greater,
    NotEqual,
    GreaterEqual,
    Always
};

enum class FillMode
{
    Solid,
    Wireframe
};

enum class CullMode
{
    None,
    Front,
    Back
};

enum class FrontFace
{
    Clockwise,
    CounterClockwise
};

enum class BlendFactor
{
    Zero,
    One,
    SourceColor,
    InverseSourceColor,
    SourceAlpha,
    InverseSourceAlpha,
    DestinationColor,
    InverseDestinationColor,
    DestinationAlpha,
    InverseDestinationAlpha
};

enum class BlendOperation
{
    Add,
    Subtract,
    ReverseSubtract,
    Minimum,
    Maximum
};

constexpr TextureUsage operator|(const TextureUsage lhs, const TextureUsage rhs)
{
    return static_cast<TextureUsage>(static_cast<std::uint32_t>(lhs) | static_cast<std::uint32_t>(rhs));
}

constexpr TextureUsage operator&(const TextureUsage lhs, const TextureUsage rhs)
{
    return static_cast<TextureUsage>(static_cast<std::uint32_t>(lhs) & static_cast<std::uint32_t>(rhs));
}

constexpr ResourceState operator|(const ResourceState lhs, const ResourceState rhs)
{
    return static_cast<ResourceState>(static_cast<std::uint32_t>(lhs) | static_cast<std::uint32_t>(rhs));
}

constexpr ResourceState operator&(const ResourceState lhs, const ResourceState rhs)
{
    return static_cast<ResourceState>(static_cast<std::uint32_t>(lhs) & static_cast<std::uint32_t>(rhs));
}

constexpr bool HasAnyFlag(const TextureUsage value, const TextureUsage flags)
{
    return (value & flags) != TextureUsage::None;
}

constexpr bool HasAnyFlag(const ResourceState value, const ResourceState flags)
{
    return (value & flags) != ResourceState::Undefined;
}

struct TextureDescription
{
    TextureDimension dimension = TextureDimension::Texture2D;
    std::uint32_t width = 1;
    std::uint32_t height = 1;
    std::uint32_t arrayLayers = 1;
    std::uint32_t mipLevels = 1;
    std::uint32_t sampleCount = 1;
    Format format = Format::Rgba8Unorm;
    TextureUsage usage = TextureUsage::ShaderResource;
    MemoryAccess memoryAccess = MemoryAccess::GpuOnly;
};

struct SamplerDescription
{
    Filter filter = Filter::Linear;
    AddressMode addressU = AddressMode::Repeat;
    AddressMode addressV = AddressMode::Repeat;
    AddressMode addressW = AddressMode::Repeat;
    CompareOperation comparison = CompareOperation::Always;
    float mipLodBias = 0.0f;
    std::uint32_t maxAnisotropy = 1;
    float minLod = 0.0f;
    float maxLod = 3.402823466e+38f;
};

struct RasterizerDescription
{
    FillMode fillMode = FillMode::Solid;
    CullMode cullMode = CullMode::Back;
    FrontFace frontFace = FrontFace::Clockwise;
    std::int32_t depthBias = 0;
    float depthBiasClamp = 0.0f;
    float slopeScaledDepthBias = 0.0f;
    bool depthClipEnabled = true;
    bool multisampleEnabled = false;
    bool conservativeRasterEnabled = false;
};

struct DepthStencilDescription
{
    bool depthTestEnabled = true;
    bool depthWriteEnabled = true;
    CompareOperation depthComparison = CompareOperation::Less;
    bool stencilEnabled = false;
    std::uint8_t stencilReadMask = 0xff;
    std::uint8_t stencilWriteMask = 0xff;
};

struct BlendAttachmentDescription
{
    bool blendEnabled = false;
    BlendFactor sourceColor = BlendFactor::One;
    BlendFactor destinationColor = BlendFactor::Zero;
    BlendOperation colorOperation = BlendOperation::Add;
    BlendFactor sourceAlpha = BlendFactor::One;
    BlendFactor destinationAlpha = BlendFactor::Zero;
    BlendOperation alphaOperation = BlendOperation::Add;
    std::uint8_t colorWriteMask = 0x0f;
};

bool IsDepthFormat(Format format);
bool ValidateTextureDescription(const TextureDescription& description, std::string* outError = nullptr);
} // namespace Prism::RHI
