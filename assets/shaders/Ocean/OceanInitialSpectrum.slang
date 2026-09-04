#include "../ShaderBindings.hlsli"

static const float Pi = 3.14159265358979323846;
static const float Gravity = 9.81;

PRISM_VK_BINDING(0) cbuffer InitialSpectrumConstants : register(b0)
{
    uint resolution;
    uint randomSeed;
    uint sampleCount;
    uint constantsPadding;
    float4 baseDirectionSpeedFetch;
    float4 basePeakingAmplitudeCutoff;
    float4 baseDependencyPadding;
    float4 swellDirectionSpeedFetch;
    float4 swellPeakingAmplitudeCutoff;
    float4 swellDependencyPadding;
    float4 cascadePatchLengths;
    float4 cascadeLowerWavelengths;
    float4 cascadeUpperWavelengths;
};

PRISM_VK_BINDING(16) Texture2DArray<float4> initialSpectrumRead
    : register(t0);
PRISM_VK_BINDING(17) StructuredBuffer<uint4> sampleCoordinates
    : register(t1);
PRISM_VK_BINDING(32) RWTexture2DArray<float4> initialSpectrumWrite
    : register(u0);
PRISM_VK_BINDING(36) RWStructuredBuffer<float4> sampledSpectrum
    : register(u4);

uint Hash32(uint state)
{
    state ^= state >> 16u;
    state *= 0x7feb352du;
    state ^= state >> 15u;
    state *= 0x846ca68bu;
    state ^= state >> 16u;
    return state;
}

float HashUnitFloat(uint2 coordinate, uint cascadeIndex, uint stream)
{
    uint state = randomSeed ^ (coordinate.x * 0x9e3779b9u)
        ^ (coordinate.y * 0x85ebca6bu)
        ^ (cascadeIndex * 0xc2b2ae35u)
        ^ (stream * 0x27d4eb2fu);
    return float(Hash32(state) & 0x00ffffffu) / 16777216.0;
}

float2 DeterministicGaussian(uint2 coordinate, uint cascadeIndex,
    uint streamOffset)
{
    const float u1 = max(HashUnitFloat(coordinate, cascadeIndex,
        streamOffset),
        0.000001);
    const float u2 = HashUnitFloat(coordinate, cascadeIndex,
        streamOffset + 1u);
    const float radius = sqrt(-2.0 * log(u1));
    const float angle = 2.0 * Pi * u2;
    return radius * float2(cos(angle), sin(angle));
}

float DirectionalSpreading(float directionDot, float dependency)
{
    return pow(max(0.0, 0.5 * (clamp(directionDot, -1.0, 1.0) + 1.0)),
        max(dependency, 0.0));
}

float DirectionalNormalization(float dependency)
{
    // Exact for the common exponents 0 and 1 and close over WaveWorks' 0..1
    // dependency range, without requiring gamma functions in shader code.
    return (max(dependency, 0.0) + 1.0) / (2.0 * Pi);
}

float FrequencyToWaveNumberDensity(float frequencyEnergy,
    float angularFrequency, float waveNumber, float dependency)
{
    if (angularFrequency <= 0.0 || waveNumber <= 0.0)
    {
        return 0.0;
    }
    // JONSWAP returns S(omega), while a 2D FFT samples d(kx)d(ky).
    // P(k,theta) = S(omega) * (d omega / d k) / k.
    const float jacobian = Gravity
        / (2.0 * angularFrequency * waveNumber);
    return max(frequencyEnergy, 0.0) * jacobian
        * DirectionalNormalization(dependency);
}

float JonswapEnergy(float waveNumber, float windSpeed, float fetchMeters,
    float peaking, float cutoffLength, float cutoffPower,
    float amplitudeMultiplier)
{
    if (waveNumber <= 0.0 || windSpeed <= 0.0 || fetchMeters <= 0.0)
    {
        return 0.0;
    }
    const float omega = sqrt(Gravity * waveNumber);
    const float peakOmega = 22.0
        * pow(Gravity * Gravity / (windSpeed * fetchMeters), 0.33);
    const float sigma = omega <= peakOmega ? 0.07 : 0.09;
    const float delta = (omega - peakOmega) / (sigma * peakOmega);
    const float gammaTerm = pow(max(peaking, 1.0),
        exp(-0.5 * delta * delta));
    const float inverseOmega = peakOmega / omega;
    const float base = 0.0081 * Gravity * Gravity / pow(omega, 5.0)
        * exp(-1.25 * pow(inverseOmega, 4.0));
    float cutoff = 1.0;
    if (cutoffLength > 0.0 && cutoffPower > 0.0)
    {
        const float wavelength = 2.0 * Pi / waveNumber;
        cutoff = 1.0 - exp(-pow(max(wavelength / cutoffLength, 0.0),
            cutoffPower));
    }
    // The exposed control scales displacement amplitude, while this function
    // returns spectral energy. Square it before h0 takes the square root.
    const float amplitudeScale = max(amplitudeMultiplier, 0.0);
    return max(0.0, base * gammaTerm * cutoff
        * amplitudeScale * amplitudeScale);
}

float SmoothStep01(float value)
{
    const float t = clamp(value, 0.0, 1.0);
    return t * t * (3.0 - 2.0 * t);
}

float CascadeBandWeight(float wavelength, uint cascadeIndex)
{
    float4 weights = 0.0.xxxx;
    float total = 0.0;
    [unroll]
    for (uint index = 0u; index < 4u; ++index)
    {
        const float lower = cascadeLowerWavelengths[index];
        const float upper = cascadeUpperWavelengths[index];
        if (upper > lower && wavelength >= lower && wavelength <= upper)
        {
            const float position = log(wavelength / lower)
                / max(log(upper / lower), 0.000001);
            const float edge = min(SmoothStep01(position * 2.0),
                SmoothStep01((1.0 - position) * 2.0));
            weights[index] = max(edge, 0.0001);
            total += weights[index];
        }
    }
    if (total <= 0.0)
    {
        uint nearest = 0u;
        float nearestDistance = 3.402823466e+38;
        [unroll]
        for (uint index = 0u; index < 4u; ++index)
        {
            const float distance = abs(log(wavelength)
                - log(max(cascadePatchLengths[index], 0.000001)));
            if (distance < nearestDistance)
            {
                nearest = index;
                nearestDistance = distance;
            }
        }
        return cascadeIndex == nearest ? 1.0 : 0.0;
    }
    return weights[cascadeIndex] / total;
}

float4 EvaluateInitialSpectrum(uint2 coordinate, uint cascadeIndex)
{
    const float patchLength = cascadePatchLengths[cascadeIndex];
    const int2 centered = int2(coordinate) - int(resolution / 2u);
    const float deltaK = 2.0 * Pi / patchLength;
    const float2 waveVector = deltaK * float2(centered);
    const float waveNumber = length(waveVector);
    if (waveNumber <= 0.000001)
    {
        return 0.0.xxxx;
    }
    const float2 direction = waveVector / waveNumber;
    const float2 baseDirection = normalize(baseDirectionSpeedFetch.xy);
    const float2 swellDirection = normalize(swellDirectionSpeedFetch.xy);
    const float angularFrequency = sqrt(Gravity * waveNumber);
    const float baseFrequencyEnergy = JonswapEnergy(waveNumber,
        baseDirectionSpeedFetch.z, baseDirectionSpeedFetch.w,
        basePeakingAmplitudeCutoff.x, basePeakingAmplitudeCutoff.z,
        basePeakingAmplitudeCutoff.w, basePeakingAmplitudeCutoff.y)
        * DirectionalSpreading(dot(direction, baseDirection),
            baseDependencyPadding.x);
    const float swellFrequencyEnergy = JonswapEnergy(waveNumber,
        swellDirectionSpeedFetch.z, swellDirectionSpeedFetch.w,
        swellPeakingAmplitudeCutoff.x, swellPeakingAmplitudeCutoff.z,
        swellPeakingAmplitudeCutoff.w, swellPeakingAmplitudeCutoff.y)
        * DirectionalSpreading(dot(direction, swellDirection),
            swellDependencyPadding.x);
    const float baseEnergy = FrequencyToWaveNumberDensity(
        baseFrequencyEnergy, angularFrequency, waveNumber,
        baseDependencyPadding.x);
    const float swellEnergy = FrequencyToWaveNumberDensity(
        swellFrequencyEnergy, angularFrequency, waveNumber,
        swellDependencyPadding.x);
    const float wavelength = 2.0 * Pi / waveNumber;
    const float bandWeight = CascadeBandWeight(wavelength, cascadeIndex);
    const float coefficientScale = float(resolution * resolution) * deltaK;
    const float baseAmplitude = sqrt(max(0.0, baseEnergy * bandWeight)
        * 0.5) * coefficientScale;
    const float swellAmplitude = sqrt(max(0.0, swellEnergy * bandWeight)
        * 0.5) * coefficientScale;

    // Base wind and swell use independent deterministic Gaussian streams.
    // Keeping their complex h0 coefficients separate allows debug controls
    // to remove swell without perturbing the base-wind realization.
    const float2 baseH0 = DeterministicGaussian(
        coordinate, cascadeIndex, 0u) * baseAmplitude;
    const float2 swellH0 = DeterministicGaussian(
        coordinate, cascadeIndex, 2u) * swellAmplitude;
    return float4(baseH0, swellH0);
}

[numthreads(8, 8, 1)]
void GenerateInitialSpectrumCS(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    if (dispatchThreadId.x >= resolution || dispatchThreadId.y >= resolution
        || dispatchThreadId.z >= 4u)
    {
        return;
    }
    initialSpectrumWrite[dispatchThreadId] = EvaluateInitialSpectrum(
        dispatchThreadId.xy, dispatchThreadId.z);
}

[numthreads(64, 1, 1)]
void SampleInitialSpectrumCS(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    const uint index = dispatchThreadId.x;
    if (index >= sampleCount)
    {
        return;
    }
    const uint4 coordinate = sampleCoordinates[index];
    sampledSpectrum[index] = initialSpectrumRead.Load(
        int4(coordinate.xyz, 0));
}
