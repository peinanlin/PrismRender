#include "Renderer/DemoSceneSettings.h"

#include "Core/Environment.h"
#include "Renderer/RenderSettings.h"
#include "Scene/DemoSceneCatalog.h"

#include <string>

namespace
{
void ApplyOceanAutomationOverrides(Prism::Renderer::OceanSettings& ocean)
{
    using namespace Prism::Renderer;
    const std::string quality =
        Prism::Core::ReadEnvironmentVariableValue(
            "PRISM_RENDER_OCEAN_QUALITY");
    if (quality == "normal")
        ocean.quality = OceanSimulationQuality::Normal;
    else if (quality == "high")
        ocean.quality = OceanSimulationQuality::High;
    else if (quality == "extreme")
        ocean.quality = OceanSimulationQuality::Extreme;

    const std::string preset =
        Prism::Core::ReadEnvironmentVariableValue(
            "PRISM_RENDER_OCEAN_PRESET");
    if (preset == "calm")
    {
        ocean.baseWind.speed = 0.0f;
        ocean.swell.amplitudeMultiplier = 0.0f;
        ocean.local.enabled = false;
    }
    else if (preset == "no-swell")
    {
        ocean.swell.amplitudeMultiplier = 0.0f;
    }
    else if (preset == "spectral-only")
    {
        ocean.local.enabled = false;
    }
    else if (preset == "strong-wind")
    {
        ocean.baseWind.speed = 7.0f;
        ocean.baseWind.amplitudeMultiplier = 1.35f;
        ocean.swell.amplitudeMultiplier = 0.55f;
        ocean.local.enabled = false;
    }
    else if (preset == "rough-water")
    {
        ocean.baseWind.speed = 8.0f;
        ocean.baseWind.amplitudeMultiplier = 1.6f;
        ocean.swell.amplitudeMultiplier = 0.7f;
        ocean.lateralMultiplier = 1.4f;
        ocean.local.enabled = false;
    }
    else if (preset == "whitecap")
    {
        ocean.baseWind.speed = 8.0f;
        ocean.baseWind.amplitudeMultiplier = 1.5f;
        ocean.swell.amplitudeMultiplier = 0.5f;
        // High-wind whitecaps need horizontal crest compression as well as
        // vertical energy. Keep this explicit in the validation preset rather
        // than lowering the calm/reference thresholds globally.
        ocean.lateralMultiplier = 2.0f;
        ocean.foam.whitecapsThreshold = 0.42f;
        ocean.foam.generationThreshold = 0.18f;
        ocean.foam.generationAmount = 0.28f;
        ocean.local.enabled = false;
    }
    else if (preset == "whitecap-energy")
    {
        ocean.baseWind.speed = 8.0f;
        ocean.baseWind.amplitudeMultiplier = 1.5f;
        ocean.swell.amplitudeMultiplier = 0.5f;
        ocean.lateralMultiplier = 2.0f;
        ocean.foam.whitecapsThreshold = 0.42f;
        ocean.foam.generationThreshold = 0.18f;
        ocean.foam.generationAmount = 0.28f;
        ocean.local.enabled = false;
        ocean.debug.showFoamEnergy = true;
    }
    else if (preset == "low-sun")
    {
        ocean.shading.sunAngleDegrees = 5.0f;
    }
    else if (preset == "wake")
    {
        ocean.local.enabled = true;
        ocean.local.demoEmittersEnabled = true;
        ocean.local.rainEmitterEnabled = false;
        ocean.local.wakeEmitterEnabled = true;
    }
    else if (preset == "local-only")
    {
        // Validation view for the moving-hull wave equation.  Keep the
        // reference local-wave controls and remove only the spectral field so
        // local displacement, folding and foam can be inspected in isolation.
        ocean.baseWind.amplitudeMultiplier = 0.0f;
        ocean.swell.amplitudeMultiplier = 0.0f;
        ocean.local.enabled = true;
        ocean.local.demoEmittersEnabled = true;
        ocean.local.rainEmitterEnabled = false;
        ocean.local.wakeEmitterEnabled = true;
    }
    else if (preset == "rain")
    {
        ocean.local.enabled = true;
        ocean.local.demoEmittersEnabled = true;
        ocean.local.rainEmitterEnabled = true;
        ocean.local.wakeEmitterEnabled = false;
    }
    else if (preset == "cascade")
    {
        ocean.debug.showCascades = true;
    }
    else if (preset == "foam")
    {
        ocean.debug.showFoamEnergy = true;
    }
    else if (preset == "normal")
    {
        ocean.debug.showNormals = true;
    }
    else if (preset == "moments")
    {
        ocean.debug.showSlopeMoments = true;
    }
    else if (preset == "geometry-lod")
    {
        ocean.debug.showGeometryLod = true;
    }
    else if (preset == "wireframe")
    {
        ocean.debug.wireframe = true;
    }

    const std::string waterDebug =
        Prism::Core::ReadEnvironmentVariableValue(
            "PRISM_RENDER_WATER_DEBUG");
    if (waterDebug == "mask")
        ocean.optics.debugView = WaterOpticsDebugView::WaterMask;
    else if (waterDebug == "depth")
        ocean.optics.debugView = WaterOpticsDebugView::WaterDepth;
    else if (waterDebug == "normal")
        ocean.optics.debugView = WaterOpticsDebugView::NormalRoughness;
    else if (waterDebug == "absorption")
        ocean.optics.debugView = WaterOpticsDebugView::Absorption;
    else if (waterDebug == "scattering")
        ocean.optics.debugView = WaterOpticsDebugView::Scattering;
    else if (waterDebug == "thickness")
        ocean.optics.debugView = WaterOpticsDebugView::Thickness;
    else if (waterDebug == "refraction-hit")
        ocean.optics.debugView = WaterOpticsDebugView::RefractionHit;
    else if (waterDebug == "foam")
        ocean.optics.debugView = WaterOpticsDebugView::Foam;
    else if (waterDebug == "distance-tier")
        ocean.optics.debugView = WaterOpticsDebugView::DistanceTier;
    else if (waterDebug == "caustic-energy")
        ocean.optics.debugView = WaterOpticsDebugView::CausticEnergy;
    else if (waterDebug == "caustic-cascades")
        ocean.optics.debugView = WaterOpticsDebugView::CausticCascades;
    else if (waterDebug == "volumetric")
        ocean.optics.debugView = WaterOpticsDebugView::VolumetricAccumulation;
    else if (waterDebug == "volumetric-history")
        ocean.optics.debugView = WaterOpticsDebugView::VolumetricHistoryRejection;
    if (Prism::Core::ReadEnvironmentVariableValue(
            "PRISM_RENDER_WATER_VOLUMETRICS") == "off")
        ocean.optics.volumetrics.enabled = false;

    const std::string causticMode =
        Prism::Core::ReadEnvironmentVariableValue(
            "PRISM_RENDER_WATER_CAUSTICS");
    if (causticMode == "off")
    {
        ocean.optics.caustics.enabled = false;
        ocean.optics.caustics.mode = WaterCausticsMode::Disabled;
    }
    else if (causticMode == "single")
        ocean.optics.caustics.mode = WaterCausticsMode::SingleChannel;
    else if (causticMode == "rgb")
        ocean.optics.caustics.mode = WaterCausticsMode::Chromatic;

    const std::string waterQuality =
        Prism::Core::ReadEnvironmentVariableValue(
            "PRISM_RENDER_WATER_QUALITY");
    if (waterQuality == "normal")
        ocean.optics.quality = WaterOpticsQuality::Normal;
    else if (waterQuality == "high")
        ocean.optics.quality = WaterOpticsQuality::High;
    else if (waterQuality == "extreme")
        ocean.optics.quality = WaterOpticsQuality::Extreme;

    const std::string rayMarch =
        Prism::Core::ReadEnvironmentVariableValue(
            "PRISM_RENDER_WATER_RAY_MARCH");
    if (rayMarch == "1" || rayMarch == "true")
        ocean.optics.refraction.highPrecision = true;
}
} // namespace

namespace Prism::Renderer
{
void ApplyDemoSceneSettings(
    const Scene::DemoSceneId sceneId,
    RenderSettings& settings)
{
    settings = RenderSettings{};
    switch (sceneId)
    {
    case Scene::DemoSceneId::EditorPreview:
        return;

    case Scene::DemoSceneId::Showcase:
        settings.editorGridEnabled = false;
        // The showcase demonstrates real material/mesh instancing. The
        // dedicated GPU Driven Lab keeps the per-object indirect path.
        settings.gpuDrivenEnabled = false;
        settings.gpuInstancingEnabled = true;
        settings.ambientIntensity = 0.07f;
        settings.iblIntensity = 0.45f;
        settings.skyZenithColor = {0.015f, 0.045f, 0.12f};
        settings.skyHorizonColor = {0.12f, 0.22f, 0.38f};
        settings.groundColor = {0.012f, 0.016f, 0.025f};
        settings.atmosphereDensity = 0.82f;
        settings.exposure = 0.92f;
        settings.bloomThreshold = 0.85f;
        settings.bloomIntensity = 0.62f;
        settings.shadowDistance = 115.0f;
        return;

    case Scene::DemoSceneId::ReflectionLab:
        settings.editorGridEnabled = false;
        settings.planarReflectionsEnabled = true;
        settings.planarReflectionPlaneHeight = 0.0f;
        settings.planarReflectionIntensity = 0.86f;
        settings.screenSpaceReflectionsEnabled = true;
        settings.ambientIntensity = 0.055f;
        settings.iblIntensity = 0.42f;
        settings.skyZenithColor = {0.012f, 0.035f, 0.10f};
        settings.skyHorizonColor = {0.10f, 0.20f, 0.36f};
        settings.groundColor = {0.008f, 0.012f, 0.022f};
        settings.exposure = 0.94f;
        settings.bloomThreshold = 0.88f;
        settings.bloomIntensity = 0.52f;
        return;

    case Scene::DemoSceneId::ShadowLab:
        settings.editorGridEnabled = false;
        settings.screenSpaceReflectionsEnabled = false;
        // Keep this lab focused on the shadow filter. GTAO is screen-space and
        // can otherwise look like a shadow defect that changes with the camera.
        settings.gtaoEnabled = false;
        settings.ambientIntensity = 0.055f;
        settings.iblIntensity = 0.30f;
        settings.iblDiffuseStrength = 0.72f;
        settings.iblSpecularStrength = 0.72f;
        settings.skyZenithColor = {0.018f, 0.035f, 0.075f};
        settings.skyHorizonColor = {0.10f, 0.16f, 0.26f};
        settings.groundColor = {0.012f, 0.014f, 0.020f};
        settings.exposure = 1.0f;
        settings.bloomThreshold = 1.25f;
        settings.bloomIntensity = 0.32f;
        settings.shadowBias = 0.0025f;
        settings.shadowDistance = 135.0f;
        settings.cascadeSplitLambda = 0.68f;
        settings.shadowFilterMode = ShadowFilterMode::Evsm;
        return;

    case Scene::DemoSceneId::LightingLab:
        settings.editorGridEnabled = false;
        settings.deferredRenderingEnabled = false;
        settings.forwardPlusEnabled = true;
        settings.localLightShadowsEnabled = false;
        settings.screenSpaceReflectionsEnabled = false;
        settings.ambientIntensity = 0.015f;
        settings.iblIntensity = 0.12f;
        settings.iblDiffuseStrength = 0.55f;
        settings.iblSpecularStrength = 0.65f;
        settings.skyZenithColor = {0.012f, 0.022f, 0.055f};
        settings.skyHorizonColor = {0.055f, 0.085f, 0.16f};
        settings.groundColor = {0.008f, 0.010f, 0.016f};
        settings.exposure = 0.88f;
        settings.bloomThreshold = 0.72f;
        settings.bloomIntensity = 0.42f;
        return;

    case Scene::DemoSceneId::GpuDrivenLab:
        settings.editorGridEnabled = false;
        settings.screenSpaceReflectionsEnabled = false;
        settings.localLightShadowsEnabled = false;
        settings.gpuDrivenEnabled = true;
        settings.gpuInstancingEnabled = false;
        settings.frustumCullingEnabled = true;
        settings.occlusionCullingEnabled = true;
        settings.ambientIntensity = 0.035f;
        settings.iblIntensity = 0.24f;
        settings.exposure = 0.92f;
        return;

    case Scene::DemoSceneId::PostProcessLab:
        settings.editorGridEnabled = false;
        settings.screenSpaceReflectionsEnabled = false;
        settings.localLightShadowsEnabled = false;
        settings.ambientIntensity = 0.015f;
        settings.iblIntensity = 0.10f;
        settings.exposure = 0.86f;
        settings.bloomThreshold = 0.72f;
        settings.bloomIntensity = 0.95f;
        return;

    case Scene::DemoSceneId::RenderGraphLab:
        settings.editorGridEnabled = false;
        settings.localLightShadowsEnabled = true;
        settings.screenSpaceReflectionsEnabled = true;
        settings.gpuDrivenEnabled = true;
        settings.ambientIntensity = 0.025f;
        settings.iblIntensity = 0.18f;
        settings.exposure = 0.90f;
        settings.bloomThreshold = 0.78f;
        settings.bloomIntensity = 0.72f;
        return;

    case Scene::DemoSceneId::MaterialLab:
        settings.editorGridEnabled = false;
        settings.screenSpaceReflectionsEnabled = false;
        settings.ambientIntensity = 0.045f;
        settings.iblIntensity = 0.48f;
        settings.iblDiffuseStrength = 0.88f;
        settings.iblSpecularStrength = 1.15f;
        settings.exposure = 1.0f;
        settings.bloomIntensity = 0.28f;
        return;

    case Scene::DemoSceneId::AssetStreamingLab:
        settings.editorGridEnabled = false;
        settings.screenSpaceReflectionsEnabled = false;
        settings.localLightShadowsEnabled = false;
        settings.ambientIntensity = 0.04f;
        settings.iblIntensity = 0.30f;
        settings.exposure = 0.96f;
        return;

    case Scene::DemoSceneId::AtmosphereLab:
        settings.editorGridEnabled = false;
        settings.physicalAtmosphereEnabled = true;
        settings.atmosphereBrightness = 2.0f;
        settings.screenSpaceReflectionsEnabled = false;
        settings.localLightShadowsEnabled = false;
        settings.ambientIntensity = 0.018f;
        settings.iblIntensity = 0.16f;
        settings.iblDiffuseStrength = 0.55f;
        settings.iblSpecularStrength = 0.72f;
        settings.exposure = 1.12f;
        settings.bloomThreshold = 1.1f;
        settings.bloomIntensity = 0.38f;
        settings.atmosphereHeightKm = 100.0f;
        settings.atmosphereRayleighScaleHeightKm = 8.0f;
        settings.atmosphereMieScaleHeightKm = 1.2f;
        settings.atmosphereMieAnisotropy = 0.82f;
        settings.atmosphereMultipleScattering = 1.15f;
        settings.sunAngularRadiusDegrees = 0.53f;
        settings.shadowDistance = 180.0f;
        return;

    case Scene::DemoSceneId::LargeWorldLab:
        settings.relativeToEyeEnabled = true;
        settings.editorGridEnabled = false;
        settings.physicalAtmosphereEnabled = false;
        settings.shadowsEnabled = false;
        settings.cascadeShadowsEnabled = false;
        settings.pointLightsEnabled = false;
        settings.clusteredLightingEnabled = false;
        settings.spotLightsEnabled = false;
        settings.localLightShadowsEnabled = false;
        settings.gpuDrivenEnabled = false;
        settings.planarReflectionsEnabled = false;
        settings.screenSpaceReflectionsEnabled = false;
        settings.gtaoEnabled = false;
        settings.ambientIntensity = 0.035f;
        settings.iblIntensity = 0.32f;
        settings.skyZenithColor = {0.025f, 0.09f, 0.22f};
        settings.skyHorizonColor = {0.22f, 0.42f, 0.68f};
        settings.groundColor = {0.015f, 0.022f, 0.035f};
        settings.exposure = 1.0f;
        settings.bloomIntensity = 0.24f;
        return;

    case Scene::DemoSceneId::TerrainVirtualTextureLab:
        settings.virtualTerrainEnabled = true;
        // Keep the diagnostic physical atlas out of the terrain material by
        // default. It remains available from the Inspector for VT debugging.
        settings.terrainVirtualTextureEnabled = false;
        // Use the continuous procedural palette by default. The diagnostic
        // virtual texture stays disabled, so this does not sample its atlas.
        settings.terrainMaterialColorsEnabled = true;
        settings.terrainTileDebugEnabled = false;
        settings.interactiveTerrainEnabled = true;
        settings.terrainSculptEnabled = false;
        settings.terrainErosionEnabled = true;
        settings.terrainWorldSize = 4096.0f;
        settings.terrainTileBorderWidth = 2.0f;
        settings.terrainHeightScale = 1000.0f;
        settings.terrainBaseHeight =
            -0.45f * settings.terrainHeightScale;
        settings.terrainBrushRadius = 220.0f;
        settings.terrainBrushStrength = 0.20f;
        settings.editorGridEnabled = false;
        settings.shadowsEnabled = false;
        settings.cascadeShadowsEnabled = false;
        settings.localLightShadowsEnabled = false;
        settings.pointLightsEnabled = false;
        settings.clusteredLightingEnabled = false;
        settings.spotLightsEnabled = false;
        // Terrain patches use the regular geometry path. This keeps the
        // heightfield usable when indirect culling buffers are unavailable.
        settings.gpuDrivenEnabled = false;
        settings.gpuInstancingEnabled = false;
        settings.frustumCullingEnabled = true;
        settings.occlusionCullingEnabled = true;
        settings.gameCameraEnabled = true;
        settings.screenSpaceReflectionsEnabled = false;
        settings.planarReflectionsEnabled = false;
        settings.physicalAtmosphereEnabled = true;
        settings.atmosphereBrightness = 2.0f;
        settings.ambientIntensity = 0.30f;
        settings.iblIntensity = 0.80f;
        settings.iblDiffuseStrength = 1.15f;
        settings.skyZenithColor = {0.08f, 0.24f, 0.52f};
        settings.skyHorizonColor = {0.58f, 0.76f, 0.92f};
        settings.groundColor = {0.10f, 0.16f, 0.10f};
        settings.exposure = 1.12f;
        settings.bloomThreshold = 1.15f;
        settings.bloomIntensity = 0.28f;
        settings.shadowDistance = 700.0f;
        return;

    case Scene::DemoSceneId::OceanLab:
        // Keep the legacy FFT path as the migration baseline. The structured
        // settings already carry the WaveWorks Reference values and will be
        // consumed by SpectralOcean once that implementation is enabled.
        settings.ocean.implementation = OceanImplementation::LegacyFft;
        settings.ocean.debug.simulateWater = true;
        settings.ocean.debug.renderWater = true;
        settings.ocean.cameraFollowEnabled = true;
        settings.fftOceanEnabled = true;
        settings.oceanCameraFollowEnabled = true;
        settings.oceanPatchLength = 320.0f;
        settings.oceanWindDirection = {0.72f, 0.42f};
        settings.oceanWindSpeed = 15.0f;
        settings.oceanSpectrumAmplitude = 10.5f;
        settings.oceanChoppiness = 1.18f;
        settings.oceanDeepWaterColor = {
            0.004f, 0.018f, 0.021f};
        settings.oceanScatteringColor = {
            0.018f, 0.058f, 0.057f};
        settings.oceanFoamColor = {
            0.72f, 0.78f, 0.80f};
        settings.SyncLegacyOceanFields();
        settings.editorGridEnabled = false;
        settings.shadowsEnabled = false;
        settings.cascadeShadowsEnabled = false;
        settings.localLightShadowsEnabled = false;
        settings.pointLightsEnabled = false;
        settings.clusteredLightingEnabled = false;
        settings.spotLightsEnabled = false;
        settings.gpuDrivenEnabled = false;
        // Ocean optical scattering is authored in the surface shader. Keep the
        // Lab on the forward path so the surface type remains available all
        // the way through lighting instead of being lost in the compact
        // deferred G-buffer.
        settings.deferredRenderingEnabled = false;
        settings.forwardPlusEnabled = false;
        settings.physicalAtmosphereEnabled = true;
        settings.atmosphereBrightness = 1.18f;
        // The forward ocean gets its reflection from Fresnel/IBL. SSR depends
        // on deferred surface buffers, so it is intentionally inactive here.
        settings.screenSpaceReflectionsEnabled = false;
        settings.planarReflectionsEnabled = false;
        settings.ambientIntensity = 0.012f;
        settings.iblIntensity = 0.68f;
        settings.iblSpecularStrength = 1.08f;
        settings.skyZenithColor = {0.030f, 0.075f, 0.11f};
        settings.skyHorizonColor = {0.38f, 0.34f, 0.22f};
        settings.groundColor = {0.010f, 0.026f, 0.030f};
        settings.sunAngularRadiusDegrees = 0.38f;
        settings.exposure = 1.0f;
        settings.bloomThreshold = 0.82f;
        settings.bloomIntensity = 0.30f;
        return;

    case Scene::DemoSceneId::WaveWorksLab:
    case Scene::DemoSceneId::HpWaterOceanLab:
        settings.ocean = sceneId == Scene::DemoSceneId::HpWaterOceanLab
            ? OceanSettings::HpWaterReference()
            : OceanSettings::WaveWorksReference();
        ApplyOceanAutomationOverrides(settings.ocean);
        if (sceneId == Scene::DemoSceneId::HpWaterOceanLab)
        {
            // The reference camera grazes a receiver plane ten metres below
            // the surface, so its visible intersections extend much farther
            // than the surface points themselves. Keep the inner cascade
            // detailed while letting the middle cascade cover those valid
            // underwater receivers.
            settings.ocean.optics.caustics.nearCoverageMeters = 256.0f;
            settings.ocean.optics.caustics.middleCoverageMeters = 2048.0f;
        }
        settings.gameCameraEnabled = true;
        settings.ocean.debug.simulateWater = true;
        settings.ocean.debug.renderWater = true;
        settings.ocean.cameraFollowEnabled = true;
        // Flat fields remain render-compatibility inputs until the dedicated
        // ocean surface pipeline owns all geometry and shading constants.
        // Do not call SyncLegacyOceanFields here: it intentionally selects the
        // Phillips implementation for the original FFT Ocean Lab.
        settings.fftOceanEnabled = true;
        settings.oceanCameraFollowEnabled = true;
        settings.oceanPatchLength =
            settings.ocean.simulationPeriodMeters;
        settings.oceanWindDirection =
            settings.ocean.baseWind.direction;
        settings.oceanWindSpeed = settings.ocean.baseWind.speed;
        settings.oceanSpectrumAmplitude =
            settings.ocean.baseWind.amplitudeMultiplier;
        settings.oceanChoppiness = settings.ocean.lateralMultiplier;
        settings.oceanDeepWaterColor =
            settings.ocean.shading.deepWaterColor;
        settings.oceanScatteringColor =
            settings.ocean.shading.scatteringColor;
        settings.oceanFoamColor = settings.ocean.shading.foamColor;
        settings.editorGridEnabled = false;
        // This presentation lab contains no editable terrain. The generic
        // RenderSettings defaults enable terrain simulation for the terrain
        // tools, which otherwise schedules an unused compute pass here.
        settings.interactiveTerrainEnabled = false;
        settings.terrainSculptEnabled = false;
        settings.terrainErosionEnabled = false;
        settings.shadowsEnabled = false;
        settings.cascadeShadowsEnabled = false;
        settings.localLightShadowsEnabled = false;
        settings.pointLightsEnabled = false;
        settings.clusteredLightingEnabled = false;
        settings.spotLightsEnabled = false;
        settings.gpuDrivenEnabled = false;
        settings.deferredRenderingEnabled = false;
        settings.forwardPlusEnabled = false;
        settings.physicalAtmosphereEnabled = true;
        settings.atmosphereBrightness = 1.25f;
        settings.screenSpaceReflectionsEnabled = false;
        settings.planarReflectionsEnabled = false;
        settings.ambientIntensity = 0.015f;
        settings.iblIntensity = 0.92f;
        settings.iblSpecularStrength = 1.08f;
        settings.skyZenithColor = {0.10f, 0.18f, 0.24f};
        settings.skyHorizonColor = {0.82f, 0.76f, 0.58f};
        // Water reflects the lower environment at normal incidence. A nearly
        // black ground hemisphere made troughs collapse to black even under a
        // bright sky; retain blue-green depth without crushing the midtones.
        settings.groundColor = {0.032f, 0.060f, 0.070f};
        settings.sunAngularRadiusDegrees = 0.53f;
        settings.exposure = 1.16f;
        settings.bloomThreshold = 0.82f;
        settings.bloomIntensity = 0.32f;
        return;

    case Scene::DemoSceneId::PbfLab:
    case Scene::DemoSceneId::FluidRenderLab:
    case Scene::DemoSceneId::FluidCausticsLab:
    case Scene::DemoSceneId::FluidToonLab:
    {
        settings.fluid.enabled = true;
        settings.editorGridEnabled = false;
        settings.gpuDrivenEnabled = false;
        settings.gpuInstancingEnabled = false;
        settings.planarReflectionsEnabled = false;
        settings.screenSpaceReflectionsEnabled = false;
        // The first integration intentionally starts without temporal
        // accumulation; the reconstructed surface already has a temporal
        // signal and needs a dedicated reactive mask before TAA is useful.
        settings.temporalAntiAliasingEnabled = false;
        settings.ambientIntensity = 0.10f;
        settings.iblIntensity = 0.58f;
        settings.iblDiffuseStrength = 0.96f;
        settings.iblSpecularStrength = 1.04f;
        // The procedural fallback cubemap is intentionally contrasty for
        // reflections. Keep it available to the water, but let the authored
        // daylight gradient dominate the visible sky instead of importing
        // its dark lower hemisphere into the background.
        settings.skyEnvironmentBlend = 0.12f;
        settings.skyZenithColor = {0.12f, 0.30f, 0.56f};
        settings.skyHorizonColor = {0.68f, 0.80f, 0.90f};
        settings.groundColor = {0.15f, 0.17f, 0.19f};
        settings.exposure = 1.04f;
        settings.bloomThreshold = 1.18f;
        settings.bloomIntensity = 0.24f;
        settings.shadowDistance = 42.0f;
        settings.fluid.renderMode = FluidRenderMode::Realistic;
        settings.fluid.toonEnabled = false;
        settings.fluid.foamEnabled = false;
        settings.fluid.causticsEnabled = false;
        // Use the 64K surface LOD. A denser 256K preset multiplied PBF and
        // sphere-splat cost without improving a liquid that was allowed to
        // spread over a very large receiver. Keeping the original volume in a
        // compact domain produces a thicker, better sampled sheet at a much
        // lower cost. Length scales follow the cube root of the 2x base count.
        settings.fluid.particleCount = 64u * 32u * 32u;
        settings.fluid.maxParticlesPerCell = 64u;
        settings.fluid.particleMass = 0.2916f;
        settings.fluid.particleRadius = 0.03572f;
        settings.fluid.smoothingRadius = 0.14287f;
        // The reference uses a much denser particle set. At this 64K LOD a
        // 2x sphere is only approximately tangent to the next splat, leaving
        // the lateral silhouette visibly beaded. A modest overlap closes the
        // edge without paying the simulation cost of another particle LOD.
        settings.fluid.renderParticleRadiusScale = 2.35f;
        settings.fluid.thicknessScale = 0.47f;
        // The visible scene remains a ground-only setup. Numerical boundaries
        // sit at the ground edge to keep the compact demo from spreading into
        // an increasingly thin, visibly particle-sized film.
        settings.fluid.domainMin.x = -3.0f;
        settings.fluid.domainMin.z = -3.0f;
        settings.fluid.domainMax.x = 3.0f;
        settings.fluid.domainMax.z = 3.0f;
        // Release one compact reservoir from the near-left corner instead of
        // spawning the old pair of central dam columns. Keep its base close to
        // the floor: a high-altitude 20 m^3 block produces an artificial impact
        // plume before the PBF solver can redistribute pressure.
        settings.fluid.spawnLayout = FluidSpawnLayout::Block;
        settings.fluid.spawnMin = {-2.75f, 0.08f, -2.75f};
        settings.fluid.spawnMax = {-0.05f, 2.78f, -0.05f};
        // Reject disconnected spray before sphere splats reach the surface
        // reconstruction. Coherent sheets remain above this density, while
        // isolated impact particles no longer appear as large floating balls.
        settings.fluid.minimumRenderDensityRatio = 0.20f;
        settings.fluid.minimumSplashNeighborCount = 10u;
        settings.fluid.splashVelocityThreshold = 1.75f;
        settings.fluid.splashDensityRatioThreshold = 0.45f;
        settings.fluid.maxPositionCorrection = 0.03f;
        settings.fluid.viscosity = 0.05f;
        settings.fluid.boundaryRestitution = 0.0f;
        settings.fluid.boundaryCornerDamping = 0.08f;
        settings.fluid.boundaryCornerUpwardVelocityLimit = 0.0f;
        settings.fluid.maxVelocity = 5.0f;
        settings.fluid.surfaceCoverageThreshold = 0.08f;
        // Match the reference coarse-to-fine depth reconstruction. With the
        // 64K performance LOD, the wide normal baseline below replaces the
        // missing particle sampling density in close camera views.
        settings.fluid.bilateralIterations = 8u;
        settings.fluid.bilateralRadius = 15u;
        settings.fluid.bilateralSpatialSigma = 7.5f;
        settings.fluid.normalSmoothingRadius = 6u;
        settings.fluid.silhouetteSmoothingRadius = 6u;

        if (sceneId == Scene::DemoSceneId::PbfLab)
        {
            settings.fluid.demoPipeline =
                FluidDemoPipeline::PbfParticles;
            settings.fluid.particleCount =
                FluidSettings::DefaultParticleCount;
            settings.fluid.maxParticlesPerCell = 64u;
            settings.fluid.particleMass = 0.5832f;
            settings.fluid.particleRadius = 0.045f;
            settings.fluid.smoothingRadius = 0.18f;
            settings.fluid.renderMode = FluidRenderMode::Particles;
            settings.fluid.renderParticleRadiusScale = 0.95f;
            // Keep the direct particle visualization, but do not draw lone
            // under-supported particles as persistent corner spray. The 0.20
            // cutoff matches the reference particle renderer's surface test.
            settings.fluid.minimumRenderDensityRatio = 0.20f;
            settings.fluid.splashDensityRatioThreshold = 0.45f;
            settings.skyZenithColor = {0.10f, 0.25f, 0.48f};
            settings.skyHorizonColor = {0.60f, 0.73f, 0.84f};
            settings.groundColor = {0.12f, 0.14f, 0.17f};
            settings.bloomIntensity = 0.0f;
        }
        else if (sceneId
                 == Scene::DemoSceneId::FluidCausticsLab)
        {
            settings.fluid.demoPipeline =
                FluidDemoPipeline::ScreenSpaceCaustics;
            settings.fluid.causticsEnabled = true;
            settings.fluid.causticsDebugView = false;
            settings.fluid.waterColor = {0.018f, 0.16f, 0.25f};
            settings.fluid.absorption = {0.42f, 0.18f, 0.06f};
            settings.fluid.scattering = {0.008f, 0.025f, 0.065f};
            settings.fluid.refractionScale = 0.014f;
            settings.fluid.reflectionStrength = 0.34f;
            settings.fluid.thicknessScale = 0.26f;
            settings.fluid.causticsIntensity = 1.8f;
            settings.fluid.causticsRefractionScalePixels = 64.0f;
            settings.fluid.causticsFocusStrength = 1.8f;
            settings.fluid.causticsFocusPower = 1.4f;
            settings.fluid.causticsBlurRadius = 1u;
            settings.fluid.causticsBlurSigma = 0.8f;
            settings.skyZenithColor = {0.09f, 0.24f, 0.50f};
            settings.skyHorizonColor = {0.74f, 0.82f, 0.86f};
            settings.groundColor = {0.16f, 0.14f, 0.11f};
            settings.exposure = 0.98f;
            settings.bloomThreshold = 1.35f;
            settings.bloomIntensity = 0.12f;
        }
        else if (sceneId
                 == Scene::DemoSceneId::FluidToonLab)
        {
            settings.fluid.demoPipeline =
                FluidDemoPipeline::ScreenSpaceToonFoam;
            settings.fluid.renderMode = FluidRenderMode::Toon;
            settings.fluid.toonEnabled = true;
            settings.fluid.foamEnabled = true;
            // Reference-style blue bands with a one-pixel validity outline.
            // The lower foam interval and radius-four erosion leave foam on
            // coherent low-density spray instead of whitening every boundary.
            // Prism's HDR/ACES output needs a deeper linear input than the
            // reference OpenGL demo to retain saturated blue after tonemapping.
            settings.fluid.waterColor = {0.03f, 0.18f, 0.68f};
            settings.fluid.toonBands = 3u;
            settings.fluid.toonEdgeWidth = 1.0f;
            settings.fluid.foamDensityThreshold = 0.28f;
            settings.fluid.foamErosionIterations = 4u;
            settings.skyZenithColor = {0.10f, 0.28f, 0.62f};
            settings.skyHorizonColor = {0.62f, 0.80f, 0.96f};
            settings.groundColor = {0.16f, 0.22f, 0.31f};
            settings.bloomIntensity = 0.12f;
        }
        else
        {
            settings.fluid.demoPipeline =
                FluidDemoPipeline::ScreenSpaceRealistic;
        }
        return;
    }

    case Scene::DemoSceneId::Count:
        return;
    }
}
} // namespace Prism::Renderer
