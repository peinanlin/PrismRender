#include "Automation/McpServer.h"

#include "Automation/HarnessTools.h"
#include "Engine/CommandSystem.h"

#include <stdexcept>
#include <string>
#include <utility>

namespace Prism::Automation
{
namespace
{
using json = nlohmann::json;
}

McpServer::McpServer(
    HarnessTools& tools,
    Engine::CommandProcessor& processor)
    : m_runner(&tools)
    , m_processor(processor)
{
}

nlohmann::json McpServer::HandleMessage(
    const nlohmann::json& message)
{
    const json id = message.contains("id")
        ? message.at("id")
        : json(nullptr);
    if (!message.is_object()
        || message.value("jsonrpc", std::string{})
            != "2.0")
    {
        return Error(id, -32600, "Invalid JSON-RPC request.");
    }

    const std::string method =
        message.value("method", std::string{});
    if (!message.contains("id"))
    {
        return nullptr;
    }
    if (method == "initialize")
    {
        const json params =
            message.value("params", json::object());
        return Success(id, {
            {"protocolVersion",
             params.value(
                 "protocolVersion",
                 std::string("2025-06-18"))},
            {"capabilities",
             {{"tools", {{"listChanged", false}}}}},
            {"serverInfo",
             {{"name", "PrismRender"},
              {"version", "0.1.0"}}},
            {"instructions",
             "Call prism.describe first, then use prism.execute with an engine or Harness command."}});
    }
    if (method == "ping")
    {
        return Success(id, json::object());
    }
    if (method == "tools/list")
    {
        return Success(id, ToolList());
    }
    if (method == "tools/call")
    {
        try
        {
            return Success(
                id,
                HandleToolCall(
                    message.value(
                        "params",
                        json::object())));
        }
        catch (const std::exception& exception)
        {
            return Error(id, -32602, exception.what());
        }
    }
    return Error(id, -32601, "Method not found: " + method);
}

nlohmann::json McpServer::HandleToolCall(
    const nlohmann::json& params)
{
    const std::string name =
        params.value("name", std::string{});
    const json arguments =
        params.value("arguments", json::object());
    json request;
    if (name == "prism.describe")
    {
        request = {
            {"requestId",
             "mcp-describe-"
                 + std::to_string(++m_requestCounter)},
            {"command", "engine.describe"},
            {"arguments", json::object()}};
    }
    else if (name == "prism.execute")
    {
        const std::string command =
            arguments.value("command", std::string{});
        if (command.empty())
        {
            throw std::runtime_error(
                "prism.execute requires a command.");
        }
        request = {
            {"requestId",
             arguments.value(
                 "requestId",
                 "mcp-request-"
                     + std::to_string(++m_requestCounter))},
            {"command", command},
            {"arguments",
             arguments.value(
                 "arguments",
                 json::object())}};
    }
    else
    {
        throw std::runtime_error(
            "Unknown Prism MCP tool: " + name);
    }

    json result = m_runner.Execute(request, m_processor);
    return {
        {"content",
         json::array({
             {{"type", "text"},
              {"text", result.dump()}}})},
        {"structuredContent", result},
        {"isError", !result.value("success", false)}};
}

nlohmann::json McpServer::ToolList()
{
    return {
        {"tools",
         json::array({
             {
                 {"name", "prism.describe"},
                 {"title", "Describe PrismRender"},
                 {"description",
                  "Returns the current engine command, component, renderer, asset, diagnostics, and automation capabilities."},
                 {"inputSchema",
                  {{"type", "object"},
                   {"properties", json::object()},
                   {"additionalProperties", false}}},
                 {"annotations",
                  {{"readOnlyHint", true},
                   {"idempotentHint", true}}},
             },
             {
                 {"name", "prism.execute"},
                 {"title", "Execute PrismRender Command"},
                 {"description",
                  "Executes one structured PrismRender Engine or Harness command in the persistent deterministic World."},
                 {"inputSchema",
                  {{"type", "object"},
                   {"required", json::array({"command"})},
                   {"properties",
                    {
                        {"requestId",
                         {{"type", "string"},
                          {"description",
                           "Stable idempotency key. Generated when omitted."}}},
                        {"command",
                         {{"type", "string"}}},
                        {"arguments",
                         {{"type", "object"}}},
                    }},
                   {"additionalProperties", false}}},
             }})}};
}

nlohmann::json McpServer::Success(
    const nlohmann::json& id,
    nlohmann::json result)
{
    return {
        {"jsonrpc", "2.0"},
        {"id", id},
        {"result", std::move(result)}};
}

nlohmann::json McpServer::Error(
    const nlohmann::json& id,
    const int code,
    std::string message)
{
    return {
        {"jsonrpc", "2.0"},
        {"id", id},
        {"error",
         {{"code", code},
          {"message", std::move(message)}}}};
}
} // namespace Prism::Automation
