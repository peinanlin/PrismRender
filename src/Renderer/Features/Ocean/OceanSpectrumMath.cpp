#include "Renderer/Features/Ocean/OceanSpectrumMath.h"

#include <algorithm>
#include <cstdint>
#include <cmath>

namespace Prism::Renderer
{
namespace
{
constexpr float Pi = 3.14159265358979323846f;

std::uint32_t Hash32(std::uint32_t state) noexcept
{
    state ^= state >> 16u;
    state *= 0x7feb352du;
    state ^= state >> 15u;
    state *= 0x846ca68bu;
    state ^= state >> 16u;
    return state;
}

float HashUnitFloat(const std::uint32_t coordinateX,
    const std::uint32_t coordinateY, const std::uint32_t cascadeIndex,
    const std::uint32_t seed, const std::uint32_t stream) noexcept
{
    std::uint32_t state = seed ^ (coordinateX * 0x9e3779b9u)
        ^ (coordinateY * 0x85ebca6bu)
        ^ (cascadeIndex * 0xc2b2ae35u)
        ^ (stream * 0x27d4eb2fu);
    state = Hash32(state);
    return static_cast<float>(state & 0x00ffffffu) / 16777216.0f;
}

DirectX::XMFLOAT2 DeterministicGaussianStream(
    const std::uint32_t coordinateX, const std::uint32_t coordinateY,
    const std::uint32_t cascadeIndex, const std::uint32_t seed,
    const std::uint32_t streamOffset) noexcept
{
    const float u1 = std::max(HashUnitFloat(coordinateX, coordinateY,
        cascadeIndex, seed, streamOffset), 1.0e-6f);
    const float u2 = HashUnitFloat(coordinateX, coordinateY,
        cascadeIndex, seed, streamOffset + 1u);
    const float radius = std::sqrt(-2.0f * std::log(u1));
    const float angle = 2.0f * Pi * u2;
    return {radius * std::cos(angle), radius * std::sin(angle)};
}

DirectX::XMFLOAT2 NormalizeDirection(const DirectX::XMFLOAT2& value,
    const DirectX::XMFLOAT2& fallback) noexcept
{
    const float length = std::sqrt(value.x * value.x + value.y * value.y);
    return length > 1.0e-6f && std::isfinite(length)
        ? DirectX::XMFLOAT2{value.x / length, value.y / length}
        : fallback;
}

float EvaluateJonswap(const float waveNumber, const float windSpeed,
    const float fetchMeters, const float peaking,
    const float cutoffLength, const float cutoffPower,
    const float amplitudeMultiplier, const float gravity)
{
    if (!(waveNumber > 0.0f) || !(windSpeed > 0.0f)
        || !(fetchMeters > 0.0f) || !(gravity > 0.0f)
        || !std::isfinite(waveNumber) || !std::isfinite(windSpeed)
        || !std::isfinite(fetchMeters))
    {
        return 0.0f;
    }

    const float angularFrequency = std::sqrt(gravity * waveNumber);
    const float peakFrequency = 22.0f * std::pow(gravity * gravity
        / (windSpeed * fetchMeters), 0.33f);
    if (!(angularFrequency > 0.0f) || !(peakFrequency > 0.0f))
    {
        return 0.0f;
    }

    const float sigma = angularFrequency <= peakFrequency ? 0.07f : 0.09f;
    const float frequencyDelta = (angularFrequency - peakFrequency)
        / (sigma * peakFrequency);
    const float peakBoost = std::max(1.0f, peaking);
    const float gammaTerm = std::pow(peakBoost,
        std::exp(-0.5f * frequencyDelta * frequencyDelta));
    const float inverseFrequency = peakFrequency / angularFrequency;
    const float base = 0.0081f * gravity * gravity
        / std::pow(angularFrequency, 5.0f)
        * std::exp(-1.25f * std::pow(inverseFrequency, 4.0f));

    float cutoff = 1.0f;
    if (cutoffLength > 0.0f && cutoffPower > 0.0f)
    {
        const float wavelength = 2.0f * Pi / waveNumber;
        cutoff = 1.0f - std::exp(-std::pow(
            std::max(0.0f, wavelength / cutoffLength), cutoffPower));
    }
    // WaveWorks defines this control as a wave-amplitude multiplier.  The
    // spectrum stores variance/energy and is square-rooted when h0 is built,
    // so square the control here to preserve its documented linear meaning.
    const float amplitudeScale = std::max(0.0f, amplitudeMultiplier);
    return std::max(0.0f, base * gammaTerm * cutoff
        * amplitudeScale * amplitudeScale);
}
}

float DeepWaterAngularFrequency(const float waveNumber, const float gravity)
{
    if (!(waveNumber > 0.0f) || !(gravity > 0.0f)
        || !std::isfinite(waveNumber) || !std::isfinite(gravity))
    {
        return 0.0f;
    }
    return std::sqrt(gravity * waveNumber);
}

float BeaufortToMetersPerSecond(const float beaufort) noexcept
{
    if (!std::isfinite(beaufort) || beaufort <= 0.0f)
    {
        return 0.0f;
    }
    // Standard continuous Beaufort relation: B = (v / 0.836)^(2/3).
    return 0.836f * std::pow(std::clamp(beaufort, 0.0f, 12.0f), 1.5f);
}

float JonswapPeakWavelength(const float windSpeed,
    const float fetchMeters, const float gravity) noexcept
{
    if (!(windSpeed > 0.0f) || !(fetchMeters > 0.0f)
        || !(gravity > 0.0f) || !std::isfinite(windSpeed)
        || !std::isfinite(fetchMeters) || !std::isfinite(gravity))
    {
        return 0.0f;
    }
    const float peakOmega = 22.0f * std::pow(
        gravity * gravity / (windSpeed * fetchMeters), 0.33f);
    const float peakWaveNumber = peakOmega * peakOmega / gravity;
    return peakWaveNumber > 0.0f
        ? 2.0f * Pi / peakWaveNumber
        : 0.0f;
}

float DirectionalSpreading(const float directionDot, const float dependency)
{
    const float clampedDot = std::clamp(directionDot, -1.0f, 1.0f);
    const float exponent = std::max(0.0f, dependency);
    return std::pow(std::max(0.0f, 0.5f * (clampedDot + 1.0f)), exponent);
}

namespace
{
float DirectionalNormalization(const float dependency) noexcept
{
    // Normalizes the cosine-power lobe over azimuth. This compact form is
    // exact at exponents 0 and 1 and remains close over WaveWorks' 0..1 range.
    return (std::max(0.0f, dependency) + 1.0f) / (2.0f * Pi);
}

float FrequencyToWaveNumberDensity(const OceanSpectrumSample& sample,
    const float waveNumber, const float dependency,
    const float gravity) noexcept
{
    if (!(sample.angularFrequency > 0.0f) || !(waveNumber > 0.0f))
    {
        return 0.0f;
    }

    // JONSWAP is an angular-frequency spectrum S(omega). A 2D FFT samples
    // wave-number area, so P(k,theta) = S(omega) * (d omega / d k) / k.
    // Omitting this Jacobian suppresses long waves and exaggerates capillary
    // detail, which made the previous result look static and noisy.
    const float jacobian = gravity
        / (2.0f * sample.angularFrequency * waveNumber);
    return std::max(0.0f, sample.energy) * jacobian
        * DirectionalNormalization(dependency);
}
}

float JonswapEnergy(const float waveNumber, const float windSpeed,
    const float fetchMeters, const float spectrumPeaking,
    const float smallWavesCutoffLength, const float smallWavesCutoffPower,
    const float amplitudeMultiplier, const float gravity)
{
    return EvaluateJonswap(waveNumber, windSpeed, fetchMeters,
        spectrumPeaking, smallWavesCutoffLength, smallWavesCutoffPower,
        amplitudeMultiplier, gravity);
}

OceanSpectrumSample EvaluateSpectrum(const float waveNumber,
    const float directionDot, const OceanWindSpectrumSettings& wind,
    const float gravity)
{
    OceanSpectrumSample sample{};
    sample.angularFrequency = DeepWaterAngularFrequency(waveNumber, gravity);
    sample.directionalWeight = DirectionalSpreading(directionDot,
        wind.dependency);
    sample.energy = JonswapEnergy(waveNumber, wind.speed,
        wind.fetchKilometers * 1000.0f, wind.spectrumPeaking,
        wind.smallWavesCutoffLength, wind.smallWavesCutoffPower,
        wind.amplitudeMultiplier, gravity) * sample.directionalWeight;
    return sample;
}

OceanSpectrumSample EvaluateSpectrum(const float waveNumber,
    const float directionDot, const OceanSwellSettings& swell,
    const float gravity)
{
    OceanSpectrumSample sample{};
    sample.angularFrequency = DeepWaterAngularFrequency(waveNumber, gravity);
    sample.directionalWeight = DirectionalSpreading(directionDot,
        swell.dependency);
    sample.energy = JonswapEnergy(waveNumber, swell.speed,
        swell.fetchKilometers * 1000.0f, swell.spectrumPeaking,
        swell.smallWavesCutoffLength, swell.smallWavesCutoffPower,
        swell.amplitudeMultiplier, gravity) * sample.directionalWeight;
    return sample;
}

DirectX::XMFLOAT2 DeterministicGaussian(
    const std::uint32_t coordinateX, const std::uint32_t coordinateY,
    const std::uint32_t cascadeIndex, const std::uint32_t seed) noexcept
{
    return DeterministicGaussianStream(coordinateX, coordinateY,
        cascadeIndex, seed, 0u);
}

OceanInitialSpectrumComponents EvaluateInitialSpectrumComponents(
    const OceanSettings& settings, const float patchLengthMeters,
    const float bandWeight, const std::uint32_t resolution,
    const std::uint32_t coordinateX, const std::uint32_t coordinateY,
    const std::uint32_t cascadeIndex, const std::uint32_t seed,
    const float gravity)
{
    OceanInitialSpectrumComponents result{};
    if (!(patchLengthMeters > 0.0f) || resolution == 0u
        || coordinateX >= resolution || coordinateY >= resolution
        || !std::isfinite(patchLengthMeters)
        || !std::isfinite(bandWeight))
    {
        return result;
    }

    const float centeredX = static_cast<float>(
        static_cast<std::int32_t>(coordinateX)
        - static_cast<std::int32_t>(resolution / 2u));
    const float centeredY = static_cast<float>(
        static_cast<std::int32_t>(coordinateY)
        - static_cast<std::int32_t>(resolution / 2u));
    const float deltaK = 2.0f * Pi / patchLengthMeters;
    const float waveX = deltaK * centeredX;
    const float waveY = deltaK * centeredY;
    const float waveNumber = std::sqrt(waveX * waveX + waveY * waveY);
    if (!(waveNumber > 1.0e-6f))
    {
        return result;
    }

    const float inverseWaveNumber = 1.0f / waveNumber;
    const DirectX::XMFLOAT2 baseDirection = NormalizeDirection(
        settings.baseWind.direction, {1.0f, 0.0f});
    const DirectX::XMFLOAT2 swellDirection = NormalizeDirection(
        settings.swell.direction, {0.0f, 1.0f});
    const float baseDot = (waveX * baseDirection.x
        + waveY * baseDirection.y) * inverseWaveNumber;
    const float swellDot = (waveX * swellDirection.x
        + waveY * swellDirection.y) * inverseWaveNumber;
    OceanWindSpectrumSettings effectiveBaseWind = settings.baseWind;
    if (settings.useBeaufortScale)
    {
        effectiveBaseWind.speed = BeaufortToMetersPerSecond(
            settings.baseWind.speed);
    }
    const OceanSpectrumSample base = EvaluateSpectrum(
        waveNumber, baseDot, effectiveBaseWind, gravity);
    const OceanSpectrumSample swell = EvaluateSpectrum(
        waveNumber, swellDot, settings.swell, gravity);
    const float clampedBandWeight = std::clamp(bandWeight, 0.0f, 1.0f);
    result.baseEnergy = FrequencyToWaveNumberDensity(base, waveNumber,
        effectiveBaseWind.dependency, gravity) * clampedBandWeight;
    result.swellEnergy = FrequencyToWaveNumberDensity(swell, waveNumber,
        settings.swell.dependency, gravity) * clampedBandWeight;
    result.angularFrequency = DeepWaterAngularFrequency(waveNumber, gravity);

    const float coefficientScale = static_cast<float>(resolution)
        * static_cast<float>(resolution) * deltaK;
    const float baseAmplitude = std::sqrt(result.baseEnergy * 0.5f)
        * coefficientScale;
    const float swellAmplitude = std::sqrt(result.swellEnergy * 0.5f)
        * coefficientScale;
    const DirectX::XMFLOAT2 baseGaussian = DeterministicGaussianStream(
        coordinateX, coordinateY, cascadeIndex, seed, 0u);
    const DirectX::XMFLOAT2 swellGaussian = DeterministicGaussianStream(
        coordinateX, coordinateY, cascadeIndex, seed, 2u);
    result.baseH0 = {baseGaussian.x * baseAmplitude,
        baseGaussian.y * baseAmplitude};
    result.swellH0 = {swellGaussian.x * swellAmplitude,
        swellGaussian.y * swellAmplitude};
    if (!std::isfinite(result.baseH0.x)
        || !std::isfinite(result.baseH0.y)
        || !std::isfinite(result.swellH0.x)
        || !std::isfinite(result.swellH0.y)
        || !std::isfinite(result.baseEnergy)
        || !std::isfinite(result.swellEnergy)
        || !std::isfinite(result.angularFrequency))
    {
        return {};
    }
    return result;
}

OceanInitialSpectrumSample EvaluateInitialSpectrumTexel(
    const OceanSettings& settings, const float patchLengthMeters,
    const float bandWeight, const std::uint32_t resolution,
    const std::uint32_t coordinateX, const std::uint32_t coordinateY,
    const std::uint32_t cascadeIndex, const std::uint32_t seed,
    const float gravity)
{
    OceanInitialSpectrumSample result{};
    const OceanInitialSpectrumComponents components =
        EvaluateInitialSpectrumComponents(settings, patchLengthMeters,
            bandWeight, resolution, coordinateX, coordinateY,
            cascadeIndex, seed, gravity);
    result.real = components.baseH0.x + components.swellH0.x;
    result.imaginary = components.baseH0.y + components.swellH0.y;
    result.energy = components.baseEnergy + components.swellEnergy;
    result.angularFrequency = components.angularFrequency;
    if (!std::isfinite(result.real) || !std::isfinite(result.imaginary)
        || !std::isfinite(result.energy)
        || !std::isfinite(result.angularFrequency))
    {
        return {};
    }
    return result;
}
} // namespace Prism::Renderer
