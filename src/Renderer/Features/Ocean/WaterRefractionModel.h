#pragma once

#include <DirectXMath.h>

#include <cstdint>
#include <span>

namespace Prism::Renderer
{
// CPU reference for depth reconstruction (row-vector, untransposed inverse VP).
[[nodiscard]] float ReconstructWaterLinearDepthMeters(
    DirectX::XMFLOAT2 uv, float deviceDepth,
    const DirectX::XMFLOAT4X4& inverseViewProjection,
    DirectX::XMFLOAT3 cameraPosition, DirectX::XMFLOAT3 cameraForward) noexcept;

enum class WaterRefractionFallback : std::uint32_t
{
    None = 0u,
    OffScreen = 1u,
    Foreground = 2u,
    ZeroThickness = 3u,
    NonFinite = 4u,
    NoIntersection = 5u
};

struct WaterApproximateRefractionParameters
{
    float distortionStrength = 0.025f;
    float maximumUvOffset = 0.08f;
    float thicknessOffset = 0.08f;
    float maximumThickness = 80.0f;
    float edgeMargin = 0.0f;
};

struct WaterApproximateRefractionInput
{
    DirectX::XMFLOAT2 uv{0.5f, 0.5f};
    DirectX::XMFLOAT2 screenNormal{};
    float waterDepth = 0.0f;
    float opaqueDepth = 0.0f;
    float candidateOpaqueDepth = 0.0f;
};

struct WaterApproximateRefractionResult
{
    DirectX::XMFLOAT2 sampleUv{0.5f, 0.5f};
    float thickness = 0.0f;
    WaterRefractionFallback fallback = WaterRefractionFallback::NonFinite;

    [[nodiscard]] bool IsValidHit() const noexcept
    {
        return fallback == WaterRefractionFallback::None;
    }
};

[[nodiscard]] WaterApproximateRefractionResult
EvaluateApproximateWaterRefraction(
    const WaterApproximateRefractionInput& input,
    const WaterApproximateRefractionParameters& parameters) noexcept;

struct WaterRayMarchRefractionParameters
{
    std::uint32_t sampleCount = 8u;
    float maximumUvOffset = 0.08f;
    float thicknessOffset = 0.08f;
    float maximumThickness = 80.0f;
    float stepScale = 1.6f;
    float jitter = 0.5f;
    float edgeMargin = 0.0f;
};

struct WaterRayMarchRefractionInput
{
    DirectX::XMFLOAT2 uv{0.5f, 0.5f};
    DirectX::XMFLOAT2 screenDirection{};
    float waterDepth = 0.0f;
};

[[nodiscard]] WaterRayMarchRefractionParameters
NormalizeWaterRayMarchRefractionParameters(
    const WaterRayMarchRefractionParameters& parameters) noexcept;

[[nodiscard]] float WaterRefractionFrameNoise(
    std::uint32_t pixelX,
    std::uint32_t pixelY,
    std::uint32_t frameIndex) noexcept;

[[nodiscard]] WaterApproximateRefractionResult
EvaluateRayMarchedWaterRefraction(
    const WaterRayMarchRefractionInput& input,
    const WaterRayMarchRefractionParameters& parameters,
    std::span<const float> opaqueDepthSamples) noexcept;
} // namespace Prism::Renderer
