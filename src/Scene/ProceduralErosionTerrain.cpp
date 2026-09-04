/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include "Scene/ProceduralErosionTerrain.h"

#include <algorithm>
#include <cmath>
#include <utility>

// The erosion procedure is derived from Rune Skovbo Johansen's Advanced
// Terrain Erosion Filter and its public MPL-2.0 reference implementations:
// https://blog.runevision.com/2026/03/fast-and-gorgeous-erosion-filter.html
// https://github.com/korbindeman/bevy_erosion_filter

namespace Prism::Scene
{
namespace
{
constexpr float Tau = 6.28318530717958647692f;
constexpr float MinimumLength = 1.0e-10f;

struct Float2
{
    float x = 0.0f;
    float y = 0.0f;
};

struct HeightSlope
{
    float height = 0.0f;
    Float2 slope{};
};

struct PhacelleSample
{
    float cosine = 0.0f;
    float sine = 0.0f;
    Float2 sideDirection{};
};

struct ErosionResult
{
    HeightSlope delta{};
    float magnitude = 0.0f;
    float ridgeMap = 0.0f;
};

Float2 operator+(const Float2 left, const Float2 right)
{
    return {left.x + right.x, left.y + right.y};
}

Float2 operator-(const Float2 left, const Float2 right)
{
    return {left.x - right.x, left.y - right.y};
}

Float2 operator*(const Float2 value, const float scale)
{
    return {value.x * scale, value.y * scale};
}

Float2 operator/(const Float2 value, const float scale)
{
    return {value.x / scale, value.y / scale};
}

Float2& operator+=(Float2& left, const Float2 right)
{
    left.x += right.x;
    left.y += right.y;
    return left;
}

HeightSlope operator+(const HeightSlope& left, const HeightSlope& right)
{
    return {
        left.height + right.height,
        left.slope + right.slope};
}

HeightSlope operator-(const HeightSlope& left, const HeightSlope& right)
{
    return {
        left.height - right.height,
        left.slope - right.slope};
}

HeightSlope operator*(const HeightSlope& value, const float scale)
{
    return {value.height * scale, value.slope * scale};
}

HeightSlope& operator+=(HeightSlope& left, const HeightSlope& right)
{
    left.height += right.height;
    left.slope += right.slope;
    return left;
}

float Dot(const Float2 left, const Float2 right)
{
    return left.x * right.x + left.y * right.y;
}

float Length(const Float2 value)
{
    return std::sqrt(Dot(value, value));
}

Float2 NormalizeSafe(const Float2 value)
{
    const float length = Length(value);
    return length > MinimumLength ? value / length : Float2{};
}

Float2 Lerp(const Float2 start, const Float2 end, const float amount)
{
    return start + (end - start) * amount;
}

HeightSlope Lerp(
    const HeightSlope& start,
    const HeightSlope& end,
    const float amount)
{
    return start + (end - start) * amount;
}

float Saturate(const float value)
{
    return std::clamp(value, 0.0f, 1.0f);
}

float Fract(const float value)
{
    return value - std::floor(value);
}

Float2 Floor(const Float2 value)
{
    return {std::floor(value.x), std::floor(value.y)};
}

Float2 Fract(const Float2 value)
{
    return {Fract(value.x), Fract(value.y)};
}

float Sign(const float value)
{
    return value > 0.0f ? 1.0f : (value < 0.0f ? -1.0f : 0.0f);
}

float SmoothStep(const float start, const float end, const float value)
{
    if (end <= start)
    {
        return value >= end ? 1.0f : 0.0f;
    }
    const float t = Saturate((value - start) / (end - start));
    return t * t * (3.0f - 2.0f * t);
}

float SmoothStepDerivative(
    const float start,
    const float end,
    const float value)
{
    if (end <= start || value <= start || value >= end)
    {
        return 0.0f;
    }
    const float t = (value - start) / (end - start);
    return 6.0f * t * (1.0f - t) / (end - start);
}

float PowInverse(const float value, const float power)
{
    return 1.0f - std::pow(1.0f - Saturate(value), power);
}

float EaseOut(const float value)
{
    const float inverse = 1.0f - Saturate(value);
    return 1.0f - inverse * inverse;
}

float SmoothStart(const float value, const float smoothing)
{
    if (smoothing <= MinimumLength)
    {
        return value;
    }
    if (value >= smoothing)
    {
        return value - 0.5f * smoothing;
    }
    return 0.5f * value * value / smoothing;
}

Float2 Hash2(const Float2 input)
{
    constexpr Float2 coefficients{0.3183099f, 0.3678794f};
    const Float2 value{
        input.x * coefficients.x + coefficients.y,
        input.y * coefficients.y + coefficients.x};
    const float scalar = Fract(value.x * value.y * (value.x + value.y));
    return {
        -1.0f + 2.0f * Fract(16.0f * coefficients.x * scalar),
        -1.0f + 2.0f * Fract(16.0f * coefficients.y * scalar)};
}

HeightSlope ValueNoise(const Float2 position)
{
    const Float2 base = Floor(position);
    const Float2 fraction = Fract(position);
    const Float2 smooth{
        fraction.x * fraction.x * (3.0f - 2.0f * fraction.x),
        fraction.y * fraction.y * (3.0f - 2.0f * fraction.y)};
    const Float2 smoothDerivative{
        6.0f * fraction.x * (1.0f - fraction.x),
        6.0f * fraction.y * (1.0f - fraction.y)};

    const float h00 = Hash2(base).x;
    const float h10 = Hash2(base + Float2{1.0f, 0.0f}).x;
    const float h01 = Hash2(base + Float2{0.0f, 1.0f}).x;
    const float h11 = Hash2(base + Float2{1.0f, 1.0f}).x;
    const float row0 = std::lerp(h00, h10, smooth.x);
    const float row1 = std::lerp(h01, h11, smooth.x);
    return {
        std::lerp(row0, row1, smooth.y),
        {
            std::lerp(h10 - h00, h11 - h01, smooth.y)
                * smoothDerivative.x,
            (row1 - row0) * smoothDerivative.y}};
}

HeightSlope FractalNoise(
    const Float2 position,
    const std::uint32_t octaveCount = 6)
{
    HeightSlope result{};
    float amplitude = 0.5f;
    float frequency = 1.0f;
    for (std::uint32_t octave = 0; octave < octaveCount; ++octave)
    {
        HeightSlope noise = ValueNoise(position * frequency);
        noise.slope = noise.slope * frequency;
        result += noise * amplitude;
        frequency *= 2.03f;
        amplitude *= 0.51f;
    }
    return result;
}

HeightSlope ScaleInputDerivative(
    HeightSlope sample,
    const Float2 inputScale)
{
    sample.slope.x *= inputScale.x;
    sample.slope.y *= inputScale.y;
    return sample;
}

HeightSlope GaussianMountain(
    const Float2 normalizedPosition,
    const Float2 center,
    const float sharpness)
{
    const Float2 offset = normalizedPosition - center;
    const float height = std::exp(-Dot(offset, offset) * sharpness);
    return {
        height,
        {
            height * -2.0f * sharpness * offset.x,
            height * -2.0f * sharpness * offset.y}};
}

PhacelleSample SamplePhacelle(
    const Float2 position,
    const Float2 normalizedDirection,
    const float frequency,
    const float offsetCycles,
    const float normalization)
{
    const Float2 sideDirection{
        -normalizedDirection.y * frequency * Tau,
        normalizedDirection.x * frequency * Tau};
    const float phaseOffset = offsetCycles * Tau;
    const Float2 integerPosition = Floor(position);
    const Float2 fractionalPosition = Fract(position);
    Float2 phaseDirection{};
    float weightSum = 0.0f;

    for (std::int32_t offsetX = -1; offsetX <= 2; ++offsetX)
    {
        for (std::int32_t offsetY = -1; offsetY <= 2; ++offsetY)
        {
            const Float2 gridOffset{
                static_cast<float>(offsetX),
                static_cast<float>(offsetY)};
            const Float2 gridPoint = integerPosition + gridOffset;
            const Float2 randomOffset = Hash2(gridPoint) * 0.5f;
            const Float2 distance =
                fractionalPosition - gridOffset - randomOffset;
            const float weight = std::max(
                std::exp(-Dot(distance, distance) * 2.0f) - 0.01111f,
                0.0f);
            const float waveInput =
                Dot(distance, sideDirection) + phaseOffset;
            phaseDirection += Float2{
                std::cos(waveInput),
                std::sin(waveInput)} * weight;
            weightSum += weight;
        }
    }

    if (weightSum <= MinimumLength)
    {
        return {1.0f, 0.0f, sideDirection};
    }
    const Float2 interpolated = phaseDirection / weightSum;
    const float denominator = std::max(
        1.0f - Saturate(normalization),
        std::max(Length(interpolated), MinimumLength));
    const Float2 normalizedWave = interpolated / denominator;
    return {
        normalizedWave.x,
        normalizedWave.y,
        sideDirection};
}

ErosionResult ApplyErosion(
    const Float2 worldPosition,
    const HeightSlope& base,
    const float fadeTargetInput,
    const ProceduralErosionTerrainConfig& config)
{
    const float scale = std::max(config.erosionScale, 1.0f);
    const float cellScale = std::max(config.cellScale, 0.01f);
    float strength = config.erosionStrength * scale;
    float fadeTarget = std::clamp(fadeTargetInput, -1.0f, 1.0f);
    HeightSlope heightAndSlope = base;
    float frequency = 1.0f / (scale * cellScale);
    const float slopeLength = std::max(Length(base.slope), MinimumLength);
    float magnitude = 0.0f;
    float roundingMultiplier = 1.0f;
    const float roundingForInput = std::lerp(
        config.creaseRounding,
        config.ridgeRounding,
        Saturate(fadeTarget + 0.5f))
        * config.inputRoundingMultiplier;
    float combinedMask = EaseOut(SmoothStart(
        slopeLength * config.inputOnset,
        roundingForInput * config.inputOnset));
    float ridgeMapCombinedMask =
        EaseOut(slopeLength * config.ridgeMapInputOnset);
    float ridgeMapFadeTarget = fadeTarget;
    Float2 gullySlope = Lerp(
        base.slope,
        NormalizeSafe(base.slope) * config.assumedSlope,
        Saturate(config.assumedSlopeBlend));

    const std::uint32_t octaveCount =
        std::clamp(config.erosionOctaves, 1u, 8u);
    for (std::uint32_t octave = 0; octave < octaveCount; ++octave)
    {
        const PhacelleSample phacelle = SamplePhacelle(
            worldPosition * frequency,
            NormalizeSafe(gullySlope),
            cellScale,
            0.25f,
            config.normalization);
        const Float2 waveDerivative =
            phacelle.sideDirection * -frequency;
        const float sloping = std::abs(phacelle.sine);
        gullySlope += waveDerivative
            * (Sign(phacelle.sine) * strength * config.gullyWeight);

        const HeightSlope octaveSample{
            phacelle.cosine,
            waveDerivative * phacelle.sine};
        const HeightSlope faded = Lerp(
            {fadeTarget, {}},
            octaveSample * config.gullyWeight,
            combinedMask);
        heightAndSlope += faded * strength;
        magnitude += strength;
        fadeTarget = faded.height;

        const float roundingForOctave = std::lerp(
            config.creaseRounding,
            config.ridgeRounding,
            Saturate(phacelle.cosine + 0.5f))
            * roundingMultiplier;
        const float newMask = EaseOut(SmoothStart(
            sloping * config.octaveOnset,
            roundingForOctave * config.octaveOnset));
        combinedMask =
            PowInverse(combinedMask, config.detail) * newMask;

        ridgeMapFadeTarget = std::lerp(
            ridgeMapFadeTarget,
            octaveSample.height,
            ridgeMapCombinedMask);
        ridgeMapCombinedMask *=
            EaseOut(sloping * config.ridgeMapOctaveOnset);

        strength *= config.gain;
        frequency *= config.lacunarity;
        roundingMultiplier *= config.octaveRoundingMultiplier;
    }

    heightAndSlope.height -= config.carveBias * magnitude;
    return {
        heightAndSlope - base,
        magnitude,
        ridgeMapFadeTarget * (1.0f - ridgeMapCombinedMask)};
}
} // namespace

ProceduralErosionTerrain::ProceduralErosionTerrain(
    ProceduralErosionTerrainConfig config)
    : m_config(std::move(config))
{
    m_config.worldSize = std::max(m_config.worldSize, 1.0f);
    m_config.erosionScale = std::max(m_config.erosionScale, 1.0f);
    m_config.cellScale = std::max(m_config.cellScale, 0.01f);
    m_config.erosionOctaves =
        std::clamp(m_config.erosionOctaves, 1u, 8u);
    m_config.lacunarity = std::max(m_config.lacunarity, 1.01f);
    m_config.gain = std::clamp(m_config.gain, 0.0f, 1.0f);
    m_config.normalization = Saturate(m_config.normalization);
    m_config.assumedSlopeBlend = Saturate(m_config.assumedSlopeBlend);
}

ProceduralTerrainSample ProceduralErosionTerrain::SampleBase(
    const float worldX,
    const float worldZ) const
{
    const float inverseWorldSize = 1.0f / m_config.worldSize;
    const Float2 normalized{
        worldX * inverseWorldSize,
        worldZ * inverseWorldSize};

    HeightSlope continental = FractalNoise(
        normalized * 3.2f + Float2{11.0f, -7.0f});
    continental = ScaleInputDerivative(
        continental,
        {3.2f, 3.2f});
    HeightSlope ridgedNoise = FractalNoise(
        normalized * 7.5f + Float2{-4.0f, 9.0f});
    ridgedNoise = ScaleInputDerivative(
        ridgedNoise,
        {7.5f, 7.5f});
    const float ridgeBase = 1.0f - std::abs(ridgedNoise.height);
    const HeightSlope ridge{
        ridgeBase * ridgeBase * ridgeBase,
        ridgedNoise.slope
            * (-Sign(ridgedNoise.height) * 3.0f
               * ridgeBase * ridgeBase)};

    const float valleyAxis = normalized.x * 4.0f + normalized.y * 0.7f;
    const float valleyHeight = std::exp(-valleyAxis * valleyAxis);
    const HeightSlope valley{
        valleyHeight,
        {
            valleyHeight * -2.0f * valleyAxis * 4.0f,
            valleyHeight * -2.0f * valleyAxis * 0.7f}};
    const HeightSlope mountainA = GaussianMountain(
        normalized,
        {-0.18f, 0.10f},
        22.0f);
    const HeightSlope mountainB = GaussianMountain(
        normalized,
        {0.24f, -0.16f},
        31.0f);

    const float radius = Length(normalized);
    const float unclampedFalloff = 1.15f - radius * 1.25f;
    const float edgeFalloff = Saturate(unclampedFalloff);
    Float2 falloffSlope{};
    if (unclampedFalloff > 0.0f
        && unclampedFalloff < 1.0f
        && radius > MinimumLength)
    {
        falloffSlope = normalized * (-1.25f / radius);
    }

    HeightSlope raw{};
    raw += continental * 13.0f;
    raw += ridge * 38.0f;
    raw += mountainA * 54.0f;
    raw += mountainB * 42.0f;
    raw += valley * -13.0f;
    HeightSlope base{
        raw.height * edgeFalloff,
        raw.slope * edgeFalloff + falloffSlope * raw.height};
    if (base.height < -4.0f)
    {
        base = {-4.0f, {}};
    }

    constexpr float VerticalScale = 3.5f;
    base = base * VerticalScale;
    base.slope = base.slope * inverseWorldSize;
    return {base.height, base.slope.x, base.slope.y, 0.0f};
}

ProceduralTerrainSample ProceduralErosionTerrain::Sample(
    const float worldX,
    const float worldZ) const
{
    const ProceduralTerrainSample baseSample = SampleBase(worldX, worldZ);
    const HeightSlope base{
        baseSample.height,
        {baseSample.slopeX, baseSample.slopeZ}};
    const float altitudeRange = std::max(
        m_config.peakAltitude - m_config.valleyAltitude,
        1.0f);
    const float fadeTarget = std::clamp(
        (base.height - m_config.valleyAltitude)
                / altitudeRange * 2.0f
            - 1.0f,
        -1.0f,
        1.0f);
    const float erosionMask = SmoothStep(
        m_config.erosionFadeStartAltitude,
        m_config.erosionFadeEndAltitude,
        base.height);
    if (erosionMask <= 0.0f || m_config.erosionStrength <= 0.0f)
    {
        return baseSample;
    }

    const ErosionResult erosion = ApplyErosion(
        {worldX, worldZ},
        base,
        fadeTarget,
        m_config);
    const float maskDerivative = SmoothStepDerivative(
        m_config.erosionFadeStartAltitude,
        m_config.erosionFadeEndAltitude,
        base.height);
    const Float2 analyticalSlope = base.slope
        + erosion.delta.slope * erosionMask
        + base.slope
            * (erosion.delta.height * maskDerivative);
    // The analytical filter contains sub-metre derivatives that a 32-64 m
    // terrain mesh cannot represent. Using those derivatives as vertex normals
    // makes otherwise continuous triangles read as black shards. Match normals
    // to the generated mesh footprint while retaining the analytical height.
    const float normalSampleDistance = std::max(
        m_config.worldSize / 128.0f,
        1.0f);
    const auto sampleHeight = [&](const float x, const float z)
    {
        const ProceduralTerrainSample localBaseSample = SampleBase(x, z);
        const HeightSlope localBase{
            localBaseSample.height,
            {localBaseSample.slopeX, localBaseSample.slopeZ}};
        const float localFadeTarget = std::clamp(
            (localBase.height - m_config.valleyAltitude)
                    / altitudeRange * 2.0f
                - 1.0f,
            -1.0f,
            1.0f);
        const float localMask = SmoothStep(
            m_config.erosionFadeStartAltitude,
            m_config.erosionFadeEndAltitude,
            localBase.height);
        if (localMask <= 0.0f || m_config.erosionStrength <= 0.0f)
        {
            return localBase.height;
        }
        return localBase.height
            + ApplyErosion(
                  {x, z}, localBase, localFadeTarget, m_config)
                  .delta.height
                * localMask;
    };
    const Float2 meshScaleSlope{
        (sampleHeight(worldX + normalSampleDistance, worldZ)
         - sampleHeight(worldX - normalSampleDistance, worldZ))
            / (normalSampleDistance * 2.0f),
        (sampleHeight(worldX, worldZ + normalSampleDistance)
         - sampleHeight(worldX, worldZ - normalSampleDistance))
            / (normalSampleDistance * 2.0f)};
    const Float2 finalSlope = {
        std::lerp(meshScaleSlope.x, analyticalSlope.x, 0.04f),
        std::lerp(meshScaleSlope.y, analyticalSlope.y, 0.04f)};
    return {
        base.height + erosion.delta.height * erosionMask,
        finalSlope.x,
        finalSlope.y,
        erosion.ridgeMap * erosionMask};
}

DirectX::XMFLOAT3 ProceduralErosionTerrain::SampleNormal(
    const float worldX,
    const float worldZ) const
{
    const ProceduralTerrainSample sample = Sample(worldX, worldZ);
    DirectX::XMFLOAT3 normal{};
    DirectX::XMStoreFloat3(
        &normal,
        DirectX::XMVector3Normalize(DirectX::XMVectorSet(
            -sample.slopeX,
            1.0f,
            -sample.slopeZ,
            0.0f)));
    return normal;
}

const ProceduralErosionTerrainConfig&
ProceduralErosionTerrain::GetConfig() const
{
    return m_config;
}
} // namespace Prism::Scene
