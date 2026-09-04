#include "Renderer/Features/Ocean/WaterRefractionModel.h"

#include <algorithm>
#include <cmath>

namespace Prism::Renderer
{
float ReconstructWaterLinearDepthMeters(DirectX::XMFLOAT2 uv, float deviceDepth,
    const DirectX::XMFLOAT4X4& inverseViewProjection,
    DirectX::XMFLOAT3 cameraPosition, DirectX::XMFLOAT3 cameraForward) noexcept
{
    using namespace DirectX;
    const XMVECTOR world = XMVector4Transform(XMVectorSet(
        uv.x * 2.0f - 1.0f, 1.0f - uv.y * 2.0f, deviceDepth, 1.0f),
        XMLoadFloat4x4(&inverseViewProjection));
    const float w = XMVectorGetW(world);
    if (!std::isfinite(w) || std::abs(w) < 1.0e-6f) return 0.0f;
    const float depth = XMVectorGetX(XMVector3Dot(
        XMVectorSubtract(XMVectorScale(world, 1.0f / w), XMLoadFloat3(&cameraPosition)),
        XMLoadFloat3(&cameraForward)));
    return std::isfinite(depth) ? depth : 0.0f;
}

namespace
{
bool Finite(const float value) noexcept
{
    return std::isfinite(value);
}

DirectX::XMFLOAT2 SafeOriginalUv(
    const DirectX::XMFLOAT2 uv) noexcept
{
    if (!Finite(uv.x) || !Finite(uv.y))
        return {0.5f, 0.5f};
    return {std::clamp(uv.x, 0.0f, 1.0f),
        std::clamp(uv.y, 0.0f, 1.0f)};
}
} // namespace

WaterApproximateRefractionResult EvaluateApproximateWaterRefraction(
    const WaterApproximateRefractionInput& input,
    const WaterApproximateRefractionParameters& parameters) noexcept
{
    WaterApproximateRefractionResult result{};
    result.sampleUv = SafeOriginalUv(input.uv);
    const bool finite = Finite(input.uv.x) && Finite(input.uv.y)
        && Finite(input.screenNormal.x) && Finite(input.screenNormal.y)
        && Finite(input.waterDepth) && Finite(input.opaqueDepth)
        && Finite(input.candidateOpaqueDepth)
        && Finite(parameters.distortionStrength)
        && Finite(parameters.maximumUvOffset)
        && Finite(parameters.thicknessOffset)
        && Finite(parameters.maximumThickness)
        && Finite(parameters.edgeMargin)
        && parameters.maximumThickness > 1.0e-6f;
    if (!finite)
        return result;

    const float thickness = input.opaqueDepth - input.waterDepth;
    result.thickness = std::clamp(
        thickness, 0.0f, parameters.maximumThickness);
    if (thickness <= std::max(parameters.thicknessOffset, 0.0f))
    {
        result.fallback = WaterRefractionFallback::ZeroThickness;
        return result;
    }

    const float normalizedThickness = std::clamp(
        (thickness - std::max(parameters.thicknessOffset, 0.0f))
            / parameters.maximumThickness,
        0.0f,
        1.0f);
    float offsetX = input.screenNormal.x
        * parameters.distortionStrength * normalizedThickness;
    float offsetY = input.screenNormal.y
        * parameters.distortionStrength * normalizedThickness;
    const float offsetLength = std::sqrt(
        offsetX * offsetX + offsetY * offsetY);
    const float maximumOffset = std::max(parameters.maximumUvOffset, 0.0f);
    if (offsetLength > maximumOffset && offsetLength > 1.0e-8f)
    {
        const float scale = maximumOffset / offsetLength;
        offsetX *= scale;
        offsetY *= scale;
    }
    const DirectX::XMFLOAT2 candidate{
        input.uv.x + offsetX,
        input.uv.y + offsetY};
    const float margin = std::clamp(parameters.edgeMargin, 0.0f, 0.499f);
    if (candidate.x < margin || candidate.x > 1.0f - margin
        || candidate.y < margin || candidate.y > 1.0f - margin)
    {
        result.fallback = WaterRefractionFallback::OffScreen;
        return result;
    }
    if (input.candidateOpaqueDepth
        <= input.waterDepth + std::max(parameters.thicknessOffset, 0.0f))
    {
        result.fallback = WaterRefractionFallback::Foreground;
        return result;
    }

    result.sampleUv = candidate;
    result.fallback = WaterRefractionFallback::None;
    return result;
}

WaterRayMarchRefractionParameters
NormalizeWaterRayMarchRefractionParameters(
    const WaterRayMarchRefractionParameters& parameters) noexcept
{
    const WaterRayMarchRefractionParameters defaults{};
    WaterRayMarchRefractionParameters normalized = parameters;
    normalized.sampleCount = std::clamp(parameters.sampleCount, 1u, 64u);
    normalized.maximumUvOffset = Finite(parameters.maximumUvOffset)
        ? std::clamp(parameters.maximumUvOffset, 0.0f, 0.5f)
        : defaults.maximumUvOffset;
    normalized.thicknessOffset = Finite(parameters.thicknessOffset)
        ? std::clamp(parameters.thicknessOffset, 0.0f, 10.0f)
        : defaults.thicknessOffset;
    normalized.maximumThickness = Finite(parameters.maximumThickness)
        ? std::clamp(parameters.maximumThickness, 0.01f, 10000.0f)
        : defaults.maximumThickness;
    normalized.stepScale = Finite(parameters.stepScale)
        ? std::clamp(parameters.stepScale, 1.01f, 8.0f)
        : defaults.stepScale;
    normalized.jitter = Finite(parameters.jitter)
        ? std::clamp(parameters.jitter, 0.0f, 1.0f)
        : defaults.jitter;
    normalized.edgeMargin = Finite(parameters.edgeMargin)
        ? std::clamp(parameters.edgeMargin, 0.0f, 0.499f)
        : defaults.edgeMargin;
    return normalized;
}

float WaterRefractionFrameNoise(
    const std::uint32_t pixelX,
    const std::uint32_t pixelY,
    const std::uint32_t frameIndex) noexcept
{
    std::uint32_t hash = pixelX * 0x8da6b343u;
    hash ^= pixelY * 0xd8163841u;
    hash ^= frameIndex * 0xcb1ab31fu;
    hash ^= hash >> 16u;
    hash *= 0x7feb352du;
    hash ^= hash >> 15u;
    hash *= 0x846ca68bu;
    hash ^= hash >> 16u;
    return static_cast<float>(hash & 0x00ffffffu)
        / static_cast<float>(0x01000000u);
}

WaterApproximateRefractionResult EvaluateRayMarchedWaterRefraction(
    const WaterRayMarchRefractionInput& input,
    const WaterRayMarchRefractionParameters& parameters,
    const std::span<const float> opaqueDepthSamples) noexcept
{
    WaterApproximateRefractionResult result{};
    result.sampleUv = SafeOriginalUv(input.uv);
    const bool inputFinite = Finite(input.uv.x) && Finite(input.uv.y)
        && Finite(input.screenDirection.x)
        && Finite(input.screenDirection.y) && Finite(input.waterDepth);
    if (!inputFinite)
        return result;

    const WaterRayMarchRefractionParameters normalized =
        NormalizeWaterRayMarchRefractionParameters(parameters);
    const std::uint32_t sampleCount = std::min(
        normalized.sampleCount,
        static_cast<std::uint32_t>(opaqueDepthSamples.size()));
    if (sampleCount == 0u)
    {
        result.fallback = WaterRefractionFallback::NoIntersection;
        return result;
    }

    const float directionLength = std::sqrt(
        input.screenDirection.x * input.screenDirection.x
        + input.screenDirection.y * input.screenDirection.y);
    const float directionX = directionLength > 1.0e-8f
        ? input.screenDirection.x / directionLength : 0.0f;
    const float directionY = directionLength > 1.0e-8f
        ? input.screenDirection.y / directionLength : 0.0f;
    bool rejectedForeground = false;
    for (std::uint32_t index = 0u; index < sampleCount; ++index)
    {
        const float exponent =
            (static_cast<float>(index) + normalized.jitter + 1.0f)
            / static_cast<float>(sampleCount);
        const float progress = std::clamp(
            (std::pow(normalized.stepScale, exponent) - 1.0f)
                / (normalized.stepScale - 1.0f),
            0.0f,
            1.0f);
        const DirectX::XMFLOAT2 candidate{
            input.uv.x + directionX * normalized.maximumUvOffset * progress,
            input.uv.y + directionY * normalized.maximumUvOffset * progress};
        const float margin = normalized.edgeMargin;
        if (candidate.x < margin || candidate.x > 1.0f - margin
            || candidate.y < margin || candidate.y > 1.0f - margin)
        {
            result.fallback = WaterRefractionFallback::OffScreen;
            return result;
        }

        const float sceneDepth = opaqueDepthSamples[index];
        if (!Finite(sceneDepth))
            return result;
        if (sceneDepth <= input.waterDepth + normalized.thicknessOffset)
        {
            rejectedForeground = true;
            continue;
        }
        const float rayDepth = input.waterDepth
            + normalized.thicknessOffset
            + normalized.maximumThickness * progress;
        if (sceneDepth <= rayDepth + normalized.thicknessOffset)
        {
            result.sampleUv = candidate;
            result.thickness = std::clamp(sceneDepth - input.waterDepth,
                0.0f, normalized.maximumThickness);
            result.fallback = WaterRefractionFallback::None;
            return result;
        }
    }
    result.fallback = rejectedForeground
        ? WaterRefractionFallback::Foreground
        : WaterRefractionFallback::NoIntersection;
    return result;
}
} // namespace Prism::Renderer
