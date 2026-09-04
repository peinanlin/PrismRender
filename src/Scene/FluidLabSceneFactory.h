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

enum class FluidLabSceneKind : std::uint32_t
{
    PbfParticles,
    ScreenSpace,
    Caustics,
    ToonFoam
};

struct FluidLabSceneSummary
{
    std::uint32_t renderObjectCount = 0;
    std::uint32_t basinObjectCount = 0;
};

class FluidLabSceneFactory
{
public:
    static FluidLabSceneSummary Populate(
        Asset::AssetRegistry& assetRegistry,
        RHI::IGraphicsDevice& device,
        RenderScene& scene,
        FluidLabSceneKind kind =
            FluidLabSceneKind::ScreenSpace);

    static void ConfigureWorld(
        RenderScene& scene,
        float cameraAspectRatio,
        FluidLabSceneKind kind =
            FluidLabSceneKind::ScreenSpace);
};
} // namespace Prism::Scene
