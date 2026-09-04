#include "Renderer/Features/Ocean/WaterOpticsSettings.h"

#include <algorithm>
#include <cmath>

namespace Prism::Renderer
{
namespace
{
template <typename T>
void ClampFinite(T& value, const T fallback, const T minimum,
    const T maximum, bool& adjusted)
{
    if (!std::isfinite(static_cast<double>(value)))
    {
        value = fallback;
        adjusted = true;
        return;
    }
    const T clamped = std::clamp(value, minimum, maximum);
    adjusted = adjusted || clamped != value;
    value = clamped;
}

void ClampNonNegativeColor(DirectX::XMFLOAT3& value,
    const DirectX::XMFLOAT3& fallback, bool& adjusted)
{
    ClampFinite(value.x, fallback.x, 0.0f, 64.0f, adjusted);
    ClampFinite(value.y, fallback.y, 0.0f, 64.0f, adjusted);
    ClampFinite(value.z, fallback.z, 0.0f, 64.0f, adjusted);
}

std::uint32_t NearestPowerOfTwo(std::uint32_t value)
{
    value = std::clamp(value, 64u, 2048u);
    std::uint32_t lower = 64u;
    while (lower * 2u <= value)
        lower *= 2u;
    const std::uint32_t upper = std::min(lower * 2u, 2048u);
    return value - lower <= upper - value ? lower : upper;
}

bool Different(float lhs, float rhs)
{
    return std::abs(lhs - rhs) > 1.0e-6f;
}

bool Different(const DirectX::XMFLOAT3& lhs,
    const DirectX::XMFLOAT3& rhs)
{
    return Different(lhs.x, rhs.x)
        || Different(lhs.y, rhs.y)
        || Different(lhs.z, rhs.z);
}

void ClampUnsigned(std::uint32_t& value, const std::uint32_t minimum,
    const std::uint32_t maximum, bool& adjusted)
{
    const std::uint32_t clamped = std::clamp(value, minimum, maximum);
    adjusted = adjusted || clamped != value;
    value = clamped;
}

float SmoothStep(const float minimum, const float maximum,
    const float value) noexcept
{
    if (!(maximum > minimum))
        return value >= maximum ? 1.0f : 0.0f;
    const float t = std::clamp(
        (value - minimum) / (maximum - minimum), 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}
} // namespace

WaterDistanceTierWeights EvaluateWaterDistanceTierWeights(
    const float cameraRelativeDistanceMeters,
    const WaterDistanceQualitySettings& source) noexcept
{
    WaterDistanceQualitySettings settings = source;
    bool ignored = false;
    ClampFinite(settings.nearEndMeters, 120.0f,
        1.0f, 99998.0f, ignored);
    ClampFinite(settings.middleEndMeters, 800.0f,
        2.0f, 99999.0f, ignored);
    ClampFinite(settings.farEndMeters, 4000.0f,
        3.0f, 100000.0f, ignored);
    if (settings.middleEndMeters <= settings.nearEndMeters)
        settings.middleEndMeters = std::min(
            settings.nearEndMeters + 1.0f, 99999.0f);
    if (settings.farEndMeters <= settings.middleEndMeters)
        settings.farEndMeters = std::min(
            settings.middleEndMeters + 1.0f, 100000.0f);
    ClampFinite(settings.transitionFraction, 0.12f,
        0.01f, 0.49f, ignored);

    const float distance = std::isfinite(cameraRelativeDistanceMeters)
        ? std::max(cameraRelativeDistanceMeters, 0.0f) : 0.0f;
    const float nearSpan = settings.nearEndMeters;
    const float middleSpan = settings.middleEndMeters
        - settings.nearEndMeters;
    const float farSpan = settings.farEndMeters
        - settings.middleEndMeters;
    const float nearHalfWidth = std::max(
        std::min(nearSpan, middleSpan) * settings.transitionFraction,
        1.0e-4f);
    const float farHalfWidth = std::max(
        std::min(middleSpan, farSpan) * settings.transitionFraction,
        1.0e-4f);
    const float enteredMiddle = SmoothStep(
        settings.nearEndMeters - nearHalfWidth,
        settings.nearEndMeters + nearHalfWidth, distance);
    const float enteredFar = SmoothStep(
        settings.middleEndMeters - farHalfWidth,
        settings.middleEndMeters + farHalfWidth, distance);

    WaterDistanceTierWeights result{};
    result.nearWeight = std::clamp(1.0f - enteredMiddle, 0.0f, 1.0f);
    result.middleWeight = std::clamp(
        enteredMiddle - enteredFar, 0.0f, 1.0f);
    result.farWeight = std::clamp(enteredFar, 0.0f, 1.0f);
    const float sum = result.nearWeight + result.middleWeight
        + result.farWeight;
    if (sum > 0.0f && std::isfinite(sum))
    {
        result.nearWeight /= sum;
        result.middleWeight /= sum;
        result.farWeight /= sum;
    }
    else
    {
        result = {};
    }
    result.normalizedDistance = std::clamp(
        distance / settings.farEndMeters, 0.0f, 1.0f);
    return result;
}

WaterDistanceTierWeights EvaluateWaterDistanceTierWeights(
    const DirectX::XMFLOAT3& cameraPosition,
    const DirectX::XMFLOAT3& surfacePosition,
    const WaterDistanceQualitySettings& settings) noexcept
{
    const double dx = static_cast<double>(surfacePosition.x)
        - static_cast<double>(cameraPosition.x);
    const double dy = static_cast<double>(surfacePosition.y)
        - static_cast<double>(cameraPosition.y);
    const double dz = static_cast<double>(surfacePosition.z)
        - static_cast<double>(cameraPosition.z);
    const double squaredDistance = dx * dx + dy * dy + dz * dz;
    const float distance = std::isfinite(squaredDistance)
        ? static_cast<float>(std::sqrt(std::max(squaredDistance, 0.0)))
        : 0.0f;
    return EvaluateWaterDistanceTierWeights(distance, settings);
}

WaterOpticsSettings WaterOpticsSettings::HpWaterReference()
{
    WaterOpticsSettings settings{};
    settings.quality = WaterOpticsQuality::High;
    settings.material = {};
    settings.refraction = {};
    settings.caustics = {};
    settings.volumetrics = {};
    settings.distance = {};
    settings.debugView = WaterOpticsDebugView::None;
    return settings;
}

bool WaterOpticsSettings::ValidateAndNormalize(std::string* message)
{
    bool adjusted = false;
    const WaterOpticsSettings fallback = HpWaterReference();

    if (quality > WaterOpticsQuality::Extreme)
    {
        quality = fallback.quality;
        adjusted = true;
    }
    if (caustics.mode > WaterCausticsMode::Chromatic)
    {
        caustics.mode = fallback.caustics.mode;
        adjusted = true;
    }
    if (debugView > WaterOpticsDebugView::VolumetricHistoryRejection)
    {
        debugView = WaterOpticsDebugView::None;
        adjusted = true;
    }

    ClampNonNegativeColor(material.absorption,
        fallback.material.absorption, adjusted);
    ClampNonNegativeColor(material.scattering,
        fallback.material.scattering, adjusted);
    ClampFinite(material.roughness, fallback.material.roughness,
        0.001f, 1.0f, adjusted);
    ClampFinite(material.indexOfRefraction,
        fallback.material.indexOfRefraction, 1.0001f, 3.0f, adjusted);
    ClampFinite(material.phaseG, fallback.material.phaseG,
        -0.95f, 0.95f, adjusted);
    ClampFinite(material.thinLayerStrength,
        fallback.material.thinLayerStrength, 0.0f, 8.0f, adjusted);
    ClampFinite(material.backlitStrength,
        fallback.material.backlitStrength, 0.0f, 8.0f, adjusted);

    ClampUnsigned(refraction.rayMarchSampleCount, 1u, 64u, adjusted);
    ClampFinite(refraction.distortionStrength,
        fallback.refraction.distortionStrength, 0.0f, 1.0f, adjusted);
    ClampFinite(refraction.maximumUvOffset,
        fallback.refraction.maximumUvOffset, 0.0f, 0.5f, adjusted);
    ClampFinite(refraction.thicknessOffsetMeters,
        fallback.refraction.thicknessOffsetMeters, 0.0f, 10.0f, adjusted);
    ClampFinite(refraction.maximumThicknessMeters,
        fallback.refraction.maximumThicknessMeters, 0.01f, 10000.0f,
        adjusted);
    ClampFinite(refraction.rayStepScale,
        fallback.refraction.rayStepScale, 1.01f, 8.0f, adjusted);

    const std::uint32_t causticResolution =
        NearestPowerOfTwo(caustics.resolution);
    adjusted = adjusted || causticResolution != caustics.resolution;
    caustics.resolution = causticResolution;
    ClampFinite(caustics.nearCoverageMeters,
        fallback.caustics.nearCoverageMeters, 1.0f, 10000.0f, adjusted);
    ClampFinite(caustics.middleCoverageMeters,
        fallback.caustics.middleCoverageMeters, 1.0f, 20000.0f, adjusted);
    if (caustics.middleCoverageMeters <= caustics.nearCoverageMeters)
    {
        caustics.middleCoverageMeters = std::min(
            caustics.nearCoverageMeters * 4.0f, 20000.0f);
        adjusted = true;
    }
    ClampFinite(caustics.intensity, fallback.caustics.intensity,
        0.0f, 100.0f, adjusted);
    ClampFinite(caustics.dispersion, fallback.caustics.dispersion,
        0.0f, 0.1f, adjusted);
    ClampFinite(caustics.edgeFadeFraction,
        fallback.caustics.edgeFadeFraction, 0.01f, 0.49f, adjusted);

    ClampFinite(volumetrics.resolutionScale,
        fallback.volumetrics.resolutionScale, 0.25f, 1.0f, adjusted);
    ClampUnsigned(volumetrics.sampleCount, 1u, 128u, adjusted);
    ClampUnsigned(volumetrics.atrousIterations, 0u, 6u, adjusted);
    ClampFinite(volumetrics.maximumDistanceMeters,
        fallback.volumetrics.maximumDistanceMeters, 0.1f, 10000.0f,
        adjusted);
    ClampFinite(volumetrics.historyWeight,
        fallback.volumetrics.historyWeight, 0.0f, 0.99f, adjusted);
    ClampFinite(volumetrics.depthRejectionMeters,
        fallback.volumetrics.depthRejectionMeters, 0.001f, 100.0f,
        adjusted);
    ClampFinite(volumetrics.surfaceHysteresisMeters,
        fallback.volumetrics.surfaceHysteresisMeters, 0.0f, 10.0f,
        adjusted);

    ClampFinite(distance.nearEndMeters, fallback.distance.nearEndMeters,
        1.0f, 99998.0f, adjusted);
    ClampFinite(distance.middleEndMeters,
        fallback.distance.middleEndMeters, 2.0f, 99999.0f, adjusted);
    ClampFinite(distance.farEndMeters, fallback.distance.farEndMeters,
        3.0f, 100000.0f, adjusted);
    if (distance.middleEndMeters <= distance.nearEndMeters)
    {
        distance.middleEndMeters = std::min(
            std::max(distance.nearEndMeters * 4.0f,
                distance.nearEndMeters + 1.0f), 99999.0f);
        adjusted = true;
    }
    if (distance.farEndMeters <= distance.middleEndMeters)
    {
        distance.farEndMeters = std::min(
            std::max(distance.middleEndMeters * 4.0f,
                distance.middleEndMeters + 1.0f), 100000.0f);
        adjusted = true;
    }
    ClampFinite(distance.transitionFraction,
        fallback.distance.transitionFraction, 0.01f, 0.49f, adjusted);

    if (message != nullptr)
    {
        *message = adjusted
            ? "One or more water-optics values were clamped to a valid range."
            : std::string{};
    }
    return !adjusted;
}

WaterOpticsDirtyScope ClassifyWaterOpticsDirtyScopes(
    const WaterOpticsSettings& previous,
    const WaterOpticsSettings& current) noexcept
{
    const bool materialChanged =
        Different(previous.material.absorption, current.material.absorption)
        || Different(previous.material.scattering,
            current.material.scattering)
        || Different(previous.material.roughness, current.material.roughness)
        || Different(previous.material.indexOfRefraction,
            current.material.indexOfRefraction)
        || Different(previous.material.phaseG, current.material.phaseG)
        || Different(previous.material.thinLayerStrength,
            current.material.thinLayerStrength)
        || Different(previous.material.backlitStrength,
            current.material.backlitStrength);
    const bool refractionChanged =
        previous.refraction.enabled != current.refraction.enabled
        || previous.refraction.highPrecision
            != current.refraction.highPrecision
        || previous.refraction.rayMarchSampleCount
            != current.refraction.rayMarchSampleCount
        || Different(previous.refraction.distortionStrength,
            current.refraction.distortionStrength)
        || Different(previous.refraction.maximumUvOffset,
            current.refraction.maximumUvOffset)
        || Different(previous.refraction.thicknessOffsetMeters,
            current.refraction.thicknessOffsetMeters)
        || Different(previous.refraction.maximumThicknessMeters,
            current.refraction.maximumThicknessMeters)
        || Different(previous.refraction.rayStepScale,
            current.refraction.rayStepScale);
    const bool causticsChanged =
        previous.caustics.enabled != current.caustics.enabled
        || previous.caustics.mode != current.caustics.mode
        || previous.caustics.resolution != current.caustics.resolution
        || Different(previous.caustics.nearCoverageMeters,
            current.caustics.nearCoverageMeters)
        || Different(previous.caustics.middleCoverageMeters,
            current.caustics.middleCoverageMeters)
        || Different(previous.caustics.intensity, current.caustics.intensity)
        || Different(previous.caustics.dispersion,
            current.caustics.dispersion)
        || Different(previous.caustics.edgeFadeFraction,
            current.caustics.edgeFadeFraction);
    const bool volumetricsChanged =
        previous.volumetrics.enabled != current.volumetrics.enabled
        || Different(previous.volumetrics.resolutionScale,
            current.volumetrics.resolutionScale)
        || previous.volumetrics.sampleCount
            != current.volumetrics.sampleCount
        || Different(previous.volumetrics.maximumDistanceMeters,
            current.volumetrics.maximumDistanceMeters)
        || Different(previous.volumetrics.historyWeight,
            current.volumetrics.historyWeight)
        || Different(previous.volumetrics.depthRejectionMeters,
            current.volumetrics.depthRejectionMeters)
        || previous.volumetrics.atrousIterations
            != current.volumetrics.atrousIterations
        || Different(previous.volumetrics.surfaceHysteresisMeters,
            current.volumetrics.surfaceHysteresisMeters);
    const bool distanceChanged =
        Different(previous.distance.nearEndMeters,
            current.distance.nearEndMeters)
        || Different(previous.distance.middleEndMeters,
            current.distance.middleEndMeters)
        || Different(previous.distance.farEndMeters,
            current.distance.farEndMeters)
        || Different(previous.distance.transitionFraction,
            current.distance.transitionFraction);
    const bool qualityChanged = previous.quality != current.quality;
    const bool resetChanged = previous.historyResetSerial
        != current.historyResetSerial;
    const bool debugChanged = previous.debugView != current.debugView;
    if (!materialChanged && !refractionChanged && !causticsChanged
        && !volumetricsChanged && !distanceChanged && !qualityChanged
        && !resetChanged && !debugChanged)
    {
        return WaterOpticsDirtyScope::None;
    }

    WaterOpticsDirtyScope scopes = WaterOpticsDirtyScope::Constants;
    if (qualityChanged
        || previous.caustics.resolution != current.caustics.resolution
        || Different(previous.volumetrics.resolutionScale,
            current.volumetrics.resolutionScale))
    {
        scopes = scopes | WaterOpticsDirtyScope::Resources;
    }
    if (qualityChanged || resetChanged || refractionChanged || materialChanged || distanceChanged
        || causticsChanged || volumetricsChanged)
    {
        scopes = scopes | WaterOpticsDirtyScope::History;
    }
    if (causticsChanged)
        scopes = scopes | WaterOpticsDirtyScope::Caustics;
    if (volumetricsChanged)
        scopes = scopes | WaterOpticsDirtyScope::Volumetrics;
    return scopes;
}
} // namespace Prism::Renderer
