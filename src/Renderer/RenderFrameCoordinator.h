#pragma once

#include "Core/Threading/TaskExecutorFactory.h"
#include "RHI/Profiling/GpuProfiler.h"
#include "Renderer/Features/RenderFeatureContext.h"
#include "Renderer/RenderHistorySettingsTracker.h"

#include <array>
#include <cstddef>
#include <filesystem>
#include <memory>
#include <optional>

namespace Prism::Asset
{
class Texture;
}

namespace Prism::RHI
{
class IRenderBackend;
}

namespace Prism::Scene
{
class RenderScene;
}

namespace Prism::Renderer
{
class SceneRenderer;
class SceneRendererSharedResources;

enum class RenderViewKind
{
    Game,
    Scene
};

struct RenderFrameViewPlan
{
    std::array<RenderViewKind, 2> order{
        RenderViewKind::Game,
        RenderViewKind::Scene};
    std::size_t count = 1;
    LogicalFrameId logicalFrameId = 0;
    std::uint32_t frameIndex = 0;
    std::optional<RenderViewKind> sharedSimulationProducer{
        RenderViewKind::Game};
    bool gameProducesSharedSimulation = true;
    bool sceneProducesSharedSimulation = false;
};

// Owns the renderer instances and their device-lifetime shared resources.
// Execution remains inline in P1; this class does not create a render thread.
class RenderFrameCoordinator final
{
public:
    explicit RenderFrameCoordinator(
        bool sceneViewEnabled,
        Core::TaskExecutorKind taskExecutorKind =
            Core::TaskExecutorKind::Inline);
    ~RenderFrameCoordinator();

    RenderFrameCoordinator(const RenderFrameCoordinator&) = delete;
    RenderFrameCoordinator& operator=(
        const RenderFrameCoordinator&) = delete;

    [[nodiscard]] SceneRenderer& GetGameRenderer() noexcept;
    [[nodiscard]] const SceneRenderer& GetGameRenderer() const noexcept;
    [[nodiscard]] SceneRenderer* GetSceneRenderer() noexcept;
    [[nodiscard]] const SceneRenderer* GetSceneRenderer() const noexcept;
    [[nodiscard]] std::shared_ptr<SceneRendererSharedResources>
        GetSharedResources() const noexcept;
    [[nodiscard]] Core::TaskExecutorStatistics
        GetTaskExecutorStatistics() const noexcept;

    void Initialize(
        RHI::IRenderBackend& backend,
        const std::filesystem::path& shaderPath,
        const Scene::RenderScene& scene,
        std::shared_ptr<Asset::Texture> environmentCubemap);
    void SetFrameDiagnosticsEnabled(bool enabled) noexcept;
    void SetProfilingMode(
        RHI::GpuProfiler::SamplingMode mode) noexcept;
    void RecreateSwapChainResources(RHI::IRenderBackend& backend);
    void NotifySceneChanged(const Scene::RenderScene& scene);
    void BeginLogicalFrame(
        LogicalFrameId logicalFrameId,
        std::uint32_t frameIndex,
        bool gameConsumerActive,
        bool sceneConsumerActive);
    void Shutdown() noexcept;

    [[nodiscard]] RenderFrameViewPlan BuildViewPlan(
        bool renderSceneView) const noexcept;
    [[nodiscard]] std::size_t
        GetSharedSimulationProducerCount() const noexcept;
    [[nodiscard]] std::uint64_t
        GetSharedSimulationSelectionCount() const noexcept;
    [[nodiscard]] RenderHistorySettingsUpdate ObserveHistorySettings(
        RenderViewKind viewKind);

private:
    bool m_sceneViewEnabled = false;
    std::unique_ptr<Core::ITaskExecutor> m_taskExecutor;
    std::unique_ptr<SceneRenderer> m_gameRenderer;
    std::unique_ptr<SceneRenderer> m_sceneRenderer;
    std::shared_ptr<SceneRendererSharedResources> m_sharedResources;
    LogicalFrameId m_selectedLogicalFrameId = 0;
    std::uint32_t m_selectedFrameIndex = 0;
    bool m_gameConsumerActive = true;
    bool m_sceneConsumerActive = false;
    std::optional<RenderViewKind> m_sharedSimulationProducer{
        RenderViewKind::Game};
    std::uint64_t m_sharedSimulationSelectionCount = 0;
    std::array<RenderHistorySettingsTracker, 2>
        m_historySettingsTrackers;
};
} // namespace Prism::Renderer
