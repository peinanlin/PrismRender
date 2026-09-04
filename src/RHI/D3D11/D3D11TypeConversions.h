#pragma once

#include "RHI/GraphicsTypes.h"

#include <d3d11.h>
#include <dxgiformat.h>

namespace Prism::RHI::D3D11
{
DXGI_FORMAT ToNativeFormat(Format format);
UINT ToNativeBindFlags(TextureUsage usage);
UINT ToNativeBindFlags(ResourceState state);
D3D11_USAGE ToNativeUsage(MemoryAccess memoryAccess);
UINT ToNativeCpuAccessFlags(MemoryAccess memoryAccess);
D3D11_FILTER ToNativeFilter(Filter filter);
D3D11_TEXTURE_ADDRESS_MODE ToNativeAddressMode(AddressMode addressMode);
D3D11_COMPARISON_FUNC ToNativeCompareOperation(CompareOperation operation);
D3D11_BLEND ToNativeBlendFactor(BlendFactor factor);
D3D11_BLEND_OP ToNativeBlendOperation(BlendOperation operation);
D3D11_TEXTURE2D_DESC ToNativeTextureDescription(const TextureDescription& description);
D3D11_SAMPLER_DESC ToNativeSamplerDescription(const SamplerDescription& description);
D3D11_RASTERIZER_DESC ToNativeRasterizerDescription(const RasterizerDescription& description);
D3D11_DEPTH_STENCIL_DESC ToNativeDepthStencilDescription(const DepthStencilDescription& description);
D3D11_BLEND_DESC ToNativeBlendDescription(const BlendAttachmentDescription& description);
} // namespace Prism::RHI::D3D11
