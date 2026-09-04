#include "RHI/FramePacing.h"
#include "RHI/Vulkan/VulkanDeviceSelection.h"
#if defined(PRISM_RENDER_HAS_D3D12)
#include "RHI/D3D12/D3D12Presentation.h"
#include <dxgi1_6.h>
#endif

#include <array>
#include <iostream>
#include <stdexcept>

namespace
{
void Expect(const bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
}
}

int main()
{
    using namespace Prism::RHI;
    try
    {
        const std::array fifoOnly{VK_PRESENT_MODE_FIFO_KHR};
        const std::array mailbox{
            VK_PRESENT_MODE_FIFO_KHR,
            VK_PRESENT_MODE_MAILBOX_KHR};
        const std::array allModes{
            VK_PRESENT_MODE_FIFO_KHR,
            VK_PRESENT_MODE_MAILBOX_KHR,
            VK_PRESENT_MODE_IMMEDIATE_KHR};
        Expect(Vulkan::ChooseVulkanPresentMode(
                    allModes, PresentationIntent::Synchronized)
                == VK_PRESENT_MODE_FIFO_KHR,
            "Synchronized Vulkan pacing did not select FIFO.");
        Expect(Vulkan::ChooseVulkanPresentMode(
                    mailbox, PresentationIntent::LowLatencySynchronized)
                == VK_PRESENT_MODE_MAILBOX_KHR,
            "Low-latency Vulkan pacing did not select MAILBOX.");
        Expect(Vulkan::ChooseVulkanPresentMode(
                    allModes, PresentationIntent::Immediate)
                == VK_PRESENT_MODE_IMMEDIATE_KHR,
            "Benchmark Vulkan pacing did not select IMMEDIATE first.");
        Expect(Vulkan::ChooseVulkanPresentMode(
                    mailbox, PresentationIntent::Immediate)
                == VK_PRESENT_MODE_MAILBOX_KHR
                && Vulkan::ChooseVulkanPresentMode(
                    fifoOnly, PresentationIntent::Immediate)
                == VK_PRESENT_MODE_FIFO_KHR,
            "Vulkan present fallback order changed.");
#if defined(PRISM_RENDER_HAS_D3D12)
        const auto smooth = D3D12::BuildD3D12PresentationPlan(
            MakeFramePacingConfiguration(
                FramePacingProfile::InteractiveSmooth), true);
        Expect(smooth.syncInterval == 1
                && smooth.presentFlags == 0
                && smooth.maximumFrameLatency == 2
                && (smooth.swapChainFlags
                    & DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT)
                    != 0,
            "D3D12 interactive plan changed.");
        const auto lowLatency = D3D12::BuildD3D12PresentationPlan(
            MakeFramePacingConfiguration(
                FramePacingProfile::LowLatency), true);
        Expect(lowLatency.syncInterval == 1
                && lowLatency.maximumFrameLatency == 1,
            "D3D12 low-latency plan changed.");
        const auto benchmark = D3D12::BuildD3D12PresentationPlan(
            MakeFramePacingConfiguration(
                FramePacingProfile::Benchmark), true);
        Expect(benchmark.syncInterval == 0
                && benchmark.presentFlags == DXGI_PRESENT_ALLOW_TEARING
                && benchmark.tearingEnabled
                && (benchmark.swapChainFlags
                    & DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING) != 0,
            "D3D12 benchmark tearing plan changed.");
        const auto noTearing = D3D12::BuildD3D12PresentationPlan(
            MakeFramePacingConfiguration(
                FramePacingProfile::Benchmark), false);
        Expect(noTearing.syncInterval == 0
                && noTearing.presentFlags == 0
                && !noTearing.tearingEnabled
                && !noTearing.fallbackReason.empty(),
            "D3D12 unsupported-tearing fallback changed.");
#endif
        std::cout << "Presentation pacing tests passed.\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
