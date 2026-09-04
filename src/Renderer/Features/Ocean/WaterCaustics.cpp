#include "Renderer/Features/Ocean/WaterCaustics.h"

#include <algorithm>
#include <cmath>

namespace Prism::Renderer
{
namespace
{
float SmoothStep(const float value) noexcept
{
    const float t = std::clamp(value, 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

WaterCausticCascadeTransform BuildCascade(
    const DirectX::XMFLOAT2& cameraPositionMeters,
    const float coverageMeters,
    const std::uint32_t resolution) noexcept
{
    WaterCausticCascadeTransform result{};
    result.coverageMeters = std::max(coverageMeters, 1.0f);
    result.texelSizeMeters = result.coverageMeters
        / static_cast<float>(std::max(resolution, 1u));
    const auto Snap = [texelSize = result.texelSizeMeters](
                          const float value) noexcept {
        return std::floor(value / texelSize + 0.5f) * texelSize;
    };
    result.centerMeters = {
        Snap(cameraPositionMeters.x), Snap(cameraPositionMeters.y)};
    const float inverseCoverage = 1.0f / result.coverageMeters;
    result.worldToUv = {inverseCoverage, inverseCoverage,
        0.5f - result.centerMeters.x * inverseCoverage,
        0.5f - result.centerMeters.y * inverseCoverage};
    return result;
}

float CascadeEdgeWeight(const DirectX::XMFLOAT2& worldPositionMeters,
    const WaterCausticCascadeTransform& cascade,
    const float edgeFadeFraction) noexcept
{
    const float halfCoverage = std::max(cascade.coverageMeters * 0.5f,
        0.5f);
    const float normalizedEdge = std::max(
        std::abs(worldPositionMeters.x - cascade.centerMeters.x),
        std::abs(worldPositionMeters.y - cascade.centerMeters.y))
        / halfCoverage;
    const float distanceInside = 1.0f - normalizedEdge;
    return SmoothStep(distanceInside
        / std::clamp(edgeFadeFraction, 0.01f, 0.49f));
}

constexpr WaterCausticHistoryInvalidation operator|(
    const WaterCausticHistoryInvalidation left,
    const WaterCausticHistoryInvalidation right) noexcept
{
    return static_cast<WaterCausticHistoryInvalidation>(
        static_cast<std::uint32_t>(left)
        | static_cast<std::uint32_t>(right));
}
} // namespace

WaterCausticCascadeSet BuildWaterCausticCascadeSet(
    const DirectX::XMFLOAT2& cameraPositionMeters,
    const WaterCausticsSettings& source) noexcept
{
    WaterCausticsSettings settings = source;
    settings.resolution = std::clamp(settings.resolution, 64u, 2048u);
    settings.nearCoverageMeters = std::clamp(
        std::isfinite(settings.nearCoverageMeters)
            ? settings.nearCoverageMeters : 64.0f,
        1.0f, 10000.0f);
    settings.middleCoverageMeters = std::clamp(
        std::isfinite(settings.middleCoverageMeters)
            ? settings.middleCoverageMeters : 256.0f,
        settings.nearCoverageMeters + 1.0f, 20000.0f);
    settings.edgeFadeFraction = std::clamp(
        std::isfinite(settings.edgeFadeFraction)
            ? settings.edgeFadeFraction : 0.12f,
        0.01f, 0.49f);
    const DirectX::XMFLOAT2 finiteCamera{
        std::isfinite(cameraPositionMeters.x) ? cameraPositionMeters.x : 0.0f,
        std::isfinite(cameraPositionMeters.y) ? cameraPositionMeters.y : 0.0f};

    WaterCausticCascadeSet result{};
    result.resolution = settings.resolution;
    result.edgeFadeFraction = settings.edgeFadeFraction;
    result.cascades[0] = BuildCascade(finiteCamera,
        settings.nearCoverageMeters, settings.resolution);
    result.cascades[1] = BuildCascade(finiteCamera,
        settings.middleCoverageMeters, settings.resolution);
    return result;
}

WaterCausticCascadeWeights EvaluateWaterCausticCascadeWeights(
    const DirectX::XMFLOAT2& worldPositionMeters,
    const WaterCausticCascadeSet& cascades) noexcept
{
    const float nearRaw = CascadeEdgeWeight(worldPositionMeters,
        cascades.cascades[0], cascades.edgeFadeFraction);
    const float middleRaw = CascadeEdgeWeight(worldPositionMeters,
        cascades.cascades[1], cascades.edgeFadeFraction);
    WaterCausticCascadeWeights result{};
    result.nearWeight = std::clamp(nearRaw, 0.0f, 1.0f);
    result.middleWeight = std::clamp(
        (1.0f - result.nearWeight) * middleRaw, 0.0f, 1.0f);
    result.coverageWeight = std::clamp(
        result.nearWeight + result.middleWeight, 0.0f, 1.0f);
    return result;
}

bool WaterCausticHistoryState::Update(
    const WaterCausticHistoryKey& key) noexcept
{
    WaterCausticHistoryInvalidation invalidation =
        WaterCausticHistoryInvalidation::None;
    if (!m_valid)
        invalidation = invalidation
            | WaterCausticHistoryInvalidation::FirstFrame;
    if (m_valid && (key.cascades.resolution != m_key.cascades.resolution
        || std::abs(key.cascades.cascades[0].coverageMeters
            - m_key.cascades.cascades[0].coverageMeters) > 1.0e-5f
        || std::abs(key.cascades.cascades[1].coverageMeters
            - m_key.cascades.cascades[1].coverageMeters) > 1.0e-5f))
    {
        invalidation = invalidation
            | WaterCausticHistoryInvalidation::Resources;
    }
    if (m_valid)
    {
        const float dx = key.cascades.cascades[0].centerMeters.x
            - m_key.cascades.cascades[0].centerMeters.x;
        const float dy = key.cascades.cascades[0].centerMeters.y
            - m_key.cascades.cascades[0].centerMeters.y;
        const float jumpThreshold = std::max(
            key.cascades.cascades[0].coverageMeters * 0.25f, 1.0f);
        if (dx * dx + dy * dy > jumpThreshold * jumpThreshold)
            invalidation = invalidation
                | WaterCausticHistoryInvalidation::CameraJump;
    }
    if (m_valid && key.surfaceHistoryVersion
        != m_key.surfaceHistoryVersion)
    {
        invalidation = invalidation
            | WaterCausticHistoryInvalidation::SurfaceHistory;
    }
    if (m_valid && key.explicitResetSerial != m_key.explicitResetSerial)
        invalidation = invalidation
            | WaterCausticHistoryInvalidation::ExplicitReset;

    m_key = key;
    m_lastInvalidation = invalidation;
    if (invalidation != WaterCausticHistoryInvalidation::None)
        ++m_version;
    m_valid = true;
    return invalidation != WaterCausticHistoryInvalidation::None;
}

void WaterCausticHistoryState::Reset() noexcept
{
    m_valid = false;
    m_lastInvalidation = WaterCausticHistoryInvalidation::ExplicitReset;
    ++m_version;
}
} // namespace Prism::Renderer
