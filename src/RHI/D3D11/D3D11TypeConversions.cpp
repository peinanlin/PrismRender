#include "RHI/D3D11/D3D11TypeConversions.h"

#include <algorithm>
#include <stdexcept>
#include <string>

namespace Prism::RHI::D3D11
{
namespace
{
[[noreturn]] void ThrowUnsupported(const char* typeName)
{
    throw std::invalid_argument(std::string("Unsupported D3D11 ") + typeName + " conversion.");
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

UINT ToNativeBindFlags(const TextureUsage usage)
{
    UINT flags = 0;
    if (HasAnyFlag(usage, TextureUsage::ShaderResource)) flags |= D3D11_BIND_SHADER_RESOURCE;
    if (HasAnyFlag(usage, TextureUsage::RenderTarget)) flags |= D3D11_BIND_RENDER_TARGET;
    if (HasAnyFlag(usage, TextureUsage::DepthStencil)) flags |= D3D11_BIND_DEPTH_STENCIL;
    if (HasAnyFlag(usage, TextureUsage::UnorderedAccess)) flags |= D3D11_BIND_UNORDERED_ACCESS;
    return flags;
}

UINT ToNativeBindFlags(const ResourceState state)
{
    UINT flags = 0;
    if (HasAnyFlag(state, ResourceState::ShaderResource)) flags |= D3D11_BIND_SHADER_RESOURCE;
    if (HasAnyFlag(state, ResourceState::RenderTarget)) flags |= D3D11_BIND_RENDER_TARGET;
    if (HasAnyFlag(state, ResourceState::DepthWrite | ResourceState::DepthRead)) flags |= D3D11_BIND_DEPTH_STENCIL;
    if (HasAnyFlag(state, ResourceState::UnorderedAccess)) flags |= D3D11_BIND_UNORDERED_ACCESS;
    if (HasAnyFlag(state, ResourceState::VertexBuffer)) flags |= D3D11_BIND_VERTEX_BUFFER;
    if (HasAnyFlag(state, ResourceState::IndexBuffer)) flags |= D3D11_BIND_INDEX_BUFFER;
    if (HasAnyFlag(state, ResourceState::ConstantBuffer)) flags |= D3D11_BIND_CONSTANT_BUFFER;
    return flags;
}

D3D11_USAGE ToNativeUsage(const MemoryAccess memoryAccess)
{
    switch (memoryAccess)
    {
    case MemoryAccess::GpuOnly: return D3D11_USAGE_DEFAULT;
    case MemoryAccess::CpuToGpu: return D3D11_USAGE_DYNAMIC;
    case MemoryAccess::GpuToCpu: return D3D11_USAGE_STAGING;
    }
    ThrowUnsupported("usage");
}

UINT ToNativeCpuAccessFlags(const MemoryAccess memoryAccess)
{
    switch (memoryAccess)
    {
    case MemoryAccess::GpuOnly: return 0;
    case MemoryAccess::CpuToGpu: return D3D11_CPU_ACCESS_WRITE;
    case MemoryAccess::GpuToCpu: return D3D11_CPU_ACCESS_READ;
    }
    ThrowUnsupported("CPU access");
}

D3D11_FILTER ToNativeFilter(const Filter filter)
{
    switch (filter)
    {
    case Filter::Nearest: return D3D11_FILTER_MIN_MAG_MIP_POINT;
    case Filter::Linear: return D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    case Filter::Anisotropic: return D3D11_FILTER_ANISOTROPIC;
    case Filter::ComparisonLinear: return D3D11_FILTER_COMPARISON_MIN_MAG_LINEAR_MIP_POINT;
    }
    ThrowUnsupported("filter");
}

D3D11_TEXTURE_ADDRESS_MODE ToNativeAddressMode(const AddressMode addressMode)
{
    switch (addressMode)
    {
    case AddressMode::Repeat: return D3D11_TEXTURE_ADDRESS_WRAP;
    case AddressMode::MirroredRepeat: return D3D11_TEXTURE_ADDRESS_MIRROR;
    case AddressMode::ClampToEdge: return D3D11_TEXTURE_ADDRESS_CLAMP;
    case AddressMode::ClampToBorder: return D3D11_TEXTURE_ADDRESS_BORDER;
    }
    ThrowUnsupported("address mode");
}

D3D11_COMPARISON_FUNC ToNativeCompareOperation(const CompareOperation operation)
{
    switch (operation)
    {
    case CompareOperation::Never: return D3D11_COMPARISON_NEVER;
    case CompareOperation::Less: return D3D11_COMPARISON_LESS;
    case CompareOperation::Equal: return D3D11_COMPARISON_EQUAL;
    case CompareOperation::LessEqual: return D3D11_COMPARISON_LESS_EQUAL;
    case CompareOperation::Greater: return D3D11_COMPARISON_GREATER;
    case CompareOperation::NotEqual: return D3D11_COMPARISON_NOT_EQUAL;
    case CompareOperation::GreaterEqual: return D3D11_COMPARISON_GREATER_EQUAL;
    case CompareOperation::Always: return D3D11_COMPARISON_ALWAYS;
    }
    ThrowUnsupported("comparison operation");
}

D3D11_BLEND ToNativeBlendFactor(const BlendFactor factor)
{
    switch (factor)
    {
    case BlendFactor::Zero: return D3D11_BLEND_ZERO;
    case BlendFactor::One: return D3D11_BLEND_ONE;
    case BlendFactor::SourceColor: return D3D11_BLEND_SRC_COLOR;
    case BlendFactor::InverseSourceColor: return D3D11_BLEND_INV_SRC_COLOR;
    case BlendFactor::SourceAlpha: return D3D11_BLEND_SRC_ALPHA;
    case BlendFactor::InverseSourceAlpha: return D3D11_BLEND_INV_SRC_ALPHA;
    case BlendFactor::DestinationColor: return D3D11_BLEND_DEST_COLOR;
    case BlendFactor::InverseDestinationColor: return D3D11_BLEND_INV_DEST_COLOR;
    case BlendFactor::DestinationAlpha: return D3D11_BLEND_DEST_ALPHA;
    case BlendFactor::InverseDestinationAlpha: return D3D11_BLEND_INV_DEST_ALPHA;
    }
    ThrowUnsupported("blend factor");
}

D3D11_BLEND_OP ToNativeBlendOperation(const BlendOperation operation)
{
    switch (operation)
    {
    case BlendOperation::Add: return D3D11_BLEND_OP_ADD;
    case BlendOperation::Subtract: return D3D11_BLEND_OP_SUBTRACT;
    case BlendOperation::ReverseSubtract: return D3D11_BLEND_OP_REV_SUBTRACT;
    case BlendOperation::Minimum: return D3D11_BLEND_OP_MIN;
    case BlendOperation::Maximum: return D3D11_BLEND_OP_MAX;
    }
    ThrowUnsupported("blend operation");
}

D3D11_TEXTURE2D_DESC ToNativeTextureDescription(const TextureDescription& description)
{
    std::string validationError;
    if (!ValidateTextureDescription(description, &validationError))
    {
        throw std::invalid_argument(validationError);
    }

    D3D11_TEXTURE2D_DESC native{};
    native.Width = description.width;
    native.Height = description.height;
    native.MipLevels = description.mipLevels;
    native.ArraySize = description.arrayLayers;
    native.Format = ToNativeFormat(description.format);
    native.SampleDesc.Count = description.sampleCount;
    native.Usage = ToNativeUsage(description.memoryAccess);
    native.BindFlags = description.memoryAccess == MemoryAccess::GpuToCpu ? 0 : ToNativeBindFlags(description.usage);
    native.CPUAccessFlags = ToNativeCpuAccessFlags(description.memoryAccess);
    native.MiscFlags = description.dimension == TextureDimension::TextureCube ? D3D11_RESOURCE_MISC_TEXTURECUBE : 0;
    return native;
}

D3D11_SAMPLER_DESC ToNativeSamplerDescription(const SamplerDescription& description)
{
    D3D11_SAMPLER_DESC native{};
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

D3D11_RASTERIZER_DESC ToNativeRasterizerDescription(const RasterizerDescription& description)
{
    D3D11_RASTERIZER_DESC native{};
    native.FillMode = description.fillMode == FillMode::Wireframe ? D3D11_FILL_WIREFRAME : D3D11_FILL_SOLID;
    switch (description.cullMode)
    {
    case CullMode::None: native.CullMode = D3D11_CULL_NONE; break;
    case CullMode::Front: native.CullMode = D3D11_CULL_FRONT; break;
    case CullMode::Back: native.CullMode = D3D11_CULL_BACK; break;
    }
    native.FrontCounterClockwise = description.frontFace == FrontFace::CounterClockwise;
    native.DepthBias = description.depthBias;
    native.DepthBiasClamp = description.depthBiasClamp;
    native.SlopeScaledDepthBias = description.slopeScaledDepthBias;
    native.DepthClipEnable = description.depthClipEnabled;
    native.MultisampleEnable = description.multisampleEnabled;
    return native;
}

D3D11_DEPTH_STENCIL_DESC ToNativeDepthStencilDescription(const DepthStencilDescription& description)
{
    D3D11_DEPTH_STENCIL_DESC native{};
    native.DepthEnable = description.depthTestEnabled;
    native.DepthWriteMask = description.depthWriteEnabled ? D3D11_DEPTH_WRITE_MASK_ALL : D3D11_DEPTH_WRITE_MASK_ZERO;
    native.DepthFunc = ToNativeCompareOperation(description.depthComparison);
    native.StencilEnable = description.stencilEnabled;
    native.StencilReadMask = description.stencilReadMask;
    native.StencilWriteMask = description.stencilWriteMask;
    native.FrontFace.StencilFailOp = D3D11_STENCIL_OP_KEEP;
    native.FrontFace.StencilDepthFailOp = D3D11_STENCIL_OP_KEEP;
    native.FrontFace.StencilPassOp = D3D11_STENCIL_OP_KEEP;
    native.FrontFace.StencilFunc = D3D11_COMPARISON_ALWAYS;
    native.BackFace = native.FrontFace;
    return native;
}

D3D11_BLEND_DESC ToNativeBlendDescription(const BlendAttachmentDescription& description)
{
    D3D11_BLEND_DESC native{};
    D3D11_RENDER_TARGET_BLEND_DESC& target = native.RenderTarget[0];
    target.BlendEnable = description.blendEnabled;
    target.SrcBlend = ToNativeBlendFactor(description.sourceColor);
    target.DestBlend = ToNativeBlendFactor(description.destinationColor);
    target.BlendOp = ToNativeBlendOperation(description.colorOperation);
    target.SrcBlendAlpha = ToNativeBlendFactor(description.sourceAlpha);
    target.DestBlendAlpha = ToNativeBlendFactor(description.destinationAlpha);
    target.BlendOpAlpha = ToNativeBlendOperation(description.alphaOperation);
    target.RenderTargetWriteMask = description.colorWriteMask;
    return native;
}
} // namespace Prism::RHI::D3D11
