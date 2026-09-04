#include "Core/Application/FrameRateLimiter.h"

#include <algorithm>
#include <thread>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#endif

namespace Prism::Core
{
FrameRateLimiter::~FrameRateLimiter() noexcept
{
#if defined(_WIN32)
    if (m_waitableTimer != nullptr)
    {
        CloseHandle(static_cast<HANDLE>(m_waitableTimer));
        m_waitableTimer = nullptr;
    }
#endif
}

void FrameRateLimiter::Configure(
    const std::optional<std::uint32_t> targetFps) noexcept
{
    if (m_targetFps == targetFps)
    {
        return;
    }
    m_targetFps = targetFps;
    m_interval = targetFps.has_value() && *targetFps > 0
        ? std::chrono::duration_cast<Clock::duration>(
            std::chrono::duration<double>(1.0 / *targetFps))
        : Clock::duration{};
    Reset();
}

double FrameRateLimiter::Wait()
{
    if (!m_targetFps.has_value())
    {
        return 0.0;
    }
    const Clock::time_point start = Clock::now();
    if (!m_deadlineValid)
    {
        m_nextDeadline = start + m_interval;
        m_deadlineValid = true;
        return 0.0;
    }
    if (start > m_nextDeadline + m_interval * 4)
    {
        m_nextDeadline = start + m_interval;
        return 0.0;
    }
    constexpr auto SpinWindow = std::chrono::microseconds(200);
    if (start + SpinWindow < m_nextDeadline)
    {
        const Clock::time_point coarseDeadline =
            m_nextDeadline - SpinWindow;
#if defined(_WIN32)
        if (m_waitableTimer == nullptr)
        {
            constexpr DWORD HighResolutionTimerFlag = 0x00000002;
            m_waitableTimer = CreateWaitableTimerExW(
                nullptr,
                nullptr,
                HighResolutionTimerFlag,
                TIMER_MODIFY_STATE | SYNCHRONIZE);
            if (m_waitableTimer == nullptr)
            {
                m_waitableTimer = CreateWaitableTimerW(
                    nullptr, FALSE, nullptr);
            }
        }
        bool timerWaited = false;
        if (m_waitableTimer != nullptr)
        {
            const auto remaining = coarseDeadline - Clock::now();
            using HundredNanoseconds =
                std::chrono::duration<long long, std::ratio<1, 10'000'000>>;
            const long long ticks = (std::max)(
                1LL,
                std::chrono::duration_cast<HundredNanoseconds>(
                    remaining).count());
            LARGE_INTEGER dueTime{};
            dueTime.QuadPart = -ticks;
            if (SetWaitableTimer(
                    static_cast<HANDLE>(m_waitableTimer),
                    &dueTime,
                    0,
                    nullptr,
                    nullptr,
                    FALSE))
            {
                timerWaited = WaitForSingleObject(
                    static_cast<HANDLE>(m_waitableTimer),
                    INFINITE) == WAIT_OBJECT_0;
            }
        }
        if (!timerWaited)
        {
            std::this_thread::sleep_until(coarseDeadline);
        }
#else
        std::this_thread::sleep_until(coarseDeadline);
#endif
    }
    while (Clock::now() < m_nextDeadline)
    {
        std::this_thread::yield();
    }
    const Clock::time_point completed = Clock::now();
    m_nextDeadline += m_interval;
    return std::chrono::duration<double, std::milli>(
        completed - start).count();
}

void FrameRateLimiter::Reset() noexcept
{
    m_deadlineValid = false;
    m_nextDeadline = {};
}
} // namespace Prism::Core
