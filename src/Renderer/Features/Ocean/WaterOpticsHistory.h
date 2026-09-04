#pragma once

#include "Renderer/Features/Ocean/WaterOpticsSettings.h"

#include <cstdint>

namespace Prism::Renderer
{
struct WaterOpticsHistoryKey
{
    std::uint32_t width = 0u;
    std::uint32_t height = 0u;
    WaterOpticsQuality quality = WaterOpticsQuality::High;
    std::uint64_t cameraCutVersion = 0u;
    std::uint64_t sceneVersion = 0u;
    std::uint64_t surfaceHistoryVersion = 0u;
    std::uint32_t explicitResetSerial = 0u;

    bool operator==(const WaterOpticsHistoryKey&) const noexcept = default;
};

enum class WaterOpticsHistoryInvalidation : std::uint32_t
{
    None = 0u,
    Initialization = 1u << 0u,
    Extent = 1u << 1u,
    Quality = 1u << 2u,
    CameraCut = 1u << 3u,
    Scene = 1u << 4u,
    SurfaceHistory = 1u << 5u,
    ExplicitReset = 1u << 6u
};

constexpr WaterOpticsHistoryInvalidation operator|(
    WaterOpticsHistoryInvalidation lhs,
    WaterOpticsHistoryInvalidation rhs) noexcept
{
    return static_cast<WaterOpticsHistoryInvalidation>(
        static_cast<std::uint32_t>(lhs)
        | static_cast<std::uint32_t>(rhs));
}

constexpr bool HasHistoryInvalidation(
    WaterOpticsHistoryInvalidation value,
    WaterOpticsHistoryInvalidation flag) noexcept
{
    return (static_cast<std::uint32_t>(value)
        & static_cast<std::uint32_t>(flag)) != 0u;
}

[[nodiscard]] WaterOpticsHistoryInvalidation
ClassifyWaterOpticsHistoryInvalidation(
    const WaterOpticsHistoryKey& previous,
    const WaterOpticsHistoryKey& current) noexcept;

class WaterOpticsHistoryState
{
public:
    [[nodiscard]] bool Update(const WaterOpticsHistoryKey& key) noexcept;
    void Reset() noexcept;

    [[nodiscard]] bool IsValid() const noexcept { return m_valid; }
    [[nodiscard]] std::uint64_t GetVersion() const noexcept
    {
        return m_version;
    }
    [[nodiscard]] WaterOpticsHistoryInvalidation
    GetLastInvalidation() const noexcept
    {
        return m_lastInvalidation;
    }
    [[nodiscard]] const WaterOpticsHistoryKey& GetKey() const noexcept
    {
        return m_key;
    }

private:
    WaterOpticsHistoryKey m_key{};
    std::uint64_t m_version = 0u;
    WaterOpticsHistoryInvalidation m_lastInvalidation =
        WaterOpticsHistoryInvalidation::None;
    bool m_valid = false;
};
} // namespace Prism::Renderer
