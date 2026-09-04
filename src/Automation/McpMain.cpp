#include "Automation/HarnessTools.h"
#include "Automation/McpServer.h"
#include "Engine/CommandSystem.h"

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>

#include <json.hpp>

namespace
{
std::filesystem::path ParseProjectRoot(
    const int argumentCount,
    char** arguments)
{
    std::filesystem::path projectRoot =
        std::filesystem::current_path();
    for (int index = 1; index < argumentCount; ++index)
    {
        const std::string argument(arguments[index]);
        if (argument == "--project-root"
            && index + 1 < argumentCount)
        {
            projectRoot = arguments[++index];
        }
        else
        {
            throw std::runtime_error(
                "Usage: PrismMcpServer [--project-root PATH]");
        }
    }
    return std::filesystem::weakly_canonical(
        std::filesystem::absolute(projectRoot));
}
} // namespace

int main(int argumentCount, char** arguments)
{
    try
    {
        const std::filesystem::path projectRoot =
            ParseProjectRoot(argumentCount, arguments);
        const std::filesystem::path executableDirectory =
            std::filesystem::absolute(arguments[0])
                .lexically_normal()
                .parent_path();
        Prism::Engine::CommandProcessor processor(projectRoot);
        Prism::Automation::HarnessTools tools(
            projectRoot,
            executableDirectory);
        Prism::Automation::McpServer server(tools, processor);

        std::string line;
        while (std::getline(std::cin, line))
        {
            if (line.empty())
            {
                continue;
            }
            nlohmann::json response;
            try
            {
                response = server.HandleMessage(
                    nlohmann::json::parse(line));
            }
            catch (const nlohmann::json::parse_error& error)
            {
                response = {
                    {"jsonrpc", "2.0"},
                    {"id", nullptr},
                    {"error",
                     {{"code", -32700},
                      {"message", error.what()}}}};
            }
            if (!response.is_null())
            {
                std::cout << response.dump() << '\n';
                std::cout.flush();
            }
        }
        return EXIT_SUCCESS;
    }
    catch (const std::exception& exception)
    {
        std::cerr << "PrismMcpServer failed: "
                  << exception.what() << '\n';
        return EXIT_FAILURE;
    }
}
