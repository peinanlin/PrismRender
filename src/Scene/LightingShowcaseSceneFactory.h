#pragma once

#include <cstdint>

namespace Prism::Asset
{
class AssetRegistry;
}

namespace Prism::RHI
{
class IGraphicsDevice;
}

namespace Prism::Scene
{
class RenderScene;

struct LightingShowcaseSceneSummary
{
    std::uint32_t renderObjectCount = 0;
    std::uint32_t materialSubjectCount = 0;
    std::uint32_t pointLightCount = 0;
};

class LightingShowcaseSceneFactory
{
public:
    static LightingShowcaseSceneSummary Populate(
        Asset::AssetRegistry& assetRegistry,
        RHI::IGraphicsDevice& device,
        RenderScene& scene);

    static void ConfigureWorld(
        RenderScene& scene,
        float cameraAspectRatio);
};
} // namespace Prism::Scene
