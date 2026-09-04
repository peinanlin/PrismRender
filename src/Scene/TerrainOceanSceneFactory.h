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

enum class OceanLabSceneKind : std::uint8_t
{
    LegacyFft,
    WaveWorksReference,
    HpWaterReference
};

struct TerrainOceanSceneSummary
{
    std::uint32_t renderObjectCount = 0;
    std::uint32_t patchCount = 0;
    std::uint32_t maxQuadtreeLevel = 0;
};

class TerrainOceanSceneFactory
{
public:
    static TerrainOceanSceneSummary PopulateTerrain(
        Asset::AssetRegistry& assetRegistry,
        RHI::IGraphicsDevice& device,
        RenderScene& scene);
    static TerrainOceanSceneSummary PopulateOcean(
        Asset::AssetRegistry& assetRegistry,
        RHI::IGraphicsDevice& device,
        RenderScene& scene,
        OceanLabSceneKind kind = OceanLabSceneKind::LegacyFft);
    static void ConfigureTerrainWorld(
        RenderScene& scene,
        float cameraAspectRatio);
    static void ConfigureOceanWorld(
        RenderScene& scene,
        float cameraAspectRatio,
        OceanLabSceneKind kind = OceanLabSceneKind::LegacyFft);
};
} // namespace Prism::Scene
