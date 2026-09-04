#include "ShaderBindings.hlsli"

struct GpuObjectRecord
{
    float4 centerRadius;
    uint indexCount;
    uint firstIndex;
    int vertexOffset;
    uint firstInstance;
    uint enabled;
    uint drawBatchIndex;
    uint drawArgumentOffset;
    uint padding2;
    float4 lodData;
    uint parentObjectIndex;
    uint hierarchyEnabled;
    uint hierarchyPadding0;
    uint hierarchyPadding1;
};

struct DrawIndexedArguments
{
    uint indexCount;
    uint instanceCount;
    uint firstIndex;
    int vertexOffset;
    uint firstInstance;
};

PRISM_VK_BINDING(0) cbuffer GpuCullingConstants : register(b0)
{
    float4 frustumPlanes[6];
    float4x4 worldToView;
    float4x4 viewProjection;
    float4 viewportProjection;
    uint objectCount;
    uint frustumCullingEnabled;
    uint occlusionCullingEnabled;
    float occlusionDepthBias;
    uint drawBatchCount;
    uint cullingPadding0;
    uint cullingPadding1;
    uint cullingPadding2;
};

PRISM_VK_BINDING(16)
Texture2D<float> previousHiZ : register(t0);

PRISM_VK_BINDING(32)
RWStructuredBuffer<GpuObjectRecord> objectRecords : register(u0);

PRISM_VK_BINDING(33)
RWStructuredBuffer<DrawIndexedArguments> drawArguments : register(u1);

PRISM_VK_BINDING(34)
RWStructuredBuffer<uint> visibilityResults : register(u2);

PRISM_VK_BINDING(35)
RWStructuredBuffer<uint> batchDrawCounts : register(u3);

static const uint VisibilityUnknown = 0;
static const uint VisibilityVisible = 1;
static const uint VisibilityLodRejected = 2;
static const uint VisibilityFrustumCulled = 3;
static const uint VisibilityOcclusionCulled = 4;
static const uint VisibilityDisabled = 5;

[numthreads(64, 1, 1)]
void ClearDrawCountsCS(
    uint3 dispatchThreadId : SV_DispatchThreadID)
{
    const uint batchIndex = dispatchThreadId.x;
    if (batchIndex < drawBatchCount)
    {
        batchDrawCounts[batchIndex] = 0u;
    }
}

bool ShouldSplitQuadtreeNode(GpuObjectRecord node)
{
    const uint level = (uint)node.lodData.z;
    const uint maxLevel = (uint)node.lodData.w;
    if (level >= maxLevel)
    {
        return false;
    }
    const float3 viewCenter =
        mul(float4(node.centerRadius.xyz, 1.0), worldToView).xyz;
    const float viewDepth = max(abs(viewCenter.z), 0.25);
    const float projectedDiameterPixels =
        node.lodData.x
        * viewportProjection.w
        * viewportProjection.y
        / viewDepth;
    return projectedDiameterPixels > node.lodData.y;
}

bool IsSelectedQuadtreeLeaf(GpuObjectRecord node)
{
    if (ShouldSplitQuadtreeNode(node))
    {
        return false;
    }
    uint parentIndex = node.parentObjectIndex;
    [loop]
    for (uint ancestor = 0; ancestor < 8; ++ancestor)
    {
        if (parentIndex == 0xffffffffu)
        {
            break;
        }
        if (parentIndex >= objectCount)
        {
            return false;
        }
        const GpuObjectRecord parent =
            objectRecords[parentIndex];
        if (parent.hierarchyEnabled == 0
            || !ShouldSplitQuadtreeNode(parent))
        {
            return false;
        }
        parentIndex = parent.parentObjectIndex;
    }
    return true;
}

bool IsOccluded(float4 centerRadius)
{
    const float4 viewCenter =
        mul(float4(centerRadius.xyz, 1.0), worldToView);
    const float radius = centerRadius.w;
    if (viewCenter.z <= radius)
    {
        return false;
    }

    const float4 clipCenter =
        mul(float4(centerRadius.xyz, 1.0), viewProjection);
    if (clipCenter.w <= 0.0)
    {
        return false;
    }

    const float2 centerNdc =
        clipCenter.xy / clipCenter.w;
    const float nearestViewDepth =
        max(viewCenter.z - radius, 0.0001);
    const float2 radiusNdc =
        viewportProjection.zw
        * radius / nearestViewDepth;
    const float2 minUv = saturate(
        float2(
            centerNdc.x - radiusNdc.x,
            -centerNdc.y - radiusNdc.y)
            * 0.5 + 0.5);
    const float2 maxUv = saturate(
        float2(
            centerNdc.x + radiusNdc.x,
            -centerNdc.y + radiusNdc.y)
            * 0.5 + 0.5);
    const float2 extentPixels =
        max(
            (maxUv - minUv)
                * viewportProjection.xy,
            1.0);

    uint hiZWidth;
    uint hiZHeight;
    uint hiZMipCount;
    previousHiZ.GetDimensions(
        0,
        hiZWidth,
        hiZHeight,
        hiZMipCount);
    const float maxExtent =
        max(extentPixels.x, extentPixels.y);
    const uint mipLevel = min(
        (uint)max(
            ceil(log2(maxExtent)) - 1.0,
            0.0),
        hiZMipCount - 1);
    uint mipWidth;
    uint mipHeight;
    uint ignoredMipCount;
    previousHiZ.GetDimensions(
        mipLevel,
        mipWidth,
        mipHeight,
        ignoredMipCount);

    const float2 centerUv =
        (minUv + maxUv) * 0.5;
    const float2 sampleUvs[5] = {
        minUv,
        float2(maxUv.x, minUv.y),
        centerUv,
        float2(minUv.x, maxUv.y),
        maxUv};
    float farthestOccluderDepth = 0.0;
    [unroll]
    for (uint sampleIndex = 0;
         sampleIndex < 5;
         ++sampleIndex)
    {
        const uint2 texel = min(
            uint2(
                sampleUvs[sampleIndex]
                * float2(mipWidth, mipHeight)),
            uint2(mipWidth - 1, mipHeight - 1));
        farthestOccluderDepth = max(
            farthestOccluderDepth,
            previousHiZ.Load(
                int3(texel, mipLevel)));
    }

    const float centerDepth =
        clipCenter.z / clipCenter.w;
    const float nearestDepth = max(
        centerDepth
            - radius / nearestViewDepth,
        0.0);
    return nearestDepth
        > farthestOccluderDepth
            + occlusionDepthBias;
}

[numthreads(64, 1, 1)]
void BuildDrawArgumentsCS(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    const uint objectIndex = dispatchThreadId.x;
    if (objectIndex >= objectCount)
    {
        return;
    }

    const GpuObjectRecord objectRecord =
        objectRecords[objectIndex];
    uint visibilityReason = objectRecord.enabled != 0
        ? VisibilityVisible
        : VisibilityDisabled;
    if (visibilityReason == VisibilityVisible
        && objectRecord.hierarchyEnabled != 0)
    {
        if (!IsSelectedQuadtreeLeaf(objectRecord))
        {
            visibilityReason = VisibilityLodRejected;
        }
    }
    if (visibilityReason == VisibilityVisible
        && frustumCullingEnabled != 0)
    {
        [unroll]
        for (uint planeIndex = 0; planeIndex < 6; ++planeIndex)
        {
            const float distanceToPlane =
                dot(
                    frustumPlanes[planeIndex],
                    float4(objectRecord.centerRadius.xyz, 1.0));
            if (distanceToPlane < -objectRecord.centerRadius.w)
            {
                visibilityReason = VisibilityFrustumCulled;
                break;
            }
        }
    }
    if (visibilityReason == VisibilityVisible
        && occlusionCullingEnabled != 0
        && IsOccluded(
            objectRecord.centerRadius))
    {
        visibilityReason = VisibilityOcclusionCulled;
    }

    DrawIndexedArguments arguments;
    arguments.indexCount = objectRecord.indexCount;
    arguments.instanceCount =
        visibilityReason == VisibilityVisible ? 1u : 0u;
    arguments.firstIndex = objectRecord.firstIndex;
    arguments.vertexOffset = objectRecord.vertexOffset;
    arguments.firstInstance = objectRecord.firstInstance;
    if (visibilityReason == VisibilityVisible)
    {
        uint localDrawIndex = 0u;
        InterlockedAdd(
            batchDrawCounts[
                objectRecord.drawBatchIndex],
            1u,
            localDrawIndex);
        drawArguments[
            objectRecord.drawArgumentOffset
            + localDrawIndex] = arguments;
    }
    visibilityResults[objectIndex] = visibilityReason;
}
