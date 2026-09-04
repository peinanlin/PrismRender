#pragma once

namespace Prism::Renderer
{
struct OceanSettings;
struct OceanStatistics;
}

namespace Prism::UI
{
// Optical settings only; the shared OceanLabPanel owns spectral/local controls.
void DrawWaterOpticsPanel(Renderer::OceanSettings& settings,
    const Renderer::OceanStatistics* statistics);
}
