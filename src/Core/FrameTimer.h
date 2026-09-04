#pragma once

#include <chrono>

namespace Prism::Core
{
class FrameTimer
{
public:
    FrameTimer();

    void Tick();

    float GetDeltaSeconds() const;
    float GetDeltaMilliseconds() const;
    float GetFramesPerSecond() const;
    double GetTotalSeconds() const;

private:
    using Clock = std::chrono::steady_clock;

    Clock::time_point m_lastTick;
    double m_totalSeconds = 0.0;
    float m_deltaSeconds = 0.0f;
    float m_framesPerSecond = 0.0f;
    float m_accumulatedSeconds = 0.0f;
    int m_accumulatedFrames = 0;
};
} // namespace Prism::Core
