#pragma once

#include "Scene/RenderView.h"

#include <cstdint>
#include <functional>

namespace Prism::Platform
{
class Window;
}

namespace Prism::Renderer
{
struct RenderSettings;
}

namespace Prism::Scene
{
enum class DemoSceneId : std::uint32_t;
class RenderScene;
}

namespace Prism::Core
{
// Opt-in deterministic HPWater lifecycle driver. The sequence stores no Host
// pointer and mutates only the explicit frame dependencies supplied to Update.
class WaterValidationSequence final
{
public:
    using ActivateDemoScene =
        std::function<void(Scene::DemoSceneId)>;
    using NotifyHistoryInvalidation = std::function<void(
        Scene::RenderViewId,
        Scene::RenderViewHistoryInvalidation)>;

    explicit WaterValidationSequence(bool enabled) noexcept;

    [[nodiscard]] bool IsEnabled() const noexcept;
    [[nodiscard]] bool Update(
        std::uint64_t completedFrames,
        Renderer::RenderSettings& settings,
        bool waterOpticsPassScheduled,
        bool waterCoverageAvailable,
        Scene::RenderScene& scene,
        Platform::Window& window,
        const ActivateDemoScene& activateDemoScene,
        const NotifyHistoryInvalidation&
            notifyHistoryInvalidation) const;

private:
    bool m_enabled = false;
};
} // namespace Prism::Core
