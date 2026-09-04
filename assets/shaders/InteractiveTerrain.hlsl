#include "ShaderBindings.hlsli"

// Phacelle Noise and the erosion filter are adapted from Rune Skovbo Johansen's
// Mouse-Paint Eroded Mountains demonstration. The original implementation is
// licensed under MPL-2.0: https://mozilla.org/MPL/2.0/

static const uint TerrainResolution = 512u;
static const float Tau = 6.28318530717959f;
static const float DefaultHeight = 0.45f;

PRISM_VK_BINDING(0) cbuffer TerrainConstants : register(b0)
{
    // xy: dispatch origin, z: reset, w: brush active.
    uint4 dispatchParams;
    // xy: brush UV, z: radius, w: signed height delta.
    float4 brushParams;
    // x: erosion enabled, y: texture resolution.
    float4 erosionParams;
};

PRISM_VK_BINDING(32)
RWTexture2D<float4> rawHeightTexture : register(u0);
PRISM_VK_BINDING(33)
RWTexture2D<float4> erodedHeightTexture : register(u1);

float2 Hash22(float2 value)
{
    const float2 k = float2(0.3183099f, 0.3678794f);
    value = value * k + k.yx;
    return -1.0f + 2.0f * frac(
        16.0f * k * frac(
            value.x * value.y * (value.x + value.y)));
}

float3 GetBrushDelta(
    const float2 mapPosition,
    const float2 cursorPosition,
    const float brushSize)
{
    const float2 toCursor = cursorPosition - mapPosition;
    const float distanceToCursor = length(toCursor);
    const float2 direction = distanceToCursor > 1.0e-8f
        ? toCursor / distanceToCursor
        : 0.0f.xx;
    const float frequency = rcp(max(brushSize, 1.0e-5f));
    const float x = saturate(1.0f - frequency * distanceToCursor);
    return float3(
        x * x * (3.0f - 2.0f * x),
        direction * 6.0f * x * (1.0f - x) * frequency);
}

float4 PhacelleNoise(
    const float2 p,
    const float2 normalizedDirection,
    const float frequency,
    const float offset,
    const float normalization)
{
    const float2 sideDirection =
        normalizedDirection.yx * float2(-1.0f, 1.0f)
        * frequency * Tau;
    const float phaseOffset = offset * Tau;
    const float2 integerPart = floor(p);
    const float2 fractionalPart = frac(p);
    float2 phaseDirection = 0.0f.xx;
    float weightSum = 0.0f;
    [unroll]
    for (int y = -1; y <= 2; ++y)
    {
        [unroll]
        for (int x = -1; x <= 2; ++x)
        {
            const float2 gridOffset = float2(x, y);
            const float2 gridPoint = integerPart + gridOffset;
            const float2 randomOffset = Hash22(gridPoint) * 0.5f;
            const float2 fromCell =
                fractionalPart - gridOffset - randomOffset;
            const float squaredDistance = dot(fromCell, fromCell);
            const float weight = max(
                0.0f,
                exp(-squaredDistance * 2.0f) - 0.01111f);
            weightSum += weight;
            const float waveInput =
                dot(fromCell, sideDirection) + phaseOffset;
            phaseDirection +=
                float2(cos(waveInput), sin(waveInput)) * weight;
        }
    }
    const float2 interpolated = phaseDirection / max(weightSum, 1.0e-8f);
    float magnitude = sqrt(dot(interpolated, interpolated));
    magnitude = max(1.0f - normalization, magnitude);
    return float4(interpolated / magnitude, sideDirection);
}

float EaseOut(const float value)
{
    const float v = 1.0f - saturate(value);
    return 1.0f - v * v;
}

float SmoothStart(const float value, const float smoothing)
{
    if (smoothing <= 1.0e-8f)
    {
        return value;
    }
    return value >= smoothing
        ? value - 0.5f * smoothing
        : 0.5f * value * value / smoothing;
}

float PowInverse(const float value, const float power)
{
    return 1.0f - pow(1.0f - saturate(value), power);
}

float2 SafeNormalize(const float2 value)
{
    const float valueLength = length(value);
    return valueLength > 1.0e-10f
        ? value / valueLength
        : value;
}

float4 ErosionFilter(
    const float2 p,
    float3 heightAndSlope,
    float fadeTarget,
    out float ridgeMap)
{
    const float erosionScale = 0.15f;
    float strength = 0.22f * erosionScale;
    const float gullyWeight = 0.5f;
    const float detail = 1.5f;
    const float4 rounding = float4(0.1f, 0.0f, 0.1f, 2.0f);
    const float4 onset = float4(0.7f, 1.25f, 2.8f, 1.5f);
    const float2 assumedSlope = float2(0.7f, 1.0f);
    const float cellScale = 0.7f;
    const float normalization = 0.5f;
    const float lacunarity = 2.0f;
    const float gain = 0.5f;

    fadeTarget = clamp(fadeTarget, -1.0f, 1.0f);
    const float3 inputHeightAndSlope = heightAndSlope;
    float frequency = rcp(erosionScale * cellScale);
    const float slopeLength = max(length(heightAndSlope.yz), 1.0e-10f);
    float magnitude = 0.0f;
    float roundingMultiplier = 1.0f;
    const float inputRounding = lerp(
        rounding.y,
        rounding.x,
        saturate(fadeTarget + 0.5f)) * rounding.z;
    float combinedMask = EaseOut(
        SmoothStart(
            slopeLength * onset.x,
            inputRounding * onset.x));
    float ridgeCombinedMask = EaseOut(slopeLength * onset.z);
    float ridgeFadeTarget = fadeTarget;
    float2 gullySlope = lerp(
        heightAndSlope.yz,
        heightAndSlope.yz / slopeLength * assumedSlope.x,
        assumedSlope.y);

    [unroll]
    for (int octave = 0; octave < 5; ++octave)
    {
        float4 phacelle = PhacelleNoise(
            p * frequency,
            SafeNormalize(gullySlope),
            cellScale,
            0.25f,
            normalization);
        phacelle.zw *= -frequency;
        const float sloping = abs(phacelle.y);
        gullySlope += sign(phacelle.y)
            * phacelle.zw * strength * gullyWeight;
        const float3 gullies = float3(
            phacelle.x,
            phacelle.y * phacelle.zw);
        const float3 fadedGullies = lerp(
            float3(fadeTarget, 0.0f, 0.0f),
            gullies * gullyWeight,
            combinedMask);
        heightAndSlope += fadedGullies * strength;
        magnitude += strength;
        fadeTarget = fadedGullies.x;

        const float octaveRounding = lerp(
            rounding.y,
            rounding.x,
            saturate(phacelle.x + 0.5f)) * roundingMultiplier;
        const float newMask = EaseOut(
            SmoothStart(
                sloping * onset.y,
                octaveRounding * onset.y));
        combinedMask = PowInverse(combinedMask, detail) * newMask;
        ridgeFadeTarget = lerp(
            ridgeFadeTarget,
            gullies.x,
            ridgeCombinedMask);
        ridgeCombinedMask *= EaseOut(sloping * onset.w);

        strength *= gain;
        frequency *= lacunarity;
        roundingMultiplier *= rounding.w;
    }

    ridgeMap = ridgeFadeTarget * (1.0f - ridgeCombinedMask);
    return float4(
        heightAndSlope - inputHeightAndSlope,
        magnitude);
}

[numthreads(8, 8, 1)]
void TerrainBrushCS(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    const uint2 coordinate =
        dispatchParams.xy + dispatchThreadId.xy;
    if (any(coordinate >= TerrainResolution))
    {
        return;
    }
    const float2 uv =
        (float2(coordinate) + 0.5f) / (float)TerrainResolution;
    float3 value = rawHeightTexture[coordinate].xyz;
    if (dispatchParams.z != 0u)
    {
        value = float3(DefaultHeight, 0.0f, 0.0f);
        value += GetBrushDelta(uv, 0.5f.xx, 0.35f) * 0.14f;
    }
    if (dispatchParams.w != 0u)
    {
        value += GetBrushDelta(
            uv,
            brushParams.xy,
            brushParams.z) * brushParams.w;
        value.x = saturate(value.x);
    }
    rawHeightTexture[coordinate] = float4(value, 1.0f);
}

[numthreads(8, 8, 1)]
void TerrainErosionCS(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    const uint2 coordinate =
        dispatchParams.xy + dispatchThreadId.xy;
    if (any(coordinate >= TerrainResolution))
    {
        return;
    }
    const float2 uv =
        (float2(coordinate) + 0.5f) / (float)TerrainResolution;
    const float3 inputHeightAndSlope =
        rawHeightTexture[coordinate].xyz;
    float ridgeMap = 1.0f;
    float3 outputHeightAndSlope = inputHeightAndSlope;
    if (erosionParams.x > 0.5f)
    {
        const float fadeTarget = clamp(
            (inputHeightAndSlope.x - DefaultHeight) / 0.15f,
            -1.0f,
            1.0f);
        const float4 erosion = ErosionFilter(
            uv,
            inputHeightAndSlope,
            fadeTarget,
            ridgeMap);
        outputHeightAndSlope += erosion.xyz;
    }
    erodedHeightTexture[coordinate] = float4(
        outputHeightAndSlope,
        saturate(ridgeMap * 0.5f + 0.5f));
}
