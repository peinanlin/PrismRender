#pragma once

#include "Core/Profiling/FrameProfilerSnapshot.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace Prism::Core
{
class ProfilingLevelSchedule
{
public:
    static constexpr std::size_t MaximumSegments = 16;

    struct Segment
    {
        ProfilingLevel level = ProfilingLevel::Basic;
        std::uint64_t frameCount = 0;
    };

    [[nodiscard]] static ProfilingLevelSchedule Parse(
        std::string_view value);

    [[nodiscard]] ProfilingLevel GetLevel(
        std::uint64_t oneBasedFrameId) const;
    [[nodiscard]] std::uint64_t GetTotalFrameCount() const noexcept;
    [[nodiscard]] std::size_t GetSegmentCount() const noexcept;
    [[nodiscard]] const Segment& GetSegment(
        std::size_t index) const;

private:
    std::array<Segment, MaximumSegments> m_segments{};
    std::size_t m_segmentCount = 0;
    std::uint64_t m_totalFrameCount = 0;
};
} // namespace Prism::Core
