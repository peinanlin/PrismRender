#include "../ShaderBindings.hlsli"

static const float Pi = 3.14159265358979323846;
static const float Gravity = 9.81;

PRISM_VK_BINDING(0) cbuffer OceanEvolutionConstants : register(b0)
{
    uint resolution;
    uint activeCascadeMask;
    float absoluteTimeSeconds;
    float lateralMultiplier;
    float4 cascadePatchLengths;
};

PRISM_VK_BINDING(16) Texture2DArray<float4> initialSpectrum
    : register(t0);
PRISM_VK_BINDING(32) RWTexture2DArray<float4> spectrumA
    : register(u0);
PRISM_VK_BINDING(33) RWTexture2DArray<float4> spectrumB
    : register(u1);

float2 ComplexMultiply(float2 left, float2 right)
{
    return float2(
        left.x * right.x - left.y * right.y,
        left.x * right.y + left.y * right.x);
}

float2 ComplexConjugate(float2 value)
{
    return float2(value.x, -value.y);
}

uint ReverseIndex(uint value)
{
    uint reversed = 0u;
    for (uint bitCount = resolution; bitCount > 1u; bitCount >>= 1u)
    {
        reversed = (reversed << 1u) | (value & 1u);
        value >>= 1u;
    }
    return reversed;
}

float2 EvolveComponent(float2 h0, float2 mirroredH0, float2 phase)
{
    const float2 negativePhase = float2(phase.x, -phase.y);
    return ComplexMultiply(h0, phase)
        + ComplexMultiply(ComplexConjugate(mirroredH0), negativePhase);
}

float2 MultiplyByPositiveI(float2 value, float scale)
{
    // Positive WaveWorks-style lateral_multiplier must compress a positive
    // height crest. With PrismRender's inverse-transform convention that is
    // +i*k/|k|*h. The previous -i phase stretched crests and compressed
    // troughs, producing round humps and breaking foam at the wrong phase.
    return float2(-value.y * scale, value.x * scale);
}

[numthreads(8, 8, 1)]
void EvolveSpectrumCS(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    if (dispatchThreadId.x >= resolution || dispatchThreadId.y >= resolution
        || dispatchThreadId.z >= 4u)
    {
        return;
    }

    const uint cascade = dispatchThreadId.z;
    const uint2 outputCoordinate = uint2(
        ReverseIndex(dispatchThreadId.x),
        ReverseIndex(dispatchThreadId.y));
    const uint3 outputLocation = uint3(outputCoordinate, cascade);
    if ((activeCascadeMask & (1u << cascade)) == 0u)
    {
        spectrumA[outputLocation] = 0.0.xxxx;
        spectrumB[outputLocation] = 0.0.xxxx;
        return;
    }

    const uint2 coordinate = dispatchThreadId.xy;
    const uint2 mirroredCoordinate = uint2(
        (resolution - coordinate.x) % resolution,
        (resolution - coordinate.y) % resolution);
    const float4 h0 = initialSpectrum.Load(int4(coordinate, cascade, 0));
    const float4 mirroredH0 = initialSpectrum.Load(
        int4(mirroredCoordinate, cascade, 0));

    const int2 centered = int2(coordinate) - int(resolution / 2u);
    const float deltaK = 2.0 * Pi / cascadePatchLengths[cascade];
    const float2 waveVector = deltaK * float2(centered);
    const float waveNumber = length(waveVector);
    if (waveNumber <= 0.000001)
    {
        spectrumA[outputLocation] = 0.0.xxxx;
        spectrumB[outputLocation] = 0.0.xxxx;
        return;
    }

    const float angularFrequency = sqrt(Gravity * waveNumber);
    const float phaseAngle = fmod(angularFrequency
        * absoluteTimeSeconds, 2.0 * Pi);
    const float2 phase = float2(cos(phaseAngle), sin(phaseAngle));
    const float2 height = EvolveComponent(h0.xy, mirroredH0.xy, phase)
        + EvolveComponent(h0.zw, mirroredH0.zw, phase);
    const float2 direction = waveVector / waveNumber;
    const float2 displacementX = MultiplyByPositiveI(
        height, direction.x * lateralMultiplier);
    const float2 displacementZ = MultiplyByPositiveI(
        height, direction.y * lateralMultiplier);

    spectrumA[outputLocation] = float4(height, displacementX);
    spectrumB[outputLocation] = float4(displacementZ, 0.0, 0.0);
}
