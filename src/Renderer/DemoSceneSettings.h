#pragma once

#include <cstdint>

namespace Prism::Scene
{
enum class DemoSceneId : std::uint32_t;
}

namespace Prism::Renderer
{
struct RenderSettings;

void ApplyDemoSceneSettings(
    Scene::DemoSceneId sceneId,
    RenderSettings& settings);
} // namespace Prism::Renderer
