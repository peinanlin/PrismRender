#pragma once

namespace Prism::Renderer
{
struct RenderSettings;
struct RendererStatistics;
}

namespace Prism::UI
{
class OceanLabPanel
{
public:
    void Draw(Renderer::RenderSettings& settings,
        const Renderer::RendererStatistics* statistics = nullptr) const;
};
} // namespace Prism::UI
