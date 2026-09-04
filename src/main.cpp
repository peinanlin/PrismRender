#include "Core/ApplicationLauncher.h"
#include "Core/CpuTrace.h"
#include "Core/ProcessDiagnostics.h"

#include <cstdlib>
#include <exception>
#include <iostream>

int main(const int argumentCount, char** arguments)
{
    Prism::Core::CpuTrace::InitializeFromEnvironment();
    Prism::Core::ProcessDiagnostics::Initialize();
    Prism::Core::ProcessDiagnostics::Log(
        "info",
        "process.start",
        "PrismRender process started.",
        {{"argumentCount", argumentCount}});
    try
    {
        const int exitCode =
            Prism::Core::RunApplication(argumentCount, arguments);
        Prism::Core::ProcessDiagnostics::Log(
            "info",
            "process.exit",
            "PrismRender process completed.",
            {{"exitCode", exitCode}});
        Prism::Core::CpuTrace::Flush(true, exitCode);
        return exitCode;
    }
    catch (const std::exception& exception)
    {
        Prism::Core::ProcessDiagnostics::Log(
            "fatal",
            "process.cpp_exception",
            exception.what());
        Prism::Core::ProcessDiagnostics::WriteCrashReport(
            "cpp_exception",
            exception.what(),
            EXIT_FAILURE);
        Prism::Core::CpuTrace::Flush(false, EXIT_FAILURE);
        std::cerr << "PrismRender failed: " << exception.what() << '\n';
        return EXIT_FAILURE;
    }
    catch (...)
    {
        Prism::Core::ProcessDiagnostics::Log(
            "fatal",
            "process.unknown_exception",
            "PrismRender terminated because of an unknown exception.");
        Prism::Core::ProcessDiagnostics::WriteCrashReport(
            "unknown_exception",
            "PrismRender terminated because of an unknown exception.",
            EXIT_FAILURE);
        Prism::Core::CpuTrace::Flush(false, EXIT_FAILURE);
        std::cerr << "PrismRender failed: unknown exception\n";
        return EXIT_FAILURE;
    }
}
