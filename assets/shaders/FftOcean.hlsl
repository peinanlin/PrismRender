#include "ShaderBindings.hlsli"

static const float Pi = 3.14159265358979323846;
static const float Gravity = 9.81;

PRISM_VK_BINDING(0) cbuffer OceanConstants : register(b0)
{
    uint resolution;
    uint fftStage;
    uint fftDirection;
    float timeSeconds;
    float2 baseWindDirection;
    float baseWindSpeed;
    float baseFetchMeters;
    float basePeaking;
    float baseAmplitude;
    float2 swellDirection;
    float swellSpeed;
    float swellFetchMeters;
    float swellPeaking;
    float swellAmplitude;
    float patchLength;
    float choppiness;
    float baseCutoffLength;
    float baseCutoffPower;
    float swellCutoffLength;
    float swellCutoffPower;
    float baseDependency;
    float swellDependency;
};

PRISM_VK_BINDING(32) RWTexture2D<float4> spectrumInputA : register(u0);
PRISM_VK_BINDING(33) RWTexture2D<float4> spectrumInputB : register(u1);
PRISM_VK_BINDING(34) RWTexture2D<float4> spectrumOutputA : register(u2);
PRISM_VK_BINDING(35) RWTexture2D<float4> spectrumOutputB : register(u3);
PRISM_VK_BINDING(36) RWTexture2D<float4> displacementMap : register(u4);
PRISM_VK_BINDING(37) RWTexture2D<float4> normalFoamMap : register(u5);

float Hash(uint2 value)
{
    uint state = value.x * 747796405u
        + value.y * 2891336453u + 277803737u;
    state = (state >> ((state >> 28u) + 4u)) ^ state;
    state *= 277803737u;
    state = (state >> 22u) ^ state;
    return (float)(state & 0x00ffffffu) / 16777216.0;
}

float2 Gaussian(uint2 value)
{
    const float u1 = max(Hash(value), 0.000001);
    const float u2 = Hash(value.yx + uint2(173u, 941u));
    const float radius = sqrt(-2.0 * log(u1));
    const float angle = 2.0 * Pi * u2;
    return radius * float2(cos(angle), sin(angle));
}

float2 ComplexMultiply(float2 left, float2 right)
{
    return float2(
        left.x * right.x - left.y * right.y,
        left.x * right.y + left.y * right.x);
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

float DirectionalSpreading(float directionDot, float dependency)
{
    return pow(max(0.0, 0.5 * (clamp(directionDot, -1.0, 1.0) + 1.0)),
        max(dependency, 0.0));
}

float JonswapEnergy(float waveNumber, float windSpeed, float fetchMeters,
    float peaking, float cutoffLength, float cutoffPower,
    float amplitudeMultiplier)
{
    if (waveNumber <= 0.000001 || windSpeed <= 0.000001
        || fetchMeters <= 0.000001)
    {
        return 0.0;
    }
    const float omega = sqrt(Gravity * waveNumber);
    const float peakOmega = 22.0
        * pow(Gravity * Gravity / (windSpeed * fetchMeters), 0.33);
    const float sigma = omega <= peakOmega ? 0.07 : 0.09;
    const float peakDelta = (omega - peakOmega)
        / max(sigma * peakOmega, 0.000001);
    const float gammaTerm = pow(max(peaking, 1.0),
        exp(-0.5 * peakDelta * peakDelta));
    const float inverseOmega = peakOmega / max(omega, 0.000001);
    const float base = 0.0081 * Gravity * Gravity
        / pow(max(omega, 0.000001), 5.0)
        * exp(-1.25 * pow(inverseOmega, 4.0));
    float cutoff = 1.0;
    if (cutoffLength > 0.0 && cutoffPower > 0.0)
    {
        const float wavelength = 2.0 * Pi / waveNumber;
        cutoff = 1.0 - exp(-pow(max(wavelength / cutoffLength, 0.0),
            cutoffPower));
    }
    return max(0.0, base * gammaTerm * cutoff
        * max(amplitudeMultiplier, 0.0));
}

float2 InitialSpectrum(uint2 coordinate)
{
    const int2 centered = int2(coordinate) - int(resolution / 2u);
    const float2 waveVector =
        2.0 * Pi * float2(centered) / patchLength;
    const float waveNumber = length(waveVector);
    if (waveNumber < 0.0001)
    {
        return 0.0.xx;
    }
    const float2 waveDirection = waveVector / waveNumber;
    float spectrumEnergy = 0.0;
    float spectrumCoefficientScale = 1.0;
    if (baseCutoffPower < 0.0)
    {
        // The migration mode preserves the original lab's broad Phillips
        // appearance while the native spectral path is validated separately.
        const float largestWave = baseWindSpeed * baseWindSpeed / Gravity;
        const float directional = dot(waveDirection, baseWindDirection);
        const float damping = largestWave * 0.001;
        spectrumEnergy = baseAmplitude
            * exp(-1.0 / max(
                waveNumber * waveNumber
                * largestWave * largestWave,
                0.000001))
            / max(pow(waveNumber, 4.0), 0.000001)
            * directional * directional
            * exp(-waveNumber * waveNumber * damping * damping);
    }
    else
    {
        const float baseEnergy = JonswapEnergy(
            waveNumber, baseWindSpeed, baseFetchMeters, basePeaking,
            baseCutoffLength, baseCutoffPower, baseAmplitude)
            * DirectionalSpreading(dot(waveDirection, baseWindDirection),
                baseDependency);
        const float swellEnergy = JonswapEnergy(
            waveNumber, swellSpeed, swellFetchMeters, swellPeaking,
            swellCutoffLength, swellCutoffPower, swellAmplitude)
            * DirectionalSpreading(dot(waveDirection, swellDirection),
                swellDependency);
        spectrumEnergy = baseEnergy + swellEnergy;

        // JONSWAP is a continuous spectrum.  The inverse transform below is
        // normalized by N^2, so convert the sampled density into discrete DFT
        // coefficients using N^2 * deltaK.  The legacy Phillips lab uses an
        // intentionally art-directed amplitude and therefore keeps its old
        // scale.  This is especially important while the WaveWorks lab uses a
        // single visible cascade: without the conversion its reference
        // amplitude is roughly two orders of magnitude too small.
        const float deltaK = 2.0 * Pi / patchLength;
        spectrumCoefficientScale =
            float(resolution * resolution) * deltaK;
    }
    return Gaussian(coordinate + uint2(37u, 101u))
        * sqrt(max(spectrumEnergy, 0.0) * 0.5)
        * spectrumCoefficientScale;
}

[numthreads(8, 8, 1)]
void GenerateSpectrumCS(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    const uint2 coordinate = dispatchThreadId.xy;
    if (any(coordinate >= resolution))
    {
        return;
    }
    const uint2 mirrored =
        (resolution - coordinate) % resolution;
    const float2 h0 = InitialSpectrum(coordinate);
    const float2 h0Mirrored = InitialSpectrum(mirrored)
        * float2(1.0, -1.0);
    const int2 centered = int2(coordinate) - int(resolution / 2u);
    const float2 waveVector =
        2.0 * Pi * float2(centered) / patchLength;
    const float waveNumber = length(waveVector);
    const float omega = sqrt(Gravity * waveNumber);
    const float2 positivePhase = float2(
        cos(omega * timeSeconds),
        sin(omega * timeSeconds));
    const float2 negativePhase = float2(
        positivePhase.x,
        -positivePhase.y);
    const float2 heightSpectrum =
        ComplexMultiply(h0, positivePhase)
        + ComplexMultiply(h0Mirrored, negativePhase);
    const float inverseWaveNumber =
        waveNumber > 0.0001 ? rcp(waveNumber) : 0.0;
    const float2 displacementX = float2(
        -heightSpectrum.y,
        heightSpectrum.x)
        * (-waveVector.x * inverseWaveNumber * choppiness);
    const float2 displacementZ = float2(
        -heightSpectrum.y,
        heightSpectrum.x)
        * (-waveVector.y * inverseWaveNumber * choppiness);
    const uint2 outputCoordinate = uint2(
        ReverseIndex(coordinate.x),
        ReverseIndex(coordinate.y));
    spectrumInputA[outputCoordinate] = float4(
        heightSpectrum, displacementX);
    spectrumInputB[outputCoordinate] = float4(
        displacementZ, 0.0, 0.0);
}

float3 ReadSpatialDisplacement(uint2 coordinate)
{
    coordinate %= resolution;
    const float sign = ((coordinate.x + coordinate.y) & 1u)
        ? -1.0 : 1.0;
    const float normalization =
        sign / (float)(resolution * resolution);
    const float4 valueA = spectrumInputA[coordinate];
    const float4 valueB = spectrumInputB[coordinate];
    return float3(valueA.z, valueA.x, valueB.x)
        * normalization;
}

[numthreads(8, 8, 1)]
void BuildOceanMapsCS(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    const uint2 coordinate = dispatchThreadId.xy;
    if (any(coordinate >= resolution))
    {
        return;
    }
    const uint2 leftCoordinate = uint2(
        (coordinate.x + resolution - 1u) % resolution,
        coordinate.y);
    const uint2 rightCoordinate = uint2(
        (coordinate.x + 1u) % resolution,
        coordinate.y);
    const uint2 downCoordinate = uint2(
        coordinate.x,
        (coordinate.y + resolution - 1u) % resolution);
    const uint2 upCoordinate = uint2(
        coordinate.x,
        (coordinate.y + 1u) % resolution);
    const float3 center = ReadSpatialDisplacement(coordinate);
    const float3 left = ReadSpatialDisplacement(leftCoordinate);
    const float3 right = ReadSpatialDisplacement(rightCoordinate);
    const float3 down = ReadSpatialDisplacement(downCoordinate);
    const float3 up = ReadSpatialDisplacement(upCoordinate);
    const float cellSize = patchLength / (float)resolution;
    const float3 tangentX = float3(
        2.0 * cellSize + right.x - left.x,
        right.y - left.y,
        right.z - left.z);
    const float3 tangentZ = float3(
        up.x - down.x,
        up.y - down.y,
        2.0 * cellSize + up.z - down.z);
    const float3 normal = normalize(cross(tangentZ, tangentX));
    const float jacobianX =
        1.0 + (right.x - left.x) / (2.0 * cellSize);
    const float jacobianZ =
        1.0 + (up.z - down.z) / (2.0 * cellSize);
    const float foam = saturate(
        (0.72 - jacobianX * jacobianZ) * 2.8);
    displacementMap[coordinate] = float4(center, foam);
    normalFoamMap[coordinate] = float4(
        normal * 0.5 + 0.5,
        foam);
}
