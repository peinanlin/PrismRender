#include "Core/Application/WaterValidationSequence.h"

#include "Core/Assert.h"
#include "Platform/Window.h"
#include "Renderer/RenderSettings.h"
#include "Scene/DemoSceneCatalog.h"
#include "Scene/RenderScene.h"

#include <GLFW/glfw3.h>

#include <iostream>

namespace Prism::Core
{
WaterValidationSequence::WaterValidationSequence(
    const bool enabled) noexcept
    : m_enabled(enabled)
{
}

bool WaterValidationSequence::IsEnabled() const noexcept
{
    return m_enabled;
}

bool WaterValidationSequence::Update(
    const std::uint64_t frame,
    Renderer::RenderSettings& settings,
    const bool waterOpticsPassScheduled,
    const bool waterCoverageAvailable,
    Scene::RenderScene& scene,
    Platform::Window& window,
    const ActivateDemoScene& activateDemoScene,
    const NotifyHistoryInvalidation&
        notifyHistoryInvalidation) const
{
    if (!m_enabled)
    {
        return false;
    }

    if (frame == 192u)
    {
        activateDemoScene(Scene::DemoSceneId::WaveWorksLab);
    }
    if (frame == 204u)
    {
        activateDemoScene(Scene::DemoSceneId::OceanLab);
    }
    if (frame == 216u)
    {
        activateDemoScene(Scene::DemoSceneId::HpWaterOceanLab);
    }
    if (frame == 200u || frame == 212u)
    {
        Check(!waterOpticsPassScheduled,
            "Compatibility regression: ordinary ocean scheduled HPWater "
            "passes.");
    }

    auto& ocean = settings.ocean;
    using namespace Renderer;
    switch (frame)
    {
    case 12u:
        ocean.optics.refraction.highPrecision = true;
        break;
    case 24u:
        ocean.local.enabled = false;
        break;
    case 36u:
        ocean.local.enabled = true;
        break;
    case 48u:
        ocean.optics.material.absorption.x *= 0.6f;
        break;
    case 60u:
        ++ocean.optics.historyResetSerial;
        break;
    case 72u:
        ++ocean.debug.localResetSerial;
        break;
    case 84u:
        ++ocean.debug.fullResetSerial;
        break;
    case 132u:
        ocean.optics.quality = WaterOpticsQuality::Normal;
        break;
    case 144u:
        ocean.optics.quality = WaterOpticsQuality::High;
        ocean.optics.caustics.mode = WaterCausticsMode::Chromatic;
        break;
    case 156u:
        ocean.optics.quality = WaterOpticsQuality::Extreme;
        ocean.optics.caustics.enabled = false;
        break;
    case 168u:
        glfwSetWindowSize(window.GetNativeWindow(), 960, 600);
        break;
    case 180u:
        glfwSetWindowSize(window.GetNativeWindow(), 1280, 800);
        break;
    case 228u:
        settings.fftOceanEnabled = false;
        ocean.debug.renderWater = false;
        break;
    case 240u:
        settings.fftOceanEnabled = true;
        ocean.debug.renderWater = true;
        break;
    default:
        break;
    }

    if (frame >= 96u && frame < 120u)
    {
        // First a cut below water, then continuous small waterline
        // oscillations.
        const float height = frame < 108u
            ? -3.0f
            : (frame % 2u ? -0.04f : 0.04f);
        scene.GetGameCamera().SetLookAt(
            {-30.0f, height, 36.0625f},
            {0.0f, height - 1.0f, 39.0625f},
            {0.0f, 1.0f, 0.0f});
        scene.GetCamera() = scene.GetGameCamera();
        if (frame == 96u)
        {
            notifyHistoryInvalidation(
                Scene::GameRenderViewId,
                Scene::RenderViewHistoryInvalidation::CameraCut);
            notifyHistoryInvalidation(
                Scene::SceneRenderViewId,
                Scene::RenderViewHistoryInvalidation::CameraCut);
        }
    }
    if (frame == 120u || frame == 252u)
    {
        scene.GetGameCamera().SetLookAt(
            {-20.0f, 5.0f, 36.0625f},
            {0.0f, -3.0f, 39.0625f},
            {0.0f, 1.0f, 0.0f});
        scene.GetCamera() = scene.GetGameCamera();
        notifyHistoryInvalidation(
            Scene::GameRenderViewId,
            Scene::RenderViewHistoryInvalidation::CameraCut);
        notifyHistoryInvalidation(
            Scene::SceneRenderViewId,
            Scene::RenderViewHistoryInvalidation::CameraCut);
    }
    if (frame >= 272u && frame < 300u)
    {
        ocean.optics.debugView = static_cast<WaterOpticsDebugView>(
            (frame - 272u) / 2u);
    }
    if (frame == 300u)
    {
        ocean.optics.debugView = WaterOpticsDebugView::None;
    }
    if (frame == 320u)
    {
        Check(waterCoverageAvailable,
            "Water validation did not recover coverage after resets and "
            "scene switches.");
    }
    if (frame % 12u == 0u || frame == 320u)
    {
        std::cout << "WaterValidation frame " << frame << " completed\n";
    }

    // The lifecycle driver mutates ocean settings and cameras over time, so
    // its independent editor view must observe every validation frame.
    return true;
}
} // namespace Prism::Core
