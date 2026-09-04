#pragma once

#include "Renderer/Features/Fluid/FluidSettings.h"
#include "Renderer/Features/Ocean/OceanSettings.h"

#include <DirectXMath.h>

#include <cstdint>

namespace Prism::Renderer
{
enum class ShadowFilterMode : std::uint32_t
{
    Hard = 0,
    Pcf3x3,
    Pcf5x5,
    Pcss,
    Vsm,
    Evsm
};

// Per-viewport editor presentation. Game/Final Output keeps Shaded while the
// independent Scene renderer may select any of these modes.
enum class ViewportShadingMode : std::uint32_t
{
    Shaded = 0,
    Unlit,
    Wireframe
};

struct RenderSettings
{
    FluidSettings fluid{};
    // Structured ocean settings are the migration boundary for the native
    // WaveWorks-like implementation. The legacy flat fields below remain
    // temporarily available so existing scenes and serialized settings keep
    // their behavior until the spectral path becomes the default.
    OceanSettings ocean{};
    ViewportShadingMode viewportShadingMode =
        ViewportShadingMode::Shaded;
    // Keep CPU transforms in double-precision absolute world space, then
    // subtract the camera origin before uploading float matrices to the GPU.
    bool relativeToEyeEnabled = false;
    bool virtualTerrainEnabled = false;
    bool terrainVirtualTextureEnabled = false;
    // Neutral clay shading is the default so terrain morphology remains
    // readable. Enable this to restore the full terrain material palette.
    bool terrainMaterialColorsEnabled = false;
    bool terrainTileDebugEnabled = false;
    // Persistent GPU heightfield used by the terrain lab. Sculpt mode only
    // controls editor input; Game and Scene always share the same texture.
    bool interactiveTerrainEnabled = false;
    bool terrainSculptEnabled = false;
    bool terrainErosionEnabled = false;
    bool terrainBrushActive = false;
    bool terrainBrushLower = false;
    bool terrainResetRequested = false;
    bool terrainFullUpdateRequested = false;
    bool fftOceanEnabled = false;
    bool oceanCameraFollowEnabled = true;
    bool pbrEnabled = true;
    bool iblEnabled = true;
    bool iblSplitSumEnabled = true;
    bool skyboxEnabled = true;
    bool physicalAtmosphereEnabled = false;
    bool shadowsEnabled = true;
    bool hdrEnabled = true;
    bool bloomEnabled = true;
    bool tonemappingEnabled = true;
    bool deferredRenderingEnabled = true;
    // When deferred rendering is disabled, use the same clustered light lists
    // as the deferred path instead of the four-light compatibility loop.
    bool forwardPlusEnabled = true;
    bool directLightingEnabled = true;
    bool pointLightsEnabled = true;
    bool clusteredLightingEnabled = true;
    bool spotLightsEnabled = true;
    bool localLightShadowsEnabled = true;
    bool normalMappingEnabled = true;
    bool occlusionEnabled = true;
    bool emissiveEnabled = true;
    bool alphaMaskEnabled = true;
    bool frustumCullingEnabled = true;
    bool occlusionCullingEnabled = true;
    // Game/Final Output uses RenderScene::GetGameCamera while Scene View keeps
    // using the independently controlled editor camera.
    bool gameCameraEnabled = false;
    bool gpuInstancingEnabled = true;
    bool gpuDrivenEnabled = true;
    bool gpuVisibilityReadbackEnabled = false;
    bool temporalAntiAliasingEnabled = true;
    bool gtaoEnabled = true;
    bool screenSpaceReflectionsEnabled = true;
    bool planarReflectionsEnabled = false;
    bool editorGridEnabled = true;
    bool cascadeShadowsEnabled = true;
    bool cascadeDebugEnabled = false;
    ShadowFilterMode shadowFilterMode = ShadowFilterMode::Pcf3x3;
    float ambientIntensity = 0.08f;
    float iblIntensity = 0.35f;
    float iblDiffuseStrength = 1.0f;
    float iblSpecularStrength = 1.0f;
    float iblReflectionBlend = 0.85f;
    float iblHorizonSharpness = 3.0f;
    // Environment contribution to the visible sky. Zero keeps only the
    // authored gradient; one displays the cubemap directly.
    float skyEnvironmentBlend = 0.12f;
    DirectX::XMFLOAT3 skyZenithColor{0.06f, 0.20f, 0.48f};
    DirectX::XMFLOAT3 skyHorizonColor{0.52f, 0.72f, 0.92f};
    DirectX::XMFLOAT3 groundColor{0.04f, 0.04f, 0.05f};
    DirectX::XMFLOAT3 gridMinorColor{0.22f, 0.24f, 0.27f};
    DirectX::XMFLOAT3 gridMajorColor{0.42f, 0.45f, 0.50f};
    float atmosphereDensity = 1.0f;
    float atmosphereBrightness = 2.0f;
    float atmospherePlanetRadiusKm = 6360.0f;
    float atmosphereHeightKm = 100.0f;
    float atmosphereRayleighScaleHeightKm = 8.0f;
    float atmosphereMieScaleHeightKm = 1.2f;
    DirectX::XMFLOAT3 atmosphereRayleighScattering{
        0.005802f,
        0.013558f,
        0.033100f};
    float atmosphereMieScattering = 0.003996f;
    float atmosphereMieAbsorption = 0.000444f;
    float atmosphereMieAnisotropy = 0.8f;
    float atmosphereMultipleScattering = 1.0f;
    float terrainWorldSize = 512.0f;
    float terrainTileBorderWidth = 4.0f;
    // DefaultHeight is 0.45 in the compute shader, so this pair keeps the
    // untouched plain at y=0 while giving the painted relief a mountain-scale
    // vertical aspect relative to the 4096 m map.
    float terrainBaseHeight = -450.0f;
    float terrainHeightScale = 1000.0f;
    float terrainBrushRadius = 220.0f;
    float terrainBrushStrength = 0.20f;
    DirectX::XMFLOAT2 terrainBrushUv{0.5f, 0.5f};
    float terrainBrushDelta = 0.0f;
    std::uint64_t terrainBrushCommandRevision = 0;
    float oceanPatchLength = 320.0f;
    DirectX::XMFLOAT2 oceanWindDirection{0.85f, 0.35f};
    float oceanWindSpeed = 18.0f;
    float oceanSpectrumAmplitude = 12.0f;
    float oceanChoppiness = 1.25f;
    DirectX::XMFLOAT3 oceanDeepWaterColor{
        0.004f, 0.016f, 0.024f};
    DirectX::XMFLOAT3 oceanScatteringColor{
        0.014f, 0.055f, 0.070f};
    DirectX::XMFLOAT3 oceanFoamColor{
        0.72f, 0.78f, 0.80f};
    float sunAngularRadiusDegrees = 0.85f;
    float gridScale = 1.0f;
    float gridFadeDistance = 140.0f;
    float exposure = 1.2f;
    float bloomThreshold = 1.0f;
    float bloomIntensity = 0.85f;
    float shadowBias = 0.0035f;
    float shadowDistance = 90.0f;
    float cascadeSplitLambda = 0.65f;
    float cascadeBlendFraction = 0.08f;
    float planarReflectionPlaneHeight = 0.0f;
    float planarReflectionIntensity = 0.82f;

    // Transitional adapter for scenes serialized before OceanSettings was
    // introduced.  The legacy values remain authoritative only while the
    // LegacyFft implementation is selected; the spectral path never reads
    // these fields after migration.
    void SyncLegacyOceanFields()
    {
        ocean.implementation = OceanImplementation::LegacyFft;
        ocean.debug.simulateWater = fftOceanEnabled;
        ocean.debug.renderWater = fftOceanEnabled;
        ocean.cameraFollowEnabled = oceanCameraFollowEnabled;
        ocean.local.domainSizeMeters = oceanPatchLength;
        ocean.baseWind.direction = oceanWindDirection;
        ocean.baseWind.speed = oceanWindSpeed;
        ocean.baseWind.amplitudeMultiplier = oceanSpectrumAmplitude;
        ocean.lateralMultiplier = oceanChoppiness;
        ocean.shading.deepWaterColor = oceanDeepWaterColor;
        ocean.shading.scatteringColor = oceanScatteringColor;
        ocean.shading.foamColor = oceanFoamColor;
    }
};
} // namespace Prism::Renderer
