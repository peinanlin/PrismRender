#include "Core/BuildSymbolIdentity.h"
#include "Core/MinidumpSymbolizer.h"

#include <Windows.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace
{
using json = nlohmann::json;

void Expect(const bool condition, const std::string& message)
{
    if (!condition)
    {
        throw std::runtime_error(message);
    }
}

class EnvironmentVariableGuard
{
public:
    EnvironmentVariableGuard(
        const wchar_t* name,
        const std::wstring& value)
        : m_name(name)
    {
        const DWORD required =
            GetEnvironmentVariableW(
                m_name.c_str(),
                nullptr,
                0);
        if (required > 0)
        {
            std::vector<wchar_t> buffer(required);
            GetEnvironmentVariableW(
                m_name.c_str(),
                buffer.data(),
                required);
            m_previous = std::wstring(buffer.data());
        }
        if (!SetEnvironmentVariableW(
                m_name.c_str(),
                value.c_str()))
        {
            throw std::runtime_error(
                "Could not configure the crash fixture environment.");
        }
    }

    ~EnvironmentVariableGuard()
    {
        SetEnvironmentVariableW(
            m_name.c_str(),
            m_previous.has_value()
                ? m_previous->c_str()
                : nullptr);
    }

private:
    std::wstring m_name;
    std::optional<std::wstring> m_previous;
};

json ReadJson(const std::filesystem::path& path)
{
    std::ifstream input(path, std::ios::binary);
    Expect(
        static_cast<bool>(input),
        "A diagnostics JSON artifact was not created.");
    json result;
    input >> result;
    return result;
}

DWORD RunCrashFixture(
    const std::filesystem::path& fixture)
{
    std::wstring commandLine =
        L"\"" + fixture.wstring() + L"\"";
    std::vector<wchar_t> mutableCommand(
        commandLine.begin(),
        commandLine.end());
    mutableCommand.push_back(L'\0');

    STARTUPINFOW startupInfo{};
    startupInfo.cb = sizeof(startupInfo);
    PROCESS_INFORMATION processInfo{};
    const BOOL started = CreateProcessW(
        fixture.c_str(),
        mutableCommand.data(),
        nullptr,
        nullptr,
        FALSE,
        CREATE_NO_WINDOW,
        nullptr,
        fixture.parent_path().c_str(),
        &startupInfo,
        &processInfo);
    Expect(
        started == TRUE,
        "Could not start the crash fixture.");

    const DWORD waitResult =
        WaitForSingleObject(processInfo.hProcess, 30000);
    if (waitResult == WAIT_TIMEOUT)
    {
        TerminateProcess(processInfo.hProcess, 3);
        WaitForSingleObject(processInfo.hProcess, 5000);
    }
    DWORD exitCode = 0;
    GetExitCodeProcess(processInfo.hProcess, &exitCode);
    CloseHandle(processInfo.hThread);
    CloseHandle(processInfo.hProcess);
    Expect(
        waitResult == WAIT_OBJECT_0,
        "The crash fixture did not terminate normally.");
    return exitCode;
}
} // namespace

int main(const int argumentCount, char** arguments)
{
    try
    {
        Expect(
            argumentCount == 4,
            "Expected crash fixture, matching PDB, and mismatching PDB paths.");
        const std::filesystem::path fixture =
            std::filesystem::absolute(arguments[1]);
        const std::filesystem::path matchingPdb =
            std::filesystem::absolute(arguments[2]);
        const std::filesystem::path mismatchingPdb =
            std::filesystem::absolute(arguments[3]);
        Expect(
            std::filesystem::is_regular_file(fixture)
                && std::filesystem::is_regular_file(matchingPdb)
                && std::filesystem::is_regular_file(mismatchingPdb),
            "A Minidump symbolization test binary is missing.");

        const std::filesystem::path artifactDirectory =
            std::filesystem::current_path()
            / "automation/tests/minidump-symbolizer";
        std::filesystem::create_directories(
            artifactDirectory);
        const std::filesystem::path logPath =
            artifactDirectory / "fixture.jsonl";
        const std::filesystem::path crashPath =
            artifactDirectory / "fixture.crash.json";
        const std::filesystem::path dumpPath =
            artifactDirectory / "fixture.dmp";
        const std::filesystem::path identityPath =
            artifactDirectory / "fixture.build.json";
        const std::filesystem::path symbolizedPath =
            artifactDirectory / "fixture.symbolized.json";
        for (const std::filesystem::path& path : {
                 logPath,
                 crashPath,
                 dumpPath,
                 identityPath,
                 symbolizedPath})
        {
            std::filesystem::remove(path);
        }

        EnvironmentVariableGuard log(
            L"PRISM_RENDER_LOG_PATH",
            logPath.wstring());
        EnvironmentVariableGuard crash(
            L"PRISM_RENDER_CRASH_REPORT_PATH",
            crashPath.wstring());
        EnvironmentVariableGuard dump(
            L"PRISM_RENDER_MINIDUMP_PATH",
            dumpPath.wstring());
        EnvironmentVariableGuard identity(
            L"PRISM_RENDER_BUILD_IDENTITY_PATH",
            identityPath.wstring());
        EnvironmentVariableGuard correlation(
            L"PRISM_RENDER_CORRELATION_ID",
            L"minidump-symbolizer-test");

        const DWORD fixtureExitCode =
            RunCrashFixture(fixture);
        Expect(
            fixtureExitCode != 0,
            "The crash fixture unexpectedly exited successfully.");
        Expect(
            std::filesystem::is_regular_file(dumpPath),
            "The crash fixture did not create a Minidump.");

        const json crashReport = ReadJson(crashPath);
        Expect(
            crashReport.value("format", std::string{})
                    == "PrismCrashReport"
                && crashReport.value("version", 0u) == 2u
                && crashReport.value("minidumpWritten", false)
                && crashReport.at("buildIdentity")
                    .value("valid", false),
            "The Crash Report does not contain a valid Build/PDB identity.");
        const json identityReport = ReadJson(identityPath);
        Expect(
            identityReport.value("format", std::string{})
                    == "PrismBuildSymbolIdentity"
                && identityReport.value("valid", false)
                && identityReport.value(
                    "pdbMatchesExecutable",
                    false),
            "The startup Build/PDB identity artifact is invalid.");

        const Prism::Core::BuildSymbolIdentity buildIdentity =
            Prism::Core::CaptureBuildSymbolIdentity(
                fixture,
                matchingPdb);
        Expect(
            buildIdentity.valid
                && buildIdentity.pdbMatchesExecutable,
            "The matching executable and PDB were not recognized.");

        Prism::Core::MinidumpSymbolizationOptions options{};
        options.dumpPath = dumpPath;
        options.executablePath = fixture;
        options.pdbPath = matchingPdb;
        const Prism::Core::MinidumpSymbolizationResult symbolized =
            Prism::Core::SymbolizeMinidump(options);
        Expect(
            symbolized.success,
            "Offline Minidump symbolization failed: "
                + symbolized.errorMessage);
        Expect(
            symbolized.report.at("identity")
                    .at("matches").get<bool>()
                && symbolized.report.at("stack").size() > 1,
            "The symbolized report did not confirm identity or stack data.");

        bool foundFixtureFrame = false;
        bool foundUnwoundFrame = false;
        for (const json& frame :
             symbolized.report.at("stack"))
        {
            if (frame.value("symbol", std::string{})
                    .find("TriggerTestCrash")
                != std::string::npos)
            {
                foundFixtureFrame = true;
            }
            const std::string unwindMethod =
                frame.value(
                    "unwindMethod",
                    std::string{});
            foundUnwoundFrame =
                foundUnwoundFrame
                || unwindMethod == "stack_walk"
                || unwindMethod == "stack_scan";
        }
        Expect(
            foundFixtureFrame && foundUnwoundFrame,
            "The offline stack does not contain the fault frame and an unwound caller frame.");

        std::string writeError;
        Expect(
            Prism::Core::WriteMinidumpSymbolizationReport(
                symbolizedPath,
                symbolized,
                &writeError),
            "Could not write the offline symbolization report: "
                + writeError);
        Expect(
            ReadJson(symbolizedPath)
                .value("success", false),
            "The saved offline symbolization report is invalid.");

        options.pdbPath = mismatchingPdb;
        const Prism::Core::MinidumpSymbolizationResult mismatch =
            Prism::Core::SymbolizeMinidump(options);
        Expect(
            !mismatch.success
                && mismatch.errorCode
                    == "symbol_identity_mismatch"
                && !mismatch.report.at("identity")
                    .at("matches").get<bool>(),
            "A mismatching PDB was not rejected.");

        std::cout
            << "Prism Minidump symbolizer tests passed.\n";
        return EXIT_SUCCESS;
    }
    catch (const std::exception& exception)
    {
        std::cerr
            << "Prism Minidump symbolizer tests failed: "
            << exception.what() << '\n';
        return EXIT_FAILURE;
    }
}
