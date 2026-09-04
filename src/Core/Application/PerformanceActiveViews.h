#pragma once

#include <stdexcept>
#include <string_view>

namespace Prism::Core
{
// This diagnostic policy is deliberately unavailable to ordinary runs. It
// isolates Editor shell/UI cost from the second Scene-view renderer without
// changing the Game renderer, feature set, or submitted scene.
inline bool ParsePerformanceGameOnly(
    std::string_view value, bool performanceEnabled)
{
    if (value.empty()) return false;
    if (value != "game")
        throw std::invalid_argument(
            "Performance active views must be game.");
    if (!performanceEnabled)
        throw std::invalid_argument(
            "Performance active views require performance sampling.");
    return true;
}
} // namespace Prism::Core
