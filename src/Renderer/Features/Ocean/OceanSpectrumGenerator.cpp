#include "Renderer/Features/Ocean/OceanSpectrumGenerator.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdint>
#include <limits>

namespace Prism::Renderer
{
namespace
{
float SmoothStep(const float value)
{
    const float t = std::clamp(value, 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

void HashValue(std::uint64_t& hash, const std::uint32_t value) noexcept
{
    hash ^= value;
    hash *= 1099511628211ull;
}

void HashFloat(std::uint64_t& hash, const float value) noexcept
{
    HashValue(hash, std::bit_cast<std::uint32_t>(value));
}
}

void OceanSpectrumGenerator::Reset() noexcept
{
    m_cascades = {};
    m_spectrumKey = 0u;
    m_randomSeed = 0u;
    m_configured = false;
    m_spectrumDirty = true;
}

std::uint64_t OceanSpectrumGenerator::BuildSpectrumKey(
    const OceanSettings& settings, const std::uint32_t randomSeed) noexcept
{
    std::uint64_t hash = 1469598103934665603ull;
    HashValue(hash, randomSeed);
    HashValue(hash, static_cast<std::uint32_t>(settings.quality));
    HashFloat(hash, settings.simulationPeriodMeters);
    const auto hashDirection = [&hash](const DirectX::XMFLOAT2& direction)
    {
        HashFloat(hash, direction.x);
        HashFloat(hash, direction.y);
    };
    hashDirection(settings.baseWind.direction);
    HashFloat(hash, settings.baseWind.speed);
    HashFloat(hash, settings.baseWind.fetchKilometers);
    HashFloat(hash, settings.baseWind.spectrumPeaking);
    HashFloat(hash, settings.baseWind.amplitudeMultiplier);
    HashFloat(hash, settings.baseWind.smallWavesCutoffLength);
    HashFloat(hash, settings.baseWind.smallWavesCutoffPower);
    HashFloat(hash, settings.baseWind.dependency);
    hashDirection(settings.swell.direction);
    HashFloat(hash, settings.swell.speed);
    HashFloat(hash, settings.swell.fetchKilometers);
    HashFloat(hash, settings.swell.spectrumPeaking);
    HashFloat(hash, settings.swell.amplitudeMultiplier);
    HashFloat(hash, settings.swell.smallWavesCutoffLength);
    HashFloat(hash, settings.swell.smallWavesCutoffPower);
    HashFloat(hash, settings.swell.dependency);
    return hash;
}

bool OceanSpectrumGenerator::Configure(const OceanSettings& settings,
    const std::uint32_t randomSeed) noexcept
{
    const std::uint64_t nextKey = BuildSpectrumKey(settings, randomSeed);
    const bool changed = !m_configured || nextKey != m_spectrumKey;
    if (!changed)
    {
        return false;
    }
    const auto lengths = OceanSettings::ReferenceCascadePatchLengths();
    const std::uint32_t resolution =
        OceanSettings::ResolutionForQuality(settings.quality);
    for (std::size_t index = 0; index < m_cascades.size(); ++index)
    {
        const float referenceLength = lengths[index];
        const float patchLength = referenceLength
            * std::max(settings.simulationPeriodMeters, 1.0f) / 1000.0f;
        const float lower = NyquistTexelsPerWave * patchLength
            / static_cast<float>(resolution);
        const float upper = patchLength * FundamentalWavelengthMargin;
        // Distance fades are surface-sampling metadata, not spectral band
        // ownership. Fine bands fade once their texel footprint is smaller
        // than a useful screen-space contribution; the coarsest never fades.
        const float fadeStart = index + 1u == m_cascades.size()
            ? std::numeric_limits<float>::max()
            : 0.0f;
        const float fadeEnd = index + 1u == m_cascades.size()
            ? std::numeric_limits<float>::max()
            : patchLength * CascadeVisibilityPeriods;
        m_cascades[index] = {
            patchLength,
            lower,
            upper,
            1.0f / patchLength,
            {},
            fadeStart,
            fadeEnd,
            resolution};
    }
    m_spectrumKey = nextKey;
    m_randomSeed = randomSeed;
    m_configured = true;
    m_spectrumDirty = true;
    return true;
}

std::array<float, 4> OceanSpectrumGenerator::BandWeights(
    const float wavelengthMeters) const noexcept
{
    std::array<float, 4> weights{};
    if (!(wavelengthMeters > 0.0f)
        || !std::isfinite(wavelengthMeters))
    {
        return weights;
    }
    float total = 0.0f;
    for (std::size_t index = 0; index < weights.size(); ++index)
    {
        const auto& cascade = m_cascades[index];
        const float lower = cascade.wavelengthMinMeters;
        const float upper = cascade.wavelengthMaxMeters;
        if (!(upper > lower) || wavelengthMeters < lower
            || wavelengthMeters > upper)
        {
            continue;
        }
        const float logPosition = std::log(wavelengthMeters / lower)
            / std::max(std::log(upper / lower), 1.0e-6f);
        const float edgeFade = std::min(
            SmoothStep(logPosition * 2.0f),
            SmoothStep((1.0f - logPosition) * 2.0f));
        weights[index] = std::max(edgeFade, 1.0e-4f);
        total += weights[index];
    }
    if (total <= 0.0f)
    {
        std::size_t nearest = 0u;
        float nearestDistance = std::numeric_limits<float>::max();
        for (std::size_t index = 0; index < m_cascades.size(); ++index)
        {
            const float distance = std::abs(
                std::log(wavelengthMeters)
                - std::log(std::max(m_cascades[index].patchLengthMeters,
                    1.0e-6f)));
            if (distance < nearestDistance)
            {
                nearest = index;
                nearestDistance = distance;
            }
        }
        weights[nearest] = 1.0f;
        return weights;
    }
    for (float& weight : weights)
    {
        weight /= total;
    }
    return weights;
}

DirectX::XMFLOAT2 OceanSpectrumGenerator::ComputeCascadeUv(
    const DirectX::XMFLOAT2& undisplacedWorldPosition,
    const OceanCascadeDescription& cascade,
    const float warpAmplitude,
    const float warpFrequency) noexcept
{
    DirectX::XMFLOAT2 uv{
        undisplacedWorldPosition.x * cascade.uvScale + cascade.uvOffset.x,
        undisplacedWorldPosition.y * cascade.uvScale + cascade.uvOffset.y};
    if (!(warpAmplitude > 0.0f) || !(warpFrequency > 0.0f))
    {
        return uv;
    }
    // WaveWorks documents frequency in radians per cascade UV, not cycles.
    // The previous implementation multiplied it by 2*pi and normalized the
    // result to a constant-length direction field. That changed the public
    // parameter semantics and produced dense fingerprint-shaped distortions.
    // Evaluate both offsets from the original UV so CPU metadata and shader
    // sampling share the same separable, slowly varying warp.
    const float sourceX = uv.x;
    const float sourceY = uv.y;
    uv.x += warpAmplitude * std::cos(sourceY * warpFrequency);
    uv.y += warpAmplitude * std::sin(sourceX * warpFrequency);
    return uv;
}

float OceanSpectrumGenerator::DistanceFadeWeight(
    const float cameraDistance,
    const OceanCascadeDescription& cascade) noexcept
{
    if (!std::isfinite(cameraDistance)
        || cascade.distanceFadeStartMeters
            == std::numeric_limits<float>::max())
    {
        return 1.0f;
    }
    const float span = std::max(cascade.distanceFadeEndMeters
        - cascade.distanceFadeStartMeters, 1.0e-6f);
    return 1.0f - std::clamp(
        (cameraDistance - cascade.distanceFadeStartMeters) / span,
        0.0f,
        1.0f);
}
} // namespace Prism::Renderer
