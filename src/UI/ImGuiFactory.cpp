#include "UI/ImGuiFactory.h"

#include "UI/ImGuiSystem.h"

#if defined(PRISM_RENDER_HAS_D3D12)
#include "UI/Backends/D3D12/D3D12ImGuiRenderer.h"
#endif
#include "UI/Backends/Vulkan/VulkanImGuiRenderer.h"

namespace Prism::UI
{
std::unique_ptr<ImGuiSystem>
CreateImGuiSystem(const RHI::GraphicsApi graphicsApi)
{
#if defined(PRISM_RENDER_HAS_D3D12)
    if (graphicsApi == RHI::GraphicsApi::Direct3D12)
    {
        return std::make_unique<ImGuiSystem>(
            std::make_unique<
                RHI::D3D12::D3D12ImGuiRenderer>());
    }
#endif
    if (graphicsApi == RHI::GraphicsApi::Vulkan)
    {
        return std::make_unique<ImGuiSystem>(
            std::make_unique<
                RHI::Vulkan::VulkanImGuiRenderer>());
    }
    return nullptr;
}
} // namespace Prism::UI
