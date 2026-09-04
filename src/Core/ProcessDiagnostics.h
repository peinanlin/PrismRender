#pragma once

#include <string_view>

#include <json.hpp>

namespace Prism::Core
{
class ProcessDiagnostics
{
public:
    static void Initialize();
    static void SetGraphicsApi(std::string_view api);
    static void Log(
        std::string_view level,
        std::string_view event,
        std::string_view message,
        const nlohmann::json& details = nlohmann::json::object());
    static void WriteCrashReport(
        std::string_view kind,
        std::string_view message,
        int exitCode,
        const nlohmann::json& details = nlohmann::json::object());
};
} // namespace Prism::Core
