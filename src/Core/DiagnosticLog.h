#pragma once

#include <string_view>

#include <json.hpp>

namespace Prism::Core
{
class DiagnosticLog final
{
public:
    using Sink = void (*)(
        std::string_view level,
        std::string_view event,
        std::string_view message,
        const nlohmann::json& details);

    static void SetSink(Sink sink) noexcept;
    static void Write(
        std::string_view level,
        std::string_view event,
        std::string_view message,
        const nlohmann::json& details = nlohmann::json::object());
};
} // namespace Prism::Core
