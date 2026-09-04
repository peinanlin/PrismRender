#pragma once

#include "UI/IImGuiRenderer.h"

namespace Prism::RHI::D3D12
{
class D3D12RenderBackend;
}

namespace Prism::RHI::D3D12
{
class D3D12ImGuiRenderer final : public UI::IImGuiRenderer
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
    D3D12RenderBackend* m_backend = nullptr;
    bool m_initialized = false;
};
} // namespace Prism::RHI::D3D12
