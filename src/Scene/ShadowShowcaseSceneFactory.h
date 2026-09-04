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

struct ShadowShowcaseSceneSummary
{
    std::uint32_t renderObjectCount = 0;
    std::uint32_t directionalShadowSubjects = 0;
    std::uint32_t pointLightCount = 0;
    std::uint32_t spotLightCount = 0;
};

class ShadowShowcaseSceneFactory
{
public:
    static ShadowShowcaseSceneSummary Populate(
        Asset::AssetRegistry& assetRegistry,
        RHI::IGraphicsDevice& device,
        RenderScene& scene);

    static void ConfigureWorld(
        RenderScene& scene,
        float cameraAspectRatio);
};
} // namespace Prism::Scene
