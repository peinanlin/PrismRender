#include "ShaderBindings.hlsli"

PRISM_VK_BINDING(0) cbuffer PostProcessConstants : register(b0)
{
    float4 postProcessParams0;
    float4 postProcessParams1;
    float4 skyZenithAtmosphere;
    float4 skyHorizonPower;
    float4 groundGridEnabled;
    float4 sunDirectionIntensity;
    float4 sunColorAngularRadius;
    float4 cameraPositionTanHalfFov;
    float4 cameraForwardAspect;
    float4 cameraRightGridScale;
    float4 cameraUpGridFade;
    float4 gridMinorColorLineWidth;
    float4 gridMajorColorAxisWidth;
    float4 atmosphereLutParams;
};

#define exposure postProcessParams0.x
#define bloomThreshold postProcessParams0.y
#define bloomIntensity postProcessParams0.z
#define hdrEnabled postProcessParams0.w
#define tonemappingEnabled postProcessParams1.x
#define inverseSourceResolution postProcessParams1.yz
#define skyEnvironmentBlend postProcessParams1.w

PRISM_VK_BINDING(16) Texture2D sourceTexture : register(t0);
PRISM_VK_BINDING(17) Texture2D auxiliaryTexture : register(t1);
PRISM_VK_BINDING(18) TextureCube environmentTexture : register(t2);
PRISM_VK_BINDING(19) Texture2D atmosphereSkyViewLut : register(t3);
PRISM_VK_BINDING(32) RWTexture2D<float4> outputTexture : register(u0);
PRISM_VK_BINDING(48) SamplerState linearClampSampler : register(s0);

struct VSOutput
{
    float4 position : SV_POSITION;
    float2 uv : TEXCOORD0;
};

VSOutput FullscreenVS(uint vertexId : SV_VertexID)
{
    VSOutput output;
    output.uv = float2((vertexId << 1) & 2, vertexId & 2);
    output.position = float4(output.uv * float2(2.0f, -2.0f) + float2(-1.0f, 1.0f), 0.0f, 1.0f);
    return output;
}

float3 SampleSource(float2 uv)
{
    return sourceTexture.Sample(linearClampSampler, uv).rgb;
}

float4 BrightExtractPS(VSOutput input) : SV_TARGET
{
    float3 color = SampleSource(input.uv);
    float brightness = max(max(color.r, color.g), color.b);
    return brightness > bloomThreshold ? float4(color, 1.0f) : float4(0.0f, 0.0f, 0.0f, 1.0f);
}

float4 BlurHorizontalPS(VSOutput input) : SV_TARGET
{
    float2 texel = float2(inverseSourceResolution.x, 0.0f);
    float3 result = SampleSource(input.uv) * 0.4f;
    result += SampleSource(input.uv + texel * 1.3846154f) * 0.3f;
    result += SampleSource(input.uv - texel * 1.3846154f) * 0.3f;
    return float4(result, 1.0f);
}

float4 BlurVerticalPS(VSOutput input) : SV_TARGET
{
    float2 texel = float2(0.0f, inverseSourceResolution.y);
    float3 result = SampleSource(input.uv) * 0.4f;
    result += SampleSource(input.uv + texel * 1.3846154f) * 0.3f;
    result += SampleSource(input.uv - texel * 1.3846154f) * 0.3f;
    return float4(result, 1.0f);
}

bool IsDispatchThreadInBounds(uint2 pixel)
{
    uint width = 0;
    uint height = 0;
    outputTexture.GetDimensions(width, height);
    return pixel.x < width && pixel.y < height;
}

float2 ComputeDispatchUv(uint2 pixel)
{
    uint width = 0;
    uint height = 0;
    outputTexture.GetDimensions(width, height);
    return (float2(pixel) + 0.5f) / float2(width, height);
}

[numthreads(8, 8, 1)]
void BrightExtractCS(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    if (!IsDispatchThreadInBounds(dispatchThreadId.xy))
    {
        return;
    }
    float2 uv = ComputeDispatchUv(dispatchThreadId.xy);
    float3 color = sourceTexture.SampleLevel(linearClampSampler, uv, 0).rgb;
    float brightness = max(max(color.r, color.g), color.b);
    outputTexture[dispatchThreadId.xy] =
        brightness > bloomThreshold
        ? float4(color, 1.0f)
        : float4(0.0f, 0.0f, 0.0f, 1.0f);
}

[numthreads(8, 8, 1)]
void BlurHorizontalCS(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    if (!IsDispatchThreadInBounds(dispatchThreadId.xy))
    {
        return;
    }
    float2 uv = ComputeDispatchUv(dispatchThreadId.xy);
    float2 texel = float2(inverseSourceResolution.x, 0.0f);
    float3 result =
        sourceTexture.SampleLevel(linearClampSampler, uv, 0).rgb * 0.4f;
    result += sourceTexture.SampleLevel(
        linearClampSampler, uv + texel * 1.3846154f, 0).rgb * 0.3f;
    result += sourceTexture.SampleLevel(
        linearClampSampler, uv - texel * 1.3846154f, 0).rgb * 0.3f;
    outputTexture[dispatchThreadId.xy] = float4(result, 1.0f);
}

[numthreads(8, 8, 1)]
void BlurVerticalCS(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    if (!IsDispatchThreadInBounds(dispatchThreadId.xy))
    {
        return;
    }
    float2 uv = ComputeDispatchUv(dispatchThreadId.xy);
    float2 texel = float2(0.0f, inverseSourceResolution.y);
    float3 result =
        sourceTexture.SampleLevel(linearClampSampler, uv, 0).rgb * 0.4f;
    result += sourceTexture.SampleLevel(
        linearClampSampler, uv + texel * 1.3846154f, 0).rgb * 0.3f;
    result += sourceTexture.SampleLevel(
        linearClampSampler, uv - texel * 1.3846154f, 0).rgb * 0.3f;
    outputTexture[dispatchThreadId.xy] = float4(result, 1.0f);
}

float3 ACESFilm(float3 color)
{
    const float a = 2.51f;
    const float b = 0.03f;
    const float c = 2.43f;
    const float d = 0.59f;
    const float e = 0.14f;
    return saturate((color * (a * color + b)) / (color * (c * color + d) + e));
}

float3 LinearToSrgb(float3 color)
{
    color = saturate(color);
    float3 low = color * 12.92f;
    float3 high = 1.055f * pow(color, 1.0f / 2.4f) - 0.055f;
    return lerp(high, low, step(color, 0.0031308f));
}

float3 BuildWorldViewDirection(float2 uv)
{
    float2 ndc = float2(uv.x * 2.0f - 1.0f, 1.0f - uv.y * 2.0f);
    float tanHalfFov = cameraPositionTanHalfFov.w;
    return normalize(
        cameraForwardAspect.xyz
        + cameraRightGridScale.xyz * ndc.x * cameraForwardAspect.w * tanHalfFov
        + cameraUpGridFade.xyz * ndc.y * tanHalfFov);
}

float2 ComputeAtmosphereSkyUv(
    float3 viewDirection,
    float3 sunDirection)
{
    const float pi = 3.14159265359f;
    const float viewZenith =
        acos(clamp(viewDirection.y, -1.0f, 1.0f));
    const float2 viewHorizontal = viewDirection.xz;
    const float2 sunHorizontal = sunDirection.xz;
    float relativeAzimuth = 0.0f;
    if (dot(viewHorizontal, viewHorizontal) > 0.000001f
        && dot(sunHorizontal, sunHorizontal) > 0.000001f)
    {
        relativeAzimuth = acos(clamp(
            dot(
                normalize(viewHorizontal),
                normalize(sunHorizontal)),
            -1.0f,
            1.0f));
    }
    return float2(
        relativeAzimuth / pi,
        viewZenith / pi);
}

float3 ApplyAtmosphereBrightness(
    float3 radiance,
    float brightness)
{
    const float safeBrightness = max(brightness, 0.0f);
    const float brightnessScale = safeBrightness <= 1.0f
        ? safeBrightness
        : 1.0f + log2(safeBrightness) * 0.45f;
    const float3 scaledRadiance =
        max(radiance, 0.0f.xxx) * brightnessScale;
    const float luminance = dot(
        scaledRadiance,
        float3(0.2126f, 0.7152f, 0.0722f));
    const float shoulderStart = 0.70f;
    const float shoulderRange = 0.85f;
    const float excess = max(luminance - shoulderStart, 0.0f);
    const float compressedLuminance = luminance <= shoulderStart
        ? luminance
        : shoulderStart
            + excess / (1.0f + excess / shoulderRange);
    return scaledRadiance
        * (compressedLuminance / max(luminance, 0.00001f));
}

float3 SampleAtmosphereSky(
    float2 uv,
    float brightness,
    float sunProximity)
{
    const float highBrightness = saturate(
        (brightness - 1.0f) / 3.0f);
    float3 radiance = atmosphereSkyViewLut.SampleLevel(
        linearClampSampler, uv, 0.0f).rgb;
    const float localMieCompression =
        saturate(sunProximity)
        * highBrightness;
    const float localLuminance = dot(
        radiance,
        float3(0.2126f, 0.7152f, 0.0722f));
    radiance /= 1.0f
        + localLuminance
            * localMieCompression
            * 7.5f;
    return ApplyAtmosphereBrightness(
        radiance,
        brightness);
}

float ComputeGridLine(float2 coordinates)
{
    float2 coordinateWidth = max(fwidth(coordinates), 0.0001f.xx);
    float2 distanceToLine = abs(frac(coordinates - 0.5f) - 0.5f) / coordinateWidth;
    return 1.0f - saturate(min(distanceToLine.x, distanceToLine.y));
}

float3 EvaluateEditorGrid(float3 viewDirection, float3 skyColor)
{
    if (groundGridEnabled.w < 0.5f || viewDirection.y >= -0.0001f)
    {
        return skyColor;
    }

    float distanceToGround = -cameraPositionTanHalfFov.y / viewDirection.y;
    if (distanceToGround <= 0.0f)
    {
        return skyColor;
    }

    float3 worldPosition = cameraPositionTanHalfFov.xyz + viewDirection * distanceToGround;
    float gridScale = max(cameraRightGridScale.w, 0.01f);
    float minorLine = ComputeGridLine(worldPosition.xz / gridScale);
    float majorLine = ComputeGridLine(worldPosition.xz / (gridScale * 10.0f));
    float3 gridColor = lerp(groundGridEnabled.rgb, gridMinorColorLineWidth.rgb, minorLine * 0.72f);
    gridColor = lerp(gridColor, gridMajorColorAxisWidth.rgb, majorLine * 0.88f);

    // Do not draw separately colored world axes. At the scale used by the
    // terrain lab the regular grid fades out first, leaving the red X axis as
    // an apparently random line while orbiting the Scene camera. The origin
    // axes remain represented by the neutral major-grid lines.

    float fadeDistance = max(cameraUpGridFade.w, 1.0f);
    float distanceFade = 1.0f - smoothstep(fadeDistance * 0.30f, fadeDistance, distanceToGround);
    float grazingFade = saturate(-viewDirection.y * 8.0f);
    return lerp(skyColor, gridColor, distanceFade * grazingFade);
}

float4 SkyboxPS(VSOutput input) : SV_TARGET
{
    float3 viewDirection = BuildWorldViewDirection(input.uv);
    float3 normalizedSunDirection =
        normalize(sunDirectionIntensity.xyz);
    float upAmount = saturate(viewDirection.y);
    float horizonAmount = pow(1.0f - upAmount, max(skyHorizonPower.w, 0.1f));
    float atmosphereDensity = max(skyZenithAtmosphere.w, 0.05f);
    float3 skyColor = lerp(skyZenithAtmosphere.rgb, skyHorizonPower.rgb, horizonAmount);
    skyColor *= lerp(0.82f, 1.18f, saturate(atmosphereDensity * 0.5f));

    if (viewDirection.y < 0.0f)
    {
        float belowHorizon = saturate(-viewDirection.y * 5.0f);
        skyColor = lerp(skyHorizonPower.rgb, groundGridEnabled.rgb, belowHorizon);
    }

    if (atmosphereLutParams.x > 0.5f)
    {
        const float2 atmosphereUv =
            ComputeAtmosphereSkyUv(
                viewDirection,
                normalizedSunDirection);
        skyColor = SampleAtmosphereSky(
            atmosphereUv,
            atmosphereLutParams.w,
            smoothstep(
                0.90f,
                0.9995f,
                dot(
                    viewDirection,
                    normalizedSunDirection)));
        // Keep the geometric horizon continuous using only atmosphere data.
        // Ground-hit rays remain in the LUT, but the first part below the
        // horizon blends from the physical horizon radiance instead of a
        // separately authored Horizon/Ground color.
        if (viewDirection.y < 0.0f)
        {
            float2 horizonDirection = viewDirection.xz;
            if (dot(horizonDirection, horizonDirection)
                < 0.000001f)
            {
                horizonDirection = float2(1.0f, 0.0f);
            }
            const float3 physicalHorizonDirection = normalize(
                float3(
                    horizonDirection.x,
                    0.001f,
                    horizonDirection.y));
            const float2 physicalHorizonUv =
                ComputeAtmosphereSkyUv(
                    physicalHorizonDirection,
                    normalizedSunDirection);
            const float3 physicalHorizon =
                SampleAtmosphereSky(
                    physicalHorizonUv,
                    atmosphereLutParams.w,
                    smoothstep(
                        0.90f,
                        0.9995f,
                        dot(
                            physicalHorizonDirection,
                            normalizedSunDirection)));
            const float belowHorizon =
                smoothstep(0.0f, 0.45f, -viewDirection.y);
            skyColor = lerp(
                physicalHorizon,
                skyColor,
                belowHorizon);
        }
    }

    float3 cubemapColor = environmentTexture.SampleLevel(linearClampSampler, viewDirection, 0).rgb;
    const float environmentBlend = atmosphereLutParams.x > 0.5f
        ? min(skyEnvironmentBlend, 0.02f)
        : skyEnvironmentBlend;
    float3 baseColor = skyEnvironmentBlend > 0.0f
        ? lerp(skyColor, cubemapColor, environmentBlend)
        : skyColor;
    baseColor = EvaluateEditorGrid(viewDirection, baseColor);

    float sunDot = saturate(dot(viewDirection, normalizedSunDirection));
    float angularRadius = max(sunColorAngularRadius.w, 0.0005f);
    float sunDisk = smoothstep(cos(angularRadius * 1.65f), cos(angularRadius), sunDot);
    float sunHalo = pow(sunDot, max(512.0f, 24.0f / angularRadius));
    float horizonWarmth = pow(saturate(1.0f - normalizedSunDirection.y), 2.0f);
    float3 sunColor = sunColorAngularRadius.rgb * lerp(1.0f.xxx, float3(1.0f, 0.48f, 0.20f), horizonWarmth);
    float sunIntensity = max(sunDirectionIntensity.w, 0.0f);
    float3 sunRadiance = sunColor * (sunDisk * sunIntensity * 12.0f + sunHalo * sunIntensity * 0.45f);
    return float4(baseColor + sunRadiance, 1.0f);
}

float4 TonemapPS(VSOutput input) : SV_TARGET
{
    float3 hdrColor = sourceTexture.Sample(linearClampSampler, input.uv).rgb;
    float3 bloomColor = 0.0f.xxx;
    if (bloomIntensity > 0.0f)
    {
        bloomColor = auxiliaryTexture.Sample(
            linearClampSampler,
            input.uv).rgb * bloomIntensity;
    }
    float3 color = hdrColor + bloomColor;

    if (hdrEnabled > 0.5f && tonemappingEnabled > 0.5f)
    {
        color = ACESFilm(color * exposure);
    }
    else
    {
        color = saturate(color);
    }

    return float4(LinearToSrgb(color), 1.0f);
}
