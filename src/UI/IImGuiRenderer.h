#pragma once

#include <cstdint>

namespace Prism::RHI
{
class IRenderBackend;
class ITextureView;
}

namespace Prism::UI
{
class UiDrawPacket;
// API-specific renderer for the API-independent ImGui frame assembled by
// EditorLayer, DebugPanel, and the other UI modules.
class IImGuiRenderer
{
public:
    virtual ~IImGuiRenderer() = default;

    virtual void Initialize(RHI::IRenderBackend& backend) = 0;
    virtual void Shutdown() = 0;
    virtual void BeginFrame() = 0;
    [[nodiscard]] virtual std::uint64_t RegisterTexture(
        const RHI::ITextureView& textureView) = 0;
    virtual void UnregisterTexture(std::uint64_t textureId) = 0;
    virtual void RenderDrawData(const UiDrawPacket& drawPacket) = 0;
};
} // namespace Prism::UI
