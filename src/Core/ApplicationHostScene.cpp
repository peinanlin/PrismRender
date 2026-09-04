#include "Core/ApplicationHost.h"
#include "Core/Application/AssetRuntimeCoordinator.h"
#include "Core/Application/CaptureAutomationController.h"
#include "Core/Application/RenderRuntimeExecutionTarget.h"

#include "Asset/AssetRegistry.h"
#include "Asset/AssetStreamingManager.h"
#include "Engine/CommandSystem.h"
#include "Platform/Window.h"
#include "RHI/IFrameContext.h"
#include "RHI/IGraphicsDevice.h"
#include "RHI/IRenderBackend.h"
#include "Renderer/DemoSceneSettings.h"
#include "Renderer/RenderCapture.h"
#include "Renderer/RenderFrameCoordinator.h"
#include "Renderer/RenderSettings.h"
#include "Renderer/SceneRenderer.h"
#include "Scene/AssetStreamingSceneBridge.h"
#include "Scene/DemoSceneCatalog.h"
#include "Scene/RenderScene.h"
#include "Scene/SceneSession.h"

#include <DirectXMath.h>
#include <GLFW/glfw3.h>

#include <algorithm>
#include <new>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace Prism::Core
{
using namespace DirectX;

void ApplicationHost::TryActivateStreamingScene()
{
    Asset::AssetStreamingManager* const streamingManager =
        m_assetRuntimeCoordinator->BorrowStreamingManager();
    if (streamingManager == nullptr
        || !m_captureAutomationController
                ->ShouldAttemptStreamingActivation())
    {
        return;
    }

    const char* allocationStage = "scene bridge activation";
    Scene::AssetStreamingSceneActivationResult activation{};
    try
    {
        activation = Scene::AssetStreamingSceneBridge::Activate(
            m_captureAutomationController
                ->GetStreamingSceneAssetId(),
            *streamingManager,
            m_assetRuntimeCoordinator->GetRegistry(),
            m_sceneSession->GetRenderScene(),
            false,
            {2.8f, 0.0f, 1.5f});
    }
    catch (const std::bad_alloc&)
    {
        throw std::runtime_error(
            std::string("Asset Streaming allocation failed during ")
            + allocationStage + '.');
    }
    if (!activation.success)
    {
        m_captureAutomationController
            ->RecordStreamingActivationFailure(
                activation.errorCode,
                activation.errorMessage);
        m_sceneSession->SetLoadMessage(
            "Asset Streaming Scene activation failed: "
            + activation.errorMessage);
        return;
    }
    if (!activation.ready)
    {
        return;
    }

    m_captureAutomationController
        ->RecordStreamingActivationSuccess(
            activation.renderObjectCount);
    m_sceneSession->SetSourceLabel(
        "Streamed Asset Scene: "
        + m_captureAutomationController
              ->GetStreamingSceneAssetId());
    m_sceneSession->SetLoadMessage(
        "Asset Streaming activated "
        + std::to_string(activation.renderObjectCount)
        + " render object(s).");
    m_sceneSession->SetUsingFallbackScene(false);

    try
    {
        allocationStage = "World import";
        allocationStage = "CommandProcessor world initialization";
        m_sceneSession->InitializeWorldFromRenderScene();
    }
    catch (const std::bad_alloc&)
    {
        throw std::runtime_error(
            std::string("Asset Streaming allocation failed during ")
            + allocationStage + '.');
    }

    m_captureAutomationController
        ->NotifyStreamingSceneActivated(
            m_sceneSession->GetRenderScene());
}

void ApplicationHost::ActivateDemoScene(
    const Scene::DemoSceneId sceneId)
{
    if (!m_demoSceneSwitchingEnabled
        || sceneId
            == m_sceneSession->GetIdentity().activeDemoScene)
    {
        return;
    }

    m_pendingDemoScene = sceneId;
}

void ApplicationHost::UpdateFluidInput()
{
    Renderer::FluidSettings& fluid =
        m_gameRenderSettings.fluid;
    fluid.externalAcceleration = {};
    if (!Scene::DemoSceneCatalog::IsFluidDemo(
            m_sceneSession->GetIdentity().activeDemoScene)
        || !fluid.enabled
        || Renderer::IsDeterministicRenderCaptureEnabled())
    {
        return;
    }

    constexpr float Acceleration = 22.0f;
    if (m_window->IsKeyDown(GLFW_KEY_J))
    {
        fluid.externalAcceleration.x -= Acceleration;
    }
    if (m_window->IsKeyDown(GLFW_KEY_L))
    {
        fluid.externalAcceleration.x += Acceleration;
    }
    if (m_window->IsKeyDown(GLFW_KEY_K))
    {
        fluid.externalAcceleration.y -= Acceleration;
    }
    if (m_window->IsKeyDown(GLFW_KEY_I))
    {
        fluid.externalAcceleration.y += Acceleration;
    }
    if (m_window->IsKeyDown(GLFW_KEY_U))
    {
        fluid.externalAcceleration.z -= Acceleration;
    }
    if (m_window->IsKeyDown(GLFW_KEY_O))
    {
        fluid.externalAcceleration.z += Acceleration;
    }
}

} // namespace Prism::Core
