#include "RHI/D3D12/D3D12TypeConversions.h"

#include <algorithm>
#include <limits>
#include <stdexcept>
#include <string>

namespace Prism::RHI::D3D12
{
namespace
{
[[noreturn]] void ThrowUnsupported(const char* typeName)
{
    throw std::invalid_argument(std::string("Unsupported D3D12 ") + typeName + " conversion.");
}
} // namespace

DXGI_FORMAT ToNativeFormat(const Format format)
{
    switch (format)
    {
    case Format::Unknown: return DXGI_FORMAT_UNKNOWN;
    case Format::R8Unorm: return DXGI_FORMAT_R8_UNORM;
    case Format::R16Float: return DXGI_FORMAT_R16_FLOAT;
    case Format::R32Float: return DXGI_FORMAT_R32_FLOAT;
    case Format::R32Typeless: return DXGI_FORMAT_R32_TYPELESS;
    case Format::Rg16Float: return DXGI_FORMAT_R16G16_FLOAT;
    case Format::Rgba8Unorm: return DXGI_FORMAT_R8G8B8A8_UNORM;
    case Format::Rgba8UnormSrgb: return DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
    case Format::Bgra8Unorm: return DXGI_FORMAT_B8G8R8A8_UNORM;
    case Format::Bgra8UnormSrgb: return DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
    case Format::Rgba16Float: return DXGI_FORMAT_R16G16B16A16_FLOAT;
    case Format::Rgba32Float: return DXGI_FORMAT_R32G32B32A32_FLOAT;
    case Format::D32Float: return DXGI_FORMAT_D32_FLOAT;
    }
    ThrowUnsupported("format");
}

D3D12_RESOURCE_STATES ToNativeResourceStates(const ResourceState state)
{
    D3D12_RESOURCE_STATES nativeState = D3D12_RESOURCE_STATE_COMMON;
    if (HasAnyFlag(state, ResourceState::RenderTarget)) nativeState |= D3D12_RESOURCE_STATE_RENDER_TARGET;
    if (HasAnyFlag(state, ResourceState::DepthWrite)) nativeState |= D3D12_RESOURCE_STATE_DEPTH_WRITE;
    if (HasAnyFlag(state, ResourceState::DepthRead)) nativeState |= D3D12_RESOURCE_STATE_DEPTH_READ;
    if (HasAnyFlag(state, ResourceState::ShaderResource))
    {
        nativeState |= D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    }
    if (HasAnyFlag(state, ResourceState::UnorderedAccess)) nativeState |= D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    if (HasAnyFlag(state, ResourceState::CopySource)) nativeState |= D3D12_RESOURCE_STATE_COPY_SOURCE;
    if (HasAnyFlag(state, ResourceState::CopyDestination)) nativeState |= D3D12_RESOURCE_STATE_COPY_DEST;
    if (HasAnyFlag(state, ResourceState::VertexBuffer | ResourceState::ConstantBuffer))
    {
        nativeState |= D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER;
    }
    if (HasAnyFlag(state, ResourceState::IndexBuffer)) nativeState |= D3D12_RESOURCE_STATE_INDEX_BUFFER;
    if (HasAnyFlag(state, ResourceState::IndirectArgument)) nativeState |= D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT;
    if (HasAnyFlag(state, ResourceState::AccelerationStructure))
        nativeState |= D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE;
    return nativeState;
}

D3D12_RESOURCE_FLAGS ToNativeTextureFlags(const TextureUsage usage)
{
    D3D12_RESOURCE_FLAGS flags = D3D12_RESOURCE_FLAG_NONE;
    if (HasAnyFlag(usage, TextureUsage::RenderTarget)) flags |= D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    if (HasAnyFlag(usage, TextureUsage::DepthStencil)) flags |= D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
    if (HasAnyFlag(usage, TextureUsage::UnorderedAccess)) flags |= D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    if (!HasAnyFlag(usage, TextureUsage::ShaderResource)) flags |= D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE;
    return flags;
}

D3D12_HEAP_TYPE ToNativeHeapType(const MemoryAccess memoryAccess)
{
    switch (memoryAccess)
    {
    case MemoryAccess::GpuOnly: return D3D12_HEAP_TYPE_DEFAULT;
    case MemoryAccess::CpuToGpu: return D3D12_HEAP_TYPE_UPLOAD;
    case MemoryAccess::GpuToCpu: return D3D12_HEAP_TYPE_READBACK;
    }
    ThrowUnsupported("heap type");
}

D3D12_FILTER ToNativeFilter(const Filter filter)
{
    switch (filter)
    {
    case Filter::Nearest: return D3D12_FILTER_MIN_MAG_MIP_POINT;
    case Filter::Linear: return D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    case Filter::Anisotropic: return D3D12_FILTER_ANISOTROPIC;
    case Filter::ComparisonLinear: return D3D12_FILTER_COMPARISON_MIN_MAG_LINEAR_MIP_POINT;
    }
    ThrowUnsupported("filter");
}

D3D12_TEXTURE_ADDRESS_MODE ToNativeAddressMode(const AddressMode addressMode)
{
    switch (addressMode)
    {
    case AddressMode::Repeat: return D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    case AddressMode::MirroredRepeat: return D3D12_TEXTURE_ADDRESS_MODE_MIRROR;
    case AddressMode::ClampToEdge: return D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    case AddressMode::ClampToBorder: return D3D12_TEXTURE_ADDRESS_MODE_BORDER;
    }
    ThrowUnsupported("address mode");
}

D3D12_COMPARISON_FUNC ToNativeCompareOperation(const CompareOperation operation)
{
    switch (operation)
    {
    case CompareOperation::Never: return D3D12_COMPARISON_FUNC_NEVER;
    case CompareOperation::Less: return D3D12_COMPARISON_FUNC_LESS;
    case CompareOperation::Equal: return D3D12_COMPARISON_FUNC_EQUAL;
    case CompareOperation::LessEqual: return D3D12_COMPARISON_FUNC_LESS_EQUAL;
    case CompareOperation::Greater: return D3D12_COMPARISON_FUNC_GREATER;
    case CompareOperation::NotEqual: return D3D12_COMPARISON_FUNC_NOT_EQUAL;
    case CompareOperation::GreaterEqual: return D3D12_COMPARISON_FUNC_GREATER_EQUAL;
    case CompareOperation::Always: return D3D12_COMPARISON_FUNC_ALWAYS;
    }
    ThrowUnsupported("comparison operation");
}

D3D12_BLEND ToNativeBlendFactor(const BlendFactor factor)
{
    switch (factor)
    {
    case BlendFactor::Zero: return D3D12_BLEND_ZERO;
    case BlendFactor::One: return D3D12_BLEND_ONE;
    case BlendFactor::SourceColor: return D3D12_BLEND_SRC_COLOR;
    case BlendFactor::InverseSourceColor: return D3D12_BLEND_INV_SRC_COLOR;
    case BlendFactor::SourceAlpha: return D3D12_BLEND_SRC_ALPHA;
    case BlendFactor::InverseSourceAlpha: return D3D12_BLEND_INV_SRC_ALPHA;
    case BlendFactor::DestinationColor: return D3D12_BLEND_DEST_COLOR;
    case BlendFactor::InverseDestinationColor: return D3D12_BLEND_INV_DEST_COLOR;
    case BlendFactor::DestinationAlpha: return D3D12_BLEND_DEST_ALPHA;
    case BlendFactor::InverseDestinationAlpha: return D3D12_BLEND_INV_DEST_ALPHA;
    }
    ThrowUnsupported("blend factor");
}

D3D12_BLEND_OP ToNativeBlendOperation(const BlendOperation operation)
{
    switch (operation)
    {
    case BlendOperation::Add: return D3D12_BLEND_OP_ADD;
    case BlendOperation::Subtract: return D3D12_BLEND_OP_SUBTRACT;
    case BlendOperation::ReverseSubtract: return D3D12_BLEND_OP_REV_SUBTRACT;
    case BlendOperation::Minimum: return D3D12_BLEND_OP_MIN;
    case BlendOperation::Maximum: return D3D12_BLEND_OP_MAX;
    }
    ThrowUnsupported("blend operation");
}

D3D12_RESOURCE_DESC ToNativeTextureDescription(const TextureDescription& description)
{
    std::string validationError;
    if (!ValidateTextureDescription(description, &validationError))
    {
        throw std::invalid_argument(validationError);
    }
    if (description.arrayLayers > std::numeric_limits<UINT16>::max()
        || description.mipLevels > std::numeric_limits<UINT16>::max())
    {
        throw std::invalid_argument("D3D12 texture layers or mip levels exceed UINT16 capacity.");
    }

    D3D12_RESOURCE_DESC native{};
    native.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    native.Width = description.width;
    native.Height = description.height;
    native.DepthOrArraySize = static_cast<UINT16>(description.arrayLayers);
    native.MipLevels = static_cast<UINT16>(description.mipLevels);
    native.Format = ToNativeFormat(description.format);
    native.SampleDesc.Count = description.sampleCount;
    native.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    native.Flags = ToNativeTextureFlags(description.usage);
    return native;
}

D3D12_SHADER_RESOURCE_VIEW_DESC ToNativeSampledTextureViewDescription(
    const TextureDescription& texture, const TextureViewDescription& view)
{
    std::string validationError;
    if (view.type != TextureViewType::Sampled
        || !ValidateTextureViewDescription(texture, view, &validationError))
        throw std::invalid_argument("Invalid D3D12 sampled texture view: " + validationError);

    D3D12_SHADER_RESOURCE_VIEW_DESC native{};
    native.Format = IsDepthFormat(texture.format) ? DXGI_FORMAT_R32_FLOAT
        : ToNativeFormat(view.format == Format::Unknown ? texture.format : view.format);
    native.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    if (texture.dimension == TextureDimension::TextureCube && view.arrayLayerCount > 6u)
    {
        if (view.baseArrayLayer % 6u != 0u || view.arrayLayerCount % 6u != 0u)
            throw std::invalid_argument("D3D12 cube-array views require whole cubes.");
        native.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBEARRAY;
        native.TextureCubeArray.MostDetailedMip = view.baseMipLevel;
        native.TextureCubeArray.MipLevels = view.mipLevelCount;
        native.TextureCubeArray.First2DArrayFace = view.baseArrayLayer;
        native.TextureCubeArray.NumCubes = view.arrayLayerCount / 6u;
    }
    else if (texture.dimension == TextureDimension::TextureCube)
    {
        if (view.baseArrayLayer != 0u || view.arrayLayerCount != 6u)
            throw std::invalid_argument("D3D12 cube views require exactly six faces.");
        native.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBE;
        native.TextureCube.MostDetailedMip = view.baseMipLevel;
        native.TextureCube.MipLevels = view.mipLevelCount;
    }
    else if (texture.arrayLayers > 1u)
    {
        native.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
        native.Texture2DArray.MostDetailedMip = view.baseMipLevel;
        native.Texture2DArray.MipLevels = view.mipLevelCount;
        native.Texture2DArray.FirstArraySlice = view.baseArrayLayer;
        native.Texture2DArray.ArraySize = view.arrayLayerCount;
    }
    else
    {
        native.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        native.Texture2D.MostDetailedMip = view.baseMipLevel;
        native.Texture2D.MipLevels = view.mipLevelCount;
    }
    return native;
}

D3D12_SAMPLER_DESC ToNativeSamplerDescription(const SamplerDescription& description)
{
    D3D12_SAMPLER_DESC native{};
    native.Filter = ToNativeFilter(description.filter);
    native.AddressU = ToNativeAddressMode(description.addressU);
    native.AddressV = ToNativeAddressMode(description.addressV);
    native.AddressW = ToNativeAddressMode(description.addressW);
    native.MipLODBias = description.mipLodBias;
    native.MaxAnisotropy = std::clamp(description.maxAnisotropy, 1u, 16u);
    native.ComparisonFunc = ToNativeCompareOperation(description.comparison);
    native.MinLOD = description.minLod;
    native.MaxLOD = description.maxLod;
    return native;
}

D3D12_RASTERIZER_DESC ToNativeRasterizerDescription(const RasterizerDescription& description)
{
    D3D12_RASTERIZER_DESC native{};
    native.FillMode = description.fillMode == FillMode::Wireframe ? D3D12_FILL_MODE_WIREFRAME : D3D12_FILL_MODE_SOLID;
    switch (description.cullMode)
    {
    case CullMode::None: native.CullMode = D3D12_CULL_MODE_NONE; break;
    case CullMode::Front: native.CullMode = D3D12_CULL_MODE_FRONT; break;
    case CullMode::Back: native.CullMode = D3D12_CULL_MODE_BACK; break;
    }
    native.FrontCounterClockwise = description.frontFace == FrontFace::CounterClockwise;
    native.DepthBias = description.depthBias;
    native.DepthBiasClamp = description.depthBiasClamp;
    native.SlopeScaledDepthBias = description.slopeScaledDepthBias;
    native.DepthClipEnable = description.depthClipEnabled;
    native.MultisampleEnable = description.multisampleEnabled;
    native.ConservativeRaster = description.conservativeRasterEnabled
        ? D3D12_CONSERVATIVE_RASTERIZATION_MODE_ON
        : D3D12_CONSERVATIVE_RASTERIZATION_MODE_OFF;
    return native;
}

D3D12_DEPTH_STENCIL_DESC ToNativeDepthStencilDescription(const DepthStencilDescription& description)
{
    D3D12_DEPTH_STENCIL_DESC native{};
    native.DepthEnable = description.depthTestEnabled;
    native.DepthWriteMask = description.depthWriteEnabled ? D3D12_DEPTH_WRITE_MASK_ALL : D3D12_DEPTH_WRITE_MASK_ZERO;
    native.DepthFunc = ToNativeCompareOperation(description.depthComparison);
    native.StencilEnable = description.stencilEnabled;
    native.StencilReadMask = description.stencilReadMask;
    native.StencilWriteMask = description.stencilWriteMask;
    native.FrontFace.StencilFailOp = D3D12_STENCIL_OP_KEEP;
    native.FrontFace.StencilDepthFailOp = D3D12_STENCIL_OP_KEEP;
    native.FrontFace.StencilPassOp = D3D12_STENCIL_OP_KEEP;
    native.FrontFace.StencilFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    native.BackFace = native.FrontFace;
    return native;
}

D3D12_BLEND_DESC ToNativeBlendDescription(const BlendAttachmentDescription& description)
{
    D3D12_BLEND_DESC native{};
    D3D12_RENDER_TARGET_BLEND_DESC& target = native.RenderTarget[0];
    target.BlendEnable = description.blendEnabled;
    target.LogicOpEnable = FALSE;
    target.SrcBlend = ToNativeBlendFactor(description.sourceColor);
    target.DestBlend = ToNativeBlendFactor(description.destinationColor);
    target.BlendOp = ToNativeBlendOperation(description.colorOperation);
    target.SrcBlendAlpha = ToNativeBlendFactor(description.sourceAlpha);
    target.DestBlendAlpha = ToNativeBlendFactor(description.destinationAlpha);
    target.BlendOpAlpha = ToNativeBlendOperation(description.alphaOperation);
    target.LogicOp = D3D12_LOGIC_OP_NOOP;
    target.RenderTargetWriteMask = description.colorWriteMask;
    return native;
}

D3D12_SHADER_BYTECODE ToNativeShaderBytecode(const ShaderBinary& binary)
{
    if (!binary.IsValid() || (binary.format != ShaderBinaryFormat::Dxil && binary.format != ShaderBinaryFormat::Dxbc))
    {
        throw std::invalid_argument("D3D12 requires valid DXIL or DXBC shader bytecode.");
    }
    return {binary.Data(), binary.Size()};
}
} // namespace Prism::RHI::D3D12
