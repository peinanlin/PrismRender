#pragma once

#include "RHI/GraphicsResources.h"
#include "RHI/ShaderTypes.h"

#include <d3d12.h>
#include <dxgiformat.h>

namespace Prism::RHI::D3D12
{
DXGI_FORMAT ToNativeFormat(Format format);
D3D12_RESOURCE_STATES ToNativeResourceStates(ResourceState state);
D3D12_RESOURCE_FLAGS ToNativeTextureFlags(TextureUsage usage);
D3D12_HEAP_TYPE ToNativeHeapType(MemoryAccess memoryAccess);
D3D12_FILTER ToNativeFilter(Filter filter);
D3D12_TEXTURE_ADDRESS_MODE ToNativeAddressMode(AddressMode addressMode);
D3D12_COMPARISON_FUNC ToNativeCompareOperation(CompareOperation operation);
D3D12_BLEND ToNativeBlendFactor(BlendFactor factor);
D3D12_BLEND_OP ToNativeBlendOperation(BlendOperation operation);
D3D12_RESOURCE_DESC ToNativeTextureDescription(const TextureDescription& description);
D3D12_SHADER_RESOURCE_VIEW_DESC ToNativeSampledTextureViewDescription(
    const TextureDescription& texture, const TextureViewDescription& view);
D3D12_SAMPLER_DESC ToNativeSamplerDescription(const SamplerDescription& description);
D3D12_RASTERIZER_DESC ToNativeRasterizerDescription(const RasterizerDescription& description);
D3D12_DEPTH_STENCIL_DESC ToNativeDepthStencilDescription(const DepthStencilDescription& description);
D3D12_BLEND_DESC ToNativeBlendDescription(const BlendAttachmentDescription& description);
D3D12_SHADER_BYTECODE ToNativeShaderBytecode(const ShaderBinary& binary);
} // namespace Prism::RHI::D3D12
