#pragma once

#include <cstdint>
#include <memory>
#include <mutex>

namespace Prism::Platform
{
class Window;
}

namespace Prism::RHI
{
class IRenderBackend;
class ITextureView;
}

namespace Prism::UI
{
class IImGuiRenderer;
class UiDrawPacket;

// Owns the API-independent ImGui context and GLFW platform integration. Only
// GPU command recording and texture registration are delegated per API.
class ImGuiSystem
{
public:
    explicit ImGuiSystem(
        std::unique_ptr<IImGuiRenderer> renderer);
    ~ImGuiSystem();

    ImGuiSystem(const ImGuiSystem&) = delete;
    ImGuiSystem& operator=(const ImGuiSystem&) = delete;

    void Initialize(
        Platform::Window& window,
        RHI::IRenderBackend& backend);
    void Shutdown();
    // GLFW callbacks feed the mutable ImGui platform backend. In threaded
    // rendering they share the same context lock as packet build/draw.
    void PollPlatformEvents(Platform::Window& window);
    void BeginFrame();
    // Main-lane exception recovery for a BeginFrame that did not reach
    // BuildDrawPacket. It releases the cross-lane UI build lock.
    void AbortFrameBuild() noexcept;
    void SetViewportTextures(
        std::shared_ptr<const RHI::ITextureView> sceneTextureView,
        std::shared_ptr<const RHI::ITextureView> gameTextureView);
    [[nodiscard]] std::uint64_t GetSceneTextureId() const;
    [[nodiscard]] std::uint64_t GetGameTextureId() const;
    [[nodiscard]] std::shared_ptr<const UiDrawPacket>
        BuildDrawPacket();
    void Render(const UiDrawPacket& drawPacket);

private:
    void ReplaceTexture(
        std::shared_ptr<const RHI::ITextureView> textureView,
        std::uint64_t& textureId,
        std::shared_ptr<const RHI::ITextureView>& retainedView);

    std::unique_ptr<IImGuiRenderer> m_renderer;
    std::uint64_t m_sceneTextureId = 0;
    std::uint64_t m_gameTextureId = 0;
    std::shared_ptr<const RHI::ITextureView> m_sceneTextureView;
    std::shared_ptr<const RHI::ITextureView> m_gameTextureView;
    // BeginFrame locks this on the main lane and BuildDrawPacket releases it.
    // Render-lane backend work and descriptor replacement use the same lock,
    // so no backend reaches mutable ImGui state while a packet is built.
    mutable std::mutex m_backendMutex;
    bool m_frameBuildActive = false;
    bool m_initialized = false;
};
} // namespace Prism::UI
