#pragma once

#include <array>
#include <chrono>
#include <cstdint>

namespace Prism::RHI
{
// CPU wall time only. No GPU queries, waits, allocation or global state.
struct FramePacingStatistics
{
    enum class Phase { FrameFence, Reclaim, Acquire, ImageFence, Upload, Prepare, Submit, Present, Signal, Count };
    std::array<double, static_cast<std::size_t>(Phase::Count)> milliseconds{};
    std::uint64_t generation = 0;
    std::uint64_t fenceTarget = 0, fenceCompletedBefore = 0;
    std::uint32_t slot = 0, image = 0, imageCount = 0;
    std::int64_t acquireResult = 0, presentResult = 0, waitResult = 0;
    std::int32_t presentMode = -1; // VkPresentModeKHR, or -1 for DXGI.
    std::uint32_t syncInterval = 0, presentFlags = 0;
    bool frameFenceWaitCalled = false, imageFenceWaitCalled = false;
    bool beginCompleted = false, endCompleted = false;

    void Reset(std::uint32_t frameSlot) noexcept
    {
        const auto next = generation + 1;
        *this = {};
        generation = next;
        slot = frameSlot;
    }
};

class FramePacingScope
{
public:
    FramePacingScope(FramePacingStatistics* stats, FramePacingStatistics::Phase phase) noexcept
        : m_stats(stats), m_phase(phase)
    { if (m_stats) m_start = Clock::now(); }
    ~FramePacingScope() { End(); }
    FramePacingScope(const FramePacingScope&) = delete;
    FramePacingScope& operator=(const FramePacingScope&) = delete;
    void End() noexcept
    {
        if (!m_stats) return;
        m_stats->milliseconds[static_cast<std::size_t>(m_phase)] +=
            std::chrono::duration<double, std::milli>(Clock::now() - m_start).count();
        m_stats = nullptr;
    }
private:
    using Clock = std::chrono::steady_clock;
    FramePacingStatistics* m_stats;
    FramePacingStatistics::Phase m_phase;
    Clock::time_point m_start{};
};
} // namespace Prism::RHI
