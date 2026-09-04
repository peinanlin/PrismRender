#include "../ShaderBindings.hlsli"

PRISM_VK_BINDING(0) cbuffer OceanFoamConstants : register(b0)
{
    uint resolution;
    uint resetHistory;
    float deltaSeconds;
    float whitecapsThreshold;
    float generationThreshold;
    float generationAmount;
    float dissipationSpeed;
    float falloffSpeed;
    float4 cascadePatchLengths;
};

PRISM_VK_BINDING(16) Texture2DArray<float4> previousFoam
    : register(t0);
PRISM_VK_BINDING(17) Texture2DArray<float4> gradientFolding
    : register(t1);
PRISM_VK_BINDING(18) Texture2DArray<float4> currentDisplacement
    : register(t2);
PRISM_VK_BINDING(32) RWTexture2DArray<float4> nextFoam
    : register(u0);

// The WaveWorks controls are authored around a 60 Hz simulation step. Keep
// their per-step meaning while making the recurrence frame-rate independent.
static const float ReferenceSimulationRate = 60.0;
// Bound the semi-Lagrangian backtrace so an invalid or discontinuous frame
// cannot pull history from an unrelated part of a periodic cascade.
static const float MaximumAdvectionCells = 8.0;

int WrapFoamCoordinate(int coordinate)
{
    const int extent = int(resolution);
    const int wrapped = coordinate % extent;
    return wrapped < 0 ? wrapped + extent : wrapped;
}

float4 SamplePreviousFoamState(float2 texelCoordinate, uint cascade)
{
    const float2 baseValue = floor(texelCoordinate);
    const float2 fractionValue = frac(texelCoordinate);
    const int2 baseCoordinate = int2(baseValue);
    const int2 nextCoordinate = baseCoordinate + int2(1, 1);
    const int x0 = WrapFoamCoordinate(baseCoordinate.x);
    const int y0 = WrapFoamCoordinate(baseCoordinate.y);
    const int x1 = WrapFoamCoordinate(nextCoordinate.x);
    const int y1 = WrapFoamCoordinate(nextCoordinate.y);
    const float4 value00 = previousFoam.Load(int4(x0, y0, cascade, 0));
    const float4 value10 = previousFoam.Load(int4(x1, y0, cascade, 0));
    const float4 value01 = previousFoam.Load(int4(x0, y1, cascade, 0));
    const float4 value11 = previousFoam.Load(int4(x1, y1, cascade, 0));
    return lerp(lerp(value00, value10, fractionValue.x),
        lerp(value01, value11, fractionValue.x), fractionValue.y);
}

float SampleDisplacementHeight(int2 coordinate, uint cascade)
{
    return currentDisplacement.Load(int4(
        WrapFoamCoordinate(coordinate.x),
        WrapFoamCoordinate(coordinate.y),
        cascade,
        0)).y;
}

[numthreads(8, 8, 1)]
void UpdateSpectralFoamCS(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    if (dispatchThreadId.x >= resolution
        || dispatchThreadId.y >= resolution
        || dispatchThreadId.z >= 4u)
    {
        return;
    }

    const float dt = isfinite(deltaSeconds)
        ? clamp(deltaSeconds, 0.0, 1.0) : 0.0;
    float4 displacement = currentDisplacement.Load(
        int4(dispatchThreadId, 0));
    if (!all(isfinite(displacement)))
    {
        displacement = 0.0.xxxx;
    }
    float4 previousAtCurrent = 0.0.xxxx;
    if (resetHistory == 0u)
    {
        previousAtCurrent = previousFoam.Load(
            int4(dispatchThreadId, 0));
        if (!all(isfinite(previousAtCurrent)))
        {
            previousAtCurrent = 0.0.xxxx;
        }
    }
    // History stores the prior horizontal and vertical displacement in yzw.
    // Their finite difference is a material surface motion estimate. Backtrace
    // in cascade texels before sampling energy so whitecaps move with the wave
    // instead of remaining glued to a fixed FFT texel.
    const float patchLength = max(
        cascadePatchLengths[dispatchThreadId.z], 0.0001);
    float2 displacementDelta = displacement.xz - previousAtCurrent.yz;
    float2 advectionCells = displacementDelta
        * (float(resolution) / patchLength);
    advectionCells = clamp(advectionCells,
        -MaximumAdvectionCells.xx, MaximumAdvectionCells.xx);
    const float2 previousTexel = float2(dispatchThreadId.xy)
        - advectionCells;
    float4 advectedState = resetHistory != 0u
        ? 0.0.xxxx
        : SamplePreviousFoamState(previousTexel, dispatchThreadId.z);
    if (!all(isfinite(advectedState)))
    {
        advectedState = 0.0.xxxx;
    }
    const float previous = saturate(advectedState.x);
    const float neighborPrevious = 0.25 * (
        SamplePreviousFoamState(
            previousTexel + float2(-1.0, 0.0), dispatchThreadId.z).x
        + SamplePreviousFoamState(
            previousTexel + float2(1.0, 0.0), dispatchThreadId.z).x
        + SamplePreviousFoamState(
            previousTexel + float2(0.0, -1.0), dispatchThreadId.z).x
        + SamplePreviousFoamState(
            previousTexel + float2(0.0, 1.0), dispatchThreadId.z).x);
    const float sampledFolding = gradientFolding.Load(
        int4(dispatchThreadId, 0)).w;
    const float folding = isfinite(sampledFolding)
        ? max(sampledFolding, 0.0) : 0.0;
    // Generation threshold injects persistent turbulent energy. Whitecaps
    // threshold is a separate surface-rendering control for immediate hats;
    // the two values are not ordered endpoints of one ramp.
    const float generationCandidate = saturate(
        (folding - generationThreshold)
        / max(1.0 - generationThreshold, 0.0001));
    // Rising, folding crests entrain more air than descending folds.  A broad
    // compressed trough is not a whitecap, so vertical motion is a real gate
    // here rather than merely a cosmetic multiplier.
    const float verticalVelocity = dt > 0.00001
        ? (displacement.y - previousAtCurrent.w) / dt : 0.0;
    const float risingCrest = smoothstep(
        -0.12, 0.55, verticalVelocity);
    const int2 coordinate = int2(dispatchThreadId.xy);
    const float neighborHeight = 0.25 * (
        SampleDisplacementHeight(coordinate + int2(-1, 0),
            dispatchThreadId.z)
        + SampleDisplacementHeight(coordinate + int2(1, 0),
            dispatchThreadId.z)
        + SampleDisplacementHeight(coordinate + int2(0, -1),
            dispatchThreadId.z)
        + SampleDisplacementHeight(coordinate + int2(0, 1),
            dispatchThreadId.z));
    const float cellSize = patchLength / max(float(resolution), 1.0);
    const float crestProminence = max(
        displacement.y - neighborHeight, 0.0) / max(cellSize, 0.0001);
    const float crestSharpness = smoothstep(
        0.025, 0.18, crestProminence);
    // Principal compression, a positive local crest and upward motion must
    // agree. Requiring their intersection avoids the camera-wide white sheet
    // produced when every mildly compressed FFT texel injects history.
    // Every FFT band may break. Suppressing the fine cascades by patch length
    // removed the short white streaks that dominate the WaveWorks near field.
    const float breakingSource = generationCandidate
        * lerp(0.68, 1.0, crestSharpness)
        * lerp(0.72, 1.0, risingCrest);
    const float frameScale = dt * ReferenceSimulationRate;
    const float generated = breakingSource
        * max(generationAmount, 0.0)
        * frameScale;
    // Dissipation is spatial; falloff is temporal. Previously both values
    // were added as exponential rates, which did not match the SDK contract
    // and made the 0.985 reference retention behave like rapid decay.
    const float spatialBlend = 1.0 - pow(
        max(1.0 - saturate(dissipationSpeed) * 0.25, 0.001),
        frameScale);
    const float dissipatedHistory = lerp(
        previous, neighborPrevious, saturate(spatialBlend));
    const float temporalRetention = pow(
        clamp(falloffSpeed, 0.0001, 1.0), frameScale);
    const float candidateEnergy =
        dissipatedHistory * temporalRetention + generated;
    const float energy = isfinite(candidateEnergy)
        ? saturate(candidateEnergy) : 0.0;
    nextFoam[dispatchThreadId] = float4(
        energy, displacement.x, displacement.z, displacement.y);
}
