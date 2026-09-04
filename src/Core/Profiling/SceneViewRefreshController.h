#pragma once

#include "Core/Profiling/FrameProfilerSnapshot.h"

namespace Prism::Core
{
class SceneViewRefreshController
{
public:
    struct Inputs
    {
        double sessionTimeSeconds = 0.0;
        bool visible = false;
        bool performanceGameOnly = false;
        bool catalogOnInteraction = false;
        bool requiresRefresh = false;
        bool becameVisible = false;
        bool interacting = false;
        bool captureRequested = false;
    };

    struct Decision
    {
        SceneViewRefreshReason reason =
            SceneViewRefreshReason::Unavailable;
        bool render = false;
    };

    void SetPolicy(SceneViewRefreshPolicy policy) noexcept;
    [[nodiscard]] SceneViewRefreshPolicy GetPolicy() const noexcept;
    [[nodiscard]] Decision Evaluate(const Inputs& inputs);
    void Reset() noexcept;

private:
    static constexpr double ThirtyHertzIntervalSeconds =
        1.0 / 30.0;

    SceneViewRefreshPolicy m_previousPolicy =
        SceneViewRefreshPolicy::CatalogDefault;
    SceneViewRefreshPolicy m_policy =
        SceneViewRefreshPolicy::CatalogDefault;
    double m_nextThirtyHertzRefreshSeconds = 0.0;
    double m_previousSessionTimeSeconds = 0.0;
    bool m_hasPreviousPolicy = false;
};
} // namespace Prism::Core
