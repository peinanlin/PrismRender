#include "Renderer/Features/Ocean/OceanSettings.h"

#include "RHI/DeviceCapabilities.h"

#include <algorithm>
#include <cmath>
#include <sstream>

namespace Prism::Renderer
{
bool ShouldUseOceanTessellation(
    const OceanSettings& settings,
    const Prism::RHI::GraphicsDeviceCapabilities& capabilities) noexcept
{
    return settings.geometry.preferTessellation
        && capabilities.features.tessellationShader
        && capabilities.features.patchListTopology
        && capabilities.limits.maxTessellationControlPoints >= 3u;
}

OceanRuntimeFlags BuildOceanRuntimeFlags(
    const OceanSettings& settings) noexcept
{
    const OceanDebugSettings& debug = settings.debug;
    const auto effectiveMask = [&debug](
        const bool enabled, const std::uint32_t stageMask)
    {
        return enabled
            ? (debug.cascadeMask & stageMask
                & OceanDebugSettings::AllCascadeMask)
            : 0u;
    };
    const auto bit = [](const bool enabled, const std::uint32_t index)
    {
        return enabled ? (1u << index) : 0u;
    };

    OceanRuntimeFlags flags{};
    flags.primary =
        bit(settings.shading.useMicrofacetFresnel,
            OceanRuntimeFlagLayout::ShadingFresnelBit)
        | bit(settings.shading.useMicrofacetSpecular,
            OceanRuntimeFlagLayout::ShadingSpecularBit)
        | bit(settings.shading.useMicrofacetReflection,
            OceanRuntimeFlagLayout::ShadingReflectionBit)
        | bit(settings.shading.useAtmosphere,
            OceanRuntimeFlagLayout::ShadingAtmosphereBit)
        | (effectiveMask(debug.spectralDisplacementEnabled,
               debug.displacementCascadeMask)
            << OceanRuntimeFlagLayout::DisplacementCascadeFirstBit)
        | (effectiveMask(debug.spectralGradientEnabled,
               debug.gradientCascadeMask)
            << OceanRuntimeFlagLayout::GradientCascadeFirstBit);

    flags.secondary =
        bit(debug.showCascades,
            OceanRuntimeFlagLayout::DebugCascadesBit)
        | bit(debug.showFoamEnergy,
            OceanRuntimeFlagLayout::DebugFoamEnergyBit)
        | bit(debug.showSlopeMoments,
            OceanRuntimeFlagLayout::DebugSlopeMomentsBit)
        | bit(debug.showGeometryLod,
            OceanRuntimeFlagLayout::DebugGeometryLodBit)
        | bit(debug.showNormals,
            OceanRuntimeFlagLayout::DebugNormalsBit)
        | (effectiveMask(debug.spectralFoldingEnabled,
               debug.foldingCascadeMask)
            << OceanRuntimeFlagLayout::FoldingCascadeFirstBit)
        | (effectiveMask(debug.spectralFoamHistoryEnabled,
               debug.foamHistoryCascadeMask)
            << OceanRuntimeFlagLayout::FoamHistoryCascadeFirstBit)
        | (effectiveMask(debug.spectralMomentsEnabled,
               debug.momentsCascadeMask)
            << OceanRuntimeFlagLayout::MomentsCascadeFirstBit);
    return flags;
}
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
    adjusted = adjusted || (clamped != value);
    value = clamped;
}

void NormalizeDirection(DirectX::XMFLOAT2& direction,
    const DirectX::XMFLOAT2 fallback, bool& adjusted)
{
    if (!std::isfinite(direction.x) || !std::isfinite(direction.y))
    {
        direction = fallback;
        adjusted = true;
        return;
    }
    const float length = std::sqrt(
        direction.x * direction.x + direction.y * direction.y);
    if (!std::isfinite(length) || length < 1.0e-5f)
    {
        direction = fallback;
        adjusted = true;
        return;
    }
    direction.x /= length;
    direction.y /= length;
}

void ClampFoamSettings(OceanFoamSettings& foam,
    const OceanFoamSettings& fallback, bool& adjusted)
{
    ClampFinite(foam.whitecapsThreshold, fallback.whitecapsThreshold,
        0.0f, 1.0f, adjusted);
    ClampFinite(foam.generationThreshold, fallback.generationThreshold,
        0.0f, 1.0f, adjusted);
    ClampFinite(foam.generationAmount, fallback.generationAmount,
        0.0f, 1.0f, adjusted);
    ClampFinite(foam.dissipationSpeed, fallback.dissipationSpeed,
        0.0f, 10.0f, adjusted);
    ClampFinite(foam.falloffSpeed, fallback.falloffSpeed,
        0.0f, 1.0f, adjusted);
}
}

OceanSettings OceanSettings::WaveWorksReference()
{
    OceanSettings settings{};
    settings.implementation = OceanImplementation::SpectralOcean;
    settings.opticsModel = OceanOpticsModel::Current;
    settings.quality = OceanSimulationQuality::Extreme;
    settings.simulationApi = OceanSimulationApi::Compute;
    settings.simulationPeriodMeters = 1000.0f;
    settings.timeScale = 1.0f;
    settings.lateralMultiplier = 1.0f;
    settings.useBeaufortScale = true;
    settings.cameraFollowEnabled = true;

    // User-authored WaveWorks comparison preset. Keep the direction vector
    // normalized so the UI round-trips to the photographed 31.9 degrees.
    settings.baseWind.direction = {0.8489717f, 0.5284383f};
    settings.baseWind.speed = 9.9f;
    settings.baseWind.fetchKilometers = 0.001f;
    settings.baseWind.dependency = 1.0f;
    settings.baseWind.spectrumPeaking = 13.664f;
    settings.baseWind.smallWavesCutoffLength = 5.10f;
    settings.baseWind.smallWavesCutoffPower = 0.248f;
    settings.baseWind.amplitudeMultiplier = 2.058f;

    settings.swell.direction = {0.0f, 1.0f};
    settings.swell.speed = 29.62f;
    settings.swell.fetchKilometers = 9.502f;
    settings.swell.dependency = 1.0f;
    settings.swell.spectrumPeaking = 12.275f;
    settings.swell.smallWavesCutoffLength = 7.23f;
    settings.swell.smallWavesCutoffPower = 0.007f;
    settings.swell.amplitudeMultiplier = 0.477f;

    settings.foam = {0.5f, 0.37f, 0.12f, 0.6f, 0.985f};
    settings.local = {};
    // The authored reference camera looks approximately along +X from
    // (-30, 36). Center the invisible hull orbit in front of that camera so
    // the wake travels from foreground to background and remains observable.
    settings.local.domainCenter = {20.0f, 39.0f};
    settings.local.foam = {0.3f, 0.6f, 0.2f, 0.3f, 0.6f};
    settings.geometry = {};
    settings.shading = {};
    // Keep the WaveWorks Lab optical preset explicit so a preset reset and a
    // freshly constructed setting use the same readable daylight balance.
    settings.shading.waterColorIntensity = {0.04f, 0.04f, 0.025f, 0.38f};
    settings.shading.deepWaterColor = {0.0f, 0.2f, 0.4f};
    settings.shading.scatteringColor = {
        128.0f / 255.0f,
        160.0f / 255.0f,
        180.0f / 255.0f};
    // Wet sea foam is blue-grey rather than display white. Newly breaking
    // crests still brighten through the layered optical response, while old
    // wake history fades toward the scattering color.
    settings.shading.foamColor = {
        184.0f / 255.0f,
        199.0f / 255.0f,
        194.0f / 255.0f};
    settings.shading.underwaterFoamColor = {0.6f, 0.6f, 0.6f};
    settings.optics = WaterOpticsSettings::HpWaterReference();
    settings.query = {};
    settings.query.readbackFifoEntries = 30u;
    settings.debug = {};
    return settings;
}

OceanSettings OceanSettings::HpWaterReference()
{
    OceanSettings settings = WaveWorksReference();
    settings.opticsModel = OceanOpticsModel::HpWater;
    settings.optics = WaterOpticsSettings::HpWaterReference();
    return settings;
}

bool OceanSettings::ValidateAndNormalize(std::string* message)
{
    bool adjusted = false;
    const auto clampCascadeMask = [&adjusted](std::uint32_t& mask)
    {
        const std::uint32_t clamped =
            mask & OceanDebugSettings::AllCascadeMask;
        adjusted = adjusted || clamped != mask;
        mask = clamped;
    };
    clampCascadeMask(debug.cascadeMask);
    clampCascadeMask(debug.displacementCascadeMask);
    clampCascadeMask(debug.gradientCascadeMask);
    clampCascadeMask(debug.foldingCascadeMask);
    clampCascadeMask(debug.foamHistoryCascadeMask);
    clampCascadeMask(debug.momentsCascadeMask);
    if (implementation > OceanImplementation::SpectralOcean)
    {
        implementation = OceanImplementation::LegacyFft;
        adjusted = true;
    }
    if (opticsModel > OceanOpticsModel::HpWater)
    {
        opticsModel = OceanOpticsModel::Current;
        adjusted = true;
    }
    NormalizeDirection(baseWind.direction, {1.0f, 0.0f}, adjusted);
    NormalizeDirection(swell.direction, {0.0f, 1.0f}, adjusted);
    ClampFinite(simulationPeriodMeters, 1000.0f, 1.0f, 1000000.0f, adjusted);
    ClampFinite(timeScale, 1.0f, 0.0f, 100.0f, adjusted);
    ClampFinite(lateralMultiplier, 1.0f, 0.0f, 10.0f, adjusted);
    ClampFinite(baseWind.speed, 4.7f, 0.0f, 200.0f, adjusted);
    ClampFinite(baseWind.fetchKilometers, 0.1f, 0.001f, 100000.0f, adjusted);
    ClampFinite(baseWind.dependency, 1.0f, 0.0f, 2.0f, adjusted);
    ClampFinite(baseWind.spectrumPeaking, 3.3f, 1.0f, 20.0f, adjusted);
    ClampFinite(baseWind.smallWavesCutoffLength, 0.0f, 0.0f, 10000.0f, adjusted);
    ClampFinite(baseWind.smallWavesCutoffPower, 0.0f, 0.0f, 8.0f, adjusted);
    ClampFinite(baseWind.amplitudeMultiplier, 1.0f, 0.0f, 10.0f, adjusted);
    ClampFinite(swell.speed, 1.5f, 0.0f, 200.0f, adjusted);
    ClampFinite(swell.fetchKilometers, 520.0f, 0.001f, 100000.0f, adjusted);
    ClampFinite(swell.dependency, 1.0f, 0.0f, 2.0f, adjusted);
    ClampFinite(swell.spectrumPeaking, 10.0f, 1.0f, 20.0f, adjusted);
    ClampFinite(swell.smallWavesCutoffLength, 60.0f, 0.0f, 10000.0f, adjusted);
    ClampFinite(swell.smallWavesCutoffPower, 1.0f, 0.0f, 8.0f, adjusted);
    ClampFinite(swell.amplitudeMultiplier, 1.0f, 0.0f, 10.0f, adjusted);
    ClampFoamSettings(foam,
        {0.5f, 0.37f, 0.12f, 0.6f, 0.985f}, adjusted);
    ClampFoamSettings(local.foam,
        {0.3f, 0.6f, 0.2f, 0.3f, 0.6f}, adjusted);
    ClampFinite(local.domainSizeMeters, 200.0f, 1.0f, 100000.0f, adjusted);
    ClampFinite(local.amplitudeMultiplier, 1.0f, 0.0f, 10.0f, adjusted);
    ClampFinite(local.lateralMultiplier, 1.0f, 0.0f, 10.0f, adjusted);
    ClampFinite(local.rainRatePerSecond, 0.35f, 0.0f, 30.0f, adjusted);
    ClampFinite(local.wakeSpeedMetersPerSecond, 12.0f, 0.0f, 200.0f, adjusted);
    NormalizeDirection(local.manualVelocityDirection, {1.0f, 0.0f}, adjusted);
    ClampFinite(local.manualRadiusMeters, 5.0f, 0.01f, 10000.0f, adjusted);
    ClampFinite(local.manualStrength, 0.35f, -100.0f, 100.0f, adjusted);
    ClampFinite(geometry.minimumPatchLength, 5.0f, 0.01f, 100000.0f, adjusted);
    ClampFinite(geometry.maximumEdgeLengthPixels, 10.0f, 0.25f, 512.0f, adjusted);
    ClampFinite(geometry.meanSeaLevel, 0.0f, -100000.0f, 100000.0f, adjusted);
    ClampFinite(geometry.geomorphingDegree, 1.0f, 0.0f, 1.0f, adjusted);
    ClampFinite(shading.sunAngleDegrees, 20.0f, 0.0f, 90.0f, adjusted);
    ClampFinite(shading.sunIntensity, 1.0f, 0.0f, 100.0f, adjusted);
    ClampFinite(shading.beckmannRoughness, 0.00001f, 1.0e-7f, 1.0f, adjusted);
    ClampFinite(shading.uvWarpingAmplitude, 0.03f, 0.0f, 1.0f, adjusted);
    ClampFinite(shading.uvWarpingFrequency, 2.0f, 0.0f, 32.0f, adjusted);
    adjusted = !optics.ValidateAndNormalize(nullptr) || adjusted;

    if (geometry.cellsPerPatch < 1u)
    {
        geometry.cellsPerPatch = 64u;
        adjusted = true;
    }
    geometry.cellsPerPatch = std::min(geometry.cellsPerPatch, 1024u);
    if (geometry.maximumLod > 31u)
    {
        geometry.maximumLod = 31u;
        adjusted = true;
    }
    if (local.gridSize != 128u && local.gridSize != 256u
        && local.gridSize != 512u && local.gridSize != 1024u
        && local.gridSize != 2048u)
    {
        local.gridSize = 512u;
        adjusted = true;
    }

    if (message != nullptr)
    {
        *message = adjusted
            ? "One or more ocean values were clamped to a valid range."
            : std::string{};
    }
    return !adjusted;
}

std::uint32_t OceanSettings::ResolutionForQuality(
    const OceanSimulationQuality quality)
{
    switch (quality)
    {
    case OceanSimulationQuality::Normal:
        return 128u;
    case OceanSimulationQuality::High:
        return 256u;
    case OceanSimulationQuality::Extreme:
    default:
        return 512u;
    }
}

std::array<float, 4> OceanSettings::ReferenceCascadePatchLengths()
{
    return {15.625f, 62.5f, 250.0f, 1000.0f};
}

OceanDirtyScope ClassifyOceanDirtyScopes(
    const OceanSettings& previous,
    const OceanSettings& current) noexcept
{
    const auto different = [](const float lhs, const float rhs)
    {
        return std::abs(lhs - rhs) > 1.0e-6f;
    };
    const auto different2 = [&](const DirectX::XMFLOAT2& lhs,
        const DirectX::XMFLOAT2& rhs)
    {
        return different(lhs.x, rhs.x) || different(lhs.y, rhs.y);
    };
    const bool spectrumDifference =
        previous.quality != current.quality
        || different(previous.simulationPeriodMeters,
            current.simulationPeriodMeters)
        || previous.useBeaufortScale != current.useBeaufortScale
        || different2(previous.baseWind.direction, current.baseWind.direction)
        || different(previous.baseWind.speed, current.baseWind.speed)
        || different(previous.baseWind.fetchKilometers,
            current.baseWind.fetchKilometers)
        || different(previous.baseWind.dependency, current.baseWind.dependency)
        || different(previous.baseWind.spectrumPeaking,
            current.baseWind.spectrumPeaking)
        || different(previous.baseWind.smallWavesCutoffLength,
            current.baseWind.smallWavesCutoffLength)
        || different(previous.baseWind.smallWavesCutoffPower,
            current.baseWind.smallWavesCutoffPower)
        || different(previous.baseWind.amplitudeMultiplier,
            current.baseWind.amplitudeMultiplier)
        || different2(previous.swell.direction, current.swell.direction)
        || different(previous.swell.speed, current.swell.speed)
        || different(previous.swell.fetchKilometers,
            current.swell.fetchKilometers)
        || different(previous.swell.dependency, current.swell.dependency)
        || different(previous.swell.spectrumPeaking,
            current.swell.spectrumPeaking)
        || different(previous.swell.smallWavesCutoffLength,
            current.swell.smallWavesCutoffLength)
        || different(previous.swell.smallWavesCutoffPower,
            current.swell.smallWavesCutoffPower)
        || different(previous.swell.amplitudeMultiplier,
            current.swell.amplitudeMultiplier);
    const bool opticalDifference =
        previous.opticsModel != current.opticsModel
        || ClassifyWaterOpticsDirtyScopes(previous.optics, current.optics)
            != WaterOpticsDirtyScope::None;
    bool any = previous.implementation != current.implementation
        || opticalDifference
        || previous.quality != current.quality
        || previous.simulationApi != current.simulationApi
        || different(previous.simulationPeriodMeters,
            current.simulationPeriodMeters)
        || different(previous.timeScale, current.timeScale)
        || different(previous.lateralMultiplier, current.lateralMultiplier)
        || previous.useBeaufortScale != current.useBeaufortScale
        || previous.cameraFollowEnabled != current.cameraFollowEnabled
        || previous.asyncComputeEnabled != current.asyncComputeEnabled
        || spectrumDifference;
    const bool scopedDifference =
        previous.foam.whitecapsThreshold != current.foam.whitecapsThreshold
        || previous.foam.generationThreshold
            != current.foam.generationThreshold
        || previous.foam.generationAmount != current.foam.generationAmount
        || previous.foam.dissipationSpeed != current.foam.dissipationSpeed
        || previous.foam.falloffSpeed != current.foam.falloffSpeed
        || previous.local.enabled != current.local.enabled
        || previous.local.gridSize != current.local.gridSize
        || different(previous.local.domainSizeMeters,
            current.local.domainSizeMeters)
        || different2(previous.local.domainCenter,
            current.local.domainCenter)
        || different(previous.local.amplitudeMultiplier,
            current.local.amplitudeMultiplier)
        || different(previous.local.lateralMultiplier,
            current.local.lateralMultiplier)
        || different2(previous.local.manualPosition,
            current.local.manualPosition)
        || different2(previous.local.manualVelocityDirection,
            current.local.manualVelocityDirection)
        || different(previous.local.manualRadiusMeters,
            current.local.manualRadiusMeters)
        || different(previous.local.manualStrength,
            current.local.manualStrength)
        || previous.local.manualDisturbanceSerial
            != current.local.manualDisturbanceSerial
        || previous.local.foam.whitecapsThreshold
            != current.local.foam.whitecapsThreshold
        || previous.local.foam.generationThreshold
            != current.local.foam.generationThreshold
        || previous.local.foam.generationAmount
            != current.local.foam.generationAmount
        || previous.local.foam.dissipationSpeed
            != current.local.foam.dissipationSpeed
        || previous.local.foam.falloffSpeed
            != current.local.foam.falloffSpeed
        || previous.geometry.cellsPerPatch != current.geometry.cellsPerPatch
        || different(previous.geometry.minimumPatchLength,
            current.geometry.minimumPatchLength)
        || different(previous.geometry.maximumEdgeLengthPixels,
            current.geometry.maximumEdgeLengthPixels)
        || different(previous.geometry.meanSeaLevel,
            current.geometry.meanSeaLevel)
        || previous.geometry.maximumLod != current.geometry.maximumLod
        || different(previous.geometry.geomorphingDegree,
            current.geometry.geomorphingDegree)
        || previous.geometry.generateDiamondPattern
            != current.geometry.generateDiamondPattern
        || previous.geometry.preferTessellation
            != current.geometry.preferTessellation
        || previous.shading.useAtmosphere != current.shading.useAtmosphere
        || previous.shading.useMicrofacetFresnel
            != current.shading.useMicrofacetFresnel
        || previous.shading.useMicrofacetSpecular
            != current.shading.useMicrofacetSpecular
        || previous.shading.useMicrofacetReflection
            != current.shading.useMicrofacetReflection
        || different(previous.shading.sunAngleDegrees,
            current.shading.sunAngleDegrees)
        || different(previous.shading.sunIntensity,
            current.shading.sunIntensity)
        || different(previous.shading.beckmannRoughness,
            current.shading.beckmannRoughness)
        || different(previous.shading.uvWarpingAmplitude,
            current.shading.uvWarpingAmplitude)
        || different(previous.shading.uvWarpingFrequency,
            current.shading.uvWarpingFrequency)
        || previous.shading.deepWaterColor.x != current.shading.deepWaterColor.x
        || previous.shading.deepWaterColor.y != current.shading.deepWaterColor.y
        || previous.shading.deepWaterColor.z != current.shading.deepWaterColor.z
        || previous.shading.scatteringColor.x != current.shading.scatteringColor.x
        || previous.shading.scatteringColor.y != current.shading.scatteringColor.y
        || previous.shading.scatteringColor.z != current.shading.scatteringColor.z
        || previous.shading.waterColorIntensity.x
            != current.shading.waterColorIntensity.x
        || previous.shading.waterColorIntensity.y
            != current.shading.waterColorIntensity.y
        || previous.shading.waterColorIntensity.z
            != current.shading.waterColorIntensity.z
        || previous.shading.waterColorIntensity.w
            != current.shading.waterColorIntensity.w
        || previous.shading.foamColor.x != current.shading.foamColor.x
        || previous.shading.foamColor.y != current.shading.foamColor.y
        || previous.shading.foamColor.z != current.shading.foamColor.z
        || previous.shading.underwaterFoamColor.x
            != current.shading.underwaterFoamColor.x
        || previous.shading.underwaterFoamColor.y
            != current.shading.underwaterFoamColor.y
        || previous.shading.underwaterFoamColor.z
            != current.shading.underwaterFoamColor.z;
    if (!any && !scopedDifference)
    {
        return OceanDirtyScope::None;
    }

    OceanDirtyScope scopes = OceanDirtyScope::Constants;
    if (spectrumDifference)
    {
        scopes = scopes | OceanDirtyScope::InitialSpectrum;
    }
    if (previous.quality != current.quality
        || previous.local.gridSize != current.local.gridSize
        || different(previous.local.domainSizeMeters,
            current.local.domainSizeMeters))
    {
        scopes = scopes | OceanDirtyScope::Resources;
    }
    if (previous.geometry.cellsPerPatch != current.geometry.cellsPerPatch
        || different(previous.geometry.minimumPatchLength,
            current.geometry.minimumPatchLength)
        || different(previous.geometry.maximumEdgeLengthPixels,
            current.geometry.maximumEdgeLengthPixels)
        || previous.geometry.maximumLod != current.geometry.maximumLod)
    {
        scopes = scopes | OceanDirtyScope::Geometry;
    }
    if (previous.local.enabled != current.local.enabled
        || previous.local.gridSize != current.local.gridSize
        || different(previous.local.domainSizeMeters,
            current.local.domainSizeMeters)
        || different2(previous.local.domainCenter,
            current.local.domainCenter)
        || previous.local.foam.generationAmount
            != current.local.foam.generationAmount
        || previous.local.foam.dissipationSpeed
            != current.local.foam.dissipationSpeed
        )
    {
        scopes = scopes | OceanDirtyScope::LocalReset;
    }
    if (previous.foam.whitecapsThreshold != current.foam.whitecapsThreshold
        || previous.foam.generationThreshold
            != current.foam.generationThreshold
        || previous.foam.generationAmount != current.foam.generationAmount
        || previous.foam.dissipationSpeed != current.foam.dissipationSpeed
        || previous.foam.falloffSpeed != current.foam.falloffSpeed)
    {
        scopes = scopes | OceanDirtyScope::HistoryReset;
    }
    return scopes;
}

OceanClipmapPlacement ComputeOceanClipmapPlacement(
    const double cameraX,
    const double cameraZ,
    const float patchLength,
    const std::uint32_t resolution,
    const bool cameraFollowEnabled) noexcept
{
    OceanClipmapPlacement placement{};
    placement.snapInterval = std::max(
        static_cast<double>(patchLength)
            / static_cast<double>(std::max(resolution, 1u)),
        0.25);
    if (cameraFollowEnabled)
    {
        placement.snappedX = std::round(cameraX / placement.snapInterval)
            * placement.snapInterval;
        placement.snappedZ = std::round(cameraZ / placement.snapInterval)
            * placement.snapInterval;
    }
    return placement;
}
} // namespace Prism::Renderer
