#pragma once

#include "Renderer/RenderGraph.h"

#include <functional>

namespace Prism::Renderer
{
// Cross-feature inputs consumed by the Fluid subgraph. Simulation buffers,
// reconstruction scratch and caustic resources remain Fluid-owned.
struct FluidGraphInputs
{
    TextureHandle sceneColor;
    TextureHandle sceneDepth;
    TextureHandle environment;
};

struct FluidGraphResult
{
    TextureHandle sceneColor;
};

using FluidGraphCallback = std::function<FluidGraphResult(
    RenderGraph&,
    const FluidGraphInputs&)>;

struct FluidFeatureSlot
{
};

struct FluidGraphContribution
{
    FluidGraphCallback build;
};
} // namespace Prism::Renderer
