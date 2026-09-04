#pragma once

#include "Renderer/SharedRenderData.h"

#include <DirectXMath.h>

#include <array>

namespace Prism::Scene
{
class RenderSceneView;
}

namespace Prism::Renderer
{
struct RenderSettings;

struct CascadeShadowData
{
    std::array<DirectX::XMMATRIX, SharedShadowCascadeCount> viewProjections{};
    DirectX::XMFLOAT4 splitDistances{};
};

CascadeShadowData BuildCascadeShadowData(
    const Scene::RenderSceneView& scene,
    const RenderSettings& settings,
    std::uint32_t shadowMapResolution = SharedShadowMapResolution);
} // namespace Prism::Renderer
