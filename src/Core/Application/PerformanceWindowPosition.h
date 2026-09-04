#pragma once

#include <charconv>
#include <optional>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace Prism::Core
{
// Opt-in benchmark input only. Ordinary window placement remains unchanged.
inline std::optional<std::pair<int, int>> ParsePerformanceWindowPosition(std::string_view x, std::string_view y)
{
    if (x.empty() && y.empty()) return std::nullopt;
    auto parse = [](std::string_view text) {
        if (text.empty()) throw std::invalid_argument("Performance window position requires both coordinates.");
        int value = 0;
        const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
        if (result.ec != std::errc{} || result.ptr != text.data() + text.size() || value < -32768 || value > 32767)
            throw std::invalid_argument("Performance window position requires a pair of signed integers in [-32768,32767].");
        return value;
    };
    return std::pair{parse(x), parse(y)};
}
} // namespace Prism::Core
