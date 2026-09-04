#pragma once

#include <compare>

namespace Prism::Core
{
// CPU-side absolute world position. GPU-facing code must subtract a nearby
// render origin before converting the value to float.
struct Double3
{
    double x = 0.0;
    double y = 0.0;
    double z = 0.0;

    auto operator<=>(const Double3&) const = default;
};

[[nodiscard]] constexpr Double3 operator+(
    const Double3& left,
    const Double3& right)
{
    return {
        left.x + right.x,
        left.y + right.y,
        left.z + right.z};
}

[[nodiscard]] constexpr Double3 operator-(
    const Double3& left,
    const Double3& right)
{
    return {
        left.x - right.x,
        left.y - right.y,
        left.z - right.z};
}
} // namespace Prism::Core
