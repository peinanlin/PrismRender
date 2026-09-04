#pragma once

#include <cstdint>
#include <string_view>

namespace Prism::Renderer
{
struct RendererStatistics;
struct RenderSettings;
}

namespace Prism::UI
{
struct ViewportPerformanceStats
{
    float framesPerSecond = 0.0f;
    float frameTimeMs = 0.0f;
    float mainCpuMs = 0.0f;
    float renderCpuMs = 0.0f;
    float workerCpuMs = 0.0f;
    float gpuFrameMs = 0.0f;
    float submitPresentWaitMs = 0.0f;
    float rendererCpuMs = 0.0f;
    float rendererGpuMs = 0.0f;
    std::string_view profilingLevel = "basic";
    std::uint8_t activeViewMask = 0;
    bool mainCpuAvailable = false;
    bool renderCpuAvailable = false;
    bool workerCpuAvailable = false;
    bool gpuFrameAvailable = false;
    bool submitPresentWaitAvailable = false;
    std::uint32_t drawCalls = 0;
    std::uint32_t shadowDrawCalls = 0;
    std::uint32_t directionalShadowDrawCalls = 0;
    std::uint32_t localShadowDrawCalls = 0;
    std::uint32_t shadowLayersRendered = 0;
    std::uint32_t shadowLayersCached = 0;
    std::uint32_t geometryDrawCalls = 0;
    std::uint32_t postProcessDrawCalls = 0;
    std::uint32_t visibleObjects = 0;
    std::uint32_t renderedObjects = 0;
    std::uint32_t culledObjects = 0;
};

class GameViewportDebugOverlay
{
public:
    void DrawVisibilityToggle();

    // Draws over the current ImGui window and returns true while the HUD is
    // hovered, allowing the Scene viewport to suppress camera interaction.
    [[nodiscard]] bool Draw(
        const ViewportPerformanceStats& stats,
        Renderer::RenderSettings& settings,
        const Renderer::RendererStatistics& rendererStatistics,
        bool oceanLabActive,
        bool waveWorksActive,
        bool fluidLabActive,
        float viewportX,
        float viewportY,
        float viewportWidth,
        float viewportHeight);

private:
    bool m_visible = true;
    bool m_expanded = true;
};
} // namespace Prism::UI
