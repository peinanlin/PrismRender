#pragma once

namespace Prism::Renderer
{
struct FluidSettings;
struct FluidStatistics;
}

namespace Prism::UI
{
class FluidLabPanel
{
public:
    void Draw(
        Renderer::FluidSettings& settings,
        const Renderer::FluidStatistics& statistics) const;
};
} // namespace Prism::UI
