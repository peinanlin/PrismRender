#pragma once

#include <iosfwd>
#include <vector>

#include <json.hpp>

namespace Prism::Engine
{
class CommandProcessor;
}

namespace Prism::Automation
{
class HarnessTools;

class HarnessRunner
{
public:
    explicit HarnessRunner(HarnessTools* tools = nullptr);

    int Run(
        std::istream& input,
        std::ostream& output,
        Engine::CommandProcessor& processor) const;
    [[nodiscard]] nlohmann::json Execute(
        const nlohmann::json& request,
        Engine::CommandProcessor& processor) const;

    [[nodiscard]] static std::vector<nlohmann::json> ParseCommands(std::istream& input);

private:
    HarnessTools* m_tools = nullptr;
};
} // namespace Prism::Automation
