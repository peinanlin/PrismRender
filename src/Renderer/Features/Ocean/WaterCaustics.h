#pragma once

#include "Renderer/Features/Ocean/WaterOpticsSettings.h"

#include <DirectXMath.h>

#include <array>
#include <cstdint>

namespace Prism::Renderer
{
struct WaterCausticCascadeTransform
{
    DirectX::XMFLOAT2 centerMeters{};
    float coverageMeters = 1.0f;
    float texelSizeMeters = 1.0f;
    DirectX::XMFLOAT4 worldToUv{1.0f, 1.0f, 0.5f, 0.5f};
};

struct WaterCausticCascadeSet
{
    std::array<WaterCausticCascadeTransform, 2> cascades{};
    std::uint32_t resolution = 1u;
    float edgeFadeFraction = 0.12f;
};

struct WaterCausticCascadeWeights
{
    float nearWeight = 0.0f;
    float middleWeight = 0.0f;
    float coverageWeight = 0.0f;
};

[[nodiscard]] WaterCausticCascadeSet BuildWaterCausticCascadeSet(
    const DirectX::XMFLOAT2& cameraPositionMeters,
    const WaterCausticsSettings& settings) noexcept;

[[nodiscard]] WaterCausticCascadeWeights EvaluateWaterCausticCascadeWeights(
    const DirectX::XMFLOAT2& worldPositionMeters,
    const WaterCausticCascadeSet& cascades) noexcept;

enum class WaterCausticHistoryInvalidation : std::uint32_t
{
    None = 0u,
    FirstFrame = 1u << 0u,
    Resources = 1u << 1u,
    CameraJump = 1u << 2u,
    SurfaceHistory = 1u << 3u,
    ExplicitReset = 1u << 4u
};

struct WaterCausticHistoryKey
{
    WaterCausticCascadeSet cascades{};
    std::uint64_t surfaceHistoryVersion = 0u;
    std::uint32_t explicitResetSerial = 0u;
};

class WaterCausticHistoryState
{
public:
    bool Update(const WaterCausticHistoryKey& key) noexcept;
    void Reset() noexcept;

    [[nodiscard]] bool IsValid() const noexcept { return m_valid; }
    [[nodiscard]] std::uint64_t GetVersion() const noexcept
    {
        return m_version;
    }
    [[nodiscard]] WaterCausticHistoryInvalidation
    GetLastInvalidation() const noexcept
    {
        return m_lastInvalidation;
    }

private:
    WaterCausticHistoryKey m_key{};
    std::uint64_t m_version = 0u;
    WaterCausticHistoryInvalidation m_lastInvalidation =
        WaterCausticHistoryInvalidation::None;
    bool m_valid = false;
};
} // namespace Prism::Renderer
