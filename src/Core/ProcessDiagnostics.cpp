#include "Core/ProcessDiagnostics.h"

#include "Core/BuildSymbolIdentity.h"
#include "Core/CpuTrace.h"
#include "Core/DiagnosticLog.h"

#include <Windows.h>
#include <DbgHelp.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <mutex>
#include <sstream>
#include <string>

namespace Prism::Core
{
namespace
{
using json = nlohmann::json;

std::mutex DiagnosticMutex;
std::atomic<std::uint64_t> Sequence = 0;
std::filesystem::path LogPath;
std::filesystem::path CrashReportPath;
std::filesystem::path MinidumpPath;
std::filesystem::path BuildIdentityPath;
BuildSymbolIdentity CurrentBuildIdentity;
std::string GraphicsApi;
std::string CorrelationId;
EXCEPTION_POINTERS* ActiveException = nullptr;

std::string ReadEnvironmentVariable(const char* name)
{
    char* value = nullptr;
    std::size_t length = 0;
    if (_dupenv_s(&value, &length, name) != 0 || value == nullptr)
    {
        return {};
    }
    std::string result(value);
    std::free(value);
    return result;
}

std::string UtcTimestamp()
{
    const auto now = std::chrono::system_clock::now();
    const auto time = std::chrono::system_clock::to_time_t(now);
    const auto milliseconds =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            now.time_since_epoch()) % 1000;
    std::tm utc{};
    gmtime_s(&utc, &time);
    std::ostringstream output;
    output << std::put_time(&utc, "%Y-%m-%dT%H:%M:%S")
           << '.' << std::setfill('0') << std::setw(3)
           << milliseconds.count() << 'Z';
    return output.str();
}

void EnsureParentDirectory(const std::filesystem::path& path)
{
    if (!path.empty() && !path.parent_path().empty())
    {
        std::filesystem::create_directories(path.parent_path());
    }
}

json SymbolizeAddress(const DWORD64 address)
{
    HANDLE process = GetCurrentProcess();
    std::array<unsigned char, sizeof(SYMBOL_INFO) + MAX_SYM_NAME> storage{};
    auto* symbol = reinterpret_cast<SYMBOL_INFO*>(storage.data());
    symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
    symbol->MaxNameLen = MAX_SYM_NAME;
    DWORD64 displacement = 0;
    json result = {{"address", address}};
    if (SymFromAddr(process, address, &displacement, symbol))
    {
        result["symbol"] = symbol->Name;
        result["symbolDisplacement"] = displacement;
    }
    IMAGEHLP_LINE64 line{};
    line.SizeOfStruct = sizeof(line);
    DWORD lineDisplacement = 0;
    if (SymGetLineFromAddr64(
            process,
            address,
            &lineDisplacement,
            &line))
    {
        result["file"] = line.FileName;
        result["line"] = line.LineNumber;
        result["lineDisplacement"] = lineDisplacement;
    }
    return result;
}

json CaptureSymbolizedStack()
{
    SymSetOptions(
        SYMOPT_DEFERRED_LOADS
        | SYMOPT_LOAD_LINES
        | SYMOPT_UNDNAME);
    HANDLE process = GetCurrentProcess();
    if (!SymInitialize(process, nullptr, TRUE))
    {
        return json::array();
    }
    std::array<void*, 32> frames{};
    const USHORT count = CaptureStackBackTrace(
        2,
        static_cast<DWORD>(frames.size()),
        frames.data(),
        nullptr);
    json stack = json::array();
    for (USHORT index = 0; index < count; ++index)
    {
        stack.push_back(SymbolizeAddress(
            reinterpret_cast<DWORD64>(frames[index])));
    }
    SymCleanup(process);
    return stack;
}

bool WriteMinidump()
{
    if (MinidumpPath.empty())
    {
        return false;
    }
    EnsureParentDirectory(MinidumpPath);
    HANDLE file = CreateFileW(
        MinidumpPath.c_str(),
        GENERIC_WRITE,
        FILE_SHARE_READ,
        nullptr,
        CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL,
        nullptr);
    if (file == INVALID_HANDLE_VALUE)
    {
        return false;
    }
    MINIDUMP_EXCEPTION_INFORMATION exceptionInfo{};
    exceptionInfo.ThreadId = GetCurrentThreadId();
    exceptionInfo.ExceptionPointers = ActiveException;
    exceptionInfo.ClientPointers = FALSE;
    const BOOL written = MiniDumpWriteDump(
        GetCurrentProcess(),
        GetCurrentProcessId(),
        file,
        static_cast<MINIDUMP_TYPE>(
            MiniDumpNormal
            | MiniDumpWithThreadInfo
            | MiniDumpWithUnloadedModules),
        ActiveException != nullptr ? &exceptionInfo : nullptr,
        nullptr,
        nullptr);
    CloseHandle(file);
    return written == TRUE;
}

LONG WINAPI WriteUnhandledExceptionReport(EXCEPTION_POINTERS* exception)
{
    try
    {
        ActiveException = exception;
        json details = {{"threadId", GetCurrentThreadId()}};
        if (exception != nullptr && exception->ExceptionRecord != nullptr)
        {
            details["exceptionCode"] =
                exception->ExceptionRecord->ExceptionCode;
            std::ostringstream address;
            address << exception->ExceptionRecord->ExceptionAddress;
            details["exceptionAddress"] = address.str();
        }
        ProcessDiagnostics::Log(
            "fatal",
            "process.seh_exception",
            "The renderer terminated because of an unhandled structured exception.",
            details);
        ProcessDiagnostics::WriteCrashReport(
            "structured_exception",
            "The renderer terminated because of an unhandled structured exception.",
            EXIT_FAILURE,
            details);
        CpuTrace::Flush(false, EXIT_FAILURE);
        ActiveException = nullptr;
    }
    catch (...)
    {
    }
    return EXCEPTION_EXECUTE_HANDLER;
}
} // namespace

void ProcessDiagnostics::Initialize()
{
    DiagnosticLog::SetSink(&ProcessDiagnostics::Log);
    std::scoped_lock lock(DiagnosticMutex);
    LogPath = ReadEnvironmentVariable("PRISM_RENDER_LOG_PATH");
    CrashReportPath =
        ReadEnvironmentVariable("PRISM_RENDER_CRASH_REPORT_PATH");
    MinidumpPath =
        ReadEnvironmentVariable("PRISM_RENDER_MINIDUMP_PATH");
    BuildIdentityPath =
        ReadEnvironmentVariable(
            "PRISM_RENDER_BUILD_IDENTITY_PATH");
    CorrelationId =
        ReadEnvironmentVariable("PRISM_RENDER_CORRELATION_ID");
    CurrentBuildIdentity = {};
    if (!BuildIdentityPath.empty()
        || !CrashReportPath.empty()
        || !MinidumpPath.empty())
    {
        CurrentBuildIdentity =
            CaptureBuildSymbolIdentity();
        if (!BuildIdentityPath.empty())
        {
            WriteBuildSymbolIdentity(
                BuildIdentityPath,
                CurrentBuildIdentity);
        }
    }
    SetUnhandledExceptionFilter(WriteUnhandledExceptionReport);
}

void ProcessDiagnostics::SetGraphicsApi(const std::string_view api)
{
    std::scoped_lock lock(DiagnosticMutex);
    GraphicsApi = api;
}

void ProcessDiagnostics::Log(
    const std::string_view level,
    const std::string_view event,
    const std::string_view message,
    const nlohmann::json& details)
{
    std::scoped_lock lock(DiagnosticMutex);
    if (LogPath.empty())
    {
        return;
    }
    try
    {
        EnsureParentDirectory(LogPath);
        std::ofstream output(
            LogPath,
            std::ios::binary | std::ios::app);
        if (!output)
        {
            return;
        }
        output << json{
            {"format", "PrismProcessLogEvent"},
            {"version", 1},
            {"sequence", Sequence.fetch_add(1)},
            {"timestampUtc", UtcTimestamp()},
            {"processId", GetCurrentProcessId()},
            {"threadId", GetCurrentThreadId()},
            {"level", level},
            {"event", event},
            {"message", message},
            {"graphicsApi", GraphicsApi},
            {"correlationId", CorrelationId},
            {"details", details}}.dump() << '\n';
        output.flush();
    }
    catch (...)
    {
    }
}

void ProcessDiagnostics::WriteCrashReport(
    const std::string_view kind,
    const std::string_view message,
    const int exitCode,
    const nlohmann::json& details)
{
    std::scoped_lock lock(DiagnosticMutex);
    if (CrashReportPath.empty())
    {
        return;
    }
    try
    {
        EnsureParentDirectory(CrashReportPath);
        const bool minidumpWritten = WriteMinidump();
        json symbolizedStack = CaptureSymbolizedStack();
        json exceptionSymbol = json::object();
        if (ActiveException != nullptr
            && ActiveException->ExceptionRecord != nullptr)
        {
            SymSetOptions(
                SYMOPT_DEFERRED_LOADS
                | SYMOPT_LOAD_LINES
                | SYMOPT_UNDNAME);
            if (SymInitialize(GetCurrentProcess(), nullptr, TRUE))
            {
                exceptionSymbol = SymbolizeAddress(
                    reinterpret_cast<DWORD64>(
                        ActiveException->ExceptionRecord
                            ->ExceptionAddress));
                SymCleanup(GetCurrentProcess());
            }
        }
        const std::filesystem::path temporary =
            CrashReportPath.parent_path()
            / (CrashReportPath.filename().string() + ".tmp");
        {
            std::ofstream output(
                temporary,
                std::ios::binary | std::ios::trunc);
            if (!output)
            {
                return;
            }
            output << std::setw(2) << json{
                {"format", "PrismCrashReport"},
                {"version", 2},
                {"timestampUtc", UtcTimestamp()},
                {"processId", GetCurrentProcessId()},
                {"threadId", GetCurrentThreadId()},
                {"graphicsApi", GraphicsApi},
                {"correlationId", CorrelationId},
                {"kind", kind},
                {"message", message},
                {"exitCode", exitCode},
                {"minidumpPath", MinidumpPath.generic_string()},
                {"minidumpWritten", minidumpWritten},
                {"buildIdentityPath",
                 BuildIdentityPath.generic_string()},
                {"buildIdentity",
                 SerializeBuildSymbolIdentity(
                     CurrentBuildIdentity)},
                {"exceptionSymbol", std::move(exceptionSymbol)},
                {"stack", std::move(symbolizedStack)},
                {"details", details}} << '\n';
        }
        if (std::filesystem::exists(CrashReportPath))
        {
            std::filesystem::remove(CrashReportPath);
        }
        std::filesystem::rename(temporary, CrashReportPath);
    }
    catch (...)
    {
    }
}
} // namespace Prism::Core
