#pragma once

#include "Renderer/RenderFeatureRegistry.h"

#include <functional>
#include <string_view>

namespace Prism::Renderer
{
class ClusteredLighting;
class FftOcean;
class GpuDrivenVisibility;
class InteractiveTerrain;
class LocalLightShadows;
class LocalWaveGpuResources;
class PlanarReflections;
class ScreenSpaceEffects;
class SkyAtmosphere;
class SpectralOceanSimulation;
class TemporalAntiAliasing;
class VarianceShadowMaps;
class FluidFeature;
class VirtualTextureCache;
class WaterOpticsFeature;
struct WaterOpticsPassCallbacks;

// Composition-only knowledge of concrete built-in Feature classes lives
// here. Lifecycle scheduling remains generic and only sees registrations.
class BuiltInRenderFeatures final
{
public:
    static constexpr std::string_view SkyAtmosphereId =
        "SkyAtmosphere";
    static constexpr std::string_view WaterOpticsId =
        "WaterOptics";
    static constexpr std::string_view FluidId = "Fluid";
    static constexpr std::string_view ClusteredLightingId =
        "ClusteredLighting";
    static constexpr std::string_view LocalLightShadowsId =
        "LocalLightShadows";
    static constexpr std::string_view VarianceShadowMapsId =
        "VarianceShadowMaps";
    static constexpr std::string_view PlanarReflectionsId =
        "PlanarReflections";
    static constexpr std::string_view ScreenSpaceEffectsId =
        "ScreenSpaceEffects";
    static constexpr std::string_view TemporalAntiAliasingId =
        "TemporalAntiAliasing";
    static constexpr std::string_view FftOceanId = "FftOcean";
    static constexpr std::string_view GpuDrivenVisibilityId =
        "GpuDrivenVisibility";
    static constexpr std::string_view SpectralOceanId =
        "SpectralOcean";
    static constexpr std::string_view LocalWaveId = "LocalWave";
    static constexpr std::string_view InteractiveTerrainId =
        "InteractiveTerrain";
    static constexpr std::string_view VirtualTextureCacheId =
        "VirtualTextureCache";

    static void RegisterSkyAtmosphere(
        RenderFeatureRegistry& registry,
        SkyAtmosphere& feature,
        std::function<void(const RenderFeatureFrameContext&)>
            prepareFrame);
    static void RegisterWaterOptics(
        RenderFeatureRegistry& registry,
        WaterOpticsFeature& feature,
        std::function<void(const RenderFeatureFrameContext&)>
            prepareFrame,
        std::function<WaterOpticsPassCallbacks(
            const RenderFeatureGraphContext&)> callbackFactory);
    static void RegisterFluid(
        RenderFeatureRegistry& registry,
        FluidFeature& feature);
    static void RegisterClusteredLighting(
        RenderFeatureRegistry& registry,
        ClusteredLighting& feature);
    static void RegisterLocalLightShadows(
        RenderFeatureRegistry& registry,
        LocalLightShadows& feature);
    static void RegisterVarianceShadowMaps(
        RenderFeatureRegistry& registry,
        VarianceShadowMaps& feature);
    static void RegisterPlanarReflections(
        RenderFeatureRegistry& registry,
        PlanarReflections& feature);
    static void RegisterScreenSpaceEffects(
        RenderFeatureRegistry& registry,
        ScreenSpaceEffects& feature,
        std::function<void(const RenderFeatureResizeContext&)>
            resize);
    static void RegisterTemporalAntiAliasing(
        RenderFeatureRegistry& registry,
        TemporalAntiAliasing& feature,
        std::function<void(const RenderFeatureResizeContext&)>
            resize);
    static void RegisterFftOcean(
        RenderFeatureRegistry& registry,
        FftOcean& feature,
        std::function<bool()> selected);
    static void RegisterGpuDrivenVisibility(
        RenderFeatureRegistry& registry,
        GpuDrivenVisibility& feature,
        std::function<void(const RenderFeatureFrameContext&)>
            prepareFrame,
        std::function<void(const RenderFeatureResizeContext&)>
            resize);
    static void RegisterSpectralOcean(
        RenderFeatureRegistry& registry,
        SpectralOceanSimulation& feature,
        std::function<bool()> selected);
    static void RegisterLocalWave(
        RenderFeatureRegistry& registry,
        LocalWaveGpuResources& feature,
        std::function<bool()> selected);
    static void RegisterInteractiveTerrain(
        RenderFeatureRegistry& registry,
        InteractiveTerrain& feature,
        std::function<void(const RenderFeatureFrameContext&)>
            prepareFrame,
        std::function<bool(const RenderFeatureGraphContext&)>
            active = {});
    static void RegisterVirtualTextureCache(
        RenderFeatureRegistry& registry,
        VirtualTextureCache& feature,
        std::function<void(const RenderFeatureFrameContext&)>
            prepareFrame);

};
} // namespace Prism::Renderer
