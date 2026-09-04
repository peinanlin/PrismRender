#pragma once

#include "Core/Profiling/FrameProfilerSnapshot.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <optional>
#include <vector>

namespace Prism::Core
{
class FrameProfiler
{
public:
    static constexpr std::size_t MaximumHistoryFrames = 240;
    static constexpr std::size_t MaximumGpuFrameSlots = 16;

    struct ResolvedGpuSample
    {
        double totalMilliseconds = 0.0;
        std::uint64_t generation = 0;
        std::uint64_t frameId = 0;
    };

    void SubmitCompleted(FrameProfilerSnapshot snapshot);
    void SetRequestedLevel(ProfilingLevel level);
    [[nodiscard]] ProfilingLevel GetRequestedLevel() const;
    [[nodiscard]] std::optional<ResolvedGpuSample>
        AssociateViewGpuTiming(
            ProfiledView view,
            std::uint64_t currentFrameId,
            std::uint32_t currentSlot,
            std::uint64_t timingGeneration,
            std::uint32_t resolvedSlot,
            double totalMilliseconds);

    [[nodiscard]] std::optional<FrameProfilerSnapshot>
        GetLatest() const;
    [[nodiscard]] std::vector<FrameProfilerSnapshot>
        GetHistory() const;
    [[nodiscard]] std::size_t GetHistorySize() const;
    [[nodiscard]] bool IsShutdown() const;

    void Shutdown();

    [[nodiscard]] static std::uint64_t CurrentThreadId();

private:
    struct ViewGpuState
    {
        std::array<std::uint64_t, MaximumGpuFrameSlots>
            slotFrames{};
        std::array<std::uint64_t, MaximumGpuFrameSlots>
            consumedFrames{};
        std::uint64_t lastGeneration = 0;
    };

    static void ValidateDuration(
        const FrameProfileDuration& duration,
        const char* name);
    void ValidateSnapshot(
        const FrameProfilerSnapshot& snapshot) const;

    mutable std::mutex m_mutex;
    std::vector<FrameProfilerSnapshot> m_history =
        std::vector<FrameProfilerSnapshot>(MaximumHistoryFrames);
    std::array<std::uint64_t, FrameProfilerSnapshot::ViewCount>
        m_latestGpuGenerations{};
    std::array<ViewGpuState, FrameProfilerSnapshot::ViewCount>
        m_viewGpuStates{};
    std::size_t m_oldest = 0;
    std::size_t m_size = 0;
    std::uint64_t m_latestFrameId = 0;
    ProfilingLevel m_requestedLevel = ProfilingLevel::Basic;
    bool m_shutdown = false;
};
} // namespace Prism::Core
