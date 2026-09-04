#include "FluidCommon.hlsli"

PRISM_VK_BINDING(32)
RWStructuredBuffer<float4> particlePositions : register(u0);
PRISM_VK_BINDING(33)
RWStructuredBuffer<float4> predictedPositions : register(u1);
PRISM_VK_BINDING(34)
RWStructuredBuffer<float4> particleVelocities : register(u2);
PRISM_VK_BINDING(35)
RWStructuredBuffer<float4> velocityScratch : register(u3);
PRISM_VK_BINDING(36)
RWStructuredBuffer<float4> particleCurl : register(u4);
PRISM_VK_BINDING(37)
RWStructuredBuffer<uint> neighborCounts : register(u5);
PRISM_VK_BINDING(38)
RWStructuredBuffer<uint> neighborParticleIndices : register(u6);
PRISM_VK_BINDING(39)
RWStructuredBuffer<uint> simulationDiagnostics : register(u7);
PRISM_VK_BINDING(40)
RWStructuredBuffer<float> particleDensities : register(u8);

[numthreads(PBF_THREAD_GROUP_SIZE, 1, 1)]
void UpdateVelocityCS(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    const uint particleIndex = dispatchThreadId.x;
    if (particleIndex >= PbfParticleCount())
    {
        return;
    }
    const float inverseDeltaTime =
        1.0f / max(gravityDeltaTime.w, 1.0e-6f);
    const float3 velocity =
        (predictedPositions[particleIndex].xyz
            - particlePositions[particleIndex].xyz)
        * inverseDeltaTime;
    velocityScratch[particleIndex] =
        float4(velocity, 0.0f);
}

[numthreads(PBF_THREAD_GROUP_SIZE, 1, 1)]
void ComputeVorticityCS(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    const uint particleIndex = dispatchThreadId.x;
    if (particleIndex >= PbfParticleCount())
    {
        return;
    }
    const float3 position = predictedPositions[particleIndex].xyz;
    const float3 velocity = velocityScratch[particleIndex].xyz;
    const float smoothingRadiusSquared =
        PbfSmoothingRadius() * PbfSmoothingRadius();
    const uint capacity = PbfMaxNeighborsPerParticle();
    const uint count = neighborCounts[particleIndex];
    const uint baseIndex = particleIndex * capacity;
    float3 curl = 0.0f;

    [loop]
    for (uint slot = 0u; slot < count; ++slot)
    {
        const uint neighborIndex =
            neighborParticleIndices[baseIndex + slot];
        const float3 difference =
            position - predictedPositions[neighborIndex].xyz;
        const float squaredDistance = dot(difference, difference);
        if (squaredDistance >= smoothingRadiusSquared)
        {
            continue;
        }
        curl += cross(
            velocityScratch[neighborIndex].xyz - velocity,
            PbfSpikyGradient(difference, squaredDistance))
            * PbfParticleMass()
            * PbfInverseRestDensity();
    }
    particleCurl[particleIndex] = float4(curl, 0.0f);
}

[numthreads(PBF_THREAD_GROUP_SIZE, 1, 1)]
void FinalizeParticlesCS(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    const uint particleIndex = dispatchThreadId.x;
    if (particleIndex >= PbfParticleCount())
    {
        return;
    }
    const float3 previousPosition =
        particlePositions[particleIndex].xyz;
    float3 position = predictedPositions[particleIndex].xyz;
    const float3 baseVelocity = velocityScratch[particleIndex].xyz;
    const bool useViscosity = velocityParameters.x > 1.0e-6f;
    const bool useVorticity = velocityParameters.y > 1.0e-6f;
    const float3 curl = useVorticity
        ? particleCurl[particleIndex].xyz
        : float3(0.0f, 0.0f, 0.0f);
    const float curlMagnitude = length(curl);
    const float smoothingRadiusSquared =
        PbfSmoothingRadius() * PbfSmoothingRadius();
    const uint capacity = PbfMaxNeighborsPerParticle();
    const uint count = neighborCounts[particleIndex];
    const uint baseIndex = particleIndex * capacity;
    float3 viscosityDelta = 0.0f;
    float3 curlMagnitudeGradient = 0.0f;

    if (useViscosity || useVorticity)
    {
        [loop]
        for (uint slot = 0u; slot < count; ++slot)
        {
            const uint neighborIndex =
                neighborParticleIndices[baseIndex + slot];
            const float3 difference =
                position - predictedPositions[neighborIndex].xyz;
            const float squaredDistance = dot(difference, difference);
            if (squaredDistance >= smoothingRadiusSquared)
            {
                continue;
            }
            const float volumeWeight =
                PbfParticleMass() * PbfInverseRestDensity();
            if (useViscosity)
            {
                const float kernel = PbfPoly6(squaredDistance);
                viscosityDelta +=
                    (velocityScratch[neighborIndex].xyz
                        - baseVelocity)
                    * kernel * volumeWeight;
            }
            if (useVorticity)
            {
                curlMagnitudeGradient +=
                    (length(particleCurl[neighborIndex].xyz)
                        - curlMagnitude)
                    * PbfSpikyGradient(difference, squaredDistance)
                    * volumeWeight;
            }
        }
    }

    float3 velocity = baseVelocity
        + velocityParameters.x * viscosityDelta;
    const float3 confinementDirection =
        PbfSafeNormalize(curlMagnitudeGradient);
    if (useVorticity)
    {
        velocity += gravityDeltaTime.w
            * velocityParameters.y
            * cross(confinementDirection, curl);
    }

    const float radius = domainMaxParticleRadius.w;
    const float3 lower = domainMinCellSize.xyz + radius;
    const float3 upper = domainMaxParticleRadius.xyz - radius;
    if (position.x <= lower.x && velocity.x < 0.0f
        || position.x >= upper.x && velocity.x > 0.0f)
    {
        velocity.x *= -velocityParameters.z;
    }
    // Density projection produces small position corrections even after the
    // liquid has visually settled. Reconstructing those corrections as
    // velocity made the bottom layer bounce forever between gravity, domain
    // clamping, and pressure projection. Treat a thin region above the floor
    // as resting contact: retain energetic upward motion, but snap low-energy
    // motion to the floor and damp only its tangential component.
    static const float FloorContactSlopRadiusScale = 0.50f;
    static const float FloorNormalSleepSpeed = 0.50f;
    static const float FloorStaticFrictionSpeed = 1.00f;
    static const float FloorTangentialDampingRate = 4.0f;
    const float floorContactSlop =
        radius * FloorContactSlopRadiusScale;
    const bool floorContact =
        position.y <= lower.y + floorContactSlop;
    // Along a floor/side-wall seam, the clamps remove the horizontal pressure
    // release direction. Constraint projection can therefore create a
    // persistent numerical upward jet even with zero restitution. Detect the
    // seam with a small slop, dissipate tangential motion, and cap only the
    // artificial upward component. Free waves away from boundaries remain
    // untouched.
    const float sideContactSlop = radius * 0.75f;
    const bool xWallContact =
        position.x <= lower.x + sideContactSlop
        || position.x >= upper.x - sideContactSlop;
    const bool zWallContact =
        position.z <= lower.z + sideContactSlop
        || position.z >= upper.z - sideContactSlop;
    const bool verticalCornerContact =
        xWallContact && zWallContact;
    const bool floorWallSeamContact =
        floorContact && (xWallContact || zWallContact);
    if (verticalCornerContact || floorWallSeamContact)
    {
        const float upwardVelocityLimit =
            max(correctionParameters.z, 0.0f);
        // Constraint projection has already changed `position` before this
        // kernel reconstructs velocity. Limit that displacement as well as the
        // resulting velocity, otherwise a zero velocity cap still leaves the
        // particle one correction step higher every frame.
        position.y = min(
            position.y,
            previousPosition.y
                + upwardVelocityLimit * gravityDeltaTime.w);
        velocity.xz *= saturate(correctionParameters.y);
        velocity.y = min(
            velocity.y,
            upwardVelocityLimit);
    }
    if (floorContact && velocity.y < 0.0f)
    {
        velocity.y *= -velocityParameters.z;
    }
    if (floorContact
        && abs(velocity.y) <= FloorNormalSleepSpeed)
    {
        position.y = lower.y;
        velocity.y = 0.0f;
        if (length(velocity.xz) <= FloorStaticFrictionSpeed)
        {
            // The displacement reconstructed into velocity includes the
            // density solver's numerical correction. Restore the previous
            // tangential position as well as clearing velocity; otherwise
            // that correction visibly crawls along the floor even though
            // every frame ends with zero contact velocity.
            position.xz = previousPosition.xz;
            velocity.xz = 0.0f;
        }
        else
        {
            const float tangentialDamping = exp(
                -FloorTangentialDampingRate
                    * gravityDeltaTime.w);
            velocity.xz *= tangentialDamping;
        }
    }
    else if (position.y >= upper.y && velocity.y > 0.0f)
    {
        velocity.y *= -velocityParameters.z;
    }
    if (position.z <= lower.z && velocity.z < 0.0f
        || position.z >= upper.z && velocity.z > 0.0f)
    {
        velocity.z *= -velocityParameters.z;
    }

    const bool invalid =
        any(isnan(position)) || any(isinf(position))
        || any(isnan(velocity)) || any(isinf(velocity));
    if (invalid)
    {
        if (PbfDiagnosticsEnabled())
        {
            InterlockedAdd(simulationDiagnostics[2], 1u);
        }
        velocity = 0.0f;
    }
    const float speed = length(velocity);
    if (speed > velocityParameters.w)
    {
        velocity *= velocityParameters.w
            / max(speed, 1.0e-6f);
    }
    // Keep density coverage for slow, grounded sheets, but mark energetic
    // under-supported particles as spray. The sign is render-only metadata;
    // ComputeLambdaCS overwrites it with the next positive density sample.
    static const float SplashFloorClearanceRadiusScale = 2.0f;
    const float densityRatio = abs(particleDensities[particleIndex]);
    const bool detachedFromFloor =
        position.y > lower.y
            + radius * SplashFloorClearanceRadiusScale;
    const uint supportThreshold =
        (uint)surfaceClassificationParameters.x;
    const bool underSupported = count < supportThreshold;
    const bool severelyIsolated = count * 2u < supportThreshold;
    const bool energetic =
        length(velocity) > surfaceClassificationParameters.y;
    const bool isolatedSplash =
        densityRatio < surfaceClassificationParameters.z
        && (severelyIsolated
            || (underSupported
                && (detachedFromFloor || energetic)));
    particleDensities[particleIndex] = isolatedSplash
        ? -densityRatio
        : densityRatio;
    particlePositions[particleIndex] =
        float4(PbfClampToDomain(position), 1.0f);
    particleVelocities[particleIndex] =
        float4(velocity, 0.0f);
}
