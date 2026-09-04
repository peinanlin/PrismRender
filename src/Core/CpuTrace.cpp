#include "Core/CpuTrace.h"

#include "Core/Environment.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <map>
#include <mutex>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>
#include <thread>

#if defined(_WIN32)
#include <Windows.h>
#else
#include <unistd.h>
#endif

#include <json.hpp>

namespace Prism::Core
{
namespace
{
using Clock = std::chrono::steady_clock;
using json = nlohmann::json;
using ThreadId = std::uint64_t;

struct ActiveSpan
{
    std::uint64_t id = 0;
    std::uint64_t parentId = 0;
    std::string name;
    std::string category;
    ThreadId threadId = 0;
    Clock::time_point start;
};

struct CompletedSpan
{
    std::uint64_t id = 0;
    std::uint64_t parentId = 0;
    std::string name;
    std::string category;
    ThreadId threadId = 0;
    double startMilliseconds = 0.0;
    double durationMilliseconds = 0.0;
    bool complete = true;
};

struct SpanSummary
{
    std::string name;
    std::string category;
    std::size_t count = 0;
    double totalMilliseconds = 0.0;
    double maximumMilliseconds = 0.0;
};

std::mutex TraceMutex;
std::atomic<bool> TraceEnabled = false;
std::atomic<std::uint64_t> NextSpanId = 1;
std::filesystem::path TracePath;
std::string CorrelationId;
Clock::time_point TraceStart;
std::unordered_map<std::uint64_t, ActiveSpan> ActiveSpans;
std::vector<CompletedSpan> CompletedSpans;
thread_local std::vector<std::uint64_t> ThreadSpanStack;

ThreadId GetThreadId()
{
    return static_cast<ThreadId>(
        std::hash<std::thread::id>{}(
            std::this_thread::get_id()));
}

std::uint64_t GetProcessId()
{
#if defined(_WIN32)
    return static_cast<std::uint64_t>(
        ::GetCurrentProcessId());
#else
    return static_cast<std::uint64_t>(::getpid());
#endif
}

double ToMilliseconds(
    const Clock::duration duration)
{
    return std::chrono::duration<double, std::milli>(
        duration).count();
}

json SerializeSpan(const CompletedSpan& span)
{
    json parent = span.parentId == 0
        ? json(nullptr)
        : json(span.parentId);
    return {
        {"id", span.id},
        {"parentSpanId", std::move(parent)},
        {"name", span.name},
        {"category", span.category},
        {"threadId", span.threadId},
        {"startMilliseconds", span.startMilliseconds},
        {"durationMilliseconds", span.durationMilliseconds},
        {"complete", span.complete}};
}
} // namespace

void CpuTrace::InitializeFromEnvironment()
{
    Initialize(
        ReadEnvironmentVariableValue(
            "PRISM_RENDER_CPU_TRACE_PATH"),
        ReadEnvironmentVariableValue(
            "PRISM_RENDER_CORRELATION_ID"));
}

void CpuTrace::Initialize(
    const std::filesystem::path& outputPath,
    const std::string_view correlationId)
{
    std::scoped_lock lock(TraceMutex);
    TracePath = outputPath;
    CorrelationId = correlationId;
    TraceStart = Clock::now();
    ActiveSpans.clear();
    CompletedSpans.clear();
    ThreadSpanStack.clear();
    NextSpanId.store(1);
    TraceEnabled.store(!TracePath.empty());
}

bool CpuTrace::IsEnabled()
{
    return TraceEnabled.load();
}

std::uint64_t CpuTrace::BeginSpan(
    const std::string_view name,
    const std::string_view category)
{
    if (!TraceEnabled.load())
    {
        return 0;
    }
    ActiveSpan span{};
    span.id = NextSpanId.fetch_add(1);
    span.parentId = ThreadSpanStack.empty()
        ? 0
        : ThreadSpanStack.back();
    span.name = name;
    span.category = category;
    span.threadId = GetThreadId();
    span.start = Clock::now();
    {
        std::scoped_lock lock(TraceMutex);
        ActiveSpans.emplace(span.id, span);
    }
    ThreadSpanStack.push_back(span.id);
    return span.id;
}

void CpuTrace::EndSpan(const std::uint64_t spanId)
{
    if (spanId == 0 || !TraceEnabled.load())
    {
        return;
    }
    const Clock::time_point end = Clock::now();
    ActiveSpan active{};
    {
        std::scoped_lock lock(TraceMutex);
        const auto found = ActiveSpans.find(spanId);
        if (found == ActiveSpans.end())
        {
            return;
        }
        active = std::move(found->second);
        ActiveSpans.erase(found);
        CompletedSpans.push_back({
            active.id,
            active.parentId,
            std::move(active.name),
            std::move(active.category),
            active.threadId,
            ToMilliseconds(active.start - TraceStart),
            ToMilliseconds(end - active.start),
            true});
    }

    if (!ThreadSpanStack.empty()
        && ThreadSpanStack.back() == spanId)
    {
        ThreadSpanStack.pop_back();
    }
    else
    {
        const auto found = std::ranges::find(
            ThreadSpanStack,
            spanId);
        if (found != ThreadSpanStack.end())
        {
            ThreadSpanStack.erase(found);
        }
    }
}

bool CpuTrace::Flush(
    const bool success,
    const int exitCode,
    std::string* outError)
{
    if (!TraceEnabled.load())
    {
        return true;
    }
    try
    {
        const Clock::time_point end = Clock::now();
        std::vector<CompletedSpan> spans;
        std::filesystem::path outputPath;
        std::string correlationId;
        Clock::time_point traceStart;
        {
            std::scoped_lock lock(TraceMutex);
            spans = CompletedSpans;
            spans.reserve(spans.size() + ActiveSpans.size());
            for (const auto& [id, active] : ActiveSpans)
            {
                (void)id;
                spans.push_back({
                    active.id,
                    active.parentId,
                    active.name,
                    active.category,
                    active.threadId,
                    ToMilliseconds(active.start - TraceStart),
                    ToMilliseconds(end - active.start),
                    false});
            }
            outputPath = TracePath;
            correlationId = CorrelationId;
            traceStart = TraceStart;
        }
        std::ranges::sort(
            spans,
            [](const CompletedSpan& left,
               const CompletedSpan& right)
            {
                if (left.startMilliseconds
                    != right.startMilliseconds)
                {
                    return left.startMilliseconds
                        < right.startMilliseconds;
                }
                return left.id < right.id;
            });

        std::map<std::pair<std::string, std::string>, SpanSummary>
            summaries;
        json serializedSpans = json::array();
        for (const CompletedSpan& span : spans)
        {
            serializedSpans.push_back(SerializeSpan(span));
            SpanSummary& summary = summaries[
                {span.category, span.name}];
            summary.name = span.name;
            summary.category = span.category;
            ++summary.count;
            summary.totalMilliseconds +=
                span.durationMilliseconds;
            summary.maximumMilliseconds = std::max(
                summary.maximumMilliseconds,
                span.durationMilliseconds);
        }
        json serializedSummaries = json::array();
        for (const auto& [key, summary] : summaries)
        {
            (void)key;
            serializedSummaries.push_back({
                {"name", summary.name},
                {"category", summary.category},
                {"count", summary.count},
                {"totalMilliseconds",
                 summary.totalMilliseconds},
                {"maximumMilliseconds",
                 summary.maximumMilliseconds}});
        }

        if (!outputPath.parent_path().empty())
        {
            std::filesystem::create_directories(
                outputPath.parent_path());
        }
        const std::filesystem::path temporary =
            outputPath.parent_path()
            / (outputPath.filename().string() + ".tmp");
        {
            std::ofstream output(
                temporary,
                std::ios::binary | std::ios::trunc);
            if (!output)
            {
                throw std::runtime_error(
                    "Could not create the CPU Trace report.");
            }
            output << json{
                {"format", "PrismCpuTrace"},
                {"version", 1},
                {"correlationId", correlationId},
                {"processId", GetProcessId()},
                {"success", success},
                {"exitCode", exitCode},
                {"totalDurationMilliseconds",
                 ToMilliseconds(end - traceStart)},
                {"spans", std::move(serializedSpans)},
                {"summary", std::move(serializedSummaries)}}.dump(2)
                   << '\n';
        }
        if (std::filesystem::exists(outputPath))
        {
            std::filesystem::remove(outputPath);
        }
        std::filesystem::rename(temporary, outputPath);
        return true;
    }
    catch (const std::exception& exception)
    {
        if (outError != nullptr)
        {
            *outError = exception.what();
        }
        return false;
    }
}

CpuTraceSpan::CpuTraceSpan(
    const std::string_view name,
    const std::string_view category)
    : m_spanId(CpuTrace::BeginSpan(name, category))
{
}

CpuTraceSpan::~CpuTraceSpan()
{
    End();
}

CpuTraceSpan::CpuTraceSpan(CpuTraceSpan&& other) noexcept
    : m_spanId(std::exchange(other.m_spanId, 0))
{
}

CpuTraceSpan& CpuTraceSpan::operator=(
    CpuTraceSpan&& other) noexcept
{
    if (this != &other)
    {
        End();
        m_spanId = std::exchange(other.m_spanId, 0);
    }
    return *this;
}

void CpuTraceSpan::End()
{
    if (m_spanId != 0)
    {
        CpuTrace::EndSpan(m_spanId);
        m_spanId = 0;
    }
}
} // namespace Prism::Core
