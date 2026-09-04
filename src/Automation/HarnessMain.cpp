#include "Automation/HarnessRunner.h"
#include "Automation/HarnessTools.h"
#include "Engine/CommandSystem.h"

#include <Windows.h>

#include <array>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>

namespace
{
struct Options
{
    std::filesystem::path projectRoot = std::filesystem::current_path();
    std::filesystem::path commands;
    std::filesystem::path output;
    std::filesystem::path replay;
    std::filesystem::path journalOut;
    bool help = false;
};

std::string ReadOptionValue(
    const int argumentCount,
    char** arguments,
    int& index,
    const std::string_view name)
{
    const std::string_view argument(arguments[index]);
    const std::string prefix = std::string(name) + '=';
    if (argument.starts_with(prefix))
    {
        return std::string(argument.substr(prefix.size()));
    }
    if (argument == name && index + 1 < argumentCount)
    {
        return arguments[++index];
    }
    return {};
}

Options ParseOptions(const int argumentCount, char** arguments)
{
    Options options;
    for (int index = 1; index < argumentCount; ++index)
    {
        const std::string_view argument(arguments[index]);
        if (argument == "--help" || argument == "-h") options.help = true;
        else if (argument == "--headless") {}
        else if (argument.starts_with("--project-root")) options.projectRoot = ReadOptionValue(argumentCount, arguments, index, "--project-root");
        else if (argument.starts_with("--commands")) options.commands = ReadOptionValue(argumentCount, arguments, index, "--commands");
        else if (argument.starts_with("--output")) options.output = ReadOptionValue(argumentCount, arguments, index, "--output");
        else if (argument.starts_with("--replay")) options.replay = ReadOptionValue(argumentCount, arguments, index, "--replay");
        else if (argument.starts_with("--journal-out")) options.journalOut = ReadOptionValue(argumentCount, arguments, index, "--journal-out");
        else throw std::invalid_argument("Unknown PrismHarness option: " + std::string(argument));
    }
    return options;
}

std::filesystem::path ResolveProjectPath(
    const std::filesystem::path& projectRoot,
    const std::filesystem::path& path)
{
    const std::filesystem::path root = std::filesystem::absolute(projectRoot).lexically_normal();
    const std::filesystem::path candidate = std::filesystem::absolute(
        path.is_absolute() ? path : root / path).lexically_normal();
    auto rootIterator = root.begin();
    auto candidateIterator = candidate.begin();
    for (; rootIterator != root.end(); ++rootIterator, ++candidateIterator)
    {
        if (candidateIterator == candidate.end()
            || _stricmp(rootIterator->string().c_str(), candidateIterator->string().c_str()) != 0)
        {
            throw std::runtime_error("Harness file access is restricted to the project root.");
        }
    }
    return candidate;
}

void PrintHelp()
{
    std::cout
        << "PrismHarness --headless [--project-root <path>] [--commands <json|jsonl>]\n"
        << "             [--output <jsonl>] [--journal-out <json>] [--replay <journal>]\n"
        << "When --commands is omitted, JSON Lines commands are read from standard input.\n";
}

std::filesystem::path GetExecutableDirectory()
{
    std::array<wchar_t, 32768> path{};
    const DWORD length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    if (length == 0 || length >= path.size())
    {
        throw std::runtime_error("Could not resolve the PrismHarness executable path.");
    }
    return std::filesystem::path(std::wstring_view(path.data(), length)).parent_path();
}
} // namespace

int main(const int argumentCount, char** arguments)
{
    try
    {
        const Options options = ParseOptions(argumentCount, arguments);
        if (options.help)
        {
            PrintHelp();
            return EXIT_SUCCESS;
        }

        Prism::Engine::CommandProcessor processor(options.projectRoot);
        if (!options.replay.empty())
        {
            std::string error;
            if (!processor.ReplayJournalFile(options.replay, &error))
            {
                throw std::runtime_error("Journal replay failed: " + error);
            }
        }

        std::unique_ptr<std::ifstream> commandFile;
        std::istream* input = &std::cin;
        if (!options.commands.empty())
        {
            commandFile = std::make_unique<std::ifstream>(
                ResolveProjectPath(options.projectRoot, options.commands), std::ios::binary);
            if (!*commandFile) throw std::runtime_error("Could not open the Harness command file.");
            input = commandFile.get();
        }

        std::unique_ptr<std::ofstream> outputFile;
        std::ostream* output = &std::cout;
        if (!options.output.empty())
        {
            const std::filesystem::path path = ResolveProjectPath(options.projectRoot, options.output);
            if (!path.parent_path().empty()) std::filesystem::create_directories(path.parent_path());
            outputFile = std::make_unique<std::ofstream>(path, std::ios::binary | std::ios::trunc);
            if (!*outputFile) throw std::runtime_error("Could not open the Harness output file.");
            output = outputFile.get();
        }

        Prism::Automation::HarnessTools tools(options.projectRoot, GetExecutableDirectory());
        const Prism::Automation::HarnessRunner runner(&tools);
        const int result = runner.Run(*input, *output, processor);
        if (!options.journalOut.empty())
        {
            std::string error;
            if (!processor.SaveJournal(options.journalOut, &error))
            {
                throw std::runtime_error("Could not save the command journal: " + error);
            }
        }
        return result;
    }
    catch (const std::exception& exception)
    {
        std::cerr << "PrismHarness failed: " << exception.what() << '\n';
        return EXIT_FAILURE;
    }
}
