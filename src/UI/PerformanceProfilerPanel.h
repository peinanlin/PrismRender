#pragma once

#include "RHI/FramePacing.h"

#include <optional>

namespace Prism::Core
{
class FrameProfiler;
class SceneViewRefreshController;
}

namespace Prism::UI
{
class PerformanceProfilerPanel
{
public:
    [[nodiscard]] std::optional<RHI::FramePacingProfile> Draw(
        Core::FrameProfiler& profiler,
        Core::SceneViewRefreshController& sceneViewRefresh);

private:
    bool m_open = true;
};
} // namespace Prism::UI
