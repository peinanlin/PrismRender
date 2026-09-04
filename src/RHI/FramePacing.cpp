#include "RHI/FramePacing.h"

#include <charconv>
#include <stdexcept>

namespace Prism::RHI
{
namespace
{
[[nodiscard]] std::uint32_t ParsePositiveInteger(
    const std::string_view value,
    const char* field)
{
    if (value.empty())
    {
        throw std::invalid_argument(
            std::string(field) + " requires a positive integer.");
    }
    std::uint32_t parsed = 0;
    const auto [end, error] = std::from_chars(
        value.data(), value.data() + value.size(), parsed);
    if (error != std::errc{}
        || end != value.data() + value.size()
        || parsed == 0)
    {
        throw std::invalid_argument(
            std::string(field) + " requires a positive integer.");
    }
    return parsed;
}
} // namespace

FramePacingConfiguration MakeFramePacingConfiguration(
    const FramePacingProfile profile)
{
    switch (profile)
    {
    case FramePacingProfile::InteractiveSmooth:
        return {profile, PresentationIntent::Synchronized, std::nullopt, 2};
    case FramePacingProfile::LowLatency:
        return {profile, PresentationIntent::LowLatencySynchronized,
            std::nullopt, 1};
    case FramePacingProfile::Benchmark:
        return {profile, PresentationIntent::Immediate, std::nullopt, 2};
    case FramePacingProfile::Custom:
        return {profile, PresentationIntent::Synchronized, std::nullopt, 2};
    }
    throw std::invalid_argument("Unknown frame-pacing profile.");
}

void ValidateFramePacingConfiguration(
    const FramePacingConfiguration& configuration)
{
    if (configuration.maxQueuedFrames == 0
        || configuration.maxQueuedFrames > MaximumQueuedFrames)
    {
        throw std::invalid_argument(
            "Maximum queued frames must be between 1 and 8.");
    }
    if (configuration.targetFps.has_value()
        && *configuration.targetFps == 0)
    {
        throw std::invalid_argument(
            "Target FPS must be a positive integer or omitted.");
    }
    if (configuration.profile == FramePacingProfile::LowLatency
        && (configuration.presentation
                != PresentationIntent::LowLatencySynchronized
            || configuration.maxQueuedFrames != 1
            || configuration.targetFps.has_value()))
    {
        throw std::invalid_argument(
            "The low-latency preset cannot be overridden; use custom.");
    }
    if (configuration.profile == FramePacingProfile::InteractiveSmooth
        && configuration != MakeFramePacingConfiguration(
            FramePacingProfile::InteractiveSmooth))
    {
        throw std::invalid_argument(
            "The interactive-smooth preset cannot be overridden; use custom.");
    }
    if (configuration.profile == FramePacingProfile::Benchmark
        && configuration != MakeFramePacingConfiguration(
            FramePacingProfile::Benchmark))
    {
        throw std::invalid_argument(
            "The benchmark preset cannot be overridden; use custom.");
    }
}

FramePacingConfiguration ParseFramePacingConfiguration(
    const std::string_view profile,
    const std::string_view presentation,
    const std::string_view targetFps,
    const std::string_view maxQueuedFrames)
{
    const FramePacingProfile parsedProfile =
        ParseFramePacingProfile(profile);
    FramePacingConfiguration result =
        MakeFramePacingConfiguration(parsedProfile);
    const bool hasCustomFields = !presentation.empty()
        || !targetFps.empty() || !maxQueuedFrames.empty();
    if (parsedProfile != FramePacingProfile::Custom)
    {
        if (hasCustomFields)
        {
            throw std::invalid_argument(
                "Frame-pacing preset fields can only be overridden by custom.");
        }
        return result;
    }
    if (presentation.empty() || maxQueuedFrames.empty())
    {
        throw std::invalid_argument(
            "Custom frame pacing requires presentation and max queued frames.");
    }
    result.presentation = ParsePresentationIntent(presentation);
    result.maxQueuedFrames = ParsePositiveInteger(
        maxQueuedFrames, "Maximum queued frames");
    if (!targetFps.empty() && targetFps != "uncapped")
    {
        result.targetFps = ParsePositiveInteger(targetFps, "Target FPS");
    }
    ValidateFramePacingConfiguration(result);
    return result;
}

FramePacingProfile ParseFramePacingProfile(const std::string_view value)
{
    if (value.empty() || value == "interactive-smooth")
        return FramePacingProfile::InteractiveSmooth;
    if (value == "low-latency") return FramePacingProfile::LowLatency;
    if (value == "benchmark") return FramePacingProfile::Benchmark;
    if (value == "custom") return FramePacingProfile::Custom;
    throw std::invalid_argument(
        "Frame-pacing profile must be interactive-smooth, low-latency, benchmark, or custom.");
}

PresentationIntent ParsePresentationIntent(const std::string_view value)
{
    if (value == "synchronized") return PresentationIntent::Synchronized;
    if (value == "low-latency-synchronized")
        return PresentationIntent::LowLatencySynchronized;
    if (value == "immediate") return PresentationIntent::Immediate;
    throw std::invalid_argument(
        "Presentation intent must be synchronized, low-latency-synchronized, or immediate.");
}

std::string_view ToString(const FramePacingProfile profile) noexcept
{
    switch (profile)
    {
    case FramePacingProfile::InteractiveSmooth: return "interactive-smooth";
    case FramePacingProfile::LowLatency: return "low-latency";
    case FramePacingProfile::Benchmark: return "benchmark";
    case FramePacingProfile::Custom: return "custom";
    }
    return "unknown";
}

std::string_view ToString(const PresentationIntent intent) noexcept
{
    switch (intent)
    {
    case PresentationIntent::Synchronized: return "synchronized";
    case PresentationIntent::LowLatencySynchronized:
        return "low-latency-synchronized";
    case PresentationIntent::Immediate: return "immediate";
    }
    return "unknown";
}

std::string_view ToString(const FrameAdmissionSource source) noexcept
{
    switch (source)
    {
    case FrameAdmissionSource::None: return "none";
    case FrameAdmissionSource::DxgiFrameLatencyWaitableObject:
        return "dxgi-frame-latency-waitable-object";
    case FrameAdmissionSource::VulkanAcquire: return "vulkan-acquire";
    case FrameAdmissionSource::VulkanPresentWait: return "vulkan-present-wait";
    case FrameAdmissionSource::FrameQueueFallback: return "frame-queue-fallback";
    }
    return "unknown";
}
} // namespace Prism::RHI
