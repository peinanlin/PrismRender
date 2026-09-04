#pragma once

#include <chrono>
#include <cstdint>
#include <optional>

namespace Prism::Core
{
// Application-level limiter. Display synchronization remains a backend
// concern; this class only enforces an optional monotonic deadline.
class FrameRateLimiter final
{
public:
    FrameRateLimiter() = default;
    ~FrameRateLimiter() noexcept;
    FrameRateLimiter(const FrameRateLimiter&) = delete;
    FrameRateLimiter& operator=(const FrameRateLimiter&) = delete;

    void Configure(std::optional<std::uint32_t> targetFps) noexcept;
    [[nodiscard]] double Wait();
    void Reset() noexcept;

private:
    using Clock = std::chrono::steady_clock;
    std::optional<std::uint32_t> m_targetFps;
    Clock::duration m_interval{};
    Clock::time_point m_nextDeadline{};
    bool m_deadlineValid = false;
#if defined(_WIN32)
    void* m_waitableTimer = nullptr;
#endif
};
} // namespace Prism::Core
