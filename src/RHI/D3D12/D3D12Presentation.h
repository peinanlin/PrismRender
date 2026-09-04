#pragma once

#include "RHI/FramePacing.h"

#include <cstdint>
#include <string>

namespace Prism::RHI::D3D12
{
struct D3D12PresentationPlan
{
    std::uint32_t swapChainFlags = 0;
    std::uint32_t syncInterval = 1;
    std::uint32_t presentFlags = 0;
    std::uint32_t maximumFrameLatency = 2;
    PresentationIntent effectivePresentation =
        PresentationIntent::Synchronized;
    bool tearingEnabled = false;
    std::string fallbackReason;
};

[[nodiscard]] D3D12PresentationPlan BuildD3D12PresentationPlan(
    const FramePacingConfiguration& configuration,
    bool tearingSupported);
} // namespace Prism::RHI::D3D12
