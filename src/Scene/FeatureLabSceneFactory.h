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

enum class FeatureLabSceneKind
{
    GpuDriven,
    PostProcess,
    RenderGraph,
    Material,
    AssetStreaming,
    Atmosphere,
    LargeWorld
};

struct FeatureLabSceneSummary
{
    std::uint32_t renderObjectCount = 0;
    std::uint32_t subjectCount = 0;
    std::uint32_t pointLightCount = 0;
};

class FeatureLabSceneFactory
{
public:
    static FeatureLabSceneSummary Populate(
        FeatureLabSceneKind kind,
        Asset::AssetRegistry& assetRegistry,
        RHI::IGraphicsDevice& device,
        RenderScene& scene);
    static void ConfigureWorld(
        FeatureLabSceneKind kind,
        RenderScene& scene,
        float cameraAspectRatio);
};
} // namespace Prism::Scene
