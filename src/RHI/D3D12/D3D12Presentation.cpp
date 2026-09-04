#include "RHI/D3D12/D3D12Presentation.h"

#include <dxgi1_6.h>

namespace Prism::RHI::D3D12
{
D3D12PresentationPlan BuildD3D12PresentationPlan(
    const FramePacingConfiguration& configuration,
    const bool tearingSupported)
{
    ValidateFramePacingConfiguration(configuration);
    D3D12PresentationPlan result{};
    result.swapChainFlags =
        DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;
    if (tearingSupported)
    {
        result.swapChainFlags |= DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING;
    }
    result.maximumFrameLatency =
        configuration.presentation
                == PresentationIntent::LowLatencySynchronized
            ? 1u
            : configuration.maxQueuedFrames;
    result.effectivePresentation = configuration.presentation;
    if (configuration.presentation == PresentationIntent::Immediate)
    {
        result.syncInterval = 0;
        if (tearingSupported)
        {
            result.presentFlags = DXGI_PRESENT_ALLOW_TEARING;
            result.tearingEnabled = true;
        }
        else
        {
            result.fallbackReason =
                "DXGI windowed tearing is unavailable; using Present(0, 0).";
        }
    }
    return result;
}
} // namespace Prism::RHI::D3D12
