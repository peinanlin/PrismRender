#include "Core/DiagnosticLog.h"

#include <atomic>

namespace Prism::Core
{
namespace
{
std::atomic<DiagnosticLog::Sink> CurrentSink = nullptr;
}

void DiagnosticLog::SetSink(const Sink sink) noexcept
{
    CurrentSink.store(sink, std::memory_order_release);
}

void DiagnosticLog::Write(
    const std::string_view level,
    const std::string_view event,
    const std::string_view message,
    const nlohmann::json& details)
{
    if (const Sink sink = CurrentSink.load(std::memory_order_acquire))
    {
        sink(level, event, message, details);
    }
}
} // namespace Prism::Core
