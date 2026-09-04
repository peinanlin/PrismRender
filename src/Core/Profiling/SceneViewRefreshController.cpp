#include "Core/Profiling/SceneViewRefreshController.h"

#include <cmath>
#include <stdexcept>

namespace Prism::Core
{
SceneViewRefreshController::Decision
SceneViewRefreshController::Evaluate(const Inputs& inputs)
{
    if (!std::isfinite(inputs.sessionTimeSeconds)
        || inputs.sessionTimeSeconds < 0.0)
    {
        throw std::invalid_argument(
            "Scene View refresh time must be finite and non-negative.");
    }
    if (!m_hasPreviousPolicy
        || m_policy != m_previousPolicy
        || inputs.sessionTimeSeconds < m_previousSessionTimeSeconds)
    {
        m_nextThirtyHertzRefreshSeconds =
            inputs.sessionTimeSeconds;
    }
    m_previousPolicy = m_policy;
    m_previousSessionTimeSeconds = inputs.sessionTimeSeconds;
    m_hasPreviousPolicy = true;

    // Explicit Scene capture is reliable even when the docked viewport is
    // hidden or its interactive refresh policy is paused.
    if (inputs.captureRequested)
    {
        return {SceneViewRefreshReason::Capture, true};
    }
    if (!inputs.visible)
    {
        return {SceneViewRefreshReason::Hidden, false};
    }
    if (inputs.performanceGameOnly)
    {
        return {SceneViewRefreshReason::Paused, false};
    }

    SceneViewRefreshPolicy effectivePolicy = m_policy;
    if (effectivePolicy == SceneViewRefreshPolicy::CatalogDefault)
    {
        effectivePolicy = inputs.catalogOnInteraction
            ? SceneViewRefreshPolicy::OnInteraction
            : SceneViewRefreshPolicy::Live;
    }
    switch (effectivePolicy)
    {
    case SceneViewRefreshPolicy::Live:
        return {SceneViewRefreshReason::Live, true};
    case SceneViewRefreshPolicy::OnInteraction:
    {
        const bool render = inputs.requiresRefresh
            || inputs.becameVisible
            || inputs.interacting;
        return {
            inputs.interacting
                ? SceneViewRefreshReason::Interaction
                : SceneViewRefreshReason::CatalogDefault,
            render};
    }
    case SceneViewRefreshPolicy::ThirtyHertz:
    {
        const bool immediate = inputs.requiresRefresh
            || inputs.becameVisible;
        const bool rateDue =
            inputs.sessionTimeSeconds + 1.0e-9
            >= m_nextThirtyHertzRefreshSeconds;
        const bool render = immediate || rateDue;
        if (render)
        {
            m_nextThirtyHertzRefreshSeconds =
                inputs.sessionTimeSeconds
                + ThirtyHertzIntervalSeconds;
        }
        return {
            inputs.interacting
                ? SceneViewRefreshReason::Interaction
                : SceneViewRefreshReason::RateLimited,
            render};
    }
    case SceneViewRefreshPolicy::Paused:
        return {SceneViewRefreshReason::Paused, false};
    case SceneViewRefreshPolicy::CatalogDefault:
        break;
    }
    throw std::logic_error(
        "Scene View refresh policy was not resolved.");
}

void SceneViewRefreshController::SetPolicy(
    const SceneViewRefreshPolicy policy) noexcept
{
    m_policy = policy;
}

SceneViewRefreshPolicy
SceneViewRefreshController::GetPolicy() const noexcept
{
    return m_policy;
}

void SceneViewRefreshController::Reset() noexcept
{
    m_previousPolicy = SceneViewRefreshPolicy::CatalogDefault;
    m_nextThirtyHertzRefreshSeconds = 0.0;
    m_previousSessionTimeSeconds = 0.0;
    m_hasPreviousPolicy = false;
}
} // namespace Prism::Core
