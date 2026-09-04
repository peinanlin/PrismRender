#include "../ShaderBindings.hlsli"

PRISM_VK_BINDING(0) cbuffer LocalWaveConstants : register(b0)
{
    uint resolution;
    float deltaSeconds;
    float domainSizeMeters;
    float cellSizeMeters;
    float amplitudeMultiplier;
    float lateralMultiplier;
    float waveSpeedMetersPerSecond;
    float damping;
    float foamWhitecapsThreshold;
    float foamGenerationThreshold;
    float foamGenerationAmount;
    float foamDissipation;
    float foamFalloff;
    uint resetHistory;
    uint disturbanceCount;
    uint padding;
    float2 domainCenter;
    float2 domainPadding;
};

struct LocalWaveDisturbance
{
    float2 position;
    float radius;
    float strength;
    float2 velocityDirection;
};

PRISM_VK_BINDING(16) Texture2D<float> heightRead : register(t0);
PRISM_VK_BINDING(17) Texture2D<float> velocityRead : register(t1);
PRISM_VK_BINDING(18) Texture2D<float> foamRead : register(t2);
PRISM_VK_BINDING(19) StructuredBuffer<LocalWaveDisturbance> disturbances
    : register(t3);
PRISM_VK_BINDING(32) RWTexture2D<float> heightWrite : register(u0);
PRISM_VK_BINDING(33) RWTexture2D<float> velocityWrite : register(u1);
PRISM_VK_BINDING(34) RWTexture2D<float> foamWrite : register(u2);
PRISM_VK_BINDING(35) RWTexture2D<float4> displacementWrite : register(u3);
PRISM_VK_BINDING(36) RWTexture2D<float4> gradientWrite : register(u4);

uint2 ClampCoordinate(int2 coordinate)
{
    return uint2(clamp(coordinate, 0, int(resolution) - 1));
}

float HeightAt(int2 coordinate)
{
    return heightRead.Load(int3(ClampCoordinate(coordinate), 0));
}

float VelocityAt(int2 coordinate)
{
    return velocityRead.Load(int3(ClampCoordinate(coordinate), 0));
}

float FoamAt(int2 coordinate)
{
    return foamRead.Load(int3(ClampCoordinate(coordinate), 0));
}

float SampleFoam(float2 coordinate)
{
    const float2 clampedCoordinate = clamp(coordinate, 0.0.xx,
        (float(resolution) - 1.001).xx);
    const int2 baseCoordinate = int2(floor(clampedCoordinate));
    const float2 fractionValue = frac(clampedCoordinate);
    const float value00 = FoamAt(baseCoordinate);
    const float value10 = FoamAt(baseCoordinate + int2(1, 0));
    const float value01 = FoamAt(baseCoordinate + int2(0, 1));
    const float value11 = FoamAt(baseCoordinate + int2(1, 1));
    return lerp(lerp(value00, value10, fractionValue.x),
        lerp(value01, value11, fractionValue.x), fractionValue.y);
}

[numthreads(8, 8, 1)]
void SimulateLocalWaveCS(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    if (dispatchThreadId.x >= resolution || dispatchThreadId.y >= resolution)
    {
        return;
    }
    const int2 coordinate = int2(dispatchThreadId.xy);
    if (resetHistory != 0u)
    {
        heightWrite[dispatchThreadId.xy] = 0.0;
        velocityWrite[dispatchThreadId.xy] = 0.0;
        foamWrite[dispatchThreadId.xy] = 0.0;
        displacementWrite[dispatchThreadId.xy] = 0.0.xxxx;
        gradientWrite[dispatchThreadId.xy] = float4(0.0, 1.0, 0.0, 0.0);
        return;
    }
    const float height = HeightAt(coordinate);
    const float velocity = velocityRead.Load(int3(dispatchThreadId.xy, 0));
    const float heightLeft = HeightAt(coordinate + int2(-1, 0));
    const float heightRight = HeightAt(coordinate + int2(1, 0));
    const float heightDown = HeightAt(coordinate + int2(0, -1));
    const float heightUp = HeightAt(coordinate + int2(0, 1));
    const float velocityLeft = VelocityAt(coordinate + int2(-1, 0));
    const float velocityRight = VelocityAt(coordinate + int2(1, 0));
    const float velocityDown = VelocityAt(coordinate + int2(0, -1));
    const float velocityUp = VelocityAt(coordinate + int2(0, 1));
    const float discreteLaplacian = heightLeft + heightRight
        + heightDown + heightUp
        - 4.0 * height;
    const float safeCell = max(cellSizeMeters, 0.001);
    const float waveSpeed = max(waveSpeedMetersPerSecond, 0.0);
    const float stableDelta = min(max(deltaSeconds, 0.0),
        0.45 * safeCell / max(waveSpeed, 0.001));
    const float laplacian = discreteLaplacian / (safeCell * safeCell);
    float nextVelocity = velocity
        + laplacian * waveSpeed * waveSpeed * stableDelta;
    nextVelocity *= exp(-max(damping, 0.0) * stableDelta);
    float injectedHeight = 0.0;
    float injectedVelocity = 0.0;
    float2 disturbanceFlowCells = 0.0.xx;
    for (uint index = 0u; index < disturbanceCount; ++index)
    {
        const LocalWaveDisturbance disturbance = disturbances[index];
        const float2 worldPosition = (float2(dispatchThreadId.xy)
            / max(float(resolution), 1.0) - 0.5.xx) * domainSizeMeters;
        const float2 centeredWorldPosition = worldPosition + domainCenter;
        const float distanceToSource = distance(centeredWorldPosition,
            disturbance.position);
        const float normalizedRadius = distanceToSource
            / max(disturbance.radius, 0.001);
        const float radiusSquared = normalizedRadius * normalizedRadius;
        // The reference sample submits positive disturbance amounts.  The
        // compensating trough is generated by the wave equation itself; a
        // custom negative annulus makes adjacent samples in the 10x10 hull
        // cancel one another and leaves only mechanical concentric ripples.
        const float wavelet = exp(-2.0 * radiusSquared);
        // WaveWorks disturbance.z is an impulse amount in the simulated
        // surface domain, not an acceleration that must be integrated by dt
        // a second time.  Keep the GPU path equivalent to the CPU oracle:
        // inject the wavelet into height and a smaller part into velocity.
        injectedHeight += disturbance.strength * wavelet;
        injectedVelocity += disturbance.strength * wavelet * 0.25;
        const float directionLengthSquared = dot(
            disturbance.velocityDirection,
            disturbance.velocityDirection);
        const float2 sourceDirection = directionLengthSquared > 0.000001
            ? disturbance.velocityDirection
                * rsqrt(directionLengthSquared)
            : 0.0.xx;
        const float2 sourceToSurface = centeredWorldPosition
            - disturbance.position;
        const float2 radialDirection = sourceToSurface
            / max(length(sourceToSurface), safeCell * 0.25);
        // The compact hull pushes the water. It does not paint foam directly;
        // the propagated surface must subsequently satisfy the folding,
        // elevation and rising-crest tests below.
        const float trailingCrest = smoothstep(-0.20, 0.55,
            dot(radialDirection, -sourceDirection));
        const float shoulder = smoothstep(0.18, 0.48, normalizedRadius)
            * (1.0 - smoothstep(0.66, 1.0, normalizedRadius));
        disturbanceFlowCells += sourceDirection * shoulder * trailingCrest
            * abs(disturbance.strength) * stableDelta
            / safeCell * 0.85;
    }
    const float2 edgeCells = min(float2(dispatchThreadId.xy),
        float2(resolution - 1u - dispatchThreadId.xy));
    const float boundaryFade = smoothstep(0.0,
        max(float(resolution) * 0.12, 1.0),
        min(edgeCells.x, edgeCells.y));
    const float finalVelocity = (nextVelocity + injectedVelocity)
        * boundaryFade;
    const float nextHeight = (height + finalVelocity * stableDelta
        + injectedHeight)
        * boundaryFade;

    // A breaking candidate is a rising, elevated crest under compression.
    // abs(laplacian) also classifies troughs and rebound ripples as breaking,
    // which fills the whole local domain with foam after history accumulation.
    const float folding = max(-discreteLaplacian, 0.0)
        * max(lateralMultiplier, 0.0) * 12.0;
    const float crestHeight = smoothstep(0.01, 0.16,
        nextHeight * max(amplitudeMultiplier, 0.0));
    const float risingCrest = smoothstep(-0.02, 0.10, finalVelocity);
    // Generation threshold controls persistent turbulent energy. Whitecaps
    // threshold is independent and is evaluated by surface shading for the
    // immediate wave-hat signal.
    const float foldedBreaking = saturate(
        (folding - foamGenerationThreshold)
        / max(1.0 - foamGenerationThreshold, 0.0001))
        * crestHeight * risingCrest;
    const float breaking = foldedBreaking;
    // The time derivative of the height gradient approximates horizontal
    // orbital surface motion. Add the moving disturbance direction near a
    // wake source, then backtrace the scalar foam history. This keeps old
    // wake foam from remaining glued to the local grid without turning the
    // height solver into a full Navier-Stokes simulation.
    const float2 velocityGradientCells = 0.5 * float2(
        velocityRight - velocityLeft,
        velocityUp - velocityDown);
    float2 foamFlowCells = -velocityGradientCells
        * max(lateralMultiplier, 0.0) * stableDelta * 0.35
        + disturbanceFlowCells;
    foamFlowCells = clamp(foamFlowCells, -2.0.xx, 2.0.xx);
    const float2 foamBacktrace = float2(dispatchThreadId.xy)
        - foamFlowCells;
    const float previousFoam = saturate(SampleFoam(foamBacktrace));
    const float neighborFoam = 0.25 * (
        SampleFoam(foamBacktrace + float2(-1.0, 0.0))
        + SampleFoam(foamBacktrace + float2(1.0, 0.0))
        + SampleFoam(foamBacktrace + float2(0.0, -1.0))
        + SampleFoam(foamBacktrace + float2(0.0, 1.0)));
    const float frameScale = stableDelta * 60.0;
    const float spatialBlend = 1.0 - pow(
        max(1.0 - saturate(foamDissipation) * 0.25, 0.001),
        frameScale);
    const float spatiallyDissipated = lerp(
        previousFoam, neighborFoam, saturate(spatialBlend));
    // Local foam has to survive for the physical age of a wake, not just for
    // a handful of 60 Hz steps.  Treat the exposed falloff value as one-second
    // retention here: the reference 0.6 value then leaves a visible multi-
    // second Kelvin trail instead of a dotted sequence of source stamps.
    const float temporalRetention = pow(
        clamp(foamFalloff, 0.0001, 1.0), stableDelta);
    const float foam = saturate(spatiallyDissipated * temporalRetention
        + breaking * max(foamGenerationAmount, 0.0)
            * frameScale) * boundaryFade;

    heightWrite[dispatchThreadId.xy] = nextHeight;
    velocityWrite[dispatchThreadId.xy] = finalVelocity;
    foamWrite[dispatchThreadId.xy] = foam;

    const float invCell = 1.0 / safeCell;
    // Build the map from a one-step prediction instead of pairing nextHeight
    // with the previous height field's slope. The old temporal mismatch
    // rounded a fast moving crest and made its highlight lag behind geometry.
    const float predictedLeft = heightLeft + velocityLeft * stableDelta;
    const float predictedRight = heightRight + velocityRight * stableDelta;
    const float predictedDown = heightDown + velocityDown * stableDelta;
    const float predictedUp = heightUp + velocityUp * stableDelta;
    const float dx = (predictedRight - predictedLeft) * 0.5 * invCell;
    const float dz = (predictedUp - predictedDown) * 0.5 * invCell;
    const float amplitude = max(amplitudeMultiplier, 0.0);
    const float choppiness = max(lateralMultiplier, 0.0);
    const float3 normal = normalize(float3(
        -dx * amplitude, 1.0, -dz * amplitude));
    // Horizontal displacement is approximately slope / dominant wave number.
    // The local solver does not carry a spectral k, so use a bounded two-cell
    // characteristic length. The previous 0.35-cell scale was too small to
    // produce the pointed trochoidal crest visible in the reference wake.
    const float characteristicLength = max(safeCell * 1.75, 0.35);
    const float maximumLateral = safeCell * 1.35;
    const float2 lateral = clamp(
        -float2(dx, dz) * characteristicLength * choppiness,
        -maximumLateral.xx, maximumLateral.xx);
    displacementWrite[dispatchThreadId.xy] = float4(
        lateral.x, nextHeight * amplitude,
        lateral.y, folding);
    gradientWrite[dispatchThreadId.xy] = float4(normal, foam);
}
