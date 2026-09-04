#pragma once

#include <DirectXMath.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace Prism::Renderer
{
inline constexpr std::uint32_t SharedShadowCascadeCount = 3;
inline constexpr std::uint32_t SharedMaxPointLightCount = 4;
inline constexpr std::uint32_t SharedAuxiliaryDirectionalLightCount = 2;
inline constexpr std::uint32_t SharedShadowMapResolution = 2048;

struct alignas(16) SharedPointLightData
{
    DirectX::XMFLOAT3 position{};
    float range = 0.0f;
    DirectX::XMFLOAT3 color{};
    float intensity = 0.0f;
};

struct alignas(16) SharedDirectionalLightData
{
    DirectX::XMFLOAT3 direction{0.0f, -1.0f, 0.0f};
    float intensity = 0.0f;
    DirectX::XMFLOAT3 color{1.0f, 1.0f, 1.0f};
    float padding = 0.0f;
};

// This layout mirrors FrameConstants in Mesh.hlsl, Deferred.hlsl, and Shadow.hlsl.
struct alignas(16) SharedFrameConstants
{
    DirectX::XMFLOAT3 cameraPosition{};
    float pointLightCount = 0.0f;
    DirectX::XMFLOAT3 directionalLightDirection{};
    float directionalLightIntensity = 1.0f;
    DirectX::XMFLOAT3 directionalLightColor{1.0f, 1.0f, 1.0f};
    float ambientIntensity = 0.08f;
    std::array<SharedPointLightData, SharedMaxPointLightCount> pointLights{};
    std::array<DirectX::XMFLOAT4X4, SharedShadowCascadeCount> lightViewProjections{};
    DirectX::XMFLOAT4 cascadeSplits{};
    DirectX::XMFLOAT3 skyZenithColor{0.22f, 0.30f, 0.42f};
    float pbrEnabled = 1.0f;
    DirectX::XMFLOAT3 skyHorizonColor{0.58f, 0.64f, 0.72f};
    float iblEnabled = 1.0f;
    DirectX::XMFLOAT3 groundColor{0.04f, 0.04f, 0.05f};
    float iblIntensity = 0.35f;
    float iblDiffuseStrength = 1.0f;
    float iblSpecularStrength = 1.0f;
    float iblReflectionBlend = 0.85f;
    float iblHorizonSharpness = 1.5f;
    float iblSplitSumEnabled = 1.0f;
    float shadowBias = 0.0035f;
    float shadowsEnabled = 1.0f;
    float gtaoEnabled = 1.0f;
    float directLightingEnabled = 1.0f;
    float pointLightsEnabled = 1.0f;
    float normalMappingEnabled = 1.0f;
    float occlusionEnabled = 1.0f;
    float emissiveEnabled = 1.0f;
    float alphaMaskEnabled = 1.0f;
    float clusteredLightingEnabled = 1.0f;
    float shadowFilterMode = 1.0f;
    DirectX::XMFLOAT3 cameraForward{0.0f, 0.0f, 1.0f};
    float cascadeBlendFraction = 0.08f;
    DirectX::XMFLOAT3 oceanDeepWaterColor{
        0.004f, 0.016f, 0.024f};
    float physicalAtmosphereEnabled = 0.0f;
    DirectX::XMFLOAT3 oceanScatteringColor{
        0.014f, 0.055f, 0.070f};
    float atmosphereBrightness = 1.0f;
    DirectX::XMFLOAT3 oceanFoamColor{
        0.72f, 0.78f, 0.80f};
    // Applied only by the FFT-ocean surface path; unlike ambientIntensity it
    // is not scaled by the water albedo a second time.
    float oceanScatteringStrength = 1.0f;
    // Auxiliary directional lights are direct-light-only fills. The primary
    // light above remains the sole atmosphere and shadow-cascade authority.
    std::array<SharedDirectionalLightData,
        SharedAuxiliaryDirectionalLightCount> auxiliaryDirectionalLights{};
    // Populated from OceanSpectrumGenerator for the dedicated ocean shader.
    // Ordinary mesh shaders intentionally keep their original FrameConstants
    // reflection prefix and do not declare these trailing values.
    DirectX::XMFLOAT4 oceanCascadePatchLengths{};
    DirectX::XMFLOAT4 oceanCascadeLowerWavelengths{};
    DirectX::XMFLOAT4 oceanCascadeUpperWavelengths{};
    DirectX::XMFLOAT4 oceanCascadeUvScales{};
    DirectX::XMFLOAT4 oceanCascadeUvOffsetX{};
    DirectX::XMFLOAT4 oceanCascadeUvOffsetY{};
    DirectX::XMFLOAT4 oceanCascadeFadeStarts{};
    DirectX::XMFLOAT4 oceanCascadeFadeEnds{};
    DirectX::XMFLOAT4 oceanCascadeLayerOrder{0.0f, 1.0f, 2.0f, 3.0f};
};

struct alignas(16) SharedObjectConstants
{
    DirectX::XMFLOAT4X4 world{};
    DirectX::XMFLOAT4X4 normalMatrix{};
    DirectX::XMFLOAT4X4 worldViewProjection{};
    DirectX::XMFLOAT4X4
        previousWorldViewProjection{};
    DirectX::XMFLOAT4 renderFeatureParams{};
    DirectX::XMFLOAT4 renderFeatureParams2{};
    DirectX::XMFLOAT4 renderFeatureParams3{};
    // Ocean UV-warp/geomorph controls are separate from the local-wave
    // domain, which already consumes renderFeatureParams3.
    DirectX::XMFLOAT4 renderFeatureParams4{};
    // The base entry in instanceObjectIndices for this draw. It is explicit
    // because D3D12 SV_InstanceID does not include StartInstanceLocation.
    DirectX::XMFLOAT4 drawInstanceParams{};
};

struct alignas(16) SharedMaterialConstants
{
    DirectX::XMFLOAT4 albedoColor{1.0f, 1.0f, 1.0f, 1.0f};
    DirectX::XMFLOAT3 emissiveColor{0.0f, 0.0f, 0.0f};
    float metallic = 0.0f;
    float roughness = 0.5f;
    float useAlbedoTexture = 1.0f;
    float useMetallicRoughnessTexture = 0.0f;
    float useNormalTexture = 0.0f;
    float useOcclusionTexture = 0.0f;
    float useEmissiveTexture = 0.0f;
    float occlusionStrength = 1.0f;
    float normalScale = 1.0f;
    float emissiveStrength = 1.0f;
    float alphaCutoff = 0.5f;
    float alphaMode = 0.0f;
    float padding = 0.0f;
};

struct alignas(16) SharedShadowPassConstants
{
    std::uint32_t cascadeIndex = 0;
    std::uint32_t padding[3]{};
};

struct alignas(16) SharedPostProcessConstants
{
    DirectX::XMFLOAT4 postProcessParams0{1.2f, 1.0f, 0.85f, 1.0f};
    DirectX::XMFLOAT4 postProcessParams1{1.0f, 1.0f, 1.0f, 1.0f};
    DirectX::XMFLOAT4 skyZenithAtmosphere{0.22f, 0.30f, 0.42f, 1.0f};
    DirectX::XMFLOAT4 skyHorizonPower{0.58f, 0.64f, 0.72f, 1.5f};
    DirectX::XMFLOAT4 groundGridEnabled{0.04f, 0.04f, 0.05f, 1.0f};
    DirectX::XMFLOAT4 sunDirectionIntensity{0.0f, 1.0f, 0.0f, 1.0f};
    DirectX::XMFLOAT4 sunColorAngularRadius{1.0f, 0.95f, 0.84f, 0.015f};
    DirectX::XMFLOAT4 cameraPositionTanHalfFov{0.0f, 0.0f, 0.0f, 0.414f};
    DirectX::XMFLOAT4 cameraForwardAspect{0.0f, 0.0f, 1.0f, 16.0f / 9.0f};
    DirectX::XMFLOAT4 cameraRightGridScale{1.0f, 0.0f, 0.0f, 1.0f};
    DirectX::XMFLOAT4 cameraUpGridFade{0.0f, 1.0f, 0.0f, 140.0f};
    DirectX::XMFLOAT4 gridMinorColorLineWidth{0.22f, 0.24f, 0.27f, 1.0f};
    DirectX::XMFLOAT4 gridMajorColorAxisWidth{0.42f, 0.45f, 0.50f, 1.0f};
    DirectX::XMFLOAT4 atmosphereLutParams{};
};

static_assert(std::is_standard_layout_v<SharedFrameConstants>);
static_assert(sizeof(SharedPointLightData) == 32);
static_assert(sizeof(SharedDirectionalLightData) == 32);
static_assert(sizeof(SharedFrameConstants) == 768);
static_assert(sizeof(SharedObjectConstants) == 336);
static_assert(sizeof(SharedMaterialConstants) == 80);
static_assert(sizeof(SharedShadowPassConstants) == 16);
static_assert(sizeof(SharedPostProcessConstants) == 224);
static_assert(offsetof(SharedFrameConstants, lightViewProjections) == 176);
static_assert(offsetof(SharedFrameConstants, cascadeSplits) == 368);
static_assert(offsetof(SharedFrameConstants, cameraForward) == 496);
static_assert(offsetof(SharedFrameConstants,
    auxiliaryDirectionalLights) == 560);
} // namespace Prism::Renderer
