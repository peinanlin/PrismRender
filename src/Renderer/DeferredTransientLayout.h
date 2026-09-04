#pragma once

#include "RHI/TransientResources.h"

#include <array>
#include <cstdint>
#include <vector>

namespace Prism::Renderer
{
inline constexpr std::uint32_t DeferredGBufferCount = 4;

const std::array<
    RHI::Format,
    DeferredGBufferCount>&
GetDeferredGBufferFormats();
std::vector<RHI::TransientTextureRequest>
BuildDeferredTransientTextureRequests(
    std::uint32_t width,
    std::uint32_t height);
std::uint32_t CalculateHiZMipCount(
    std::uint32_t width,
    std::uint32_t height);
} // namespace Prism::Renderer
