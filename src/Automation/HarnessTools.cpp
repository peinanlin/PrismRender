#include "Automation/HarnessTools.h"

#include "Asset/AssetCache.h"
#include "Asset/CookedAssetIO.h"
#include "Asset/AssetDatabase.h"
#include "Asset/AssetStreamingManager.h"
#include "Asset/SlangShaderCompiler.h"
#include "Automation/PerformanceBaseline.h"
#include "Core/MinidumpSymbolizer.h"
#include "Engine/CommandSystem.h"
#include "RHI/ShaderTypes.h"
#include "Tools/GoldenImageComparator.h"

#include <Windows.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <map>
#include <limits>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace Prism::Automation
{
namespace
{
using json = nlohmann::json;

class ToolError final : public std::runtime_error
{
public:
    ToolError(std::string code, std::string message, json details = json::object())
        : std::runtime_error(std::move(message))
        , m_code(std::move(code))
        , m_details(std::move(details))
    {
    }

    [[nodiscard]] const std::string& GetCode() const { return m_code; }
    [[nodiscard]] const json& GetDetails() const { return m_details; }

private:
    std::string m_code;
    json m_details;
};

std::string Lowercase(std::string value)
{
    std::ranges::transform(value, value.begin(), [](const unsigned char character)
    {
        return static_cast<char>(std::tolower(character));
    });
    return value;
}

std::wstring Widen(const std::string& value)
{
    if (value.empty()) return {};
    const int length = MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), nullptr, 0);
    if (length <= 0) throw ToolError("invalid_utf8", "A Harness path or argument is not valid UTF-8.");
    std::wstring result(static_cast<std::size_t>(length), L'\0');
    MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), result.data(), length);
    return result;
}

class EnvironmentVariableGuard
{
public:
    EnvironmentVariableGuard(const wchar_t* name, const std::wstring& value)
        : m_name(name)
    {
        const DWORD required = GetEnvironmentVariableW(m_name.c_str(), nullptr, 0);
        if (required > 0)
        {
            std::vector<wchar_t> buffer(required);
            GetEnvironmentVariableW(m_name.c_str(), buffer.data(), required);
            m_previous = std::wstring(buffer.data());
        }
        if (!SetEnvironmentVariableW(m_name.c_str(), value.c_str()))
        {
            throw ToolError("environment_failed", "Could not configure the renderer child process environment.");
        }
    }

    ~EnvironmentVariableGuard()
    {
        SetEnvironmentVariableW(m_name.c_str(), m_previous.has_value() ? m_previous->c_str() : nullptr);
    }

    EnvironmentVariableGuard(const EnvironmentVariableGuard&) = delete;
    EnvironmentVariableGuard& operator=(const EnvironmentVariableGuard&) = delete;

private:
    std::wstring m_name;
    std::optional<std::wstring> m_previous;
};

std::uint32_t ReadTimeout(const json& arguments)
{
    const std::uint64_t timeout = arguments.value("timeoutMilliseconds", 120000ull);
    if (timeout < 1000ull || timeout > 600000ull)
    {
        throw ToolError("invalid_timeout", "timeoutMilliseconds must be between 1000 and 600000.");
    }
    return static_cast<std::uint32_t>(timeout);
}

std::string ReadGraphicsApi(const json& arguments)
{
    const std::string api = Lowercase(arguments.value("api", std::string("d3d12")));
    if (api != "d3d12" && api != "vulkan")
    {
        throw ToolError("unsupported_api", "Harness rendering tools support d3d12 and vulkan.");
    }
    return api;
}

std::string ReadCaptureStage(const json& arguments)
{
    const std::string stage = Lowercase(arguments.value("stage", std::string("tonemap")));
    static constexpr std::array<std::string_view, 8> Stages = {
        "shadow", "gbuffer0", "gbuffer1", "gbuffer2", "gbuffer3", "hdr", "bloom", "tonemap"};
    if (std::ranges::find(Stages, stage) == Stages.end())
    {
        throw ToolError("unsupported_capture_stage", "Unknown capture stage: " + stage);
    }
    return stage;
}

std::string ReadQueueMode(const json& arguments)
{
    const std::string mode =
        Lowercase(arguments.value(
            "queueMode",
            std::string("auto")));
    if (mode != "auto"
        && mode != "serial"
        && mode != "native")
    {
        throw ToolError(
            "unsupported_queue_mode",
            "queueMode must be auto, serial, or native.");
    }
    return mode;
}

std::uint32_t ReadDimension(
    const json& arguments,
    const char* name,
    const std::uint32_t fallback)
{
    const std::uint64_t value = arguments.value(name, static_cast<std::uint64_t>(fallback));
    if (value < 64 || value > 8192)
    {
        throw ToolError(
            "invalid_render_dimension",
            std::string(name) + " must be between 64 and 8192.");
    }
    return static_cast<std::uint32_t>(value);
}

std::uint32_t ReadAsyncWorkloadMultiplier(
    const json& arguments)
{
    const std::uint64_t value = arguments.value(
        "asyncWorkloadMultiplier",
        1ull);
    if (value < 1 || value > 64)
    {
        throw ToolError(
            "invalid_async_workload_multiplier",
            "asyncWorkloadMultiplier must be between 1 and 64.");
    }
    return static_cast<std::uint32_t>(value);
}

std::string SanitizeArtifactName(const std::string_view value)
{
    std::string result;
    result.reserve(std::min<std::size_t>(value.size(), 96));
    for (const unsigned char character : value)
    {
        if (result.size() == 96)
        {
            break;
        }
        result.push_back(
            std::isalnum(character) || character == '-' || character == '_'
                ? static_cast<char>(character)
                : '_');
    }
    return result.empty() ? "request" : result;
}

struct RendererProcessResult
{
    DWORD exitCode = 0;
    bool timedOut = false;
    std::filesystem::path structuredLogPath;
    std::filesystem::path stdoutPath;
    std::filesystem::path stderrPath;
    std::filesystem::path crashReportPath;
    std::filesystem::path minidumpPath;
    std::filesystem::path cpuTracePath;
    std::filesystem::path performanceIdentityPath;
    std::filesystem::path buildIdentityPath;
    json structuredEvents = json::array();
    std::string stdoutTail;
    std::string stderrTail;
    json crashReport = json::object();
    json cpuTrace = json::object();
    json performanceIdentity = json::object();
    json buildIdentity = json::object();
};

[[noreturn]] void ThrowRendererFailure(
    const RendererProcessResult& process,
    const std::string& api);

std::string ReadTextTail(
    const std::filesystem::path& path,
    const std::size_t maximumBytes = 16 * 1024)
{
    std::ifstream input(path, std::ios::binary);
    if (!input)
    {
        return {};
    }
    input.seekg(0, std::ios::end);
    const std::streamoff size = input.tellg();
    const std::streamoff offset = size > static_cast<std::streamoff>(maximumBytes)
        ? size - static_cast<std::streamoff>(maximumBytes)
        : 0;
    input.seekg(offset, std::ios::beg);
    std::ostringstream output;
    output << input.rdbuf();
    return output.str();
}

json ReadJsonLines(const std::filesystem::path& path)
{
    json events = json::array();
    std::ifstream input(path, std::ios::binary);
    if (!input)
    {
        return events;
    }
    std::string line;
    while (std::getline(input, line))
    {
        if (line.empty())
        {
            continue;
        }
        try
        {
            events.push_back(json::parse(line));
        }
        catch (const json::parse_error&)
        {
            events.push_back({
                {"format", "PrismProcessLogParseError"},
                {"version", 1},
                {"raw", line}});
        }
    }
    return events;
}

json ReadOptionalJson(const std::filesystem::path& path)
{
    std::ifstream input(path, std::ios::binary);
    if (!input)
    {
        return json::object();
    }
    try
    {
        json document;
        input >> document;
        return document;
    }
    catch (const std::exception& exception)
    {
        return {
            {"format", "PrismCrashReportParseError"},
            {"version", 1},
            {"message", exception.what()}};
    }
}

json ReadGpuTimingReport(const std::filesystem::path& path)
{
    std::ifstream input(path, std::ios::binary);
    if (!input)
    {
        throw ToolError(
            "gpu_timing_report_missing",
            "The renderer did not produce the requested GPU timing report.",
            {{"path", path.generic_string()}});
    }
    json report;
    input >> report;
    if (report.value("format", std::string{})
            != "PrismGpuTimingReport"
        || (report.value("version", 0u) != 1u
            && report.value("version", 0u) != 2u))
    {
        throw ToolError(
            "gpu_timing_report_invalid",
            "The renderer produced an invalid GPU timing report.",
            {{"path", path.generic_string()}});
    }
    return report;
}

json ReadCpuTraceReport(
    const std::filesystem::path& path,
    const std::string_view expectedCorrelationId)
{
    std::ifstream input(path, std::ios::binary);
    if (!input)
    {
        throw ToolError(
            "cpu_trace_missing",
            "The renderer did not produce the requested CPU trace.",
            {{"path", path.generic_string()}});
    }
    json report;
    input >> report;
    if (report.value("format", std::string{}) != "PrismCpuTrace"
        || report.value("version", 0u) != 1u
        || !report.contains("spans")
        || !report.at("spans").is_array()
        || !report.contains("summary")
        || !report.at("summary").is_array())
    {
        throw ToolError(
            "cpu_trace_invalid",
            "The renderer produced an invalid CPU trace.",
            {{"path", path.generic_string()}});
    }
    if (report.value("correlationId", std::string{})
        != expectedCorrelationId)
    {
        throw ToolError(
            "cpu_trace_correlation_mismatch",
            "The CPU trace does not belong to this Harness request.",
            {{"path", path.generic_string()},
             {"expectedCorrelationId", expectedCorrelationId},
             {"actualCorrelationId",
              report.value("correlationId", std::string{})}});
    }
    return report;
}

json ReadPerformanceIdentityReport(
    const std::filesystem::path& path,
    const std::string_view expectedApi)
{
    std::ifstream input(path, std::ios::binary);
    if (!input)
    {
        throw ToolError(
            "performance_identity_missing",
            "The renderer did not produce the requested performance identity.",
            {{"path", path.generic_string()}});
    }
    json report;
    input >> report;
    if (report.value("format", std::string{})
            != "PrismRuntimePerformanceIdentity"
        || report.value("version", 0u) != 1u
        || !report.contains("identity")
        || !report.at("identity").is_object())
    {
        throw ToolError(
            "performance_identity_invalid",
            "The renderer produced an invalid performance identity.",
            {{"path", path.generic_string()}});
    }
    const std::string actualApi =
        report.at("identity").value("graphicsApi", std::string{});
    if (actualApi != expectedApi)
    {
        throw ToolError(
            "performance_identity_api_mismatch",
            "The runtime performance identity reports a different graphics API.",
            {{"path", path.generic_string()},
             {"expectedApi", expectedApi},
             {"actualApi", actualApi}});
    }
    return report;
}

json ReadAssetStreamingReport(
    const std::filesystem::path& path,
    const std::string_view api,
    const RendererProcessResult& process)
{
    std::ifstream input(path, std::ios::binary);
    if (!input)
    {
        if (process.exitCode != 0)
        {
            ThrowRendererFailure(process, std::string(api));
        }
        throw ToolError(
            "asset_streaming_report_missing",
            "The renderer did not produce the requested Asset Streaming report.",
            {{"path", path.generic_string()}, {"api", api}});
    }

    json report;
    input >> report;
    if (report.value("format", std::string{})
            != "PrismAssetStreamingReport"
        || report.value("version", 0u) != 1u
        || !report.contains("statistics")
        || !report.at("statistics").is_object()
        || !report.contains("uploadQueue")
        || !report.at("uploadQueue").is_object()
        || !report.contains("entries")
        || !report.at("entries").is_array())
    {
        throw ToolError(
            "asset_streaming_report_invalid",
            "The renderer produced an invalid Asset Streaming report.",
            {{"path", path.generic_string()}, {"api", api}});
    }
    return report;
}

json ReadAssetStreamingSceneReport(
    const std::filesystem::path& path,
    const std::string_view api,
    const RendererProcessResult& process)
{
    std::ifstream input(path, std::ios::binary);
    if (!input)
    {
        if (process.exitCode != 0)
        {
            ThrowRendererFailure(process, std::string(api));
        }
        throw ToolError(
            "asset_streaming_scene_report_missing",
            "The renderer did not produce the requested streamed Scene report.",
            {{"path", path.generic_string()}, {"api", api}});
    }

    json report;
    input >> report;
    if (report.value("format", std::string{})
            != "PrismAssetStreamingSceneReport"
        || report.value("version", 0u) != 1u
        || !report.contains("objects")
        || !report.at("objects").is_array())
    {
        throw ToolError(
            "asset_streaming_scene_report_invalid",
            "The renderer produced an invalid streamed Scene report.",
            {{"path", path.generic_string()}, {"api", api}});
    }
    return report;
}

json ValidateAssetStreamingReports(
    const json& streamingReport,
    const json& sceneReport,
    const std::string_view api)
{
    const json& statistics =
        streamingReport.at("statistics");
    const json& uploadQueue =
        streamingReport.at("uploadQueue");
    const std::uint64_t assetCount =
        statistics.value("assetCount", 0ull);
    const std::uint64_t residentCount =
        statistics.value("residentCount", 0ull);
    const std::uint64_t activatedObjectCount =
        sceneReport.value("activatedObjectCount", 0ull);
    const std::uint64_t streamedBindingCount =
        sceneReport.value("streamedBindingCount", 0ull);

    bool streamedObjectsReady = true;
    std::uint64_t validatedStreamedObjects = 0;
    for (const json& object : sceneReport.at("objects"))
    {
        if (!object.value("streamedBinding", false))
        {
            continue;
        }
        ++validatedStreamedObjects;
        const std::string meshPath =
            object.value("meshPath", std::string{});
        const std::string materialPath =
            object.value("materialPath", std::string{});
        streamedObjectsReady = streamedObjectsReady
            && object.value("meshRuntimeReady", false)
            && object.value("materialRuntimeReady", false)
            && meshPath.starts_with("prism-asset://")
            && materialPath.starts_with("prism-asset://");
    }

    json checks = {
        {"graphicsApiMatched",
         sceneReport.value("graphicsApi", std::string{})
             == (api == "d3d12"
                     ? "Direct3D 12"
                     : "Vulkan")},
        {"assetClosurePresent", assetCount > 0},
        {"allAssetsResident",
         residentCount == assetCount
             && statistics.value("queuedCount", 0ull) == 0
             && statistics.value("loadingCount", 0ull) == 0
             && statistics.value("readyCount", 0ull) == 0
             && statistics.value("uploadingCount", 0ull) == 0
             && statistics.value("failedCount", 0ull) == 0},
        {"gpuUploadsCompleted",
         statistics.value("completedUploadCount", 0ull) > 0
             && statistics.value("completedIoCount", 0ull) > 0
             && uploadQueue.value("stagingPageCount", 0ull) > 0
             && uploadQueue.value("stagingCapacityBytes", 0ull) > 0
             && uploadQueue.value("uploadedBytes", 0ull)
                 >= statistics.value("residentBytes", 0ull)},
        {"ticketDrivenUploadPath",
         uploadQueue.value("submittedBatchCount", 0ull) > 0
             && uploadQueue.value("lastSubmittedTicket", 0ull) > 0
             && uploadQueue.value("completedTicket", 0ull)
                 >= uploadQueue.value("lastSubmittedTicket", 0ull)},
        {"uploadQueueDrained",
         uploadQueue.value("outstandingBatchCount", 0ull) == 0
             && uploadQueue.value("pendingOperationCount", 0ull) == 0
             && uploadQueue.value("pendingBytes", 0ull) == 0},
        {"sceneActivated",
         sceneReport.value("activationAttempted", false)
             && sceneReport.value("activationSucceeded", false)
             && activatedObjectCount > 0},
        {"streamedBindingsComplete",
         streamedBindingCount >= activatedObjectCount
             && validatedStreamedObjects == streamedBindingCount
             && streamedObjectsReady}};

    bool passed = true;
    for (const auto& [name, value] : checks.items())
    {
        (void)name;
        passed = passed && value.get<bool>();
    }
    return {
        {"passed", passed},
        {"checks", std::move(checks)},
        {"assetCount", assetCount},
        {"residentCount", residentCount},
        {"residentBytes",
         statistics.value("residentBytes", 0ull)},
        {"completedUploadCount",
         statistics.value("completedUploadCount", 0ull)},
        {"completedUploadTicket",
         uploadQueue.value("completedTicket", 0ull)},
        {"globalSynchronousFlushCount",
         uploadQueue.value("synchronousFlushCount", 0ull)},
        {"activatedObjectCount", activatedObjectCount},
        {"renderObjectCount",
         sceneReport.value("renderObjectCount", 0ull)},
        {"streamedBindingCount", streamedBindingCount}};
}

void ApplyRuntimePerformanceIdentity(
    const json& runtimeIdentity,
    PerformanceIdentity& identity)
{
    identity.adapterName =
        runtimeIdentity.value("adapterName", std::string{});
    identity.adapterVendorId =
        runtimeIdentity.value("vendorId", 0u);
    identity.adapterDeviceId =
        runtimeIdentity.value("deviceId", 0u);
    identity.dedicatedVideoMemoryBytes =
        runtimeIdentity.value(
            "dedicatedVideoMemoryBytes",
            0ull);
    identity.sharedSystemMemoryBytes =
        runtimeIdentity.value(
            "sharedSystemMemoryBytes",
            0ull);
    identity.driverVersionRaw =
        runtimeIdentity.value("driverVersionRaw", 0ull);
    identity.driverVersion =
        runtimeIdentity.value("driverVersion", std::string{});
    identity.apiVersion =
        runtimeIdentity.value("apiVersion", std::string{});
    identity.cpuName =
        runtimeIdentity.value("cpuName", std::string{});
    identity.buildConfiguration =
        runtimeIdentity.value(
            "buildConfiguration",
            std::string{});
    identity.compiler =
        runtimeIdentity.value("compiler", std::string{});
    identity.architecture =
        runtimeIdentity.value("architecture", std::string{});
    identity.shaderRevision =
        runtimeIdentity.value("shaderRevision", std::string{});
    identity.shaderFileCount =
        runtimeIdentity.value("shaderFileCount", 0u);
    identity.executableHash =
        runtimeIdentity.value("executableHash", std::string{});
}

json SerializeRendererProcess(
    const RendererProcessResult& process,
    const bool includeDiagnosticReports = false)
{
    json result = {
        {"exitCode", process.exitCode},
        {"timedOut", process.timedOut},
        {"structuredLogPath", process.structuredLogPath.generic_string()},
        {"stdoutPath", process.stdoutPath.generic_string()},
        {"stderrPath", process.stderrPath.generic_string()},
        {"crashReportPath", process.crashReportPath.generic_string()},
        {"minidumpPath", process.minidumpPath.generic_string()},
        {"minidumpExists", std::filesystem::is_regular_file(process.minidumpPath)},
        {"cpuTracePath", process.cpuTracePath.generic_string()},
        {"performanceIdentityPath",
         process.performanceIdentityPath.generic_string()},
        {"buildIdentityPath",
         process.buildIdentityPath.generic_string()},
        {"cpuTraceExists",
         std::filesystem::is_regular_file(process.cpuTracePath)},
        {"performanceIdentityExists",
         std::filesystem::is_regular_file(
             process.performanceIdentityPath)},
        {"buildIdentityExists",
         std::filesystem::is_regular_file(
             process.buildIdentityPath)},
        {"structuredEvents", process.structuredEvents},
        {"stdoutTail", process.stdoutTail},
        {"stderrTail", process.stderrTail},
        {"crashReport", process.crashReport}};
    if (includeDiagnosticReports)
    {
        result["cpuTrace"] = process.cpuTrace;
        result["performanceIdentity"] =
            process.performanceIdentity;
        result["buildIdentity"] =
            process.buildIdentity;
    }
    return result;
}

RendererProcessResult RunRendererProcess(
    const std::filesystem::path& renderer,
    const std::filesystem::path& projectRoot,
    const std::string& api,
    const std::uint32_t timeoutMilliseconds,
    const std::filesystem::path& structuredLogPath,
    const std::filesystem::path& stdoutPath,
    const std::filesystem::path& stderrPath,
    const std::filesystem::path& crashReportPath,
    const std::filesystem::path& minidumpPath,
    const std::filesystem::path& cpuTracePath,
    const std::filesystem::path& performanceIdentityPath,
    const std::filesystem::path& buildIdentityPath)
{
    if (!std::filesystem::is_regular_file(renderer))
    {
        throw ToolError("renderer_not_found", "PrismRender.exe was not found beside PrismHarness.exe.");
    }

    std::wstring commandLine = L"\"" + renderer.wstring() + L"\" --api=" + Widen(api);
    std::vector<wchar_t> mutableCommand(commandLine.begin(), commandLine.end());
    mutableCommand.push_back(L'\0');

    SECURITY_ATTRIBUTES security{};
    security.nLength = sizeof(security);
    security.bInheritHandle = TRUE;
    const HANDLE stdoutHandle = CreateFileW(
        stdoutPath.c_str(),
        GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        &security,
        CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL,
        nullptr);
    const HANDLE stderrHandle = CreateFileW(
        stderrPath.c_str(),
        GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        &security,
        CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL,
        nullptr);
    const HANDLE stdinHandle = CreateFileW(
        L"NUL",
        GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        &security,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL,
        nullptr);
    if (stdoutHandle == INVALID_HANDLE_VALUE
        || stderrHandle == INVALID_HANDLE_VALUE
        || stdinHandle == INVALID_HANDLE_VALUE)
    {
        if (stdoutHandle != INVALID_HANDLE_VALUE) CloseHandle(stdoutHandle);
        if (stderrHandle != INVALID_HANDLE_VALUE) CloseHandle(stderrHandle);
        if (stdinHandle != INVALID_HANDLE_VALUE) CloseHandle(stdinHandle);
        throw ToolError(
            "renderer_log_open_failed",
            "Could not create renderer stdout/stderr artifacts.");
    }

    STARTUPINFOW startupInfo{};
    startupInfo.cb = sizeof(startupInfo);
    startupInfo.dwFlags = STARTF_USESTDHANDLES;
    startupInfo.hStdOutput = stdoutHandle;
    startupInfo.hStdError = stderrHandle;
    startupInfo.hStdInput = stdinHandle;
    PROCESS_INFORMATION processInfo{};
    const BOOL started = CreateProcessW(
            renderer.c_str(),
            mutableCommand.data(),
            nullptr,
            nullptr,
            TRUE,
            CREATE_NO_WINDOW,
            nullptr,
            projectRoot.c_str(),
            &startupInfo,
            &processInfo);
    CloseHandle(stdoutHandle);
    CloseHandle(stderrHandle);
    CloseHandle(stdinHandle);
    if (!started)
    {
        throw ToolError("renderer_start_failed", "Could not start PrismRender.exe.");
    }

    RendererProcessResult result{};
    result.structuredLogPath = structuredLogPath;
    result.stdoutPath = stdoutPath;
    result.stderrPath = stderrPath;
    result.crashReportPath = crashReportPath;
    result.minidumpPath = minidumpPath;
    result.cpuTracePath = cpuTracePath;
    result.performanceIdentityPath = performanceIdentityPath;
    result.buildIdentityPath = buildIdentityPath;
    const DWORD waitResult = WaitForSingleObject(processInfo.hProcess, timeoutMilliseconds);
    if (waitResult == WAIT_TIMEOUT)
    {
        TerminateProcess(processInfo.hProcess, 3);
        WaitForSingleObject(processInfo.hProcess, 5000);
        result.exitCode = 3;
        result.timedOut = true;
    }
    else if (waitResult != WAIT_OBJECT_0)
    {
        CloseHandle(processInfo.hThread);
        CloseHandle(processInfo.hProcess);
        throw ToolError("renderer_wait_failed", "Waiting for PrismRender.exe failed.");
    }
    else
    {
        GetExitCodeProcess(processInfo.hProcess, &result.exitCode);
    }
    CloseHandle(processInfo.hThread);
    CloseHandle(processInfo.hProcess);
    result.structuredEvents = ReadJsonLines(structuredLogPath);
    result.stdoutTail = ReadTextTail(stdoutPath);
    result.stderrTail = ReadTextTail(stderrPath);
    result.crashReport = ReadOptionalJson(crashReportPath);
    result.cpuTrace = ReadOptionalJson(cpuTracePath);
    result.performanceIdentity =
        ReadOptionalJson(performanceIdentityPath);
    result.buildIdentity =
        ReadOptionalJson(buildIdentityPath);
    return result;
}

[[noreturn]] void ThrowRendererFailure(
    const RendererProcessResult& process,
    const std::string& api)
{
    const json details = {
        {"api", api},
        {"process", SerializeRendererProcess(process, true)}};
    if (process.timedOut)
    {
        throw ToolError(
            "renderer_timeout",
            "PrismRender.exe exceeded the Harness timeout.",
            details);
    }
    throw ToolError(
        "renderer_failed",
        "PrismRender.exe exited with code " + std::to_string(process.exitCode) + ".",
        details);
}

json ReadRenderedWorldReceipt(
    const std::filesystem::path& path,
    const std::string& expectedWorldHash,
    const std::string& expectedAssetManifestHash,
    const std::string& api,
    const RendererProcessResult& process)
{
    std::ifstream input(path, std::ios::binary);
    if (!input)
    {
        if (process.exitCode != 0)
        {
            ThrowRendererFailure(process, api);
        }
        throw ToolError(
            "world_receipt_missing",
            "The renderer did not produce the requested World receipt.",
            {{"path", path.generic_string()}, {"api", api}});
    }

    json receipt;
    input >> receipt;
    if (receipt.value("format", std::string{}) != "PrismRenderedWorldReceipt"
        || receipt.value("version", 0) != 1)
    {
        throw ToolError(
            "world_receipt_invalid",
            "The renderer produced an invalid World receipt.",
            {{"path", path.generic_string()}, {"api", api}});
    }
    if (!receipt.value("success", false))
    {
        const json error = receipt.value(
            "error",
            json{{"code", "world_render_failed"},
                 {"message", "The renderer rejected the Engine World snapshot."}});
        throw ToolError(
            error.value("code", std::string("world_render_failed")),
            error.value("message", std::string("The renderer rejected the Engine World snapshot.")),
            receipt);
    }

    const std::string renderedWorldHash =
        receipt.value("renderedWorldHash", std::string{});
    if (renderedWorldHash != expectedWorldHash)
    {
        throw ToolError(
            "world_hash_mismatch",
            "The renderer receipt does not match the Harness World hash.",
            {{"expectedWorldHash", expectedWorldHash},
             {"renderedWorldHash", renderedWorldHash},
             {"api", api},
             {"receiptPath", path.generic_string()}});
    }
    const std::string renderedAssetManifestHash =
        receipt.value("renderedAssetManifestHash", std::string{});
    if (renderedAssetManifestHash != expectedAssetManifestHash)
    {
        throw ToolError(
            "asset_manifest_hash_mismatch",
            "The renderer receipt does not match the Harness Asset Manifest hash.",
            {{"expectedAssetManifestHash", expectedAssetManifestHash},
             {"renderedAssetManifestHash", renderedAssetManifestHash},
             {"api", api},
             {"receiptPath", path.generic_string()}});
    }
    return receipt;
}

RHI::ShaderStage ParseShaderStage(const std::string& value)
{
    const std::string stage = Lowercase(value);
    if (stage == "vertex" || stage == "vs") return RHI::ShaderStage::Vertex;
    if (stage == "pixel" || stage == "fragment" || stage == "ps") return RHI::ShaderStage::Pixel;
    if (stage == "compute" || stage == "cs") return RHI::ShaderStage::Compute;
    throw ToolError("unsupported_shader_stage", "Unknown shader stage: " + value);
}

RHI::ShaderBinaryFormat ParseShaderFormat(const std::string& value)
{
    const std::string format = Lowercase(value);
    if (format == "dxil") return RHI::ShaderBinaryFormat::Dxil;
    if (format == "spirv" || format == "spir-v") return RHI::ShaderBinaryFormat::SpirV;
    if (format == "dxbc") return RHI::ShaderBinaryFormat::Dxbc;
    if (format == "metal" || format == "metal-source") return RHI::ShaderBinaryFormat::MetalSource;
    throw ToolError("unsupported_shader_format", "Unknown shader binary format: " + value);
}

std::string ToString(const RHI::ShaderResourceKind kind)
{
    switch (kind)
    {
    case RHI::ShaderResourceKind::ConstantBuffer: return "ConstantBuffer";
    case RHI::ShaderResourceKind::ShaderResource: return "ShaderResource";
    case RHI::ShaderResourceKind::UnorderedAccess: return "UnorderedAccess";
    case RHI::ShaderResourceKind::Sampler: return "Sampler";
    case RHI::ShaderResourceKind::PushConstant: return "PushConstant";
    case RHI::ShaderResourceKind::Unknown: return "Unknown";
    }
    return "Unknown";
}

json SerializeMetrics(const Tools::GoldenImageMetrics& metrics)
{
    return {
        {"dimensionsMatch", metrics.dimensionsMatch},
        {"meanAbsoluteError", metrics.meanAbsoluteError},
        {"rootMeanSquareError", metrics.rootMeanSquareError},
        {"changedPixelRatio", metrics.changedPixelRatio},
        {"maximumChannelError", metrics.maximumChannelError},
        {"structuralSimilarity",
         metrics.structuralSimilarity}};
}

struct GoldenThresholds
{
    double mean = 0.12;
    double rmse = 0.20;
    double changed = 0.45;
    double minimumSsim = 0.0;
    std::uint8_t pixelTolerance = 8;
};

GoldenThresholds ReadThresholds(const json& arguments, const bool strictDefaults)
{
    GoldenThresholds thresholds;
    if (strictDefaults)
    {
        thresholds.mean = 0.001;
        thresholds.rmse = 0.005;
        thresholds.changed = 0.01;
    }
    thresholds.mean = arguments.value("meanThreshold", thresholds.mean);
    thresholds.rmse = arguments.value("rmseThreshold", thresholds.rmse);
    thresholds.changed = arguments.value("changedThreshold", thresholds.changed);
    thresholds.minimumSsim =
        arguments.value(
            "minimumStructuralSimilarity",
            thresholds.minimumSsim);
    const int tolerance = arguments.value("pixelTolerance", 8);
    if (thresholds.mean < 0.0 || thresholds.mean > 1.0
        || thresholds.rmse < 0.0 || thresholds.rmse > 1.0
        || thresholds.changed < 0.0 || thresholds.changed > 1.0
        || thresholds.minimumSsim < 0.0
        || thresholds.minimumSsim > 1.0
        || tolerance < 0 || tolerance > 255)
    {
        throw ToolError("invalid_threshold", "Golden image thresholds must be normalized values in [0, 1].");
    }
    thresholds.pixelTolerance = static_cast<std::uint8_t>(tolerance);
    return thresholds;
}

json CompareImages(
    const std::filesystem::path& referencePath,
    const std::filesystem::path& candidatePath,
    const GoldenThresholds& thresholds)
{
    Tools::ImageRgba8 reference;
    Tools::ImageRgba8 candidate;
    std::string error;
    if (!Tools::LoadBitmapRgba8(referencePath, reference, &error))
    {
        throw ToolError("reference_image_failed", "Reference image: " + error);
    }
    if (!Tools::LoadBitmapRgba8(candidatePath, candidate, &error))
    {
        throw ToolError("candidate_image_failed", "Candidate image: " + error);
    }

    const Tools::GoldenImageMetrics metrics =
        Tools::CompareGoldenImages(reference, candidate, thresholds.pixelTolerance);
    const bool passed = metrics.dimensionsMatch
        && metrics.meanAbsoluteError <= thresholds.mean
        && metrics.rootMeanSquareError <= thresholds.rmse
        && metrics.changedPixelRatio <= thresholds.changed
        && metrics.structuralSimilarity
            >= thresholds.minimumSsim;
    return {
        {"reference", referencePath.generic_string()},
        {"candidate", candidatePath.generic_string()},
        {"passed", passed},
        {"metrics", SerializeMetrics(metrics)},
        {"thresholds", {
            {"meanAbsoluteError", thresholds.mean},
            {"rootMeanSquareError", thresholds.rmse},
            {"changedPixelRatio", thresholds.changed},
            {"minimumStructuralSimilarity",
             thresholds.minimumSsim},
            {"pixelTolerance", thresholds.pixelTolerance}}}};
}

json SerializeAssetDiagnostic(const Asset::AssetDiagnostic& diagnostic)
{
    return {
        {"severity", Asset::AssetDatabase::ToString(diagnostic.severity)},
        {"code", diagnostic.code},
        {"message", diagnostic.message},
        {"path", diagnostic.path}};
}

json SerializeAssetRecord(const Asset::AssetRecord& record)
{
    json diagnostics = json::array();
    for (const Asset::AssetDiagnostic& diagnostic : record.diagnostics)
    {
        diagnostics.push_back(SerializeAssetDiagnostic(diagnostic));
    }
    return {
        {"assetId", record.assetId},
        {"assetPath", record.assetPath},
        {"type", Asset::AssetDatabase::ToString(record.type)},
        {"name", record.name},
        {"sourcePath", record.sourcePath},
        {"subresource", record.subresource},
        {"contentHash", record.contentHash},
        {"importRevision", record.importRevision},
        {"builtIn", record.builtIn},
        {"dependencies", record.dependencies},
        {"diagnostics", std::move(diagnostics)},
        {"metadata", record.metadata}};
}

json SerializeImportResult(const Asset::AssetImportResult& result)
{
    json assets = json::array();
    for (const Asset::AssetRecord& asset : result.assets)
    {
        assets.push_back(SerializeAssetRecord(asset));
    }
    json diagnostics = json::array();
    for (const Asset::AssetDiagnostic& diagnostic : result.diagnostics)
    {
        diagnostics.push_back(SerializeAssetDiagnostic(diagnostic));
    }
    return {
        {"reimported", result.reimported},
        {"sourceAssetId", result.sourceAssetId},
        {"sourceAssetPath", result.sourceAssetPath},
        {"sourcePath", result.sourcePath},
        {"contentHash", result.contentHash},
        {"manifestHash", result.manifestHash},
        {"importRevision", result.importRevision},
        {"addedCount", result.addedCount},
        {"updatedCount", result.updatedCount},
        {"removedCount", result.removedCount},
        {"assets", std::move(assets)},
        {"diagnostics", std::move(diagnostics)}};
}
} // namespace

HarnessTools::HarnessTools(
    std::filesystem::path projectRoot,
    std::filesystem::path executableDirectory)
    : m_projectRoot(std::filesystem::weakly_canonical(std::filesystem::absolute(std::move(projectRoot))))
    , m_executableDirectory(std::filesystem::absolute(std::move(executableDirectory)).lexically_normal())
{
}

bool HarnessTools::CanHandle(const std::string_view command) const
{
    return command == "asset.list"
        || command == "asset.describe"
        || command == "asset.cache.status"
        || command == "asset.cache.gc"
        || command == "crash.inspect"
        || command == "crash.symbolize"
        || command == "asset.import"
        || command == "asset.reimport"
        || command == "asset.streaming.plan"
        || command == "asset.streaming.validate"
        || command == "shader.compile"
        || command == "golden.compare"
        || command == "render.capture"
        || command == "render.compare_apis"
        || command == "rdg.describe"
        || command == "performance.measure"
        || command == "performance.compare_queue_modes";
}

nlohmann::json HarnessTools::Execute(
    const nlohmann::json& request,
    const Engine::CommandProcessor& processor)
{
    std::string requestId;
    std::string command;
    try
    {
        if (!request.is_object()) throw ToolError("invalid_request", "A tool request must be a JSON object.");
        requestId = request.value("requestId", std::string{});
        command = request.value("command", std::string{});
        if (requestId.empty() || command.empty())
        {
            throw ToolError("invalid_request", "requestId and command are required.");
        }
        if (const auto cached = m_requestCache.find(requestId); cached != m_requestCache.end())
        {
            json result = cached->second;
            result["idempotentReplay"] = true;
            return result;
        }
        if (!CanHandle(command)) throw ToolError("unknown_command", "Unknown Harness tool command: " + command);

        const json arguments = request.value("arguments", json::object());
        if (!arguments.is_object()) throw ToolError("invalid_arguments", "Command arguments must be a JSON object.");
        json data;
        if (command == "asset.list") data = ListAssets(arguments);
        else if (command == "asset.describe") data = DescribeAsset(arguments);
        else if (command == "asset.cache.status") data = DescribeAssetCache(arguments);
        else if (command == "asset.cache.gc") data = GarbageCollectAssetCache(arguments);
        else if (command == "crash.inspect") data = InspectCrashReport(arguments);
        else if (command == "crash.symbolize") data = SymbolizeCrash(arguments);
        else if (command == "asset.import") data = ImportAsset(arguments);
        else if (command == "asset.reimport") data = ReimportAsset(arguments);
        else if (command == "asset.streaming.plan") data = PlanAssetStreaming(arguments);
        else if (command == "asset.streaming.validate")
        {
            data = ValidateAssetStreaming(arguments, requestId);
        }
        else if (command == "shader.compile") data = CompileShader(arguments);
        else if (command == "golden.compare") data = CompareGoldenImages(arguments);
        else if (command == "render.capture")
        {
            data = CaptureRender(arguments, processor, requestId);
        }
        else if (command == "render.compare_apis")
        {
            data = CompareGraphicsApis(arguments, processor, requestId);
        }
        else if (command == "rdg.describe")
        {
            data = DescribeRenderGraph(arguments, processor, requestId);
        }
        else if (command == "performance.compare_queue_modes")
        {
            data = CompareQueueModes(
                arguments,
                processor,
                requestId);
        }
        else
        {
            data = MeasurePerformance(arguments, processor, requestId);
        }

        json result = {
            {"success", true},
            {"requestId", requestId},
            {"command", command},
            {"transactionId", 0},
            {"worldHash", processor.ComputeStateHash()},
            {"data", std::move(data)}};
        m_requestCache.emplace(requestId, result);
        return result;
    }
    catch (const ToolError& error)
    {
        json errorObject = {{"code", error.GetCode()}, {"message", error.what()}};
        if (!error.GetDetails().empty()) errorObject["details"] = error.GetDetails();
        json result = {
            {"success", false},
            {"requestId", requestId},
            {"command", command},
            {"worldHash", processor.ComputeStateHash()},
            {"error", std::move(errorObject)}};
        if (!requestId.empty()) m_requestCache.emplace(requestId, result);
        return result;
    }
    catch (const std::exception& exception)
    {
        json result = {
            {"success", false},
            {"requestId", requestId},
            {"command", command},
            {"worldHash", processor.ComputeStateHash()},
            {"error", {{"code", "tool_failed"}, {"message", exception.what()}}}};
        if (!requestId.empty()) m_requestCache.emplace(requestId, result);
        return result;
    }
}

void HarnessTools::AugmentEngineDescription(nlohmann::json& result) const
{
    if (!result.value("success", false) || !result.contains("data")) return;
    json& data = result["data"];
    json& commands = data["commands"];
    for (const char* command : {
             "asset.list", "asset.describe", "asset.cache.status", "asset.cache.gc",
             "asset.import", "asset.reimport", "asset.streaming.plan",
             "asset.streaming.validate",
             "crash.inspect", "crash.symbolize",
             "shader.compile", "golden.compare", "render.capture", "render.compare_apis",
             "rdg.describe", "performance.measure",
             "performance.compare_queue_modes"})
    {
        commands.push_back(command);
    }
    data["capabilities"]["shaderCompilation"] = true;
    data["capabilities"]["renderCapture"] = true;
    data["capabilities"]["crossApiGoldenImage"] = true;
    data["capabilities"]["renderGraphIntrospection"] = true;
    data["capabilities"]["rdgPassCulling"] = true;
    data["capabilities"]["rdgResourceLifetimes"] = true;
    data["capabilities"]["rdgTransientAliasingPlan"] = true;
    data["capabilities"]["rdgQueueSchedule"] = true;
    data["capabilities"]["rdgDagQueueBatches"] = true;
    data["capabilities"]["rdgHistoricalGpuCostModel"] = true;
    data["capabilities"]["parallelCommandRecording"] = true;
    data["capabilities"]["deferredQueueBatchSubmission"] = true;
    data["capabilities"]["nativeMultiQueueInfrastructure"] = true;
    data["capabilities"]["independentQueueBatchSubmission"] = true;
    data["capabilities"]["nativeTransientAliasing"] = true;
    data["capabilities"]["d3d12PlacedResources"] = true;
    data["capabilities"]["vulkanAliasMemory"] = true;
    data["capabilities"]["nativeAliasingBarriers"] = true;
    data["capabilities"]["nativeAsyncCompute"] = true;
    data["capabilities"]["computeBloom"] = true;
    data["capabilities"]["computeHiZ"] = true;
    data["capabilities"]["gpuDrivenRendering"] = true;
    data["capabilities"]["gpuFrustumCulling"] = true;
    data["capabilities"]["indexedIndirectDraw"] = true;
    data["capabilities"]["crossQueueGpuTimestamps"] = true;
    data["capabilities"]["queueModeComparison"] = true;
    data["capabilities"]["asyncComputeWorkloadBenchmark"] = true;
    data["capabilities"]["currentWorldRendering"] = true;
    data["capabilities"]["renderedWorldHashVerification"] = true;
    data["capabilities"]["structuredRenderAssetErrors"] = true;
    data["capabilities"]["assetManifest"] = true;
    data["capabilities"]["stableAssetIds"] = true;
    data["capabilities"]["assetImportDiagnostics"] = true;
    data["capabilities"]["contentAddressedAssetCache"] = true;
    data["capabilities"]["assetCacheGarbageCollection"] = true;
    data["capabilities"]["assetCacheGcDryRun"] = true;
    data["capabilities"]["asynchronousAssetStreaming"] = true;
    data["capabilities"]["assetResidencyBudget"] = true;
    data["capabilities"]["assetDependencyStreaming"] = true;
    data["capabilities"]["assetStreamingRuntimeValidation"] = true;
    data["capabilities"]["assetStreamingSceneActivation"] = true;
    data["capabilities"]["assetStreamingCrossApiGoldenImage"] = true;
    data["capabilities"]["cookedAssetFormats"] = true;
    data["capabilities"]["cookedAssetV2"] = true;
    data["capabilities"]["cookedAssetCompression"] = true;
    data["capabilities"]["cookedAssetChecksums"] = true;
    data["capabilities"]["structuredChildProcessLogs"] = true;
    data["capabilities"]["crashReports"] = true;
    data["capabilities"]["windowsMinidumps"] = true;
    data["capabilities"]["pdbSymbolization"] = true;
    data["capabilities"]["offlineMinidumpSymbolization"] = true;
    data["capabilities"]["buildPdbIdentityMatching"] = true;
    data["capabilities"]["performanceBaselines"] = true;
    data["capabilities"]["runtimePerformanceIdentity"] = true;
    data["capabilities"]["cpuSpanTrace"] = true;
    data["capabilities"]["correlationIds"] = true;
    data["capabilities"]["cpuSpanBudgets"] = true;
    data["supportedAssetFormats"] = {"gltf", "glb"};
    data["supportedCookedAssetFormats"] = {
        "prismmesh",
        "prismtex",
        "prismmat"};
    data["supportedGraphicsApis"] = {"d3d12", "vulkan"};
    data["renderWorldSources"] = {"currentWorld", "startupScene", "snapshot"};
    data["renderDefaults"] = {
        {"useCurrentWorld", true},
        {"width", 1600},
        {"height", 900}};
    data["performanceMetrics"] = {
        "rendererProcessWallMilliseconds",
        "rdgPassGpuMilliseconds",
        "cpuSpanMilliseconds"};
    data["captureStages"] = {
        "shadow", "gbuffer0", "gbuffer1", "gbuffer2", "gbuffer3", "hdr", "bloom", "tonemap"};
}

nlohmann::json HarnessTools::ListAssets(const nlohmann::json& arguments) const
{
    Asset::AssetDatabase database(m_projectRoot);
    std::string loadError;
    if (!database.Load(&loadError))
    {
        throw ToolError("asset_manifest_load_failed", loadError);
    }

    std::optional<Asset::AssetType> type;
    if (arguments.contains("type"))
    {
        type = Asset::AssetDatabase::ParseAssetType(
            arguments.at("type").get<std::string>());
        if (!type.has_value())
        {
            throw ToolError(
                "unsupported_asset_type",
                "Asset type must be scene, mesh, material, or texture.");
        }
    }
    json assets = json::array();
    for (const Asset::AssetRecord& asset : database.List(
             type,
             arguments.value("query", std::string{})))
    {
        assets.push_back(SerializeAssetRecord(asset));
    }
    return {
        {"manifestPath", database.GetManifestPath().generic_string()},
        {"manifestHash", database.ComputeManifestHash()},
        {"manifestRevision", database.GetManifestRevision()},
        {"count", assets.size()},
        {"assets", std::move(assets)}};
}

nlohmann::json HarnessTools::DescribeAsset(const nlohmann::json& arguments) const
{
    if (!arguments.contains("assetId") && !arguments.contains("assetPath"))
    {
        throw ToolError(
            "missing_argument",
            "asset.describe requires assetId or assetPath.");
    }

    Asset::AssetDatabase database(m_projectRoot);
    std::string loadError;
    if (!database.Load(&loadError))
    {
        throw ToolError("asset_manifest_load_failed", loadError);
    }
    const Asset::AssetRecord* asset = arguments.contains("assetId")
        ? database.FindById(arguments.at("assetId").get<std::string>())
        : database.FindByPath(arguments.at("assetPath").get<std::string>());
    if (asset == nullptr)
    {
        throw ToolError("asset_not_found", "The requested asset is not present in the Asset Manifest.");
    }

    json dependencies = json::array();
    for (const std::string& dependencyId : asset->dependencies)
    {
        const Asset::AssetRecord* dependency = database.FindById(dependencyId);
        dependencies.push_back(dependency != nullptr
            ? SerializeAssetRecord(*dependency)
            : json{{"assetId", dependencyId}, {"missing", true}});
    }
    return {
        {"manifestPath", database.GetManifestPath().generic_string()},
        {"manifestHash", database.ComputeManifestHash()},
        {"asset", SerializeAssetRecord(*asset)},
        {"resolvedDependencies", std::move(dependencies)}};
}

nlohmann::json HarnessTools::DescribeAssetCache(
    const nlohmann::json& arguments) const
{
    Asset::AssetDatabase database(m_projectRoot);
    std::string loadError;
    if (!database.Load(&loadError))
    {
        throw ToolError("asset_manifest_load_failed", loadError);
    }

    std::optional<std::string> requestedAsset;
    if (arguments.contains("assetId"))
    {
        requestedAsset = arguments.at("assetId").get<std::string>();
    }
    else if (arguments.contains("assetPath"))
    {
        requestedAsset = arguments.at("assetPath").get<std::string>();
    }

    Asset::AssetCache cache(m_projectRoot);
    json bundles = json::array();
    std::uintmax_t totalBytes = 0;
    std::uintmax_t cookedBytes = 0;
    std::size_t validCount = 0;
    std::size_t cookedAssetCount = 0;
    std::size_t validCookedAssetCount = 0;
    const std::vector<Asset::AssetRecord> allAssets =
        database.List();
    for (const Asset::AssetRecord& source : database.GetImportedScenes())
    {
        if (requestedAsset.has_value()
            && source.assetId != *requestedAsset
            && source.assetPath != *requestedAsset)
        {
            continue;
        }
        const Asset::AssetCacheBundle bundle = cache.Resolve(source);
        totalBytes += bundle.valid ? bundle.totalBytes : 0;
        validCount += bundle.valid ? 1u : 0u;
        json cookedAssets = json::array();
        for (const Asset::AssetRecord& asset : allAssets)
        {
            if (asset.builtIn
                || asset.sourcePath != source.sourcePath
                || asset.type == Asset::AssetType::Scene)
            {
                continue;
            }
            const json cooked = asset.metadata.value(
                "cooked",
                json::object());
            const std::string cookedPath =
                cooked.value("path", std::string{});
            const std::filesystem::path resolvedCookedPath =
                cookedPath.empty()
                ? std::filesystem::path{}
                : ResolveProjectPath(cookedPath);
            const Asset::CookedAssetInfo cookedInfo =
                resolvedCookedPath.empty()
                ? Asset::CookedAssetInfo{}
                : Asset::CookedAssetIO::Inspect(
                    resolvedCookedPath);
            const bool valid = !cookedPath.empty()
                && Asset::CookedAssetIO::IsSupportedVersion(
                    cooked.value("version", 0u))
                && cookedInfo.valid;
            const std::uintmax_t bytes = valid
                ? std::filesystem::file_size(resolvedCookedPath)
                : 0;
            ++cookedAssetCount;
            validCookedAssetCount += valid ? 1u : 0u;
            cookedBytes += bytes;
            cookedAssets.push_back({
                {"assetId", asset.assetId},
                {"assetPath", asset.assetPath},
                {"type", Asset::AssetDatabase::ToString(
                    asset.type)},
                {"format", cooked.value(
                    "format",
                    std::string{})},
                {"version", cooked.value("version", 0u)},
                {"containerVersion",
                 cookedInfo.containerVersion},
                {"payloadVersion", cookedInfo.payloadVersion},
                {"compression",
                 Asset::CookedAssetIO::ToString(
                     cookedInfo.compression)},
                {"uncompressedBytes",
                 cookedInfo.uncompressedBytes},
                {"storedBytes", cookedInfo.storedBytes},
                {"payloadChecksum",
                 cookedInfo.payloadChecksum},
                {"checksumVerified",
                 cookedInfo.checksumVerified},
                {"path", cookedPath},
                {"valid", valid},
                {"bytes", bytes},
                {"error", cookedInfo.valid
                    ? std::string{}
                    : cookedInfo.errorMessage}});
        }
        bundles.push_back({
            {"assetId", source.assetId},
            {"assetPath", source.assetPath},
            {"sourcePath", source.sourcePath},
            {"contentHash", source.contentHash},
            {"valid", bundle.valid},
            {"bundlePath", bundle.bundlePath.generic_string()},
            {"cachedSourcePath", bundle.cachedSourcePath.generic_string()},
            {"fileCount", bundle.files.size()},
            {"totalBytes", bundle.totalBytes},
            {"cookedAssetCount", cookedAssets.size()},
            {"cookedAssets", std::move(cookedAssets)},
            {"error", bundle.valid
                ? json::object()
                : json{{"code", bundle.errorCode},
                       {"message", bundle.errorMessage}}}});
    }
    if (requestedAsset.has_value() && bundles.empty())
    {
        throw ToolError(
            "asset_not_found",
            "The requested imported scene is not present in the Asset Manifest.");
    }
    return {
        {"cacheRoot", cache.GetCacheRoot().generic_string()},
        {"manifestHash", database.ComputeManifestHash()},
        {"bundleCount", bundles.size()},
        {"validBundleCount", validCount},
        {"totalBytes", totalBytes},
        {"cookedCacheRoot", (
            m_projectRoot
            / "automation/cache/cooked").generic_string()},
        {"cookedContainerVersion",
         Asset::CookedAssetIO::CurrentVersion},
        {"cookedAssetCount", cookedAssetCount},
        {"validCookedAssetCount", validCookedAssetCount},
        {"cookedBytes", cookedBytes},
        {"bundles", std::move(bundles)}};
}

nlohmann::json HarnessTools::GarbageCollectAssetCache(
    const nlohmann::json& arguments) const
{
    const std::uint64_t maximumBytes =
        arguments.value("maximumBytes", 0ull);
    const std::uint64_t minimumUnusedAgeSeconds =
        arguments.value("minimumUnusedAgeSeconds", 0ull);
    if (maximumBytes > 16ull * 1024ull * 1024ull * 1024ull * 1024ull)
    {
        throw ToolError(
            "invalid_cache_capacity",
            "maximumBytes cannot exceed 16 TiB.");
    }
    if (minimumUnusedAgeSeconds > 10ull * 365ull * 24ull * 60ull * 60ull)
    {
        throw ToolError(
            "invalid_cache_age",
            "minimumUnusedAgeSeconds cannot exceed ten years.");
    }

    Asset::AssetDatabase database(m_projectRoot);
    std::string loadError;
    if (!database.Load(&loadError))
    {
        throw ToolError(
            "asset_manifest_load_failed",
            loadError);
    }
    std::vector<std::string> referencedContentHashes;
    for (const Asset::AssetRecord& source :
         database.GetImportedScenes())
    {
        referencedContentHashes.push_back(
            source.contentHash);
    }

    Asset::AssetCacheGarbageCollectionOptions options{};
    options.dryRun = arguments.value("dryRun", true);
    options.maximumBytes = maximumBytes;
    options.minimumUnusedAgeSeconds =
        minimumUnusedAgeSeconds;
    const Asset::AssetCacheGarbageCollectionResult result =
        Asset::AssetCache(m_projectRoot).CollectGarbage(
            referencedContentHashes,
            options);
    if (!result.success)
    {
        throw ToolError(
            result.errorCode,
            result.errorMessage);
    }

    json entries = json::array();
    for (const Asset::AssetCacheGarbageCollectionEntry& entry :
         result.entries)
    {
        entries.push_back({
            {"contentHash", entry.contentHash},
            {"referenced", entry.referenced},
            {"assetBundleExists",
             entry.assetBundleExists},
            {"cookedAssetsExist",
             entry.cookedAssetsExist},
            {"byteSize", entry.byteSize},
            {"lastAccessUnixMilliseconds",
             entry.lastAccessUnixMilliseconds},
            {"selected", entry.selected},
            {"removed", entry.removed},
            {"reason", entry.reason}});
    }
    return {
        {"dryRun", result.dryRun},
        {"budgetSatisfied", result.budgetSatisfied},
        {"maximumBytes", result.maximumBytes},
        {"minimumUnusedAgeSeconds",
         minimumUnusedAgeSeconds},
        {"manifestHash", database.ComputeManifestHash()},
        {"bytesBefore", result.bytesBefore},
        {"bytesAfter", result.bytesAfter},
        {"reclaimableBytes", result.reclaimableBytes},
        {"reclaimedBytes", result.reclaimedBytes},
        {"scannedEntryCount", result.scannedEntryCount},
        {"referencedEntryCount",
         result.referencedEntryCount},
        {"selectedEntryCount", result.selectedEntryCount},
        {"removedEntryCount", result.removedEntryCount},
        {"entries", std::move(entries)}};
}

nlohmann::json HarnessTools::InspectCrashReport(
    const nlohmann::json& arguments) const
{
    if (!arguments.contains("path"))
    {
        throw ToolError(
            "missing_argument",
            "crash.inspect requires a Crash Report path.");
    }
    const std::filesystem::path path = ResolveProjectPath(
        arguments.at("path").get<std::string>());
    const json report = ReadOptionalJson(path);
    const std::uint32_t version =
        report.value("version", 0u);
    if (report.value("format", std::string{}) != "PrismCrashReport"
        || (version != 1u && version != 2u))
    {
        throw ToolError(
            "crash_report_invalid",
            "The requested file is not a Prism Crash Report.",
            {{"path", path.generic_string()}});
    }
    const std::filesystem::path minidumpPath =
        report.value("minidumpPath", std::string{});
    return {
        {"path", path.generic_string()},
        {"kind", report.value("kind", std::string{})},
        {"message", report.value("message", std::string{})},
        {"graphicsApi", report.value("graphicsApi", std::string{})},
        {"exitCode", report.value("exitCode", 0)},
        {"minidumpPath", minidumpPath.generic_string()},
        {"minidumpExists", !minidumpPath.empty()
            && std::filesystem::is_regular_file(minidumpPath)},
        {"minidumpWritten", report.value("minidumpWritten", false)},
        {"buildIdentityPath",
         report.value("buildIdentityPath", std::string{})},
        {"buildIdentity",
         report.value("buildIdentity", json::object())},
        {"exceptionSymbol", report.value("exceptionSymbol", json::object())},
        {"stack", report.value("stack", json::array())},
        {"details", report.value("details", json::object())}};
}

nlohmann::json HarnessTools::SymbolizeCrash(
    const nlohmann::json& arguments) const
{
    if (!arguments.contains("dumpPath"))
    {
        throw ToolError(
            "missing_argument",
            "crash.symbolize requires dumpPath.");
    }

    Core::MinidumpSymbolizationOptions options{};
    options.dumpPath = ResolveProjectPath(
        arguments.at("dumpPath").get<std::string>());
    options.executablePath = arguments.contains("executablePath")
        ? ResolveProjectPath(
            arguments.at("executablePath").get<std::string>())
        : m_executableDirectory / "PrismRender.exe";
    if (arguments.contains("pdbPath"))
    {
        options.pdbPath = ResolveProjectPath(
            arguments.at("pdbPath").get<std::string>());
    }
    else
    {
        options.pdbPath = options.executablePath;
        options.pdbPath.replace_extension(".pdb");
    }
    if (arguments.contains("symbolPaths"))
    {
        if (!arguments.at("symbolPaths").is_array())
        {
            throw ToolError(
                "invalid_arguments",
                "symbolPaths must be an array of project paths.");
        }
        for (const json& path : arguments.at("symbolPaths"))
        {
            options.symbolPaths.push_back(
                ResolveProjectPath(path.get<std::string>()));
        }
    }
    const std::uint64_t maximumFrames =
        arguments.value("maximumFrames", 128ull);
    if (maximumFrames == 0 || maximumFrames > 1024)
    {
        throw ToolError(
            "invalid_maximum_frames",
            "maximumFrames must be between 1 and 1024.");
    }
    options.maximumFrames =
        static_cast<std::size_t>(maximumFrames);
    options.requireIdentityMatch =
        arguments.value("requireIdentityMatch", true);

    const std::filesystem::path outputPath =
        ResolveGeneratedPath(arguments.value(
            "output",
            std::string("automation/reports/symbolized/")
                + options.dumpPath.stem().string()
                + ".symbolized.json"));
    const Core::MinidumpSymbolizationResult result =
        Core::SymbolizeMinidump(options);
    std::string writeError;
    if (!Core::WriteMinidumpSymbolizationReport(
            outputPath,
            result,
            &writeError))
    {
        throw ToolError(
            "symbolization_report_write_failed",
            writeError,
            {{"path", outputPath.generic_string()}});
    }
    if (!result.success)
    {
        throw ToolError(
            result.errorCode.empty()
                ? "minidump_symbolization_failed"
                : result.errorCode,
            result.errorMessage.empty()
                ? "The Minidump could not be symbolized."
                : result.errorMessage,
            {{"reportPath", outputPath.generic_string()},
             {"report", result.report}});
    }
    return {
        {"reportPath", outputPath.generic_string()},
        {"report", result.report}};
}

nlohmann::json HarnessTools::ImportAsset(const nlohmann::json& arguments) const
{
    if (!arguments.contains("path"))
    {
        throw ToolError("missing_argument", "asset.import requires path.");
    }
    Asset::AssetDatabase database(m_projectRoot);
    std::string loadError;
    if (!database.Load(&loadError))
    {
        throw ToolError("asset_manifest_load_failed", loadError);
    }
    const Asset::AssetImportResult result = database.ImportGltf(
        ResolveProjectPath(arguments.at("path").get<std::string>()),
        false);
    if (!result.success)
    {
        throw ToolError(
            result.errorCode.empty() ? "asset_import_failed" : result.errorCode,
            result.errorMessage.empty() ? "Asset import failed." : result.errorMessage,
            SerializeImportResult(result));
    }
    return SerializeImportResult(result);
}

nlohmann::json HarnessTools::ReimportAsset(const nlohmann::json& arguments) const
{
    Asset::AssetDatabase database(m_projectRoot);
    std::string loadError;
    if (!database.Load(&loadError))
    {
        throw ToolError("asset_manifest_load_failed", loadError);
    }

    std::filesystem::path sourcePath;
    if (arguments.contains("path"))
    {
        sourcePath = ResolveProjectPath(arguments.at("path").get<std::string>());
    }
    else if (arguments.contains("assetId") || arguments.contains("assetPath"))
    {
        const Asset::AssetRecord* asset = arguments.contains("assetId")
            ? database.FindById(arguments.at("assetId").get<std::string>())
            : database.FindByPath(arguments.at("assetPath").get<std::string>());
        if (asset == nullptr || asset->builtIn)
        {
            throw ToolError(
                "asset_not_found",
                "The requested imported asset is not present in the Asset Manifest.");
        }
        sourcePath = ResolveProjectPath(asset->sourcePath);
    }
    else
    {
        throw ToolError(
            "missing_argument",
            "asset.reimport requires path, assetId, or assetPath.");
    }

    const Asset::AssetImportResult result =
        database.ImportGltf(sourcePath, true);
    if (!result.success)
    {
        throw ToolError(
            result.errorCode.empty() ? "asset_reimport_failed" : result.errorCode,
            result.errorMessage.empty() ? "Asset reimport failed." : result.errorMessage,
            SerializeImportResult(result));
    }
    return SerializeImportResult(result);
}

nlohmann::json HarnessTools::PlanAssetStreaming(
    const nlohmann::json& arguments) const
{
    if (!arguments.contains("assetId")
        && !arguments.contains("assetPath"))
    {
        throw ToolError(
            "missing_argument",
            "asset.streaming.plan requires assetId or assetPath.");
    }

    Asset::AssetStreamingConfiguration configuration{};
    configuration.residentBudgetBytes = arguments.value(
        "residentBudgetBytes",
        configuration.residentBudgetBytes);
    configuration.maxUploadsPerTick = arguments.value(
        "maxUploadsPerTick",
        configuration.maxUploadsPerTick);
    Asset::AssetStreamingManager manager(configuration);
    const std::filesystem::path manifestPath =
        arguments.contains("manifestPath")
        ? ResolveProjectPath(
              arguments.at("manifestPath").get<std::string>())
        : std::filesystem::path{};
    std::string error;
    if (!manager.Initialize(
            m_projectRoot,
            manifestPath,
            &error))
    {
        throw ToolError(
            "asset_streaming_initialize_failed",
            error);
    }

    const std::string asset =
        arguments.contains("assetId")
        ? arguments.at("assetId").get<std::string>()
        : arguments.at("assetPath").get<std::string>();
    if (!manager.Request(
            asset,
            arguments.value("priority", 0),
            arguments.value("pin", false),
            &error))
    {
        throw ToolError(
            "asset_streaming_request_failed",
            error);
    }
    const std::uint32_t timeoutMilliseconds =
        arguments.value("timeoutMilliseconds", 5000u);
    if (!manager.WaitForIo(
            std::chrono::milliseconds(timeoutMilliseconds)))
    {
        throw ToolError(
            "asset_streaming_io_timeout",
            "Cooked Asset background reads did not finish before the timeout.");
    }

    json entries = json::array();
    for (const Asset::AssetStreamingEntrySnapshot& entry :
         manager.GetEntries())
    {
        if (entry.referenceCount == 0
            && entry.state
                == Asset::AssetResidencyState::Unloaded)
        {
            continue;
        }
        entries.push_back({
            {"assetId", entry.assetId},
            {"assetPath", entry.assetPath},
            {"type", Asset::AssetDatabase::ToString(entry.type)},
            {"state", Asset::AssetStreamingManager::ToString(entry.state)},
            {"priority", entry.priority},
            {"referenceCount", entry.referenceCount},
            {"pinned", entry.pinned},
            {"residentBytes", entry.residentBytes},
            {"lastTouchedSerial", entry.lastTouchedSerial},
            {"uploadTicket", entry.uploadTicket},
            {"errorCode", entry.errorCode},
            {"errorMessage", entry.errorMessage}});
    }
    const Asset::AssetStreamingStatistics statistics =
        manager.GetStatistics();
    return {
        {"requestedAsset", asset},
        {"ioComplete",
         statistics.queuedCount == 0
             && statistics.loadingCount == 0},
        {"requiresRenderThreadUpload",
         statistics.readyCount > 0},
        {"statistics", {
            {"assetCount", statistics.assetCount},
            {"queuedCount", statistics.queuedCount},
            {"loadingCount", statistics.loadingCount},
            {"readyCount", statistics.readyCount},
            {"residentCount", statistics.residentCount},
            {"failedCount", statistics.failedCount},
            {"ioBytesRead", statistics.ioBytesRead},
            {"completedIoCount", statistics.completedIoCount},
            {"residentBudgetBytes",
             statistics.residentBudgetBytes}}},
        {"entries", std::move(entries)}};
}

nlohmann::json HarnessTools::ValidateAssetStreaming(
    const nlohmann::json& arguments,
    const std::string_view requestId) const
{
    if (arguments.contains("residentBudgetMb")
        && arguments.contains("residentBudgetBytes"))
    {
        throw ToolError(
            "conflicting_budget",
            "Specify residentBudgetMb or residentBudgetBytes, not both.");
    }

    std::uint64_t residentBudgetMb =
        arguments.value("residentBudgetMb", 512ull);
    if (arguments.contains("residentBudgetBytes"))
    {
        const std::uint64_t bytes =
            arguments.at("residentBudgetBytes").get<std::uint64_t>();
        residentBudgetMb =
            (bytes + 1024ull * 1024ull - 1ull)
            / (1024ull * 1024ull);
    }
    if (residentBudgetMb < 1ull
        || residentBudgetMb > 65536ull)
    {
        throw ToolError(
            "invalid_resident_budget",
            "The streaming resident budget must be between 1 MiB and 65536 MiB.");
    }

    const std::uint64_t maximumFrameCount =
        arguments.value("maximumFrameCount", 120ull);
    if (maximumFrameCount < 8ull
        || maximumFrameCount > 10000ull)
    {
        throw ToolError(
            "invalid_maximum_frame_count",
            "maximumFrameCount must be between 8 and 10000.");
    }

    const std::uint32_t width =
        ReadDimension(arguments, "width", 1600);
    const std::uint32_t height =
        ReadDimension(arguments, "height", 900);
    const std::string stage =
        ReadCaptureStage(arguments);
    const std::string queueMode =
        ReadQueueMode(arguments);
    const std::uint32_t asyncWorkloadMultiplier =
        ReadAsyncWorkloadMultiplier(arguments);
    const bool gpuDriven =
        arguments.value("gpuDriven", false);
    const std::uint32_t timeoutMilliseconds =
        ReadTimeout(arguments);
    const std::string artifactStem =
        SanitizeArtifactName(requestId);

    const std::filesystem::path manifestPath =
        arguments.contains("manifestPath")
        ? ResolveProjectPath(
              arguments.at("manifestPath").get<std::string>())
        : ResolveProjectPath(
              "automation/assets/AssetManifest.json");
    Asset::AssetDatabase database(
        m_projectRoot,
        manifestPath);
    std::string databaseError;
    if (!database.Load(&databaseError))
    {
        throw ToolError(
            "asset_manifest_load_failed",
            databaseError);
    }
    std::vector<Asset::AssetDiagnostic> diagnostics;
    if (!database.ValidateImportedContent(diagnostics))
    {
        json serializedDiagnostics = json::array();
        for (const Asset::AssetDiagnostic& diagnostic :
             diagnostics)
        {
            serializedDiagnostics.push_back(
                SerializeAssetDiagnostic(diagnostic));
        }
        throw ToolError(
            "asset_source_stale",
            "Imported asset content changed after import. Run asset.reimport.",
            {{"diagnostics",
              std::move(serializedDiagnostics)}});
    }
    const std::vector<Asset::AssetRecord> scenes =
        database.GetImportedScenes();
    if (scenes.empty())
    {
        throw ToolError(
            "streaming_scene_missing",
            "The Asset Manifest does not contain an imported Scene asset.");
    }
    const std::string manifestHash =
        database.ComputeManifestHash();

    auto resolveOutput =
        [&](const char* argumentName,
            const std::filesystem::path& fallback)
    {
        return ResolveGeneratedPath(
            arguments.contains(argumentName)
            ? std::filesystem::path(
                  arguments.at(argumentName)
                      .get<std::string>())
            : fallback);
    };

    struct StreamingRunPaths
    {
        std::filesystem::path capture;
        std::filesystem::path streamingReport;
        std::filesystem::path sceneReport;
        std::filesystem::path structuredLog;
        std::filesystem::path stdoutLog;
        std::filesystem::path stderrLog;
        std::filesystem::path crashReport;
        std::filesystem::path gpuTimingReport;
        std::filesystem::path cpuTrace;
        std::filesystem::path performanceIdentity;
        std::filesystem::path buildIdentity;
        std::filesystem::path minidump;
    };

    auto makePaths = [&](const std::string& api)
    {
        const std::string prefix =
            artifactStem + '-' + api + "-streaming";
        const std::filesystem::path processBase =
            ResolveGeneratedPath(
                std::filesystem::path(
                    "automation/reports/process")
                / prefix);
        StreamingRunPaths paths{};
        paths.capture = resolveOutput(
            api == "d3d12"
                ? "d3d12Output"
                : "vulkanOutput",
            std::filesystem::path(
                "automation/captures")
                / (prefix + ".bmp"));
        paths.streamingReport = resolveOutput(
            api == "d3d12"
                ? "d3d12StreamingReport"
                : "vulkanStreamingReport",
            std::filesystem::path(
                "automation/reports")
                / (prefix + "-residency.json"));
        paths.sceneReport = resolveOutput(
            api == "d3d12"
                ? "d3d12SceneReport"
                : "vulkanSceneReport",
            std::filesystem::path(
                "automation/reports")
                / (prefix + "-scene.json"));
        paths.structuredLog =
            processBase.string() + ".jsonl";
        paths.stdoutLog =
            processBase.string() + ".stdout.log";
        paths.stderrLog =
            processBase.string() + ".stderr.log";
        paths.crashReport =
            processBase.string() + ".crash.json";
        paths.gpuTimingReport =
            processBase.string() + ".gpu.json";
        paths.cpuTrace =
            processBase.string() + ".cpu.json";
        paths.performanceIdentity =
            processBase.string() + ".identity.json";
        paths.buildIdentity =
            processBase.string() + ".build.json";
        paths.minidump =
            processBase.string() + ".dmp";
        return paths;
    };

    auto runApi = [&](const std::string& api,
                      const StreamingRunPaths& paths)
    {
        const std::array<std::filesystem::path, 12>
            artifacts = {
                paths.capture,
                paths.streamingReport,
                paths.sceneReport,
                paths.structuredLog,
                paths.stdoutLog,
                paths.stderrLog,
                paths.crashReport,
                paths.gpuTimingReport,
                paths.cpuTrace,
                paths.performanceIdentity,
                paths.buildIdentity,
                paths.minidump};
        for (const std::filesystem::path& artifact :
             artifacts)
        {
            if (!artifact.parent_path().empty())
            {
                std::filesystem::create_directories(
                    artifact.parent_path());
            }
            std::filesystem::remove(artifact);
        }

        EnvironmentVariableGuard headless(
            L"PRISM_RENDER_HEADLESS", L"1");
        EnvironmentVariableGuard deterministic(
            L"PRISM_RENDER_DETERMINISTIC", L"1");
        EnvironmentVariableGuard assetStreaming(
            L"PRISM_RENDER_ASSET_STREAMING", L"1");
        EnvironmentVariableGuard streamingBudget(
            L"PRISM_RENDER_ASSET_STREAMING_BUDGET_MB",
            std::to_wstring(residentBudgetMb));
        EnvironmentVariableGuard maximumFrames(
            L"PRISM_RENDER_MAX_FRAMES",
            std::to_wstring(maximumFrameCount));
        EnvironmentVariableGuard gpuDrivenOverride(
            L"PRISM_RENDER_GPU_DRIVEN_OVERRIDE",
            gpuDriven ? L"1" : L"0");
        EnvironmentVariableGuard rdgQueueMode(
            L"PRISM_RENDER_RDG_QUEUE_MODE",
            Widen(queueMode));
        EnvironmentVariableGuard renderWidth(
            L"PRISM_RENDER_WIDTH",
            std::to_wstring(width));
        EnvironmentVariableGuard renderHeight(
            L"PRISM_RENDER_HEIGHT",
            std::to_wstring(height));
        EnvironmentVariableGuard asyncWorkload(
            L"PRISM_RENDER_ASYNC_WORKLOAD_MULTIPLIER",
            std::to_wstring(
                asyncWorkloadMultiplier));
        EnvironmentVariableGuard capturePath(
            L"PRISM_RENDER_CAPTURE_PATH",
            paths.capture.wstring());
        EnvironmentVariableGuard captureStage(
            L"PRISM_RENDER_CAPTURE_STAGE",
            Widen(stage));
        EnvironmentVariableGuard exitAfterCapture(
            L"PRISM_RENDER_EXIT_AFTER_CAPTURE", L"1");
        EnvironmentVariableGuard rdgPath(
            L"PRISM_RENDER_RDG_REPORT_PATH", L"");
        EnvironmentVariableGuard exitAfterRdg(
            L"PRISM_RENDER_EXIT_AFTER_RDG_REPORT",
            L"0");
        EnvironmentVariableGuard worldPath(
            L"PRISM_RENDER_WORLD_PATH", L"");
        EnvironmentVariableGuard expectedWorldHash(
            L"PRISM_RENDER_EXPECTED_WORLD_HASH", L"");
        EnvironmentVariableGuard worldReportPath(
            L"PRISM_RENDER_WORLD_REPORT_PATH", L"");
        EnvironmentVariableGuard assetManifestPath(
            L"PRISM_RENDER_ASSET_MANIFEST_PATH",
            manifestPath.wstring());
        EnvironmentVariableGuard
            expectedAssetManifestHash(
                L"PRISM_RENDER_EXPECTED_ASSET_MANIFEST_HASH",
                Widen(manifestHash));
        EnvironmentVariableGuard streamingReport(
            L"PRISM_RENDER_ASSET_STREAMING_REPORT_PATH",
            paths.streamingReport.wstring());
        EnvironmentVariableGuard streamingSceneReport(
            L"PRISM_RENDER_ASSET_STREAMING_SCENE_REPORT_PATH",
            paths.sceneReport.wstring());
        EnvironmentVariableGuard structuredLog(
            L"PRISM_RENDER_LOG_PATH",
            paths.structuredLog.wstring());
        EnvironmentVariableGuard crashReport(
            L"PRISM_RENDER_CRASH_REPORT_PATH",
            paths.crashReport.wstring());
        EnvironmentVariableGuard gpuTimingReport(
            L"PRISM_RENDER_GPU_TIMING_REPORT_PATH",
            paths.gpuTimingReport.wstring());
        EnvironmentVariableGuard cpuTrace(
            L"PRISM_RENDER_CPU_TRACE_PATH",
            paths.cpuTrace.wstring());
        EnvironmentVariableGuard correlationId(
            L"PRISM_RENDER_CORRELATION_ID",
            Widen(std::string(requestId)
                  + '-' + api));
        EnvironmentVariableGuard performanceIdentity(
            L"PRISM_RENDER_PERFORMANCE_IDENTITY_PATH",
            paths.performanceIdentity.wstring());
        EnvironmentVariableGuard buildIdentity(
            L"PRISM_RENDER_BUILD_IDENTITY_PATH",
            paths.buildIdentity.wstring());
        EnvironmentVariableGuard minidump(
            L"PRISM_RENDER_MINIDUMP_PATH",
            paths.minidump.wstring());

        const auto start =
            std::chrono::steady_clock::now();
        const RendererProcessResult process =
            RunRendererProcess(
                m_executableDirectory
                    / "PrismRender.exe",
                m_projectRoot,
                api,
                timeoutMilliseconds,
                paths.structuredLog,
                paths.stdoutLog,
                paths.stderrLog,
                paths.crashReport,
                paths.minidump,
                paths.cpuTrace,
                paths.performanceIdentity,
                paths.buildIdentity);
        const double elapsedMilliseconds =
            std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now()
                - start).count();
        if (process.exitCode != 0)
        {
            ThrowRendererFailure(process, api);
        }
        if (!std::filesystem::is_regular_file(
                paths.capture))
        {
            throw ToolError(
                "streaming_capture_missing",
                "The renderer exited without producing the streamed Scene capture.",
                {{"api", api},
                 {"path",
                  paths.capture.generic_string()},
                 {"process",
                  SerializeRendererProcess(
                      process, true)}});
        }

        const json assetReport =
            ReadAssetStreamingReport(
                paths.streamingReport,
                api,
                process);
        const json sceneReport =
            ReadAssetStreamingSceneReport(
                paths.sceneReport,
                api,
                process);
        const json validation =
            ValidateAssetStreamingReports(
                assetReport,
                sceneReport,
                api);
        const json gpuTimings =
            ReadGpuTimingReport(
                paths.gpuTimingReport);
        const json cpuTraceReport =
            ReadCpuTraceReport(
                paths.cpuTrace,
                std::string(requestId)
                    + '-' + api);
        const json identity =
            ReadPerformanceIdentityReport(
                paths.performanceIdentity,
                api);

        return json{
            {"api", api},
            {"capturePath",
             paths.capture.generic_string()},
            {"captureBytes",
             std::filesystem::file_size(
                 paths.capture)},
            {"streamingReportPath",
             paths.streamingReport.generic_string()},
            {"sceneReportPath",
             paths.sceneReport.generic_string()},
            {"elapsedMilliseconds",
             elapsedMilliseconds},
            {"validation", validation},
            {"streamingReport", assetReport},
            {"sceneReport", sceneReport},
            {"process",
             SerializeRendererProcess(process)},
            {"gpuTimingReportPath",
             paths.gpuTimingReport.generic_string()},
            {"gpuTimings", gpuTimings},
            {"cpuTracePath",
             paths.cpuTrace.generic_string()},
            {"cpuTrace", cpuTraceReport},
            {"performanceIdentityPath",
             paths.performanceIdentity
                 .generic_string()},
            {"performanceIdentity", identity},
            {"buildIdentityPath",
             paths.buildIdentity.generic_string()},
            {"buildIdentity",
             process.buildIdentity}};
    };

    const StreamingRunPaths d3d12Paths =
        makePaths("d3d12");
    const StreamingRunPaths vulkanPaths =
        makePaths("vulkan");
    const json d3d12 =
        runApi("d3d12", d3d12Paths);
    const json vulkan =
        runApi("vulkan", vulkanPaths);
    const json comparison =
        CompareImages(
            d3d12Paths.capture,
            vulkanPaths.capture,
            ReadThresholds(arguments, true));

    const json& d3d12Validation =
        d3d12.at("validation");
    const json& vulkanValidation =
        vulkan.at("validation");
    const json crossApiChecks = {
        {"assetCountMatched",
         d3d12Validation.at("assetCount")
             == vulkanValidation.at("assetCount")},
        {"residentCountMatched",
         d3d12Validation.at("residentCount")
             == vulkanValidation.at("residentCount")},
        {"activatedObjectCountMatched",
         d3d12Validation.at(
             "activatedObjectCount")
             == vulkanValidation.at(
                 "activatedObjectCount")},
        {"renderObjectCountMatched",
         d3d12Validation.at("renderObjectCount")
             == vulkanValidation.at(
                 "renderObjectCount")},
        {"streamedBindingCountMatched",
         d3d12Validation.at(
             "streamedBindingCount")
             == vulkanValidation.at(
                 "streamedBindingCount")},
        {"sceneAssetMatched",
         d3d12.at("sceneReport")
                 .value("sceneAsset",
                        std::string{})
             == vulkan.at("sceneReport")
                    .value("sceneAsset",
                           std::string{})}};
    bool crossApiReportsMatched = true;
    for (const auto& [name, value] :
         crossApiChecks.items())
    {
        (void)name;
        crossApiReportsMatched =
            crossApiReportsMatched
            && value.get<bool>();
    }
    const bool passed =
        d3d12Validation.at("passed").get<bool>()
        && vulkanValidation.at("passed").get<bool>()
        && crossApiReportsMatched
        && comparison.at("passed").get<bool>();

    json result = {
        {"format",
         "PrismAssetStreamingValidationResult"},
        {"version", 1},
        {"passed", passed},
        {"stage", stage},
        {"width", width},
        {"height", height},
        {"queueMode", queueMode},
        {"gpuDriven", gpuDriven},
        {"residentBudgetMb", residentBudgetMb},
        {"maximumFrameCount", maximumFrameCount},
        {"manifestPath",
         manifestPath.generic_string()},
        {"manifestHash", manifestHash},
        {"sceneAssetId",
         scenes.front().assetId},
        {"d3d12", d3d12},
        {"vulkan", vulkan},
        {"crossApiReportsMatched",
         crossApiReportsMatched},
        {"crossApiChecks", crossApiChecks},
        {"comparison", comparison}};
    if (arguments.value("enforce", true)
        && !passed)
    {
        throw ToolError(
            "asset_streaming_validation_failed",
            "Asset Streaming runtime validation failed.",
            result);
    }
    return result;
}

nlohmann::json HarnessTools::CompileShader(const nlohmann::json& arguments) const
{
    if (!arguments.contains("path") || !arguments.contains("entryPoint") || !arguments.contains("stage"))
    {
        throw ToolError("missing_argument", "shader.compile requires path, entryPoint, and stage.");
    }
    const std::filesystem::path sourcePath = ResolveProjectPath(arguments.at("path").get<std::string>());
    const RHI::ShaderStage stage = ParseShaderStage(arguments.at("stage").get<std::string>());
    const RHI::ShaderBinaryFormat format =
        ParseShaderFormat(arguments.value("format", std::string("dxil")));
    std::optional<std::filesystem::path> outputPath;
    if (arguments.contains("output"))
    {
        outputPath = ResolveGeneratedPath(arguments.at("output").get<std::string>());
    }

    Asset::SlangShaderCompiler compiler;
    const RHI::ShaderBinary binary = compiler.Compile({
        sourcePath,
        arguments.at("entryPoint").get<std::string>(),
        stage,
        format,
        arguments.value("debug", false)});
    if (!binary.IsValid()) throw ToolError("shader_compile_failed", "Slang returned an empty shader binary.");

    if (outputPath.has_value())
    {
        if (!outputPath->parent_path().empty()) std::filesystem::create_directories(outputPath->parent_path());
        std::ofstream output(*outputPath, std::ios::binary | std::ios::trunc);
        output.write(
            reinterpret_cast<const char*>(binary.bytecode.data()),
            static_cast<std::streamsize>(binary.bytecode.size()));
        if (!output) throw ToolError("shader_output_failed", "Could not write the compiled shader binary.");
    }

    json resources = json::array();
    for (const RHI::ShaderResourceBinding& resource : binary.reflection.resources)
    {
        resources.push_back({
            {"name", resource.name},
            {"kind", ToString(resource.kind)},
            {"binding", resource.bindingIndex},
            {"space", resource.bindingSpace},
            {"byteSize", resource.byteSize}});
    }
    return {
        {"path", sourcePath.generic_string()},
        {"entryPoint", binary.entryPoint},
        {"emittedEntryPoint", binary.emittedEntryPoint},
        {"stage", std::string(RHI::ToString(binary.stage))},
        {"format", std::string(RHI::ToString(binary.format))},
        {"bytecodeSize", binary.Size()},
        {"output", outputPath.has_value() ? outputPath->generic_string() : std::string{}},
        {"diagnostics", binary.diagnostics},
        {"resources", std::move(resources)}};
}

nlohmann::json HarnessTools::CompareGoldenImages(const nlohmann::json& arguments) const
{
    if (!arguments.contains("reference") || !arguments.contains("candidate"))
    {
        throw ToolError("missing_argument", "golden.compare requires reference and candidate paths.");
    }
    const std::filesystem::path reference =
        ResolveProjectPath(arguments.at("reference").get<std::string>());
    const std::filesystem::path candidate =
        ResolveProjectPath(arguments.at("candidate").get<std::string>());
    const json result = CompareImages(reference, candidate, ReadThresholds(arguments, false));
    if (arguments.value("enforce", false) && !result.at("passed").get<bool>())
    {
        throw ToolError("golden_threshold_exceeded", "Golden image thresholds were exceeded.", result);
    }
    return result;
}

HarnessTools::RenderWorldInput HarnessTools::PrepareRenderWorld(
    const nlohmann::json& arguments,
    const Engine::CommandProcessor& processor,
    const std::string_view requestId) const
{
    const bool hasSnapshotArgument = arguments.contains("worldSnapshot");
    const bool useCurrentWorld = arguments.contains("useCurrentWorld")
        ? arguments.at("useCurrentWorld").get<bool>()
        : !hasSnapshotArgument;
    if (useCurrentWorld && hasSnapshotArgument)
    {
        throw ToolError(
            "conflicting_world_source",
            "useCurrentWorld and worldSnapshot cannot be enabled together.");
    }

    RenderWorldInput result{};
    Asset::AssetDatabase assetDatabase(m_projectRoot);
    std::string assetManifestError;
    if (!assetDatabase.Load(&assetManifestError))
    {
        throw ToolError("asset_manifest_load_failed", assetManifestError);
    }
    std::vector<Asset::AssetDiagnostic> assetDiagnostics;
    if (!assetDatabase.ValidateImportedContent(assetDiagnostics))
    {
        json diagnostics = json::array();
        for (const Asset::AssetDiagnostic& diagnostic : assetDiagnostics)
        {
            diagnostics.push_back(SerializeAssetDiagnostic(diagnostic));
        }
        throw ToolError(
            "asset_source_stale",
            "Imported asset content changed after import. Run asset.reimport.",
            {{"diagnostics", std::move(diagnostics)}});
    }
    result.assetManifestPath = assetDatabase.GetManifestPath();
    result.assetManifestHash = assetDatabase.ComputeManifestHash();
    if (!useCurrentWorld && !hasSnapshotArgument)
    {
        return result;
    }

    if (useCurrentWorld)
    {
        result.enabled = true;
        result.source = "currentWorld";
        result.worldHash = processor.ComputeStateHash();
        result.snapshotPath = ResolveGeneratedPath(
            std::filesystem::path("automation/snapshots")
            / (SanitizeArtifactName(requestId) + ".prism-snapshot.json"));
        std::string saveError;
        if (!processor.SaveSnapshot(result.snapshotPath, &saveError))
        {
            throw ToolError("snapshot_save_failed", saveError);
        }
    }
    else
    {
        result.enabled = true;
        result.source = "snapshot";
        result.snapshotPath = ResolveProjectPath(
            arguments.at("worldSnapshot").get<std::string>());
    }

    Engine::CommandProcessor verifier(m_projectRoot);
    std::string loadError;
    if (!verifier.LoadSnapshotFile(result.snapshotPath, &loadError))
    {
        throw ToolError(
            "snapshot_load_failed",
            loadError,
            {{"snapshotPath", result.snapshotPath.generic_string()}});
    }
    const std::string verifiedHash = verifier.ComputeStateHash();
    if (result.worldHash.empty())
    {
        result.worldHash = verifiedHash;
    }
    else if (verifiedHash != result.worldHash)
    {
        throw ToolError(
            "snapshot_roundtrip_mismatch",
            "The saved Snapshot does not reproduce the current Harness state hash.",
            {{"worldHash", result.worldHash},
             {"snapshotHash", verifiedHash},
             {"snapshotPath", result.snapshotPath.generic_string()}});
    }
    return result;
}

nlohmann::json HarnessTools::CaptureRender(
    const nlohmann::json& arguments,
    const Engine::CommandProcessor& processor,
    const std::string_view requestId,
    const RenderWorldInput* preparedWorld) const
{
    const std::string api = ReadGraphicsApi(arguments);
    const std::string stage = ReadCaptureStage(arguments);
    const std::string queueMode = ReadQueueMode(arguments);
    const std::uint32_t width = ReadDimension(arguments, "width", 1600);
    const std::uint32_t height = ReadDimension(arguments, "height", 900);
    const std::uint32_t asyncWorkloadMultiplier =
        ReadAsyncWorkloadMultiplier(arguments);
    const bool gpuDriven =
        arguments.value("gpuDriven", false);
    const std::filesystem::path output = ResolveGeneratedPath(arguments.value(
        "output",
        std::string("automation/captures/") + api + '-' + stage + ".bmp"));
    if (!output.parent_path().empty()) std::filesystem::create_directories(output.parent_path());
    std::filesystem::remove(output);

    RenderWorldInput localWorld;
    if (preparedWorld == nullptr)
    {
        localWorld = PrepareRenderWorld(arguments, processor, requestId);
        preparedWorld = &localWorld;
    }
    const RenderWorldInput& world = *preparedWorld;
    std::filesystem::path worldReceiptPath;
    if (world.enabled)
    {
        worldReceiptPath = ResolveGeneratedPath(
            std::filesystem::path("automation/reports")
            / (SanitizeArtifactName(requestId) + '-' + api + "-world.json"));
        if (!worldReceiptPath.parent_path().empty())
        {
            std::filesystem::create_directories(worldReceiptPath.parent_path());
        }
        std::filesystem::remove(worldReceiptPath);
    }
    const std::filesystem::path processBase = ResolveGeneratedPath(
        std::filesystem::path("automation/reports/process")
        / (SanitizeArtifactName(requestId) + '-' + api + "-capture"));
    const std::filesystem::path structuredLogPath =
        processBase.string() + ".jsonl";
    const std::filesystem::path stdoutPath =
        processBase.string() + ".stdout.log";
    const std::filesystem::path stderrPath =
        processBase.string() + ".stderr.log";
    const std::filesystem::path crashReportPath =
        processBase.string() + ".crash.json";
    const std::filesystem::path gpuTimingReportPath =
        processBase.string() + ".gpu.json";
    const std::filesystem::path cpuTracePath =
        processBase.string() + ".cpu.json";
    const std::filesystem::path performanceIdentityPath =
        processBase.string() + ".identity.json";
    const std::filesystem::path buildIdentityPath =
        processBase.string() + ".build.json";
    const std::filesystem::path minidumpPath =
        processBase.string() + ".dmp";
    std::filesystem::create_directories(processBase.parent_path());
    std::filesystem::remove(structuredLogPath);
    std::filesystem::remove(stdoutPath);
    std::filesystem::remove(stderrPath);
    std::filesystem::remove(crashReportPath);
    std::filesystem::remove(gpuTimingReportPath);
    std::filesystem::remove(cpuTracePath);
    std::filesystem::remove(performanceIdentityPath);
    std::filesystem::remove(buildIdentityPath);
    std::filesystem::remove(minidumpPath);

    EnvironmentVariableGuard headless(L"PRISM_RENDER_HEADLESS", L"1");
    EnvironmentVariableGuard deterministic(L"PRISM_RENDER_DETERMINISTIC", L"1");
    EnvironmentVariableGuard gpuDrivenOverride(
        L"PRISM_RENDER_GPU_DRIVEN_OVERRIDE",
        gpuDriven ? L"1" : L"0");
    EnvironmentVariableGuard rdgQueueMode(
        L"PRISM_RENDER_RDG_QUEUE_MODE",
        Widen(queueMode));
    EnvironmentVariableGuard renderWidth(L"PRISM_RENDER_WIDTH", std::to_wstring(width));
    EnvironmentVariableGuard renderHeight(L"PRISM_RENDER_HEIGHT", std::to_wstring(height));
    EnvironmentVariableGuard asyncWorkload(
        L"PRISM_RENDER_ASYNC_WORKLOAD_MULTIPLIER",
        std::to_wstring(asyncWorkloadMultiplier));
    EnvironmentVariableGuard capturePath(L"PRISM_RENDER_CAPTURE_PATH", output.wstring());
    EnvironmentVariableGuard captureStage(L"PRISM_RENDER_CAPTURE_STAGE", Widen(stage));
    EnvironmentVariableGuard exitAfterCapture(L"PRISM_RENDER_EXIT_AFTER_CAPTURE", L"1");
    EnvironmentVariableGuard rdgPath(L"PRISM_RENDER_RDG_REPORT_PATH", L"");
    EnvironmentVariableGuard exitAfterRdg(L"PRISM_RENDER_EXIT_AFTER_RDG_REPORT", L"0");
    EnvironmentVariableGuard worldPath(
        L"PRISM_RENDER_WORLD_PATH",
        world.enabled ? world.snapshotPath.wstring() : std::wstring{});
    EnvironmentVariableGuard expectedWorldHash(
        L"PRISM_RENDER_EXPECTED_WORLD_HASH",
        world.enabled ? Widen(world.worldHash) : std::wstring{});
    EnvironmentVariableGuard worldReportPath(
        L"PRISM_RENDER_WORLD_REPORT_PATH",
        world.enabled ? worldReceiptPath.wstring() : std::wstring{});
    EnvironmentVariableGuard assetManifestPath(
        L"PRISM_RENDER_ASSET_MANIFEST_PATH",
        world.assetManifestPath.wstring());
    EnvironmentVariableGuard expectedAssetManifestHash(
        L"PRISM_RENDER_EXPECTED_ASSET_MANIFEST_HASH",
        Widen(world.assetManifestHash));
    EnvironmentVariableGuard structuredLog(
        L"PRISM_RENDER_LOG_PATH",
        structuredLogPath.wstring());
    EnvironmentVariableGuard crashReport(
        L"PRISM_RENDER_CRASH_REPORT_PATH",
        crashReportPath.wstring());
    EnvironmentVariableGuard gpuTimingReport(
        L"PRISM_RENDER_GPU_TIMING_REPORT_PATH",
        gpuTimingReportPath.wstring());
    EnvironmentVariableGuard cpuTrace(
        L"PRISM_RENDER_CPU_TRACE_PATH",
        cpuTracePath.wstring());
    EnvironmentVariableGuard correlationId(
        L"PRISM_RENDER_CORRELATION_ID",
        Widen(std::string(requestId)));
    EnvironmentVariableGuard performanceIdentity(
        L"PRISM_RENDER_PERFORMANCE_IDENTITY_PATH",
        performanceIdentityPath.wstring());
    EnvironmentVariableGuard buildIdentity(
        L"PRISM_RENDER_BUILD_IDENTITY_PATH",
        buildIdentityPath.wstring());
    EnvironmentVariableGuard minidump(
        L"PRISM_RENDER_MINIDUMP_PATH",
        minidumpPath.wstring());

    const auto start = std::chrono::steady_clock::now();
    const RendererProcessResult process = RunRendererProcess(
        m_executableDirectory / "PrismRender.exe",
        m_projectRoot,
        api,
        ReadTimeout(arguments),
        structuredLogPath,
        stdoutPath,
        stderrPath,
        crashReportPath,
        minidumpPath,
        cpuTracePath,
        performanceIdentityPath,
        buildIdentityPath);
    const auto elapsed = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - start).count();
    json worldReceipt = json::object();
    if (world.enabled)
    {
        worldReceipt = ReadRenderedWorldReceipt(
            worldReceiptPath,
            world.worldHash,
            world.assetManifestHash,
            api,
            process);
    }
    if (process.exitCode != 0)
    {
        ThrowRendererFailure(process, api);
    }
    if (!std::filesystem::is_regular_file(output))
    {
        throw ToolError("capture_missing", "The renderer exited without producing the requested capture.");
    }
    const json gpuTimings =
        ReadGpuTimingReport(gpuTimingReportPath);
    const json cpuTraceReport =
        ReadCpuTraceReport(cpuTracePath, requestId);
    const json performanceIdentityReport =
        ReadPerformanceIdentityReport(performanceIdentityPath, api);
    return {
        {"api", api},
        {"stage", stage},
        {"queueMode", queueMode},
        {"asyncWorkloadMultiplier",
         asyncWorkloadMultiplier},
        {"gpuDriven", gpuDriven},
        {"path", output.generic_string()},
        {"bytes", std::filesystem::file_size(output)},
        {"elapsedMilliseconds", elapsed},
        {"width", width},
        {"height", height},
        {"worldSource", world.source},
        {"worldHash", world.worldHash},
        {"assetManifestPath", world.assetManifestPath.generic_string()},
        {"assetManifestHash", world.assetManifestHash},
        {"renderedWorldHash", world.enabled
             ? worldReceipt.value("renderedWorldHash", std::string{})
             : std::string{}},
        {"snapshotPath", world.enabled ? world.snapshotPath.generic_string() : std::string{}},
        {"worldReceiptPath", world.enabled ? worldReceiptPath.generic_string() : std::string{}},
        {"worldReceipt", std::move(worldReceipt)},
        {"process", SerializeRendererProcess(process)},
        {"gpuTimingReportPath", gpuTimingReportPath.generic_string()},
        {"gpuTimings", gpuTimings},
        {"cpuTracePath", cpuTracePath.generic_string()},
        {"cpuTrace", cpuTraceReport},
        {"performanceIdentityPath",
         performanceIdentityPath.generic_string()},
        {"performanceIdentity", performanceIdentityReport},
        {"buildIdentityPath",
         buildIdentityPath.generic_string()},
        {"buildIdentity", process.buildIdentity}};
}

nlohmann::json HarnessTools::CompareGraphicsApis(
    const nlohmann::json& arguments,
    const Engine::CommandProcessor& processor,
    const std::string_view requestId) const
{
    const std::string stage = ReadCaptureStage(arguments);
    const RenderWorldInput world = PrepareRenderWorld(arguments, processor, requestId);
    const std::filesystem::path d3d12Path = ResolveGeneratedPath(arguments.value(
        "d3d12Output", std::string("automation/captures/d3d12-") + stage + ".bmp"));
    const std::filesystem::path vulkanPath = ResolveGeneratedPath(arguments.value(
        "vulkanOutput", std::string("automation/captures/vulkan-") + stage + ".bmp"));

    json d3d12Arguments = arguments;
    d3d12Arguments["api"] = "d3d12";
    d3d12Arguments["output"] = d3d12Path.generic_string();
    json vulkanArguments = arguments;
    vulkanArguments["api"] = "vulkan";
    vulkanArguments["output"] = vulkanPath.generic_string();
    const json d3d12Capture =
        CaptureRender(d3d12Arguments, processor, requestId, &world);
    const json vulkanCapture =
        CaptureRender(vulkanArguments, processor, requestId, &world);
    const json comparison = CompareImages(d3d12Path, vulkanPath, ReadThresholds(arguments, true));
    json result = {
        {"stage", stage},
        {"worldSource", world.source},
        {"worldHash", world.worldHash},
        {"assetManifestPath", world.assetManifestPath.generic_string()},
        {"assetManifestHash", world.assetManifestHash},
        {"renderedWorldHash", world.enabled
             ? d3d12Capture.value("renderedWorldHash", std::string{})
             : std::string{}},
        {"snapshotPath", world.enabled ? world.snapshotPath.generic_string() : std::string{}},
        {"d3d12", d3d12Capture},
        {"vulkan", vulkanCapture},
        {"comparison", comparison}};
    if (arguments.value("enforce", true) && !comparison.at("passed").get<bool>())
    {
        throw ToolError("cross_api_threshold_exceeded", "D3D12 and Vulkan captures exceeded parity thresholds.", result);
    }
    return result;
}

nlohmann::json HarnessTools::DescribeRenderGraph(
    const nlohmann::json& arguments,
    const Engine::CommandProcessor& processor,
    const std::string_view requestId) const
{
    const std::string api = ReadGraphicsApi(arguments);
    const std::string queueMode = ReadQueueMode(arguments);
    const std::uint32_t width = ReadDimension(arguments, "width", 1600);
    const std::uint32_t height = ReadDimension(arguments, "height", 900);
    const std::uint32_t asyncWorkloadMultiplier =
        ReadAsyncWorkloadMultiplier(arguments);
    const bool gpuDriven =
        arguments.value("gpuDriven", false);
    const RenderWorldInput world = PrepareRenderWorld(arguments, processor, requestId);
    const std::filesystem::path output = ResolveGeneratedPath(arguments.value(
        "output", std::string("automation/reports/rdg-") + api + ".json"));
    if (!output.parent_path().empty()) std::filesystem::create_directories(output.parent_path());
    std::filesystem::remove(output);

    std::filesystem::path worldReceiptPath;
    if (world.enabled)
    {
        worldReceiptPath = ResolveGeneratedPath(
            std::filesystem::path("automation/reports")
            / (SanitizeArtifactName(requestId) + '-' + api + "-world.json"));
        if (!worldReceiptPath.parent_path().empty())
        {
            std::filesystem::create_directories(worldReceiptPath.parent_path());
        }
        std::filesystem::remove(worldReceiptPath);
    }
    const std::filesystem::path processBase = ResolveGeneratedPath(
        std::filesystem::path("automation/reports/process")
        / (SanitizeArtifactName(requestId) + '-' + api + "-rdg"));
    const std::filesystem::path structuredLogPath =
        processBase.string() + ".jsonl";
    const std::filesystem::path stdoutPath =
        processBase.string() + ".stdout.log";
    const std::filesystem::path stderrPath =
        processBase.string() + ".stderr.log";
    const std::filesystem::path crashReportPath =
        processBase.string() + ".crash.json";
    const std::filesystem::path gpuTimingReportPath =
        processBase.string() + ".gpu.json";
    const std::filesystem::path cpuTracePath =
        processBase.string() + ".cpu.json";
    const std::filesystem::path performanceIdentityPath =
        processBase.string() + ".identity.json";
    const std::filesystem::path buildIdentityPath =
        processBase.string() + ".build.json";
    const std::filesystem::path minidumpPath =
        processBase.string() + ".dmp";
    std::filesystem::create_directories(processBase.parent_path());
    std::filesystem::remove(structuredLogPath);
    std::filesystem::remove(stdoutPath);
    std::filesystem::remove(stderrPath);
    std::filesystem::remove(crashReportPath);
    std::filesystem::remove(gpuTimingReportPath);
    std::filesystem::remove(cpuTracePath);
    std::filesystem::remove(performanceIdentityPath);
    std::filesystem::remove(buildIdentityPath);
    std::filesystem::remove(minidumpPath);

    EnvironmentVariableGuard headless(L"PRISM_RENDER_HEADLESS", L"1");
    EnvironmentVariableGuard deterministic(L"PRISM_RENDER_DETERMINISTIC", L"1");
    EnvironmentVariableGuard gpuDrivenOverride(
        L"PRISM_RENDER_GPU_DRIVEN_OVERRIDE",
        gpuDriven ? L"1" : L"0");
    EnvironmentVariableGuard rdgQueueMode(
        L"PRISM_RENDER_RDG_QUEUE_MODE",
        Widen(queueMode));
    EnvironmentVariableGuard renderWidth(L"PRISM_RENDER_WIDTH", std::to_wstring(width));
    EnvironmentVariableGuard renderHeight(L"PRISM_RENDER_HEIGHT", std::to_wstring(height));
    EnvironmentVariableGuard asyncWorkload(
        L"PRISM_RENDER_ASYNC_WORKLOAD_MULTIPLIER",
        std::to_wstring(asyncWorkloadMultiplier));
    EnvironmentVariableGuard reportPath(L"PRISM_RENDER_RDG_REPORT_PATH", output.wstring());
    EnvironmentVariableGuard exitAfterReport(L"PRISM_RENDER_EXIT_AFTER_RDG_REPORT", L"1");
    EnvironmentVariableGuard capturePath(L"PRISM_RENDER_CAPTURE_PATH", L"");
    EnvironmentVariableGuard exitAfterCapture(L"PRISM_RENDER_EXIT_AFTER_CAPTURE", L"0");
    EnvironmentVariableGuard worldPath(
        L"PRISM_RENDER_WORLD_PATH",
        world.enabled ? world.snapshotPath.wstring() : std::wstring{});
    EnvironmentVariableGuard expectedWorldHash(
        L"PRISM_RENDER_EXPECTED_WORLD_HASH",
        world.enabled ? Widen(world.worldHash) : std::wstring{});
    EnvironmentVariableGuard worldReportPath(
        L"PRISM_RENDER_WORLD_REPORT_PATH",
        world.enabled ? worldReceiptPath.wstring() : std::wstring{});
    EnvironmentVariableGuard assetManifestPath(
        L"PRISM_RENDER_ASSET_MANIFEST_PATH",
        world.assetManifestPath.wstring());
    EnvironmentVariableGuard expectedAssetManifestHash(
        L"PRISM_RENDER_EXPECTED_ASSET_MANIFEST_HASH",
        Widen(world.assetManifestHash));
    EnvironmentVariableGuard structuredLog(
        L"PRISM_RENDER_LOG_PATH",
        structuredLogPath.wstring());
    EnvironmentVariableGuard crashReport(
        L"PRISM_RENDER_CRASH_REPORT_PATH",
        crashReportPath.wstring());
    EnvironmentVariableGuard gpuTimingReport(
        L"PRISM_RENDER_GPU_TIMING_REPORT_PATH",
        gpuTimingReportPath.wstring());
    EnvironmentVariableGuard cpuTrace(
        L"PRISM_RENDER_CPU_TRACE_PATH",
        cpuTracePath.wstring());
    EnvironmentVariableGuard correlationId(
        L"PRISM_RENDER_CORRELATION_ID",
        Widen(std::string(requestId)));
    EnvironmentVariableGuard performanceIdentity(
        L"PRISM_RENDER_PERFORMANCE_IDENTITY_PATH",
        performanceIdentityPath.wstring());
    EnvironmentVariableGuard buildIdentity(
        L"PRISM_RENDER_BUILD_IDENTITY_PATH",
        buildIdentityPath.wstring());
    EnvironmentVariableGuard minidump(
        L"PRISM_RENDER_MINIDUMP_PATH",
        minidumpPath.wstring());

    const RendererProcessResult process = RunRendererProcess(
        m_executableDirectory / "PrismRender.exe",
        m_projectRoot,
        api,
        ReadTimeout(arguments),
        structuredLogPath,
        stdoutPath,
        stderrPath,
        crashReportPath,
        minidumpPath,
        cpuTracePath,
        performanceIdentityPath,
        buildIdentityPath);
    json worldReceipt = json::object();
    if (world.enabled)
    {
        worldReceipt = ReadRenderedWorldReceipt(
            worldReceiptPath,
            world.worldHash,
            world.assetManifestHash,
            api,
            process);
    }
    if (process.exitCode != 0)
    {
        ThrowRendererFailure(process, api);
    }
    std::ifstream input(output, std::ios::binary);
    if (!input) throw ToolError("rdg_report_missing", "The renderer did not produce a RenderGraph report.");
    json graph;
    input >> graph;
    if (graph.value("format", std::string{}) != "PrismRenderGraphReport")
    {
        throw ToolError("rdg_report_invalid", "The renderer produced an invalid RenderGraph report.");
    }
    const json gpuTimings =
        ReadGpuTimingReport(gpuTimingReportPath);
    const json cpuTraceReport =
        ReadCpuTraceReport(cpuTracePath, requestId);
    const json performanceIdentityReport =
        ReadPerformanceIdentityReport(performanceIdentityPath, api);
    return {
        {"api", api},
        {"queueMode", queueMode},
        {"asyncWorkloadMultiplier",
         asyncWorkloadMultiplier},
        {"gpuDriven", gpuDriven},
        {"path", output.generic_string()},
        {"width", width},
        {"height", height},
        {"worldSource", world.source},
        {"worldHash", world.worldHash},
        {"assetManifestPath", world.assetManifestPath.generic_string()},
        {"assetManifestHash", world.assetManifestHash},
        {"renderedWorldHash", world.enabled
             ? worldReceipt.value("renderedWorldHash", std::string{})
             : std::string{}},
        {"snapshotPath", world.enabled ? world.snapshotPath.generic_string() : std::string{}},
        {"worldReceiptPath", world.enabled ? worldReceiptPath.generic_string() : std::string{}},
        {"worldReceipt", std::move(worldReceipt)},
        {"graph", std::move(graph)},
        {"process", SerializeRendererProcess(process)},
        {"gpuTimingReportPath", gpuTimingReportPath.generic_string()},
        {"gpuTimings", gpuTimings},
        {"cpuTracePath", cpuTracePath.generic_string()},
        {"cpuTrace", cpuTraceReport},
        {"performanceIdentityPath",
         performanceIdentityPath.generic_string()},
        {"performanceIdentity", performanceIdentityReport},
        {"buildIdentityPath",
         buildIdentityPath.generic_string()},
        {"buildIdentity", process.buildIdentity}};
}

nlohmann::json HarnessTools::MeasurePerformance(
    const nlohmann::json& arguments,
    const Engine::CommandProcessor& processor,
    const std::string_view requestId) const
{
    const std::string api = ReadGraphicsApi(arguments);
    const std::string stage = ReadCaptureStage(arguments);
    const std::string queueMode = ReadQueueMode(arguments);
    const std::uint32_t width = ReadDimension(arguments, "width", 1600);
    const std::uint32_t height = ReadDimension(arguments, "height", 900);
    const std::uint64_t warmupCount =
        arguments.value("warmupCount", 1ull);
    const std::uint64_t sampleCount =
        arguments.value("sampleCount", 3ull);
    if (warmupCount > 5)
    {
        throw ToolError(
            "invalid_warmup_count",
            "warmupCount must be between 0 and 5.");
    }
    if (sampleCount < 1 || sampleCount > 20)
    {
        throw ToolError(
            "invalid_sample_count",
            "sampleCount must be between 1 and 20.");
    }

    const RenderWorldInput world =
        PrepareRenderWorld(arguments, processor, requestId);
    json captureArguments = arguments;
    captureArguments["api"] = api;
    captureArguments["stage"] = stage;
    captureArguments["width"] = width;
    captureArguments["height"] = height;
    captureArguments["enforce"] = false;

    json runtimeIdentity = json::object();
    const auto validateRuntimeIdentity =
        [&](const json& capture)
        {
            const json& reported =
                capture.at("performanceIdentity").at("identity");
            if (runtimeIdentity.empty())
            {
                runtimeIdentity = reported;
                return;
            }
            if (runtimeIdentity != reported)
            {
                throw ToolError(
                    "runtime_performance_identity_changed",
                    "The renderer hardware or build identity changed during performance sampling.",
                    {{"expected", runtimeIdentity},
                     {"actual", reported}});
            }
        };

    json warmups = json::array();
    for (std::uint64_t index = 0; index < warmupCount; ++index)
    {
        const std::string sampleRequest =
            SanitizeArtifactName(requestId)
            + "-warmup-" + std::to_string(index);
        captureArguments["output"] =
            "automation/captures/performance/"
            + sampleRequest + '-' + api + '-' + stage + ".bmp";
        const json capture = CaptureRender(
            captureArguments,
            processor,
            sampleRequest,
            &world);
        validateRuntimeIdentity(capture);
        warmups.push_back({
            {"index", index},
            {"elapsedMilliseconds", capture.at("elapsedMilliseconds")},
            {"cpuTracePath", capture.at("cpuTracePath")},
            {"performanceIdentityPath",
             capture.at("performanceIdentityPath")},
            {"process", capture.at("process")}});
    }

    std::vector<double> elapsedSamples;
    std::map<std::string, std::vector<double>> gpuPassSamples;
    std::map<std::string, std::vector<double>> cpuSpanSamples;
    std::map<std::string, std::vector<double>>
        queueTimelineSamples;
    json samples = json::array();
    for (std::uint64_t index = 0; index < sampleCount; ++index)
    {
        const std::string sampleRequest =
            SanitizeArtifactName(requestId)
            + "-sample-" + std::to_string(index);
        captureArguments["output"] =
            "automation/captures/performance/"
            + sampleRequest + '-' + api + '-' + stage + ".bmp";
        const json capture = CaptureRender(
            captureArguments,
            processor,
            sampleRequest,
            &world);
        validateRuntimeIdentity(capture);
        const double elapsed =
            capture.at("elapsedMilliseconds").get<double>();
        elapsedSamples.push_back(elapsed);
        for (const json& pass :
             capture.at("gpuTimings").value(
                 "passes",
                 json::array()))
        {
            gpuPassSamples[
                pass.at("name").get<std::string>()].push_back(
                    pass.at("gpuMilliseconds").get<double>());
        }
        const json& timeline =
            capture.at("gpuTimings").value(
                "timeline",
                json::object());
        for (const char* metric : {
                 "frameGpuMilliseconds",
                 "graphicsBusyMilliseconds",
                 "computeBusyMilliseconds",
                 "overlapMilliseconds",
                 "overlapRatio",
                 "computeOverlapRatio"})
        {
            if (timeline.contains(metric)
                && timeline.at(metric).is_number())
            {
                queueTimelineSamples[metric].push_back(
                    timeline.at(metric).get<double>());
            }
        }
        for (const json& span :
             capture.at("cpuTrace").value(
                 "summary",
                 json::array()))
        {
            const std::string name =
                span.value("category", std::string("uncategorized"))
                + "/" + span.at("name").get<std::string>();
            cpuSpanSamples[name].push_back(
                span.at("maximumMilliseconds").get<double>());
        }
        samples.push_back({
            {"index", index},
            {"elapsedMilliseconds", elapsed},
            {"capturePath", capture.at("path")},
            {"gpuTimings", capture.at("gpuTimings")},
            {"cpuTracePath", capture.at("cpuTracePath")},
            {"cpuTraceSummary",
             capture.at("cpuTrace").at("summary")},
            {"performanceIdentityPath",
             capture.at("performanceIdentityPath")},
            {"process", capture.at("process")}});
    }

    PerformanceBaselineRecord current{};
    current.identity.api = api;
    current.identity.stage = stage;
    current.identity.width = width;
    current.identity.height = height;
    current.identity.worldHash = world.worldHash;
    current.identity.assetManifestHash = world.assetManifestHash;
    ApplyRuntimePerformanceIdentity(
        runtimeIdentity,
        current.identity);
    current.sampleCount = elapsedSamples.size();
    current.statistics =
        PerformanceBaseline::ComputeStatistics(elapsedSamples);

    json gpuPassStatistics = json::object();
    for (const auto& [name, passSamples] : gpuPassSamples)
    {
        gpuPassStatistics[name] =
            PerformanceBaseline::Serialize(
                PerformanceBaseline::ComputeStatistics(passSamples));
    }

    json cpuSpanStatistics = json::object();
    for (const auto& [name, spanSamples] : cpuSpanSamples)
    {
        cpuSpanStatistics[name] =
            PerformanceBaseline::Serialize(
                PerformanceBaseline::ComputeStatistics(
                    spanSamples));
    }

    json queueTimelineStatistics = json::object();
    for (const auto& [name, metricSamples] :
         queueTimelineSamples)
    {
        queueTimelineStatistics[name] = {
            {"unit",
             name.ends_with("Ratio")
                 ? "ratio"
                 : "milliseconds"},
            {"statistics",
             PerformanceBaseline::Serialize(
                 PerformanceBaseline::ComputeStatistics(
                     metricSamples))}};
    }

    json result = {
        {"metric", "rendererProcessWallMilliseconds"},
        {"queueMode", queueMode},
        {"asyncWorkloadMultiplier",
         ReadAsyncWorkloadMultiplier(arguments)},
        {"identity", PerformanceBaseline::Serialize(current).at("identity")},
        {"warmupCount", warmupCount},
        {"sampleCount", sampleCount},
        {"warmups", std::move(warmups)},
        {"samples", std::move(samples)},
        {"statistics", PerformanceBaseline::Serialize(current.statistics)},
        {"gpuPassStatistics", gpuPassStatistics},
        {"queueTimelineStatistics",
         queueTimelineStatistics},
        {"cpuSpanStatistics", cpuSpanStatistics},
        {"gpuBudgetResults", json::array()},
        {"cpuBudgetResults", json::array()},
        {"runtimePerformanceIdentity", runtimeIdentity},
        {"baselineUpdated", false},
        {"comparison", json::object()}};

    if (arguments.contains("passBudgets"))
    {
        const json& budgets = arguments.at("passBudgets");
        if (!budgets.is_object())
        {
            throw ToolError(
                "invalid_gpu_budgets",
                "passBudgets must be a JSON object keyed by RDG pass name.");
        }
        bool budgetsPassed = true;
        json budgetResults = json::array();
        for (const auto& [name, serializedBudget] : budgets.items())
        {
            const double budget = serializedBudget.get<double>();
            if (budget <= 0.0)
            {
                throw ToolError(
                    "invalid_gpu_budget",
                    "GPU pass budgets must be greater than zero.");
            }
            const auto found = gpuPassSamples.find(name);
            if (found == gpuPassSamples.end())
            {
                budgetsPassed = false;
                budgetResults.push_back({
                    {"pass", name},
                    {"budgetMilliseconds", budget},
                    {"present", false},
                    {"passed", false}});
                continue;
            }
            const PerformanceStatistics statistics =
                PerformanceBaseline::ComputeStatistics(found->second);
            const bool passed =
                statistics.p95Milliseconds <= budget;
            budgetsPassed = budgetsPassed && passed;
            budgetResults.push_back({
                {"pass", name},
                {"budgetMilliseconds", budget},
                {"present", true},
                {"medianGpuMilliseconds", statistics.medianMilliseconds},
                {"p95GpuMilliseconds", statistics.p95Milliseconds},
                {"passed", passed}});
        }
        result["gpuBudgetResults"] = std::move(budgetResults);
        result["gpuBudgetsPassed"] = budgetsPassed;
        if (arguments.value("enforceGpuBudgets", true)
            && !budgetsPassed)
        {
            throw ToolError(
                "gpu_pass_budget_exceeded",
                "One or more RDG GPU pass budgets were exceeded.",
                result);
        }
    }

    if (arguments.contains("cpuSpanBudgets"))
    {
        const json& budgets = arguments.at("cpuSpanBudgets");
        if (!budgets.is_object())
        {
            throw ToolError(
                "invalid_cpu_budgets",
                "cpuSpanBudgets must be a JSON object keyed by category/span name.");
        }
        bool budgetsPassed = true;
        json budgetResults = json::array();
        for (const auto& [name, serializedBudget] :
             budgets.items())
        {
            const double budget = serializedBudget.get<double>();
            if (budget <= 0.0)
            {
                throw ToolError(
                    "invalid_cpu_budget",
                    "CPU span budgets must be greater than zero.");
            }
            const auto found = cpuSpanSamples.find(name);
            if (found == cpuSpanSamples.end())
            {
                budgetsPassed = false;
                budgetResults.push_back({
                    {"span", name},
                    {"budgetMilliseconds", budget},
                    {"present", false},
                    {"passed", false}});
                continue;
            }
            const PerformanceStatistics statistics =
                PerformanceBaseline::ComputeStatistics(
                    found->second);
            const bool passed =
                statistics.p95Milliseconds <= budget;
            budgetsPassed = budgetsPassed && passed;
            budgetResults.push_back({
                {"span", name},
                {"budgetMilliseconds", budget},
                {"present", true},
                {"medianCpuMilliseconds",
                 statistics.medianMilliseconds},
                {"p95CpuMilliseconds",
                 statistics.p95Milliseconds},
                {"passed", passed}});
        }
        result["cpuBudgetResults"] =
            std::move(budgetResults);
        result["cpuBudgetsPassed"] = budgetsPassed;
        if (arguments.value("enforceCpuBudgets", true)
            && !budgetsPassed)
        {
            throw ToolError(
                "cpu_span_budget_exceeded",
                "One or more CPU span budgets were exceeded.",
                result);
        }
    }

    if (arguments.contains("baselinePath"))
    {
        const std::filesystem::path baselinePath =
            ResolveGeneratedPath(
                arguments.at("baselinePath").get<std::string>());
        result["baselinePath"] = baselinePath.generic_string();
        if (arguments.value("updateBaseline", false))
        {
            std::string saveError;
            if (!PerformanceBaseline::Save(
                    baselinePath,
                    current,
                    &saveError))
            {
                throw ToolError(
                    "performance_baseline_save_failed",
                    saveError);
            }
            result["baselineUpdated"] = true;
            result["baseline"] =
                PerformanceBaseline::Serialize(current);
        }
        else
        {
            PerformanceBaselineRecord baseline{};
            std::string loadError;
            if (!PerformanceBaseline::Load(
                    baselinePath,
                    baseline,
                    &loadError))
            {
                throw ToolError(
                    "performance_baseline_load_failed",
                    loadError,
                    {{"baselinePath", baselinePath.generic_string()}});
            }
            const double maximumRegressionPercent =
                arguments.value("maximumRegressionPercent", 15.0);
            const PerformanceComparison comparison =
                PerformanceBaseline::Compare(
                    baseline,
                    current,
                    maximumRegressionPercent);
            result["baseline"] =
                PerformanceBaseline::Serialize(baseline);
            result["comparison"] =
                PerformanceBaseline::Serialize(comparison);
            if (!comparison.identityMatches)
            {
                throw ToolError(
                    "performance_identity_mismatch",
                    "The performance sample identity does not match the baseline.",
                    result);
            }
            if (arguments.value("enforce", true)
                && !comparison.passed)
            {
                throw ToolError(
                    "performance_regression",
                    "Renderer process performance exceeded the baseline tolerance.",
                    result);
            }
        }
    }
    else if (arguments.value("updateBaseline", false))
    {
        throw ToolError(
            "missing_argument",
            "performance.measure requires baselinePath when updateBaseline is true.");
    }

    return result;
}

nlohmann::json HarnessTools::CompareQueueModes(
    const nlohmann::json& arguments,
    const Engine::CommandProcessor& processor,
    const std::string_view requestId) const
{
    const double maximumRegressionPercent =
        arguments.value(
            "maximumRegressionPercent",
            5.0);
    if (maximumRegressionPercent < 0.0)
    {
        throw ToolError(
            "invalid_regression_tolerance",
            "maximumRegressionPercent cannot be negative.");
    }

    const RenderWorldInput world =
        PrepareRenderWorld(
            arguments,
            processor,
            requestId);
    json measureArguments = arguments;
    for (const char* key : {
             "baselinePath",
             "updateBaseline",
             "reportPath",
             "output",
             "passBudgets",
             "cpuSpanBudgets",
             "enforceGpuBudgets",
             "enforceCpuBudgets"})
    {
        measureArguments.erase(key);
    }
    measureArguments["enforce"] = false;
    if (world.enabled)
    {
        measureArguments["useCurrentWorld"] = false;
        measureArguments["worldSnapshot"] =
            world.snapshotPath.generic_string();
    }

    json serialArguments = measureArguments;
    serialArguments["queueMode"] = "serial";
    json nativeArguments = measureArguments;
    nativeArguments["queueMode"] = "native";

    const json serial = MeasurePerformance(
        serialArguments,
        processor,
        std::string(requestId) + "-serial");
    const json native = MeasurePerformance(
        nativeArguments,
        processor,
        std::string(requestId) + "-native");

    if (!serial.at("gpuPassStatistics").contains("Renderer")
        || !native.at("gpuPassStatistics").contains("Renderer"))
    {
        throw ToolError(
            "gpu_renderer_timing_missing",
            "Queue mode comparison requires the Renderer GPU timing.");
    }
    const json& serialRenderer =
        serial.at("gpuPassStatistics").at("Renderer");
    const json& nativeRenderer =
        native.at("gpuPassStatistics").at("Renderer");
    const double serialMedian =
        serialRenderer.at("medianMilliseconds").get<double>();
    const double nativeMedian =
        nativeRenderer.at("medianMilliseconds").get<double>();
    const double serialP95 =
        serialRenderer.at("p95Milliseconds").get<double>();
    const double nativeP95 =
        nativeRenderer.at("p95Milliseconds").get<double>();
    const auto percentChange =
        [](const double baseline, const double current)
        {
            if (baseline <= 0.0)
            {
                return current <= 0.0 ? 0.0 : 100.0;
            }
            return (current - baseline) * 100.0 / baseline;
        };
    const double medianRegressionPercent =
        percentChange(serialMedian, nativeMedian);
    const double p95RegressionPercent =
        percentChange(serialP95, nativeP95);
    const bool identityMatches =
        serial.at("identity") == native.at("identity")
        && serial.at("runtimePerformanceIdentity")
            == native.at("runtimePerformanceIdentity");

    const auto readTimelineMedian =
        [](const json& measurement, const char* metric)
        {
            const json& statistics =
                measurement.at("queueTimelineStatistics");
            if (!statistics.contains(metric))
            {
                return 0.0;
            }
            return statistics.at(metric)
                .at("statistics")
                .at("medianMilliseconds")
                .get<double>();
        };
    const double nativeComputeBusyMilliseconds =
        readTimelineMedian(native, "computeBusyMilliseconds");
    const double nativeOverlapMilliseconds =
        readTimelineMedian(native, "overlapMilliseconds");
    const double nativeOverlapRatio =
        readTimelineMedian(native, "overlapRatio");
    const double nativeComputeOverlapRatio =
        readTimelineMedian(native, "computeOverlapRatio");
    const bool nativeMultiQueueObserved =
        nativeComputeBusyMilliseconds > 0.0;
    bool crossQueueCalibrated = true;
    for (const json& sample : native.at("samples"))
    {
        crossQueueCalibrated =
            crossQueueCalibrated
            && sample.at("gpuTimings")
                .value("timeline", json::object())
                .value("crossQueueCalibrated", false);
    }
    const bool passed =
        identityMatches
        && crossQueueCalibrated
        && nativeMultiQueueObserved
        && medianRegressionPercent
            <= maximumRegressionPercent
        && p95RegressionPercent
            <= maximumRegressionPercent;

    json result = {
        {"format", "PrismQueueModeComparison"},
        {"version", 1},
        {"metric", "RendererGpuMilliseconds"},
        {"worldHash", world.worldHash},
        {"assetManifestHash", world.assetManifestHash},
        {"maximumRegressionPercent",
         maximumRegressionPercent},
        {"serial", serial},
        {"native", native},
        {"comparison", {
            {"identityMatches", identityMatches},
            {"crossQueueCalibrated",
             crossQueueCalibrated},
            {"nativeMultiQueueObserved",
             nativeMultiQueueObserved},
            {"serialMedianGpuMilliseconds",
             serialMedian},
            {"nativeMedianGpuMilliseconds",
             nativeMedian},
            {"serialP95GpuMilliseconds",
             serialP95},
            {"nativeP95GpuMilliseconds",
             nativeP95},
            {"medianRegressionPercent",
             medianRegressionPercent},
            {"p95RegressionPercent",
             p95RegressionPercent},
            {"medianImprovementPercent",
             -medianRegressionPercent},
            {"p95ImprovementPercent",
             -p95RegressionPercent},
            {"nativeComputeBusyMilliseconds",
             nativeComputeBusyMilliseconds},
            {"nativeOverlapMilliseconds",
             nativeOverlapMilliseconds},
            {"nativeOverlapRatio",
             nativeOverlapRatio},
            {"nativeComputeOverlapRatio",
             nativeComputeOverlapRatio},
            {"passed", passed}}}};

    if (arguments.contains("reportPath"))
    {
        const std::filesystem::path reportPath =
            ResolveGeneratedPath(
                arguments.at("reportPath").get<std::string>());
        if (!reportPath.parent_path().empty())
        {
            std::filesystem::create_directories(
                reportPath.parent_path());
        }
        std::ofstream output(
            reportPath,
            std::ios::binary | std::ios::trunc);
        if (!output)
        {
            throw ToolError(
                "queue_comparison_report_failed",
                "Could not create the queue mode comparison report.");
        }
        output << std::setw(2) << result << '\n';
        if (!output)
        {
            throw ToolError(
                "queue_comparison_report_failed",
                "Could not write the queue mode comparison report.");
        }
        result["reportPath"] =
            reportPath.generic_string();
    }

    if (arguments.value("enforce", false)
        && !passed)
    {
        throw ToolError(
            "queue_mode_comparison_failed",
            "Native multi-queue execution did not satisfy the A/B comparison requirements.",
            result);
    }
    return result;
}

std::filesystem::path HarnessTools::ResolveProjectPath(const std::filesystem::path& path) const
{
    const std::filesystem::path candidate = std::filesystem::weakly_canonical(
        std::filesystem::absolute(path.is_absolute() ? path : m_projectRoot / path));
    auto rootIterator = m_projectRoot.begin();
    auto candidateIterator = candidate.begin();
    for (; rootIterator != m_projectRoot.end(); ++rootIterator, ++candidateIterator)
    {
        if (candidateIterator == candidate.end()
            || _wcsicmp(rootIterator->c_str(), candidateIterator->c_str()) != 0)
        {
            throw ToolError("path_outside_project", "Harness file access is restricted to the project root.");
        }
    }
    return candidate;
}

std::filesystem::path HarnessTools::ResolveGeneratedPath(const std::filesystem::path& path) const
{
    const std::filesystem::path resolved = ResolveProjectPath(path);
    const std::filesystem::path relative = resolved.lexically_relative(m_projectRoot);
    if (relative.empty() || relative == ".")
    {
        throw ToolError(
            "output_path_restricted",
            "Generated files must be written under automation/ or a build* directory.");
    }
    const std::string firstComponent = Lowercase(relative.begin()->string());
    if (firstComponent != "automation" && !firstComponent.starts_with("build"))
    {
        throw ToolError(
            "output_path_restricted",
            "Generated files must be written under automation/ or a build* directory.");
    }
    return resolved;
}
} // namespace Prism::Automation
