#pragma once

#include "Automation/HarnessRunner.h"

#include <cstdint>

#include <json.hpp>

namespace Prism::Automation
{
class HarnessTools;
}

namespace Prism::Engine
{
class CommandProcessor;
}

namespace Prism::Automation
{
class McpServer
{
public:
    McpServer(
        HarnessTools& tools,
        Engine::CommandProcessor& processor);

    [[nodiscard]] nlohmann::json HandleMessage(
        const nlohmann::json& message);

private:
    [[nodiscard]] nlohmann::json HandleToolCall(
        const nlohmann::json& params);
    [[nodiscard]] static nlohmann::json ToolList();
    [[nodiscard]] static nlohmann::json Success(
        const nlohmann::json& id,
        nlohmann::json result);
    [[nodiscard]] static nlohmann::json Error(
        const nlohmann::json& id,
        int code,
        std::string message);

    HarnessRunner m_runner;
    Engine::CommandProcessor& m_processor;
    std::uint64_t m_requestCounter = 0;
};
} // namespace Prism::Automation
