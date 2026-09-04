#include "RHI/RenderBackendFactory.h"

#include "RHI/IRenderBackend.h"
#if defined(PRISM_RENDER_HAS_D3D12)
#include "RHI/D3D12/D3D12RenderBackend.h"
#endif
#include "RHI/Vulkan/VulkanRenderBackend.h"

#include <stdexcept>

namespace Prism::RHI
{
std::unique_ptr<IRenderBackend> CreateRenderBackend(
    const GraphicsApi graphicsApi,
    const FramePacingConfiguration& framePacing)
{
    switch (graphicsApi)
    {
    case GraphicsApi::Direct3D12:
#if defined(PRISM_RENDER_HAS_D3D12)
        return std::make_unique<
            D3D12::D3D12RenderBackend>(framePacing);
#else
        throw std::runtime_error(
            "Direct3D 12 is not available in this build.");
#endif
    case GraphicsApi::Vulkan:
        return std::make_unique<
            Vulkan::VulkanRenderBackend>(framePacing);
    case GraphicsApi::Direct3D11:
        throw std::runtime_error(
            "The D3D11 translator has no runnable backend.");
    }
    throw std::runtime_error("Unsupported graphics API.");
}
} // namespace Prism::RHI
