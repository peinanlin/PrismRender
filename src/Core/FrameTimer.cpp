#include "Core/FrameTimer.h"

namespace Prism::Core
{
FrameTimer::FrameTimer()
    : m_lastTick(Clock::now())
{
}

void FrameTimer::Tick()
{
    const Clock::time_point now = Clock::now();
    const std::chrono::duration<float> delta = now - m_lastTick;
    m_lastTick = now;

    m_deltaSeconds = delta.count();
    m_totalSeconds += static_cast<double>(m_deltaSeconds);
    m_accumulatedSeconds += m_deltaSeconds;
    ++m_accumulatedFrames;

    if (m_accumulatedSeconds >= 0.5f)
    {
        m_framesPerSecond = static_cast<float>(m_accumulatedFrames) / m_accumulatedSeconds;
        m_accumulatedSeconds = 0.0f;
        m_accumulatedFrames = 0;
    }
}

float FrameTimer::GetDeltaSeconds() const
{
    return m_deltaSeconds;
}

float FrameTimer::GetDeltaMilliseconds() const
{
    return m_deltaSeconds * 1000.0f;
}

float FrameTimer::GetFramesPerSecond() const
{
    return m_framesPerSecond;
}

double FrameTimer::GetTotalSeconds() const
{
    return m_totalSeconds;
}
} // namespace Prism::Core
