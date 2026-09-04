#pragma once

#include "RHI/FramePacing.h"

#include <cstdint>
#include <string_view>
#include <vector>

namespace Prism::Core
{
struct ScheduledFramePacingChange
{
    std::uint64_t frame = 0;
    RHI::FramePacingProfile profile =
        RHI::FramePacingProfile::InteractiveSmooth;
};

struct ScheduledWindowResize
{
    std::uint64_t frame = 0;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
};

[[nodiscard]] std::vector<ScheduledFramePacingChange>
ParseFramePacingAutomationSequence(std::string_view value);
[[nodiscard]] std::vector<ScheduledWindowResize>
ParseWindowResizeAutomationSequence(std::string_view value);
} // namespace Prism::Core
