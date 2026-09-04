#include "Core/MinidumpSymbolizer.h"

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace
{
struct CommandLineOptions
{
    Prism::Core::MinidumpSymbolizationOptions symbolization;
    std::filesystem::path outputPath;
};

std::string ReadValue(
    const int argumentCount,
    char** arguments,
    int& index,
    const std::string_view option)
{
    const std::string argument = arguments[index];
    const std::string prefix = std::string(option) + '=';
    if (argument.starts_with(prefix))
    {
        return argument.substr(prefix.size());
    }
    if (argument == option && index + 1 < argumentCount)
    {
        return arguments[++index];
    }
    throw std::runtime_error(
        std::string(option) + " requires a value.");
}

std::size_t ReadMaximumFrames(const std::string& value)
{
    std::size_t parsed = 0;
    const unsigned long count = std::stoul(value, &parsed);
    if (parsed != value.size() || count == 0 || count > 1024)
    {
        throw std::runtime_error(
            "--max-frames must be between 1 and 1024.");
    }
    return static_cast<std::size_t>(count);
}

CommandLineOptions ParseCommandLine(
    const int argumentCount,
    char** arguments)
{
    CommandLineOptions options{};
    for (int index = 1; index < argumentCount; ++index)
    {
        const std::string argument = arguments[index];
        if (argument == "--help" || argument == "-h")
        {
            std::cout
                << "PrismMinidumpSymbolize --dump <file.dmp> "
                   "--exe <program.exe> [--pdb <program.pdb>] "
                   "[--symbol-path <directory>] [--output <report.json>] "
                   "[--max-frames <1..1024>] [--allow-mismatch]\n";
            std::exit(EXIT_SUCCESS);
        }
        if (argument.starts_with("--dump"))
        {
            options.symbolization.dumpPath =
                ReadValue(argumentCount, arguments, index, "--dump");
        }
        else if (argument.starts_with("--exe"))
        {
            options.symbolization.executablePath =
                ReadValue(argumentCount, arguments, index, "--exe");
        }
        else if (argument.starts_with("--pdb"))
        {
            options.symbolization.pdbPath =
                ReadValue(argumentCount, arguments, index, "--pdb");
        }
        else if (argument.starts_with("--symbol-path"))
        {
            options.symbolization.symbolPaths.push_back(
                ReadValue(
                    argumentCount,
                    arguments,
                    index,
                    "--symbol-path"));
        }
        else if (argument.starts_with("--output"))
        {
            options.outputPath =
                ReadValue(argumentCount, arguments, index, "--output");
        }
        else if (argument.starts_with("--max-frames"))
        {
            options.symbolization.maximumFrames =
                ReadMaximumFrames(ReadValue(
                    argumentCount,
                    arguments,
                    index,
                    "--max-frames"));
        }
        else if (argument == "--allow-mismatch")
        {
            options.symbolization.requireIdentityMatch = false;
        }
        else
        {
            throw std::runtime_error(
                "Unknown option: " + argument);
        }
    }
    if (options.symbolization.dumpPath.empty()
        || options.symbolization.executablePath.empty())
    {
        throw std::runtime_error(
            "--dump and --exe are required.");
    }
    return options;
}
} // namespace

int main(const int argumentCount, char** arguments)
{
    try
    {
        const CommandLineOptions options =
            ParseCommandLine(argumentCount, arguments);
        const Prism::Core::MinidumpSymbolizationResult result =
            Prism::Core::SymbolizeMinidump(
                options.symbolization);
        nlohmann::json report = result.report;
        report["success"] = result.success;
        if (!result.success)
        {
            report["error"] = {
                {"code", result.errorCode},
                {"message", result.errorMessage}};
        }
        if (!options.outputPath.empty())
        {
            std::string writeError;
            if (!Prism::Core::WriteMinidumpSymbolizationReport(
                    options.outputPath,
                    result,
                    &writeError))
            {
                throw std::runtime_error(writeError);
            }
        }
        std::cout << report.dump(2) << '\n';
        return result.success
            ? EXIT_SUCCESS
            : EXIT_FAILURE;
    }
    catch (const std::exception& exception)
    {
        std::cerr << nlohmann::json{
            {"format", "PrismMinidumpSymbolization"},
            {"version", 1},
            {"success", false},
            {"error", {
                {"code", "invalid_command_line"},
                {"message", exception.what()}}}}.dump(2)
                  << '\n';
        return EXIT_FAILURE;
    }
}
