#include "Core/Application/FramePacingAutomation.h"

#include <charconv>
#include <stdexcept>
#include <string>

namespace Prism::Core
{
namespace
{
std::uint64_t ParsePositiveFrame(
    const std::string_view value,
    const char* field)
{
    std::uint64_t result = 0;
    const auto [end, error] = std::from_chars(
        value.data(), value.data() + value.size(), result);
    if (value.empty() || error != std::errc{}
        || end != value.data() + value.size() || result == 0)
    {
        throw std::invalid_argument(
            std::string(field) + " requires a positive frame number.");
    }
    return result;
}

std::uint32_t ParseDimension(
    const std::string_view value,
    const char* field)
{
    std::uint32_t result = 0;
    const auto [end, error] = std::from_chars(
        value.data(), value.data() + value.size(), result);
    if (value.empty() || error != std::errc{}
        || end != value.data() + value.size()
        || result < 64 || result > 8192)
    {
        throw std::invalid_argument(
            std::string(field) + " must be an integer from 64 to 8192.");
    }
    return result;
}

template<class Callback>
void ForEachEntry(const std::string_view value, Callback callback)
{
    std::size_t begin = 0;
    while (begin < value.size())
    {
        const std::size_t end = value.find(',', begin);
        const std::string_view entry = value.substr(
            begin,
            end == std::string_view::npos
                ? value.size() - begin
                : end - begin);
        if (entry.empty())
        {
            throw std::invalid_argument(
                "Automation sequences cannot contain empty entries.");
        }
        callback(entry);
        if (end == std::string_view::npos)
        {
            break;
        }
        if (end + 1 == value.size())
        {
            throw std::invalid_argument(
                "Automation sequences cannot contain empty entries.");
        }
        begin = end + 1;
    }
}
} // namespace

std::vector<ScheduledFramePacingChange>
ParseFramePacingAutomationSequence(const std::string_view value)
{
    std::vector<ScheduledFramePacingChange> result;
    ForEachEntry(value, [&result](const std::string_view entry)
    {
        const std::size_t separator = entry.find(':');
        if (separator == std::string_view::npos
            || entry.find(':', separator + 1) != std::string_view::npos)
        {
            throw std::invalid_argument(
                "Frame-pacing sequence entries must be frame:profile.");
        }
        ScheduledFramePacingChange change{
            ParsePositiveFrame(entry.substr(0, separator),
                "Frame-pacing sequence"),
            RHI::ParseFramePacingProfile(entry.substr(separator + 1))};
        if (change.profile == RHI::FramePacingProfile::Custom)
        {
            throw std::invalid_argument(
                "Frame-pacing automation accepts presets only.");
        }
        if ((!result.empty() && change.frame <= result.back().frame)
            || change.frame == 1)
        {
            throw std::invalid_argument(
                "Frame-pacing automation frames must increase and begin after frame 1.");
        }
        result.push_back(change);
    });
    return result;
}

std::vector<ScheduledWindowResize>
ParseWindowResizeAutomationSequence(const std::string_view value)
{
    std::vector<ScheduledWindowResize> result;
    ForEachEntry(value, [&result](const std::string_view entry)
    {
        const std::size_t separator = entry.find(':');
        const std::size_t extentSeparator = entry.find('x', separator + 1);
        if (separator == std::string_view::npos
            || extentSeparator == std::string_view::npos
            || entry.find(':', separator + 1) != std::string_view::npos
            || entry.find('x', extentSeparator + 1) != std::string_view::npos)
        {
            throw std::invalid_argument(
                "Resize sequence entries must be frame:WIDTHxHEIGHT.");
        }
        ScheduledWindowResize resize{
            ParsePositiveFrame(entry.substr(0, separator),
                "Resize sequence"),
            ParseDimension(entry.substr(
                separator + 1,
                extentSeparator - separator - 1), "Resize width"),
            ParseDimension(entry.substr(extentSeparator + 1),
                "Resize height")};
        if ((!result.empty() && resize.frame <= result.back().frame)
            || resize.frame == 1)
        {
            throw std::invalid_argument(
                "Resize automation frames must increase and begin after frame 1.");
        }
        result.push_back(resize);
    });
    return result;
}
} // namespace Prism::Core
