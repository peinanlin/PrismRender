#pragma once

#include <DirectXMath.h>

#include <cstdint>
#include <string>

namespace Prism::Renderer
{
enum class OceanOpticsModel : std::uint32_t
{
    Current = 0,
    HpWater
};

enum class WaterOpticsQuality : std::uint32_t
{
    Normal = 0,
    High,
    Extreme
};

struct WaterOpticsQualityPolicy
{
    float refractionResolutionScale = 0.75f;
    float volumetricResolutionMultiplier = 1.0f;
    std::uint32_t maximumRayMarchSamples = 8u;
};

[[nodiscard]] constexpr WaterOpticsQualityPolicy
GetWaterOpticsQualityPolicy(const WaterOpticsQuality quality) noexcept
{
    switch (quality)
    {
    case WaterOpticsQuality::Normal: return {0.5f, 0.5f, 0u};
    case WaterOpticsQuality::High: return {0.75f, 1.0f, 8u};
    case WaterOpticsQuality::Extreme: return {1.0f, 1.0f, 8u};
    }
    return {0.75f, 1.0f, 8u};
}

enum class WaterCausticsMode : std::uint32_t
{
    Disabled = 0,
    SingleChannel,
    Chromatic
};

enum class WaterOpticsDebugView : std::uint32_t
{
    None = 0,
    WaterMask,
    WaterDepth,
    NormalRoughness,
    Absorption,
    Scattering,
    Foam,
    Thickness,
    RefractionHit,
    DistanceTier,
    CausticEnergy,
    CausticCascades,
    VolumetricAccumulation,
    VolumetricHistoryRejection
};

struct WaterMaterialSettings
{
    DirectX::XMFLOAT3 absorption{0.16f, 0.055f, 0.025f};
    DirectX::XMFLOAT3 scattering{0.018f, 0.065f, 0.09f};
    float roughness = 0.035f;
    float indexOfRefraction = 1.333f;
    float phaseG = 0.8f;
    float thinLayerStrength = 0.35f;
    float backlitStrength = 0.2f;
};

struct WaterRefractionSettings
{
    bool enabled = true;
    bool highPrecision = false;
    std::uint32_t rayMarchSampleCount = 8u;
    float distortionStrength = 0.025f;
    float maximumUvOffset = 0.08f;
    float thicknessOffsetMeters = 0.08f;
    float maximumThicknessMeters = 80.0f;
    float rayStepScale = 1.6f;
};

struct WaterCausticsSettings
{
    bool enabled = true;
    WaterCausticsMode mode = WaterCausticsMode::SingleChannel;
    std::uint32_t resolution = 512u;
    float nearCoverageMeters = 64.0f;
    float middleCoverageMeters = 256.0f;
    float intensity = 1.0f;
    float dispersion = 0.003f;
    float edgeFadeFraction = 0.12f;
};

struct WaterVolumetricSettings
{
    bool enabled = true;
    float resolutionScale = 0.5f;
    std::uint32_t sampleCount = 16u;
    float maximumDistanceMeters = 160.0f;
    float historyWeight = 0.9f;
    float depthRejectionMeters = 0.5f;
    std::uint32_t atrousIterations = 2u;
    float surfaceHysteresisMeters = 0.12f;
};

struct WaterDistanceQualitySettings
{
    float nearEndMeters = 120.0f;
    float middleEndMeters = 800.0f;
    float farEndMeters = 4000.0f;
    float transitionFraction = 0.12f;
};

struct WaterDistanceTierWeights
{
    float nearWeight = 1.0f;
    float middleWeight = 0.0f;
    float farWeight = 0.0f;
    float normalizedDistance = 0.0f;
};

// Produces a smooth partition of unity for optical work only. The far weight
// intentionally remains one past farEndMeters so distance quality can never
// remove the kilometer-scale spectral surface.
[[nodiscard]] WaterDistanceTierWeights EvaluateWaterDistanceTierWeights(
    float cameraRelativeDistanceMeters,
    const WaterDistanceQualitySettings& settings) noexcept;

[[nodiscard]] WaterDistanceTierWeights EvaluateWaterDistanceTierWeights(
    const DirectX::XMFLOAT3& cameraPosition,
    const DirectX::XMFLOAT3& surfacePosition,
    const WaterDistanceQualitySettings& settings) noexcept;

struct WaterOpticsSettings
{
    WaterOpticsQuality quality = WaterOpticsQuality::High;
    WaterMaterialSettings material{};
    WaterRefractionSettings refraction{};
    WaterCausticsSettings caustics{};
    WaterVolumetricSettings volumetrics{};
    WaterDistanceQualitySettings distance{};
    WaterOpticsDebugView debugView = WaterOpticsDebugView::None;
    std::uint32_t historyResetSerial = 0u;

    [[nodiscard]] static WaterOpticsSettings HpWaterReference();
    bool ValidateAndNormalize(std::string* message = nullptr);
};

[[nodiscard]] constexpr WaterCausticsMode GetEffectiveWaterCausticsMode(
    const WaterOpticsSettings& settings) noexcept
{
    if (!settings.caustics.enabled) return WaterCausticsMode::Disabled;
    return settings.quality == WaterOpticsQuality::Normal
            && settings.caustics.mode == WaterCausticsMode::Chromatic
        ? WaterCausticsMode::SingleChannel : settings.caustics.mode;
}

enum class WaterOpticsDirtyScope : std::uint32_t
{
    None = 0u,
    Constants = 1u << 0u,
    Resources = 1u << 1u,
    History = 1u << 2u,
    Caustics = 1u << 3u,
    Volumetrics = 1u << 4u
};

constexpr WaterOpticsDirtyScope operator|(WaterOpticsDirtyScope lhs,
    WaterOpticsDirtyScope rhs) noexcept
{
    return static_cast<WaterOpticsDirtyScope>(
        static_cast<std::uint32_t>(lhs)
        | static_cast<std::uint32_t>(rhs));
}

constexpr bool HasDirtyScope(WaterOpticsDirtyScope value,
    WaterOpticsDirtyScope scope) noexcept
{
    return (static_cast<std::uint32_t>(value)
        & static_cast<std::uint32_t>(scope)) != 0u;
}

[[nodiscard]] WaterOpticsDirtyScope ClassifyWaterOpticsDirtyScopes(
    const WaterOpticsSettings& previous,
    const WaterOpticsSettings& current) noexcept;
} // namespace Prism::Renderer
