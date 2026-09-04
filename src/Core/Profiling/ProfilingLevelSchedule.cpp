#include "Core/Profiling/ProfilingLevelSchedule.h"

#include <charconv>
#include <limits>
#include <stdexcept>

namespace Prism::Core
{
ProfilingLevelSchedule ProfilingLevelSchedule::Parse(
    const std::string_view value)
{
    if (value.empty())
    {
        throw std::invalid_argument(
            "Profiling level sequence must not be empty.");
    }

    ProfilingLevelSchedule result;
    std::size_t cursor = 0;
    while (cursor < value.size())
    {
        if (result.m_segmentCount >= MaximumSegments)
        {
            throw std::invalid_argument(
                "Profiling level sequence contains too many segments.");
        }
        const std::size_t comma = value.find(',', cursor);
        const std::size_t end = comma == std::string_view::npos
            ? value.size()
            : comma;
        const std::string_view token = value.substr(
            cursor, end - cursor);
        const std::size_t colon = token.find(':');
        if (colon == std::string_view::npos
            || colon == 0
            || colon + 1 >= token.size()
            || token.find(':', colon + 1)
                != std::string_view::npos)
        {
            throw std::invalid_argument(
                "Profiling level sequence entries must use level:frames.");
        }

        const ProfilingLevel level =
            ParseProfilingLevel(token.substr(0, colon));
        std::uint64_t frameCount = 0;
        const std::string_view countText = token.substr(colon + 1);
        const auto parseResult = std::from_chars(
            countText.data(),
            countText.data() + countText.size(),
            frameCount);
        if (parseResult.ec != std::errc{}
            || parseResult.ptr != countText.data() + countText.size()
            || frameCount == 0)
        {
            throw std::invalid_argument(
                "Profiling level sequence frame counts must be positive integers.");
        }
        if (frameCount
            > std::numeric_limits<std::uint64_t>::max()
                - result.m_totalFrameCount)
        {
            throw std::overflow_error(
                "Profiling level sequence frame count overflowed.");
        }
        result.m_segments[result.m_segmentCount++] = {
            level, frameCount};
        result.m_totalFrameCount += frameCount;

        if (comma == std::string_view::npos)
        {
            break;
        }
        cursor = comma + 1;
        if (cursor == value.size())
        {
            throw std::invalid_argument(
                "Profiling level sequence must not end with a comma.");
        }
    }
    return result;
}

ProfilingLevel ProfilingLevelSchedule::GetLevel(
    const std::uint64_t oneBasedFrameId) const
{
    if (oneBasedFrameId == 0
        || oneBasedFrameId > m_totalFrameCount)
    {
        throw std::out_of_range(
            "Profiling schedule frame ID is outside the configured range.");
    }
    std::uint64_t lastFrame = 0;
    for (std::size_t index = 0; index < m_segmentCount; ++index)
    {
        lastFrame += m_segments[index].frameCount;
        if (oneBasedFrameId <= lastFrame)
        {
            return m_segments[index].level;
        }
    }
    throw std::logic_error(
        "Profiling schedule failed to resolve an in-range frame.");
}

std::uint64_t ProfilingLevelSchedule::GetTotalFrameCount() const noexcept
{
    return m_totalFrameCount;
}

std::size_t ProfilingLevelSchedule::GetSegmentCount() const noexcept
{
    return m_segmentCount;
}

const ProfilingLevelSchedule::Segment&
ProfilingLevelSchedule::GetSegment(const std::size_t index) const
{
    if (index >= m_segmentCount)
    {
        throw std::out_of_range(
            "Profiling schedule segment index is out of range.");
    }
    return m_segments[index];
}
} // namespace Prism::Core
