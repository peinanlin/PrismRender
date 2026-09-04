#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace Prism::RHI
{
inline constexpr std::uint32_t MaximumQueuedFrames = 8;

enum class FramePacingProfile : std::uint8_t
{
    InteractiveSmooth,
    LowLatency,
    Benchmark,
    Custom
};

enum class PresentationIntent : std::uint8_t
{
    Synchronized,
    LowLatencySynchronized,
    Immediate
};

enum class FrameAdmissionSource : std::uint8_t
{
    None,
    DxgiFrameLatencyWaitableObject,
    VulkanAcquire,
    VulkanPresentWait,
    FrameQueueFallback
};

struct FramePacingConfiguration
{
    FramePacingProfile profile =
        FramePacingProfile::InteractiveSmooth;
    PresentationIntent presentation =
        PresentationIntent::Synchronized;
    std::optional<std::uint32_t> targetFps;
    std::uint32_t maxQueuedFrames = 2;

    auto operator<=>(const FramePacingConfiguration&) const = default;
};

// Value-only state copied from the execution lane. Native wait handles and
// swapchain objects never cross the backend boundary.
struct FramePacingState
{
    FramePacingConfiguration requested{};
    PresentationIntent effectivePresentation =
        PresentationIntent::Synchronized;
    std::uint32_t effectiveMaxQueuedFrames = 2;
    std::uint32_t swapchainImageCount = 0;
    std::uint32_t frameResourceSlotCount = 0;
    std::uint32_t syncInterval = 1;
    std::uint32_t presentFlags = 0;
    std::uint64_t requestedGeneration = 1;
    std::uint64_t effectiveGeneration = 1;
    FrameAdmissionSource admissionSource =
        FrameAdmissionSource::None;
    std::string nativePresentMode;
    std::string fallbackReason;
    bool tearingSupported = false;
    bool tearingEnabled = false;
    bool transitionPending = false;
};

struct FrameAdmissionResult
{
    double displayWaitMilliseconds = 0.0;
    double limiterWaitMilliseconds = 0.0;
    double queueWaitMilliseconds = 0.0;
    std::uint64_t submittedFrames = 0;
    std::uint32_t outstandingFrames = 0;
    std::uint64_t configurationGeneration = 0;
};

[[nodiscard]] FramePacingConfiguration MakeFramePacingConfiguration(
    FramePacingProfile profile);
void ValidateFramePacingConfiguration(
    const FramePacingConfiguration& configuration);
[[nodiscard]] FramePacingConfiguration ParseFramePacingConfiguration(
    std::string_view profile,
    std::string_view presentation = {},
    std::string_view targetFps = {},
    std::string_view maxQueuedFrames = {});
[[nodiscard]] FramePacingProfile ParseFramePacingProfile(
    std::string_view value);
[[nodiscard]] PresentationIntent ParsePresentationIntent(
    std::string_view value);
[[nodiscard]] std::string_view ToString(FramePacingProfile profile) noexcept;
[[nodiscard]] std::string_view ToString(PresentationIntent intent) noexcept;
[[nodiscard]] std::string_view ToString(FrameAdmissionSource source) noexcept;
} // namespace Prism::RHI
