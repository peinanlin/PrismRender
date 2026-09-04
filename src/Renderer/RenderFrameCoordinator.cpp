#include "Renderer/RenderFrameCoordinator.h"

#include "Asset/Texture.h"
#include "RHI/IGraphicsDevice.h"
#include "RHI/IRenderBackend.h"
#include "Renderer/SceneRenderer.h"
#include "Renderer/SceneRendererSharedResources.h"
#include "Scene/RenderScene.h"
#include "Core/Assert.h"

#include <utility>

namespace Prism::Renderer
{
RenderFrameCoordinator::RenderFrameCoordinator(
    const bool sceneViewEnabled,
    const Core::TaskExecutorKind taskExecutorKind)
    : m_sceneViewEnabled(sceneViewEnabled),
      m_taskExecutor(Core::CreateTaskExecutor(taskExecutorKind)),
      m_gameRenderer(std::make_unique<SceneRenderer>()),
      m_sceneConsumerActive(sceneViewEnabled)
{
    Core::Check(m_taskExecutor != nullptr,
        "RenderFrameCoordinator requires a task executor.");
    m_gameRenderer->SetTaskExecutor(*m_taskExecutor);
    m_gameRenderer->SetEditorGridAllowed(false);
    if (m_sceneViewEnabled)
    {
        m_sceneRenderer = std::make_unique<SceneRenderer>();
        m_sceneRenderer->SetTaskExecutor(*m_taskExecutor);
        m_sceneRenderer->SetEditorGridAllowed(true);
    }
}

RenderFrameCoordinator::~RenderFrameCoordinator()
{
    Shutdown();
}

SceneRenderer& RenderFrameCoordinator::GetGameRenderer() noexcept
{
    return *m_gameRenderer;
}

const SceneRenderer&
RenderFrameCoordinator::GetGameRenderer() const noexcept
{
    return *m_gameRenderer;
}

SceneRenderer* RenderFrameCoordinator::GetSceneRenderer() noexcept
{
    return m_sceneRenderer.get();
}

const SceneRenderer*
RenderFrameCoordinator::GetSceneRenderer() const noexcept
{
    return m_sceneRenderer.get();
}

std::shared_ptr<SceneRendererSharedResources>
RenderFrameCoordinator::GetSharedResources() const noexcept
{
    return m_sharedResources;
}

Core::TaskExecutorStatistics
RenderFrameCoordinator::GetTaskExecutorStatistics() const noexcept
{
    return m_taskExecutor != nullptr
        ? m_taskExecutor->GetStatistics()
        : Core::TaskExecutorStatistics{};
}

void RenderFrameCoordinator::Initialize(
    RHI::IRenderBackend& backend,
    const std::filesystem::path& shaderPath,
    const Scene::RenderScene& scene,
    std::shared_ptr<Asset::Texture> environmentCubemap)
{
    m_sharedResources =
        std::make_shared<SceneRendererSharedResources>();
    m_sharedResources->Initialize(
        backend.GetGraphicsDevice(),
        environmentCubemap);
    m_gameRenderer->Initialize(
        backend,
        shaderPath,
        scene,
        environmentCubemap,
        !m_sceneViewEnabled,
        m_sharedResources,
        true,
        1);
    if (m_sceneRenderer != nullptr)
    {
        const Scene::RenderScene editorViewScene =
            scene.CreateEditorView();
        m_sceneRenderer->Initialize(
            backend,
            shaderPath,
            editorViewScene,
            std::move(environmentCubemap),
            false,
            m_sharedResources,
            false,
            2);
    }
}

void RenderFrameCoordinator::SetFrameDiagnosticsEnabled(
    const bool enabled) noexcept
{
    m_gameRenderer->SetFrameDiagnosticsEnabled(enabled);
    if (m_sceneRenderer != nullptr)
    {
        m_sceneRenderer->SetFrameDiagnosticsEnabled(enabled);
    }
}

void RenderFrameCoordinator::SetProfilingMode(
    const RHI::GpuProfiler::SamplingMode mode) noexcept
{
    m_gameRenderer->SetProfilingMode(mode);
    if (m_sceneRenderer != nullptr)
    {
        m_sceneRenderer->SetProfilingMode(mode);
    }
}

void RenderFrameCoordinator::RecreateSwapChainResources(
    RHI::IRenderBackend& backend)
{
    m_gameRenderer->RecreateSwapChainResources(backend, true);
    if (m_sceneRenderer != nullptr)
    {
        m_sceneRenderer->RecreateSwapChainResources(backend, false);
    }
}

void RenderFrameCoordinator::NotifySceneChanged(
    const Scene::RenderScene& scene)
{
    m_gameRenderer->NotifySceneChanged(scene, true);
    if (m_sceneRenderer != nullptr)
    {
        m_sceneRenderer->NotifySceneChanged(scene, false);
    }
}

void RenderFrameCoordinator::BeginLogicalFrame(
    const LogicalFrameId logicalFrameId,
    const std::uint32_t frameIndex,
    const bool gameConsumerActive,
    const bool sceneConsumerActive)
{
    Core::Check(
        logicalFrameId > 0,
        "RenderFrameCoordinator requires a non-zero LogicalFrameId.");
    Core::Check(
        logicalFrameId >= m_selectedLogicalFrameId,
        "RenderFrameCoordinator cannot select an older logical frame.");
    const bool effectiveSceneConsumer =
        sceneConsumerActive && m_sceneRenderer != nullptr;
    if (logicalFrameId == m_selectedLogicalFrameId)
    {
        Core::Check(
            frameIndex == m_selectedFrameIndex
                && gameConsumerActive == m_gameConsumerActive
                && effectiveSceneConsumer == m_sceneConsumerActive,
            "A LogicalFrameId cannot be replanned with different consumers or a different frame slot.");
        return;
    }

    m_selectedLogicalFrameId = logicalFrameId;
    m_selectedFrameIndex = frameIndex;
    m_gameConsumerActive = gameConsumerActive;
    m_sceneConsumerActive = effectiveSceneConsumer;
    if (m_gameConsumerActive && m_gameRenderer != nullptr)
    {
        m_sharedSimulationProducer = RenderViewKind::Game;
    }
    else if (m_sceneConsumerActive)
    {
        m_sharedSimulationProducer = RenderViewKind::Scene;
    }
    else
    {
        m_sharedSimulationProducer.reset();
    }
    ++m_sharedSimulationSelectionCount;

    if (m_gameRenderer != nullptr)
    {
        m_gameRenderer->SetSharedSimulationProducerForFrame(
            logicalFrameId,
            m_sharedSimulationProducer
                == RenderViewKind::Game);
    }
    if (m_sceneRenderer != nullptr)
    {
        m_sceneRenderer->SetSharedSimulationProducerForFrame(
            logicalFrameId,
            m_sharedSimulationProducer
                == RenderViewKind::Scene);
    }
}

void RenderFrameCoordinator::Shutdown() noexcept
{
    m_sceneRenderer.reset();
    m_gameRenderer.reset();
    m_sharedResources.reset();
    m_sharedSimulationProducer.reset();
    if (m_taskExecutor != nullptr)
    {
        m_taskExecutor->Shutdown();
    }
}

RenderFrameViewPlan RenderFrameCoordinator::BuildViewPlan(
    const bool renderSceneView) const noexcept
{
    RenderFrameViewPlan plan{};
    plan.count = 0;
    plan.logicalFrameId = m_selectedLogicalFrameId;
    plan.frameIndex = m_selectedFrameIndex;
    plan.sharedSimulationProducer = m_sharedSimulationProducer;
    plan.gameProducesSharedSimulation =
        m_sharedSimulationProducer == RenderViewKind::Game;
    plan.sceneProducesSharedSimulation =
        m_sharedSimulationProducer == RenderViewKind::Scene;
    if (m_gameConsumerActive && m_gameRenderer != nullptr)
    {
        plan.order[plan.count++] = RenderViewKind::Game;
    }
    if (m_sceneViewEnabled
        && renderSceneView
        && m_sceneConsumerActive
        && m_sceneRenderer != nullptr)
    {
        plan.order[plan.count++] = RenderViewKind::Scene;
    }
    return plan;
}

std::size_t
RenderFrameCoordinator::GetSharedSimulationProducerCount() const noexcept
{
    return m_sharedSimulationProducer.has_value()
            && (m_gameRenderer != nullptr
                || m_sceneRenderer != nullptr)
        ? 1u
        : 0u;
}

std::uint64_t
RenderFrameCoordinator::GetSharedSimulationSelectionCount() const noexcept
{
    return m_sharedSimulationSelectionCount;
}

RenderHistorySettingsUpdate
RenderFrameCoordinator::ObserveHistorySettings(
    const RenderViewKind viewKind)
{
    const std::size_t index = viewKind == RenderViewKind::Game ? 0u : 1u;
    SceneRenderer* const renderer = viewKind == RenderViewKind::Game
        ? m_gameRenderer.get() : m_sceneRenderer.get();
    Core::Check(
        renderer != nullptr,
        "Cannot observe settings for a disabled render view.");
    return m_historySettingsTrackers[index].Observe(
        renderer->GetSettings());
}
} // namespace Prism::Renderer
