#pragma once

#include "Core/Math/Double3.h"

#include <optional>

namespace Prism::Scene
{
class RenderScene;

struct RenderSceneBounds
{
    Core::Double3 center{};
    float radius = 0.0f;
};

std::optional<RenderSceneBounds> CalculateRenderObjectBounds(
    const RenderScene& scene);
bool FrameCameraToRenderObjects(RenderScene& scene);
} // namespace Prism::Scene
