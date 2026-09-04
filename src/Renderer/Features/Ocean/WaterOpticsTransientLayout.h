#pragma once

#include "RHI/TransientResources.h"

#include <cstdint>
#include <vector>

namespace Prism::Renderer
{
struct WaterOpticsTransientLayoutConfig
{
    std::uint32_t width = 1u;
    std::uint32_t height = 1u;
    float refractionResolutionScale = 1.0f;
    float volumetricResolutionScale = 0.5f;
    std::uint32_t causticResolution = 512u;
};

[[nodiscard]] std::vector<RHI::TransientTextureRequest>
BuildWaterOpticsTransientTextureRequests(
    const WaterOpticsTransientLayoutConfig& config);

[[nodiscard]] bool AreWaterOpticsTexturesAliasCompatible(
    const RHI::TextureDescription& left,
    const RHI::TextureDescription& right) noexcept;
} // namespace Prism::Renderer
