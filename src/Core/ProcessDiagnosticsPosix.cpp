#include "Core/ProcessDiagnostics.h"

#include "Core/Environment.h"
#include "Core/DiagnosticLog.h"

#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>

#if defined(__linux__)
#include <execinfo.h>
#endif

namespace Prism::Core
{
namespace
{
using json = nlohmann::json;

std::mutex DiagnosticMutex;
std::atomic<std::uint64_t> Sequence = 0;
std::filesystem::path LogPath;
std::filesystem::path CrashReportPath;
std::string GraphicsApi;
std::string CorrelationId;

std::uint64_t GetThreadId()
{
    return static_cast<std::uint64_t>(
        std::hash<std::thread::id>{}(
            std::this_thread::get_id()));
}

std::string UtcTimestamp()
{
    const auto now = std::chrono::system_clock::now();
    const auto time = std::chrono::system_clock::to_time_t(now);
    const auto milliseconds =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            now.time_since_epoch()) % 1000;
    std::tm utc{};
    gmtime_r(&time, &utc);
    std::ostringstream output;
    output << std::put_time(&utc, "%Y-%m-%dT%H:%M:%S")
           << '.' << std::setfill('0') << std::setw(3)
           << milliseconds.count() << 'Z';
    return output.str();
}

void EnsureParentDirectory(
    const std::filesystem::path& path)
{
    if (!path.empty() && !path.parent_path().empty())
    {
        std::filesystem::create_directories(
            path.parent_path());
    }
}

json CaptureStack()
{
    json stack = json::array();
#if defined(__linux__)
    void* frames[32]{};
    const int frameCount = backtrace(frames, 32);
    char** symbols = backtrace_symbols(frames, frameCount);
    for (int index = 0; index < frameCount; ++index)
    {
        stack.push_back({
            {"address", reinterpret_cast<std::uintptr_t>(
                frames[index])},
            {"symbol", symbols == nullptr
                ? std::string{}
                : std::string(symbols[index])}});
    }
    std::free(symbols);
#endif
    return stack;
}
} // namespace

void ProcessDiagnostics::Initialize()
{
    DiagnosticLog::SetSink(&ProcessDiagnostics::Log);
    std::scoped_lock lock(DiagnosticMutex);
    LogPath = ReadEnvironmentVariableValue(
        "PRISM_RENDER_LOG_PATH");
    CrashReportPath = ReadEnvironmentVariableValue(
        "PRISM_RENDER_CRASH_REPORT_PATH");
    CorrelationId = ReadEnvironmentVariableValue(
        "PRISM_RENDER_CORRELATION_ID");
}

void ProcessDiagnostics::SetGraphicsApi(
    const std::string_view api)
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
            {"processId", static_cast<std::uint64_t>(getpid())},
            {"threadId", GetThreadId()},
            {"level", level},
            {"event", event},
            {"message", message},
            {"graphicsApi", GraphicsApi},
            {"correlationId", CorrelationId},
            {"details", details}}.dump() << '\n';
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
                {"processId",
                 static_cast<std::uint64_t>(getpid())},
                {"threadId", GetThreadId()},
                {"graphicsApi", GraphicsApi},
                {"correlationId", CorrelationId},
                {"kind", kind},
                {"message", message},
                {"exitCode", exitCode},
                {"minidumpWritten", false},
                {"stack", CaptureStack()},
                {"details", details}} << '\n';
        }
        if (std::filesystem::exists(CrashReportPath))
        {
            std::filesystem::remove(CrashReportPath);
        }
        std::filesystem::rename(
            temporary,
            CrashReportPath);
    }
    catch (...)
    {
    }
}
} // namespace Prism::Core
