#pragma once

#include "Core/Profiling/FrameProfilerSnapshot.h"

#include <array>
#include <cstddef>
#include <span>

namespace Prism::UI
{
struct PerformanceProfilerTimeline
{
    static constexpr std::size_t Capacity = 240;

    std::array<float, Capacity> editorLoopMilliseconds{};
    std::size_t count = 0;
};

class PerformanceProfilerPanelModel
{
public:
    [[nodiscard]] static bool ShouldReadCompletedFrame(
        bool panelOpen,
        bool panelExpanded) noexcept;
    [[nodiscard]] static bool ShouldReadHistory(
        bool panelOpen,
        bool panelExpanded,
        bool timelineExpanded) noexcept;
    [[nodiscard]] static PerformanceProfilerTimeline BuildTimeline(
        std::span<const Core::FrameProfilerSnapshot> history) noexcept;
};
} // namespace Prism::UI
