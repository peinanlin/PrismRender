#pragma once

#include "RHI/Vulkan/VulkanLoader.h"
#include "UI/IImGuiRenderer.h"

namespace Prism::RHI::Vulkan
{
class VulkanRenderBackend;

class VulkanImGuiRenderer final : public UI::IImGuiRenderer
{
public:
    void Initialize(IRenderBackend& backend) override;
    void Shutdown() override;
    void BeginFrame() override;
    [[nodiscard]] std::uint64_t RegisterTexture(
        const ITextureView& textureView) override;
    void UnregisterTexture(std::uint64_t textureId) override;
    void RenderDrawData(const UI::UiDrawPacket& drawPacket) override;

private:
    VulkanRenderBackend* m_backend = nullptr;
    VkSampler m_textureSampler = VK_NULL_HANDLE;
    bool m_initialized = false;
};
} // namespace Prism::RHI::Vulkan
