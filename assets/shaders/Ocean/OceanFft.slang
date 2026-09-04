#include "../ShaderBindings.hlsli"

static const float Pi = 3.14159265358979323846;

PRISM_VK_BINDING(0) cbuffer OceanFftConstants : register(b0)
{
    uint resolution;
    uint fftStage;
    uint fftDirection;
    uint inverseTransform;
    uint bitReverseInput;
    uint normalizeOutput;
    uint padding0;
    uint padding1;
};

PRISM_VK_BINDING(32) RWTexture2D<float4> textureInputA : register(u0);
PRISM_VK_BINDING(33) RWTexture2D<float4> textureInputB : register(u1);
PRISM_VK_BINDING(34) RWTexture2D<float4> textureOutputA : register(u2);
PRISM_VK_BINDING(35) RWTexture2D<float4> textureOutputB : register(u3);
PRISM_VK_BINDING(36) RWStructuredBuffer<float2> bufferInput : register(u4);
PRISM_VK_BINDING(37) RWStructuredBuffer<float2> bufferOutput : register(u5);
PRISM_VK_BINDING(40) RWTexture2DArray<float4> arrayInputA : register(u8);
PRISM_VK_BINDING(41) RWTexture2DArray<float4> arrayInputB : register(u9);
PRISM_VK_BINDING(42) RWTexture2DArray<float4> arrayOutputA : register(u10);
PRISM_VK_BINDING(43) RWTexture2DArray<float4> arrayOutputB : register(u11);

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

float2 Twiddle(uint lane, uint block)
{
    const float sign = inverseTransform != 0u ? 1.0 : -1.0;
    const float angle = sign * 2.0 * Pi * float(lane) / float(block);
    return float2(cos(angle), sin(angle));
}

[numthreads(8, 8, 1)]
void TextureButterflyCS(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    const uint2 coordinate = dispatchThreadId.xy;
    if (any(coordinate >= resolution))
    {
        return;
    }
    const uint element = fftDirection == 0u ? coordinate.x : coordinate.y;
    const uint span = 1u << fftStage;
    const uint block = span << 1u;
    const uint blockStart = element - element % block;
    const uint lane = element % span;
    uint2 first = coordinate;
    uint2 second = coordinate;
    if (fftDirection == 0u)
    {
        first.x = blockStart + lane;
        second.x = first.x + span;
    }
    else
    {
        first.y = blockStart + lane;
        second.y = first.y + span;
    }
    const float2 twiddle = Twiddle(lane, block);
    const float4 firstA = textureInputA[first];
    const float4 secondA = textureInputA[second];
    const float4 firstB = textureInputB[first];
    const float4 secondB = textureInputB[second];
    const float4 twiddledA = float4(
        ComplexMultiply(secondA.xy, twiddle),
        ComplexMultiply(secondA.zw, twiddle));
    const float4 twiddledB = float4(
        ComplexMultiply(secondB.xy, twiddle),
        ComplexMultiply(secondB.zw, twiddle));
    const bool upper = (element % block) < span;
    textureOutputA[coordinate] = upper
        ? firstA + twiddledA : firstA - twiddledA;
    textureOutputB[coordinate] = upper
        ? firstB + twiddledB : firstB - twiddledB;
}

[numthreads(8, 8, 1)]
void TextureArrayButterflyCS(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    const uint2 coordinate = dispatchThreadId.xy;
    if (any(coordinate >= resolution) || dispatchThreadId.z >= 4u)
    {
        return;
    }
    const uint element = fftDirection == 0u ? coordinate.x : coordinate.y;
    const uint span = 1u << fftStage;
    const uint block = span << 1u;
    const uint blockStart = element - element % block;
    const uint lane = element % span;
    uint2 firstCoordinate = coordinate;
    uint2 secondCoordinate = coordinate;
    if (fftDirection == 0u)
    {
        firstCoordinate.x = blockStart + lane;
        secondCoordinate.x = firstCoordinate.x + span;
    }
    else
    {
        firstCoordinate.y = blockStart + lane;
        secondCoordinate.y = firstCoordinate.y + span;
    }
    const uint3 first = uint3(firstCoordinate, dispatchThreadId.z);
    const uint3 second = uint3(secondCoordinate, dispatchThreadId.z);
    const float2 twiddle = Twiddle(lane, block);
    const float4 firstA = arrayInputA[first];
    const float4 secondA = arrayInputA[second];
    const float4 firstB = arrayInputB[first];
    const float4 secondB = arrayInputB[second];
    const float4 twiddledA = float4(
        ComplexMultiply(secondA.xy, twiddle),
        ComplexMultiply(secondA.zw, twiddle));
    const float4 twiddledB = float4(
        ComplexMultiply(secondB.xy, twiddle),
        ComplexMultiply(secondB.zw, twiddle));
    const bool upper = (element % block) < span;
    arrayOutputA[dispatchThreadId] = upper
        ? firstA + twiddledA : firstA - twiddledA;
    arrayOutputB[dispatchThreadId] = upper
        ? firstB + twiddledB : firstB - twiddledB;
}

// One workgroup owns one complete row or column. Each active lane evaluates
// one butterfly pair and writes both results in place after taking local
// register copies. The pairs partition the shared array at every stage, so a
// single barrier between stages is sufficient. This reduces the production
// 2D transform from 2*log2(N) global texture passes to two texture passes.
groupshared float4 radixSharedA[512];
groupshared float4 radixSharedB[512];

[numthreads(256, 1, 1)]
void TextureArrayRadix2CS(uint3 groupId : SV_GroupID,
    uint3 groupThreadId : SV_GroupThreadID)
{
    const uint pairIndex = groupThreadId.x;
    const uint layer = groupId.z;
    const bool horizontal = fftDirection == 0u;
    const uint line = horizontal ? groupId.y : groupId.x;
    const bool active = pairIndex < resolution / 2u;
    if (active)
    {
        const uint firstLoad = pairIndex;
        const uint secondLoad = pairIndex + resolution / 2u;
        const uint2 firstCoordinate = horizontal
            ? uint2(firstLoad, line) : uint2(line, firstLoad);
        const uint2 secondCoordinate = horizontal
            ? uint2(secondLoad, line) : uint2(line, secondLoad);
        radixSharedA[firstLoad] = arrayInputA[
            uint3(firstCoordinate, layer)];
        radixSharedA[secondLoad] = arrayInputA[
            uint3(secondCoordinate, layer)];
        radixSharedB[firstLoad] = arrayInputB[
            uint3(firstCoordinate, layer)];
        radixSharedB[secondLoad] = arrayInputB[
            uint3(secondCoordinate, layer)];
    }
    GroupMemoryBarrierWithGroupSync();

    for (uint span = 1u; span < resolution; span <<= 1u)
    {
        if (active)
        {
            const uint block = span << 1u;
            const uint blockStart = (pairIndex / span) * block;
            const uint lane = pairIndex % span;
            const uint first = blockStart + lane;
            const uint second = first + span;
            const float2 twiddle = Twiddle(lane, block);
            const float4 firstA = radixSharedA[first];
            const float4 secondA = radixSharedA[second];
            const float4 firstB = radixSharedB[first];
            const float4 secondB = radixSharedB[second];
            const float4 twiddledA = float4(
                ComplexMultiply(secondA.xy, twiddle),
                ComplexMultiply(secondA.zw, twiddle));
            const float4 twiddledB = float4(
                ComplexMultiply(secondB.xy, twiddle),
                ComplexMultiply(secondB.zw, twiddle));
            radixSharedA[first] = firstA + twiddledA;
            radixSharedA[second] = firstA - twiddledA;
            radixSharedB[first] = firstB + twiddledB;
            radixSharedB[second] = firstB - twiddledB;
        }
        GroupMemoryBarrierWithGroupSync();
    }

    if (active)
    {
        const uint firstStore = pairIndex;
        const uint secondStore = pairIndex + resolution / 2u;
        const float normalization = normalizeOutput != 0u
            ? 1.0 / float(resolution) : 1.0;
        const uint2 firstCoordinate = horizontal
            ? uint2(firstStore, line) : uint2(line, firstStore);
        const uint2 secondCoordinate = horizontal
            ? uint2(secondStore, line) : uint2(line, secondStore);
        arrayOutputA[uint3(firstCoordinate, layer)] =
            radixSharedA[firstStore] * normalization;
        arrayOutputA[uint3(secondCoordinate, layer)] =
            radixSharedA[secondStore] * normalization;
        arrayOutputB[uint3(firstCoordinate, layer)] =
            radixSharedB[firstStore] * normalization;
        arrayOutputB[uint3(secondCoordinate, layer)] =
            radixSharedB[secondStore] * normalization;
    }
}

[numthreads(64, 1, 1)]
void BufferButterflyCS(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    const uint element = dispatchThreadId.x;
    if (element >= resolution)
    {
        return;
    }
    const uint span = 1u << fftStage;
    const uint block = span << 1u;
    const uint blockStart = element - element % block;
    const uint lane = element % span;
    uint first = blockStart + lane;
    uint second = first + span;
    if (bitReverseInput != 0u)
    {
        first = ReverseIndex(first);
        second = ReverseIndex(second);
    }
    const float2 firstValue = bufferInput[first];
    const float2 secondValue = ComplexMultiply(
        bufferInput[second], Twiddle(lane, block));
    const bool upper = (element % block) < span;
    float2 outputValue = upper
        ? firstValue + secondValue : firstValue - secondValue;
    if (normalizeOutput != 0u)
    {
        outputValue /= float(resolution);
    }
    bufferOutput[element] = outputValue;
}
