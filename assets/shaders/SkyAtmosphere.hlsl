#include "ShaderBindings.hlsli"

static const float Pi = 3.14159265359f;

PRISM_VK_BINDING(0) cbuffer AtmosphereConstants : register(b0)
{
    float4 planetParameters;
    float4 rayleighScattering;
    float4 mieParameters;
    float4 sunDirectionCameraAltitude;
    float4 lutDimensions;
};

PRISM_VK_BINDING(16)
Texture2D<float4> transmittanceLut : register(t0);
PRISM_VK_BINDING(32)
RWTexture2D<float4> outputTransmittance : register(u0);
PRISM_VK_BINDING(33)
RWTexture2D<float4> outputSkyView : register(u1);
PRISM_VK_BINDING(48)
SamplerState linearClampSampler : register(s0);

float PlanetRadius()
{
    return planetParameters.x;
}

float AtmosphereRadius()
{
    return planetParameters.y;
}

float2 DensityAtRadius(float radius)
{
    const float altitude = max(radius - PlanetRadius(), 0.0f);
    return exp(-altitude / max(planetParameters.zw, 0.001f.xx));
}

float RaySphereFarDistance(
    float3 origin,
    float3 direction,
    float radius)
{
    const float b = dot(origin, direction);
    const float c = dot(origin, origin) - radius * radius;
    const float discriminant = b * b - c;
    if (discriminant < 0.0f)
    {
        return -1.0f;
    }
    return -b + sqrt(discriminant);
}

float RaySphereNearDistance(
    float3 origin,
    float3 direction,
    float radius)
{
    const float b = dot(origin, direction);
    const float c = dot(origin, origin) - radius * radius;
    const float discriminant = b * b - c;
    if (discriminant < 0.0f)
    {
        return -1.0f;
    }
    const float distance = -b - sqrt(discriminant);
    return distance > 0.0f ? distance : -1.0f;
}

float3 Extinction(float2 opticalDepth)
{
    const float3 rayleigh =
        rayleighScattering.xyz * opticalDepth.x;
    const float mieExtinction =
        (mieParameters.x + mieParameters.y)
        * opticalDepth.y;
    return rayleigh + mieExtinction.xxx;
}

float2 TransmittanceUv(float radius, float directionCosine)
{
    const float height = saturate(
        (radius - PlanetRadius())
        / max(AtmosphereRadius() - PlanetRadius(), 0.001f));
    return float2(
        directionCosine * 0.5f + 0.5f,
        height);
}

[numthreads(8, 8, 1)]
void TransmittanceCS(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    const uint2 pixel = dispatchThreadId.xy;
    if (pixel.x >= (uint)lutDimensions.x
        || pixel.y >= (uint)lutDimensions.y)
    {
        return;
    }
    const float2 uv =
        (float2(pixel) + 0.5f)
        / lutDimensions.xy;
    const float radius = lerp(
        PlanetRadius() + 0.001f,
        AtmosphereRadius() - 0.001f,
        uv.y);
    const float directionCosine =
        uv.x * 2.0f - 1.0f;
    const float sine = sqrt(
        saturate(1.0f
            - directionCosine
                * directionCosine));
    const float3 origin = float3(0.0f, radius, 0.0f);
    const float3 direction =
        float3(sine, directionCosine, 0.0f);
    const float atmosphereDistance =
        RaySphereFarDistance(
            origin,
            direction,
            AtmosphereRadius());
    const float groundDistance =
        RaySphereNearDistance(
            origin,
            direction,
            PlanetRadius());
    if (groundDistance > 0.0f
        && groundDistance < atmosphereDistance)
    {
        outputTransmittance[pixel] = 0.0f.xxxx;
        return;
    }

    const uint sampleCount = 40;
    const float stepLength =
        atmosphereDistance / sampleCount;
    float2 opticalDepth = 0.0f.xx;
    [loop]
    for (uint sampleIndex = 0;
         sampleIndex < sampleCount;
         ++sampleIndex)
    {
        const float distance =
            (sampleIndex + 0.5f) * stepLength;
        const float radiusAtSample = length(
            origin + direction * distance);
        opticalDepth +=
            DensityAtRadius(radiusAtSample)
            * stepLength;
    }
    outputTransmittance[pixel] = float4(
        exp(-Extinction(opticalDepth)),
        1.0f);
}

float RayleighPhase(float cosineTheta)
{
    return 3.0f
        * (1.0f + cosineTheta * cosineTheta)
        / (16.0f * Pi);
}

float MiePhase(float cosineTheta)
{
    const float g = mieParameters.z;
    const float g2 = g * g;
    const float denominator = max(
        pow(1.0f + g2 - 2.0f * g * cosineTheta, 1.5f),
        0.0001f);
    return (1.0f - g2)
        / (4.0f * Pi * denominator);
}

[numthreads(8, 8, 1)]
void SkyViewCS(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    const uint2 pixel = dispatchThreadId.xy;
    if (pixel.x >= (uint)lutDimensions.z
        || pixel.y >= (uint)lutDimensions.w)
    {
        return;
    }
    const float2 uv =
        (float2(pixel) + 0.5f)
        / lutDimensions.zw;
    const float viewZenith = uv.y * Pi;
    const float relativeAzimuth = uv.x * Pi;
    const float3 viewDirection = normalize(float3(
        sin(viewZenith) * cos(relativeAzimuth),
        cos(viewZenith),
        sin(viewZenith) * sin(relativeAzimuth)));
    const float sunY = clamp(
        sunDirectionCameraAltitude.y,
        -1.0f,
        1.0f);
    const float3 sunDirection = normalize(float3(
        sqrt(saturate(1.0f - sunY * sunY)),
        sunY,
        0.0f));
    const float cameraRadius = min(
        PlanetRadius()
            + max(sunDirectionCameraAltitude.w, 0.001f),
        AtmosphereRadius() - 0.001f);
    const float3 origin =
        float3(0.0f, cameraRadius, 0.0f);
    float rayLength = RaySphereFarDistance(
        origin,
        viewDirection,
        AtmosphereRadius());
    const float groundDistance =
        RaySphereNearDistance(
            origin,
            viewDirection,
            PlanetRadius());
    const bool hitsGround =
        groundDistance > 0.0f
        && groundDistance < rayLength;
    if (hitsGround)
    {
        rayLength = groundDistance;
    }

    const float cosineTheta = dot(
        viewDirection,
        sunDirection);
    const float rayleighPhase =
        RayleighPhase(cosineTheta);
    const float miePhase = MiePhase(cosineTheta);
    const uint sampleCount = 32;
    const float stepLength = rayLength / sampleCount;
    float2 viewOpticalDepth = 0.0f.xx;
    float3 radiance = 0.0f.xxx;
    [loop]
    for (uint sampleIndex = 0;
         sampleIndex < sampleCount;
         ++sampleIndex)
    {
        const float distance =
            (sampleIndex + 0.5f) * stepLength;
        const float3 samplePosition =
            origin + viewDirection * distance;
        const float sampleRadius =
            length(samplePosition);
        const float2 density =
            DensityAtRadius(sampleRadius);
        viewOpticalDepth += density * stepLength;
        const float3 up =
            samplePosition / max(sampleRadius, 0.001f);
        const float sunCosine = dot(up, sunDirection);
        const float3 sunTransmittance =
            transmittanceLut.SampleLevel(
                linearClampSampler,
                TransmittanceUv(
                    sampleRadius,
                    sunCosine),
                0.0f).rgb;
        const float3 viewTransmittance =
            exp(-Extinction(viewOpticalDepth));
        const float3 scattering =
            rayleighScattering.xyz
                * density.x
                * rayleighPhase
            + mieParameters.x.xxx
                * density.y
                * miePhase;
        const float lostLight =
            1.0f
            - dot(
                sunTransmittance,
                float3(
                    0.2126f,
                    0.7152f,
                    0.0722f));
        const float multiScatter =
            1.0f
            + mieParameters.w
                * saturate(lostLight)
                * 0.45f;
        radiance +=
            viewTransmittance
            * sunTransmittance
            * scattering
            * stepLength
            * multiScatter;
    }
    if (hitsGround)
    {
        radiance *= 0.7f;
    }
    radiance *= max(rayleighScattering.w, 0.0f);
    outputSkyView[pixel] = float4(radiance, 1.0f);
}
