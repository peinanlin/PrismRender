#pragma once

#include <stdexcept>
#include <string_view>

namespace Prism::Core
{
enum class PerformanceWindowFocusPolicy { Default, Focused, Unfocused };

// Explicit focus policies are only available to opt-in, visible performance
// runs. Ordinary interactive windows keep the platform default behavior.
inline PerformanceWindowFocusPolicy ParsePerformanceWindowFocusPolicy(
    std::string_view value, bool performanceEnabled, bool headless)
{
    if (value.empty()) return PerformanceWindowFocusPolicy::Default;
    if (value != "default" && value != "focused" && value != "unfocused")
        throw std::invalid_argument("Performance window focus must be default, focused, or unfocused.");
    if (!performanceEnabled)
        throw std::invalid_argument("Performance window focus requires performance sampling.");
    if (value != "default" && headless)
        throw std::invalid_argument("Explicit performance window focus requires a visible window.");
    if (value == "focused") return PerformanceWindowFocusPolicy::Focused;
    if (value == "unfocused") return PerformanceWindowFocusPolicy::Unfocused;
    return PerformanceWindowFocusPolicy::Default;
}

inline bool ParsePerformanceWindowUnfocused(
    std::string_view value, bool performanceEnabled, bool headless)
{
    return ParsePerformanceWindowFocusPolicy(value, performanceEnabled, headless)
        == PerformanceWindowFocusPolicy::Unfocused;
}
} // namespace Prism::Core
