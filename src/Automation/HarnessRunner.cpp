#include "Automation/HarnessRunner.h"

#include "Automation/HarnessTools.h"
#include "Engine/CommandSystem.h"

#include <algorithm>
#include <cctype>
#include <sstream>
#include <stdexcept>
#include <string>

namespace Prism::Automation
{
HarnessRunner::HarnessRunner(HarnessTools* tools)
    : m_tools(tools)
{
}

int HarnessRunner::Run(
    std::istream& input,
    std::ostream& output,
    Engine::CommandProcessor& processor) const
{
    bool failed = false;
    for (const nlohmann::json& request : ParseCommands(input))
    {
        nlohmann::json result = Execute(request, processor);
        output << result.dump() << '\n';
        output.flush();
        failed = failed || !result.value("success", false);
    }
    return failed ? 2 : 0;
}

nlohmann::json HarnessRunner::Execute(
    const nlohmann::json& request,
    Engine::CommandProcessor& processor) const
{
    const std::string command = request.is_object()
        ? request.value("command", std::string{})
        : std::string{};
    nlohmann::json result =
        m_tools != nullptr && m_tools->CanHandle(command)
        ? m_tools->Execute(request, processor)
        : processor.Execute(request);
    if (m_tools != nullptr && command == "engine.describe")
    {
        m_tools->AugmentEngineDescription(result);
    }
    return result;
}

std::vector<nlohmann::json> HarnessRunner::ParseCommands(std::istream& input)
{
    std::ostringstream buffer;
    buffer << input.rdbuf();
    const std::string content = buffer.str();
    const auto first = std::ranges::find_if(content, [](const unsigned char character)
    {
        return !std::isspace(character);
    });
    if (first == content.end())
    {
        return {};
    }

    if (*first == '[')
    {
        const nlohmann::json document = nlohmann::json::parse(content);
        if (!document.is_array())
        {
            throw std::runtime_error("Harness command documents must be a JSON array or JSON Lines.");
        }
        return document.get<std::vector<nlohmann::json>>();
    }

    try
    {
        nlohmann::json document = nlohmann::json::parse(content);
        if (document.is_object())
        {
            return {std::move(document)};
        }
    }
    catch (const nlohmann::json::parse_error&)
    {
    }

    std::vector<nlohmann::json> commands;
    std::istringstream lines(content);
    std::string line;
    std::size_t lineNumber = 0;
    while (std::getline(lines, line))
    {
        ++lineNumber;
        if (std::ranges::all_of(line, [](const unsigned char character) { return std::isspace(character); }))
        {
            continue;
        }
        try
        {
            commands.push_back(nlohmann::json::parse(line));
        }
        catch (const nlohmann::json::parse_error& error)
        {
            throw std::runtime_error(
                "Invalid JSON Lines command at line " + std::to_string(lineNumber) + ": " + error.what());
        }
    }
    return commands;
}
} // namespace Prism::Automation
