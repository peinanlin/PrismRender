#include "ShaderBindings.hlsli"

PRISM_VK_BINDING(0) cbuffer ScreenSpaceConstants : register(b0)
{
    float4x4 viewProjection;
    float4x4 planarViewProjection;
    float3 cameraPosition;
    uint deferredEnabled;
    float2 resolution;
    float2 inverseResolution;
    float gtaoRadiusPixels;
    float gtaoStrength;
    float ssrMaxDistance;
    float ssrThickness;
    float ssrStride;
    uint gtaoEnabled;
    uint reflectionsEnabled;
    uint planarReflectionsEnabled;
    float reflectionPlaneHeight;
    float planarReflectionIntensity;
    float2 padding;
};

PRISM_VK_BINDING(16)
Texture2D<float4> positionRoughness : register(t0);
PRISM_VK_BINDING(17)
Texture2D<float4> normalMetallic : register(t1);
PRISM_VK_BINDING(18)
Texture2D<float4> hdrColor : register(t2);
PRISM_VK_BINDING(19)
Texture2D<float> hiZ : register(t3);
PRISM_VK_BINDING(20)
Texture2D<float4> ambientOcclusion : register(t4);
PRISM_VK_BINDING(21)
Texture2D<float4> planarReflection : register(t5);
PRISM_VK_BINDING(32)
RWTexture2D<float4> screenSpaceOutput : register(u0);

static const float PI = 3.14159265f;

float2 ProjectWorldToUv(
    float3 worldPosition,
    out float deviceDepth)
{
    const float4 clip =
        mul(
            float4(worldPosition, 1.0f),
            viewProjection);
    const float inverseW =
        1.0f / max(abs(clip.w), 0.00001f);
    const float3 ndc = clip.xyz * inverseW;
    deviceDepth = ndc.z;
    return ndc.xy
        * float2(0.5f, -0.5f)
        + 0.5f;
}

float2 ProjectWorldToPlanarUv(
    float3 worldPosition,
    out float deviceDepth)
{
    const float4 clip = mul(
        float4(worldPosition, 1.0f),
        planarViewProjection);
    const float inverseW =
        1.0f / max(abs(clip.w), 0.00001f);
    const float3 ndc = clip.xyz * inverseW;
    deviceDepth = ndc.z;
    return ndc.xy * float2(0.5f, -0.5f) + 0.5f;
}

[numthreads(8, 8, 1)]
void GtaoCS(
    uint3 dispatchThreadId : SV_DispatchThreadID)
{
    const uint2 pixel = dispatchThreadId.xy;
    if (pixel.x >= (uint)resolution.x
        || pixel.y >= (uint)resolution.y)
    {
        return;
    }

    const float4 centerData =
        positionRoughness.Load(
            int3(pixel, 0));
    const float3 packedNormal =
        normalMetallic.Load(
            int3(pixel, 0)).xyz;
    if (gtaoEnabled == 0
        || deferredEnabled == 0
        || dot(packedNormal, packedNormal)
            < 0.0001f)
    {
        screenSpaceOutput[pixel] =
            float4(1.0f, 0.0f, 0.0f, 0.0f);
        return;
    }

    const float3 centerPosition =
        centerData.xyz;
    const float3 normal =
        normalize(packedNormal * 2.0f - 1.0f);
    float occlusion = 0.0f;
    static const uint directionCount = 8;
    static const uint stepCount = 4;
    [unroll]
    for (uint directionIndex = 0;
         directionIndex < directionCount;
         ++directionIndex)
    {
        const float angle =
            (2.0f * PI
                * (float)directionIndex)
            / (float)directionCount;
        const float2 direction =
            float2(cos(angle), sin(angle));
        float horizon = 0.0f;
        [unroll]
        for (uint stepIndex = 1;
             stepIndex <= stepCount;
             ++stepIndex)
        {
            const float radius =
                gtaoRadiusPixels
                * ((float)stepIndex
                    / (float)stepCount);
            const int2 samplePixel =
                clamp(
                    int2(pixel)
                        + int2(
                            round(
                                direction
                                * radius)),
                    int2(0, 0),
                    int2(resolution) - 1);
            const float3 samplePosition =
                positionRoughness.Load(
                    int3(samplePixel, 0)).xyz;
            const float3 delta =
                samplePosition
                - centerPosition;
            const float distanceSquared =
                dot(delta, delta);
            if (distanceSquared > 0.0001f)
            {
                const float distanceValue =
                    sqrt(distanceSquared);
                const float horizonValue =
                    saturate(
                        dot(
                            normal,
                            delta / distanceValue)
                        - 0.05f);
                const float falloff =
                    saturate(
                        1.0f
                        - distanceValue
                            / 3.0f);
                horizon = max(
                    horizon,
                    horizonValue * falloff);
            }
        }
        occlusion += horizon;
    }
    const float visibility =
        saturate(
            1.0f
            - gtaoStrength
                * occlusion
                / (float)directionCount);
    screenSpaceOutput[pixel] =
        float4(visibility, 0.0f, 0.0f, 0.0f);
}

[numthreads(8, 8, 1)]
void ScreenSpaceReflectionsCS(
    uint3 dispatchThreadId : SV_DispatchThreadID)
{
    const uint2 pixel = dispatchThreadId.xy;
    if (pixel.x >= (uint)resolution.x
        || pixel.y >= (uint)resolution.y)
    {
        return;
    }

    const float4 currentColor =
        hdrColor.Load(int3(pixel, 0));
    if (deferredEnabled == 0
        || (reflectionsEnabled == 0
            && planarReflectionsEnabled == 0))
    {
        screenSpaceOutput[pixel] =
            currentColor;
        return;
    }

    const float4 positionData =
        positionRoughness.Load(
            int3(pixel, 0));
    const float4 normalData =
        normalMetallic.Load(
            int3(pixel, 0));
    if (dot(normalData.xyz, normalData.xyz)
        < 0.0001f)
    {
        screenSpaceOutput[pixel] =
            currentColor;
        return;
    }

    const float3 worldPosition =
        positionData.xyz;
    const float roughness =
        saturate(positionData.w);
    const float metallic =
        saturate(normalData.w);
    const float3 normal =
        normalize(normalData.xyz * 2.0f - 1.0f);
    const float3 incident =
        normalize(
            worldPosition - cameraPosition);
    const float3 reflectionDirection =
        normalize(reflect(incident, normal));
    float3 reflectionBaseColor = currentColor.rgb;
    float planarWeight = 0.0f;
    if (planarReflectionsEnabled != 0)
    {
        const float planeDistance = abs(
            worldPosition.y - reflectionPlaneHeight);
        const float planeFacing = saturate(
            (normal.y - 0.82f) / 0.18f);
        float planarDepth = 0.0f;
        const float2 planarUv = ProjectWorldToPlanarUv(
            worldPosition,
            planarDepth);
        if (planeDistance < 0.18f
            && planeFacing > 0.0f
            && all(planarUv > 0.0f)
            && all(planarUv < 1.0f)
            && planarDepth > 0.0f
            && planarDepth < 1.0f)
        {
            uint planarWidth = 0;
            uint planarHeight = 0;
            planarReflection.GetDimensions(
                planarWidth,
                planarHeight);
            const uint2 planarPixel = min(
                uint2(planarUv * float2(
                    planarWidth,
                    planarHeight)),
                uint2(planarWidth - 1u, planarHeight - 1u));
            const float3 planarColor = planarReflection.Load(
                int3(planarPixel, 0)).rgb;
            const float grazingFresnel = pow(
                1.0f - saturate(dot(normal, -incident)),
                5.0f);
            planarWeight = planeFacing
                * saturate(1.0f - roughness * 1.15f)
                * saturate(0.35f + grazingFresnel)
                * planarReflectionIntensity;
            reflectionBaseColor = lerp(
                reflectionBaseColor,
                planarColor,
                saturate(planarWeight));
        }
    }
    float3 reflectedColor = 0.0f.xxx;
    float hitConfidence = 0.0f;
    float previousDepthDelta = -1.0e-5f;
    float previousDistance = 0.0f;

    [loop]
    for (uint stepIndex = 1;
         stepIndex <= 56 && reflectionsEnabled != 0;
         ++stepIndex)
    {
        const float distanceValue =
            min(
                ssrMaxDistance,
                ssrStride
                    * (float)stepIndex);
        const float3 rayPosition =
            worldPosition
            + normal * 0.06f
            + reflectionDirection
                * distanceValue;
        float rayDepth = 0.0f;
        const float2 sampleUv =
            ProjectWorldToUv(
                rayPosition,
                rayDepth);
        if (any(sampleUv <= 0.0f)
            || any(sampleUv >= 1.0f)
            || rayDepth <= 0.0f
            || rayDepth >= 1.0f)
        {
            break;
        }

        const uint mipLevel =
            min(
                4u,
                (uint)floor(
                    log2(
                        1.0f
                        + (float)stepIndex
                            * 0.25f)));
        uint mipWidth = 0;
        uint mipHeight = 0;
        uint mipCount = 0;
        hiZ.GetDimensions(
            mipLevel,
            mipWidth,
            mipHeight,
            mipCount);
        const uint2 hiZPixel =
            min(
                uint2(
                    sampleUv
                    * float2(
                        mipWidth,
                        mipHeight)),
                uint2(
                    mipWidth - 1,
                    mipHeight - 1));
        const float sceneDepth =
            hiZ.Load(
                int3(hiZPixel, mipLevel));
        const float depthDelta =
            rayDepth - sceneDepth;
        if (depthDelta >= 0.0f
            && previousDepthDelta < 0.0f)
        {
            // The coarse Hi-Z sample only identifies a candidate interval.
            // Refine against mip zero so fixed march steps do not appear as
            // bands on large surfaces viewed at grazing angles.
            float lowerDistance = previousDistance;
            float upperDistance = distanceValue;
            float3 lowerRayPosition =
                worldPosition
                + normal * 0.06f
                + reflectionDirection
                    * lowerDistance;
            float lowerRayDepth = 0.0f;
            float2 lowerUv =
                ProjectWorldToUv(
                    lowerRayPosition,
                    lowerRayDepth);
            const uint2 lowerPixel =
                min(
                    uint2(lowerUv * resolution),
                    uint2(resolution) - 1);
            float lowerDepthDelta =
                lowerRayDepth
                - hiZ.Load(int3(lowerPixel, 0));
            if (lowerDepthDelta >= 0.0f)
            {
                lowerDistance = 0.0f;
            }

            float2 refinedUv = sampleUv;
            float3 refinedRayPosition = rayPosition;
            [loop]
            for (uint refinementStep = 0;
                 refinementStep < 6;
                 ++refinementStep)
            {
                const float candidateDistance =
                    (lowerDistance + upperDistance)
                    * 0.5f;
                const float3 candidateRayPosition =
                    worldPosition
                    + normal * 0.06f
                    + reflectionDirection
                        * candidateDistance;
                float candidateRayDepth = 0.0f;
                const float2 candidateUv =
                    ProjectWorldToUv(
                        candidateRayPosition,
                        candidateRayDepth);
                const uint2 candidatePixel =
                    min(
                        uint2(candidateUv * resolution),
                        uint2(resolution) - 1);
                const float candidateDepthDelta =
                    candidateRayDepth
                    - hiZ.Load(
                        int3(candidatePixel, 0));
                if (candidateDepthDelta >= 0.0f)
                {
                    upperDistance = candidateDistance;
                    refinedUv = candidateUv;
                    refinedRayPosition =
                        candidateRayPosition;
                }
                else
                {
                    lowerDistance = candidateDistance;
                }
            }

            const uint2 colorPixel =
                min(
                    uint2(refinedUv * resolution),
                    uint2(resolution) - 1);
            const float3 sceneWorldPosition =
                positionRoughness.Load(
                    int3(colorPixel, 0)).xyz;
            const float surfaceSeparation =
                length(
                    refinedRayPosition
                    - sceneWorldPosition);
            if (surfaceSeparation <= ssrThickness)
            {
                reflectedColor =
                    hdrColor.Load(
                        int3(colorPixel, 0)).rgb;
                const float edge =
                    saturate(
                        min(
                            min(
                                refinedUv.x,
                                refinedUv.y),
                            min(
                                1.0f - refinedUv.x,
                                1.0f - refinedUv.y))
                        * 12.0f);
                const float depthConfidence =
                    1.0f
                    - saturate(
                        surfaceSeparation
                        / max(ssrThickness, 1.0e-4f));
                hitConfidence =
                    edge
                    * depthConfidence
                    * saturate(
                        1.0f
                        - upperDistance
                            / ssrMaxDistance);
                break;
            }
        }
        previousDepthDelta = depthDelta;
        previousDistance = distanceValue;
        if (distanceValue >= ssrMaxDistance)
        {
            break;
        }
    }

    const float viewFresnel =
        pow(
            1.0f
            - saturate(
                dot(
                    normal,
                    -incident)),
            5.0f);
    const float materialWeight =
        saturate(
            lerp(
                0.08f,
                1.0f,
                metallic)
            * (1.0f - roughness)
            + viewFresnel * 0.25f);
    const float reflectionWeight =
        hitConfidence * materialWeight
            * (1.0f - saturate(planarWeight));
    // Deferred lighting already contains the IBL/specular fallback. SSR only
    // contributes when a validated screen-space hit has useful confidence.
    screenSpaceOutput[pixel] =
        float4(
            reflectionBaseColor
                + reflectedColor
                    * reflectionWeight,
            currentColor.a);
}
