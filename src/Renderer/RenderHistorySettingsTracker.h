#pragma once

#include "Renderer/RenderSettings.h"

#include <cstdint>
#include <optional>

namespace Prism::Renderer
{
struct RenderHistorySettingsUpdate
{
    std::uint64_t revision = 0;
    bool historyInvalidated = false;
};

// Interprets mutable renderer settings at the Renderer boundary and publishes
// only a small monotonic token to Scene. Scene never depends on RenderSettings.
class RenderHistorySettingsTracker final
{
public:
    [[nodiscard]] RenderHistorySettingsUpdate Observe(
        const RenderSettings& settings);
    void Reset() noexcept;

private:
    std::optional<RenderSettings> m_previous;
    std::uint64_t m_revision = 0;
};
} // namespace Prism::Renderer
