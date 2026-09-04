static const uint ShadowFilterHard = 0u;
static const uint ShadowFilterPcf3x3 = 1u;
static const uint ShadowFilterPcf5x5 = 2u;
static const uint ShadowFilterPcss = 3u;
static const uint ShadowFilterVsm = 4u;
static const uint ShadowFilterEvsm = 5u;

float ReduceLightBleeding(float probability, float amount)
{
    return saturate(
        (probability - amount)
        / max(1.0f - amount, 0.0001f));
}

float ChebyshevUpperBound(
    float2 moments,
    float receiverDepth,
    float minimumVariance,
    float lightBleedReduction)
{
    if (receiverDepth <= moments.x)
    {
        return 1.0f;
    }
    // Bilinear filtering and fp16 storage can make E[x^2] very slightly
    // smaller than E[x]^2. Repair that before evaluating the bound.
    const float secondMoment = max(
        moments.y,
        moments.x * moments.x);
    const float variance = max(
        secondMoment - moments.x * moments.x,
        minimumVariance);
    const float distanceValue = receiverDepth - moments.x;
    return ReduceLightBleeding(
        variance / (variance + distanceValue * distanceValue),
        lightBleedReduction);
}

uint GetShadowFilterMode()
{
    return ((uint)shadowFilterMode) & 15u;
}

bool IsCascadeDebugEnabled()
{
    return shadowFilterMode >= 15.5f;
}

float3 GetCascadeDebugTint(uint cascadeIndex)
{
    if (!IsCascadeDebugEnabled())
    {
        return 1.0f.xxx;
    }
    if (cascadeIndex == 0u)
    {
        return float3(1.0f, 0.45f, 0.38f);
    }
    if (cascadeIndex == 1u)
    {
        return float3(0.38f, 1.0f, 0.48f);
    }
    return float3(0.38f, 0.58f, 1.0f);
}

float SampleDirectionalPcf(
    Texture2DArray shadowTexture,
    SamplerComparisonState comparisonSampler,
    float2 uv,
    uint layer,
    float currentDepth,
    float2 texelSize,
    int radius)
{
    float visibility = 0.0f;
    float sampleCount = 0.0f;
    [loop]
    for (int y = -2; y <= 2; ++y)
    {
        [loop]
        for (int x = -2; x <= 2; ++x)
        {
            if (abs(x) <= radius && abs(y) <= radius)
            {
                visibility += shadowTexture.SampleCmpLevelZero(
                    comparisonSampler,
                    float3(
                        uv + float2(x, y) * texelSize,
                        (float)layer),
                    currentDepth);
                sampleCount += 1.0f;
            }
        }
    }
    return visibility / max(sampleCount, 1.0f);
}

float FindAverageDirectionalBlockerDepth(
    Texture2DArray shadowTexture,
    float2 uv,
    uint layer,
    float currentDepth,
    uint2 shadowExtent,
    out float blockerCount)
{
    const int2 centerPixel = int2(
        clamp(uv, 0.0f.xx, 1.0f.xx)
            * float2(shadowExtent - 1u));
    float blockerDepthSum = 0.0f;
    blockerCount = 0.0f;
    [loop]
    for (int y = -3; y <= 3; ++y)
    {
        [loop]
        for (int x = -3; x <= 3; ++x)
        {
            const int2 samplePixel = clamp(
                centerPixel + int2(x, y),
                int2(0, 0),
                int2(shadowExtent) - 1);
            const float storedDepth = shadowTexture.Load(
                int4(samplePixel, (int)layer, 0)).r;
            if (storedDepth < currentDepth)
            {
                blockerDepthSum += storedDepth;
                blockerCount += 1.0f;
            }
        }
    }
    return blockerDepthSum / max(blockerCount, 1.0f);
}

float SampleDirectionalPcss(
    Texture2DArray shadowTexture,
    SamplerComparisonState comparisonSampler,
    float2 uv,
    uint layer,
    float currentDepth,
    float2 texelSize,
    uint2 shadowExtent)
{
    float blockerCount = 0.0f;
    const float averageBlockerDepth =
        FindAverageDirectionalBlockerDepth(
            shadowTexture,
            uv,
            layer,
            currentDepth,
            shadowExtent,
            blockerCount);
    if (blockerCount < 0.5f)
    {
        return 1.0f;
    }

    // Contact-hardening approximation in shadow-map texels. Farther
    // receivers receive a wider comparison kernel.
    const float penumbra = saturate(
        (currentDepth - averageBlockerDepth)
            / max(averageBlockerDepth, 0.0001f));
    const float radius = lerp(1.0f, 7.0f, penumbra * 36.0f);
    float visibility = 0.0f;
    float sampleCount = 0.0f;
    [loop]
    for (int y = -3; y <= 3; ++y)
    {
        [loop]
        for (int x = -3; x <= 3; ++x)
        {
            const float2 offset = float2(x, y);
            if (dot(offset, offset) <= 10.0f)
            {
                visibility += shadowTexture.SampleCmpLevelZero(
                    comparisonSampler,
                    float3(
                        uv + offset * texelSize * radius / 3.0f,
                        (float)layer),
                    currentDepth);
                sampleCount += 1.0f;
            }
        }
    }
    return visibility / max(sampleCount, 1.0f);
}

float FilterDirectionalShadow(
    Texture2DArray shadowTexture,
    SamplerComparisonState comparisonSampler,
    Texture2DArray<float4> momentsTexture,
    SamplerState momentsSampler,
    float2 uv,
    uint layer,
    float currentDepth,
    float2 texelSize,
    uint2 shadowExtent)
{
    const uint mode = GetShadowFilterMode();
    if (mode == ShadowFilterVsm)
    {
        const float2 moments = momentsTexture.SampleLevel(
            momentsSampler,
            float3(uv, (float)layer),
            0.0f).xy;
        return ChebyshevUpperBound(
            moments,
            saturate(currentDepth),
            0.00002f,
            0.28f);
    }
    if (mode == ShadowFilterEvsm)
    {
        const float4 moments = momentsTexture.SampleLevel(
            momentsSampler,
            float3(uv, (float)layer),
            0.0f);
        const float exponent = 5.0f;
        const float clampedDepth = saturate(currentDepth);
        const float positiveDepth = exp(exponent * clampedDepth);
        const float negativeDepth = -exp(-exponent * clampedDepth);

        // A fixed variance in warped space is incorrect: the EVSM derivative
        // changes exponentially with depth. Scale the numerical floor using
        // the local warp derivative so fp16 cancellation does not turn into
        // large, view-dependent light leaks.
        const float positiveDerivative =
            0.0001f * exponent * positiveDepth;
        const float negativeDerivative =
            0.0001f * exponent * abs(negativeDepth);
        const float positiveMinimumVariance = max(
            positiveDerivative * positiveDerivative,
            0.000001f);
        const float negativeMinimumVariance = max(
            negativeDerivative * negativeDerivative,
            0.000001f);
        return min(
            ChebyshevUpperBound(
                moments.xy,
                positiveDepth,
                positiveMinimumVariance,
                0.28f),
            ChebyshevUpperBound(
                moments.zw,
                negativeDepth,
                negativeMinimumVariance,
                0.28f));
    }
    if (mode == ShadowFilterHard)
    {
        return shadowTexture.SampleCmpLevelZero(
            comparisonSampler,
            float3(uv, (float)layer),
            currentDepth);
    }
    if (mode == ShadowFilterPcf5x5)
    {
        return SampleDirectionalPcf(
            shadowTexture,
            comparisonSampler,
            uv,
            layer,
            currentDepth,
            texelSize,
            2);
    }
    if (mode == ShadowFilterPcss)
    {
        return SampleDirectionalPcss(
            shadowTexture,
            comparisonSampler,
            uv,
            layer,
            currentDepth,
            texelSize,
            shadowExtent);
    }
    return SampleDirectionalPcf(
        shadowTexture,
        comparisonSampler,
        uv,
        layer,
        currentDepth,
        texelSize,
        1);
}

float GetDirectionalShadowViewDepth(
    float3 worldPosition,
    float3 cameraWorldPosition,
    float3 cameraWorldForward)
{
    // Cascade splits are built from view-frustum depth slices. Euclidean
    // camera distance selects the wrong cascade near the sides of the view
    // and makes the boundary move non-linearly as the camera rotates.
    return max(
        dot(
            worldPosition - cameraWorldPosition,
            cameraWorldForward),
        0.0f);
}

uint SelectDirectionalShadowCascade(
    float viewDepth,
    float4 splitDepths)
{
    uint cascadeIndex =
        viewDepth > splitDepths.x ? 1u : 0u;
    return viewDepth > splitDepths.y
        ? 2u
        : cascadeIndex;
}

float SampleDirectionalShadowCascade(
    Texture2DArray shadowTexture,
    SamplerComparisonState comparisonSampler,
    Texture2DArray<float4> momentsTexture,
    SamplerState momentsSampler,
    float3 worldPosition,
    float4x4 lightViewProjection,
    uint cascadeIndex,
    float depthBias)
{
    const float4 shadowPosition = mul(
        float4(worldPosition, 1.0f),
        lightViewProjection);
    const float3 projected = shadowPosition.xyz
        / max(shadowPosition.w, 0.0001f);
    const float2 uv = projected.xy
        * float2(0.5f, -0.5f)
        + 0.5f;
    if (uv.x < 0.0f || uv.x > 1.0f
        || uv.y < 0.0f || uv.y > 1.0f
        || projected.z < 0.0f || projected.z > 1.0f)
    {
        return 1.0f;
    }

    uint shadowWidth = 0;
    uint shadowHeight = 0;
    uint shadowLayers = 0;
    uint shadowLevels = 0;
    shadowTexture.GetDimensions(
        0,
        shadowWidth,
        shadowHeight,
        shadowLayers,
        shadowLevels);
    const float2 texelSize =
        1.0f / float2(shadowWidth, shadowHeight);
    const float receiverDepth = projected.z
        - depthBias
            * (1.0f + (float)cascadeIndex * 0.65f);
    return FilterDirectionalShadow(
        shadowTexture,
        comparisonSampler,
        momentsTexture,
        momentsSampler,
        uv,
        cascadeIndex,
        receiverDepth,
        texelSize,
        uint2(shadowWidth, shadowHeight));
}
