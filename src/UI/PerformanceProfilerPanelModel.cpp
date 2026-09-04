#include "UI/PerformanceProfilerPanelModel.h"

#include <algorithm>

namespace Prism::UI
{
bool PerformanceProfilerPanelModel::ShouldReadCompletedFrame(
    const bool panelOpen,
    const bool panelExpanded) noexcept
{
    return panelOpen && panelExpanded;
}

bool PerformanceProfilerPanelModel::ShouldReadHistory(
    const bool panelOpen,
    const bool panelExpanded,
    const bool timelineExpanded) noexcept
{
    return panelOpen && panelExpanded && timelineExpanded;
}

PerformanceProfilerTimeline PerformanceProfilerPanelModel::BuildTimeline(
    const std::span<const Core::FrameProfilerSnapshot> history) noexcept
{
    PerformanceProfilerTimeline timeline{};
    timeline.count = std::min(history.size(), timeline.Capacity);
    const std::size_t first = history.size() - timeline.count;
    for (std::size_t index = 0; index < timeline.count; ++index)
    {
        const Core::FrameProfileDuration& duration =
            history[first + index].editorLoop;
        timeline.editorLoopMilliseconds[index] = duration.available
            ? static_cast<float>(duration.milliseconds)
            : 0.0f;
    }
    return timeline;
}
} // namespace Prism::UI
