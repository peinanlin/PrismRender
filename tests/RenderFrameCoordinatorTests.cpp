#include "Renderer/RenderFrameCoordinator.h"
#include "Renderer/SceneRenderer.h"

#include <cstdlib>
#include <iostream>
#include <stdexcept>

namespace
{
void Expect(const bool condition, const char* const message)
{
    if (!condition)
    {
        throw std::runtime_error(message);
    }
}
} // namespace

int main()
{
    try
    {
        Prism::Renderer::RenderFrameCoordinator standalone(false);
        Expect(
            standalone.GetSceneRenderer() == nullptr,
            "Standalone coordinator unexpectedly created a Scene renderer.");
        const auto standalonePlan = standalone.BuildViewPlan(true);
        Expect(
            standalonePlan.count == 1
                && standalonePlan.order[0]
                    == Prism::Renderer::RenderViewKind::Game,
            "Standalone coordinator did not preserve the Game-only plan.");

        Prism::Renderer::RenderFrameCoordinator editor(true);
        Expect(
            editor.GetSceneRenderer() != nullptr,
            "Editor coordinator did not create its Scene renderer.");
        const auto hiddenPlan = editor.BuildViewPlan(false);
        Expect(
            hiddenPlan.count == 1,
            "Hidden/on-demand Scene view was scheduled unexpectedly.");
        const auto visiblePlan = editor.BuildViewPlan(true);
        Expect(
            visiblePlan.count == 2
                && visiblePlan.order[0]
                    == Prism::Renderer::RenderViewKind::Game
                && visiblePlan.order[1]
                    == Prism::Renderer::RenderViewKind::Scene,
            "Editor coordinator changed the Game-before-Scene order.");
        Expect(
            visiblePlan.gameProducesSharedSimulation
                && !visiblePlan.sceneProducesSharedSimulation
                && editor.GetSharedSimulationProducerCount() == 1,
            "Editor coordinator assigned shared simulation to more than one view.");

        editor.BeginLogicalFrame(100u, 0u, true, true);
        const auto logicalPlan = editor.BuildViewPlan(true);
        Expect(
            logicalPlan.logicalFrameId == 100u
                && logicalPlan.frameIndex == 0u
                && logicalPlan.sharedSimulationProducer
                    == Prism::Renderer::RenderViewKind::Game
                && logicalPlan.gameProducesSharedSimulation
                && !logicalPlan.sceneProducesSharedSimulation
                && editor.GetSharedSimulationSelectionCount() == 1u,
            "Logical frame did not select exactly one preferred producer.");
        editor.BeginLogicalFrame(100u, 0u, true, true);
        Expect(
            editor.GetSharedSimulationSelectionCount() == 1u,
            "Repeated planning advanced the same LogicalFrameId twice.");

        // Frame slot zero is reused here, but the monotonic logical id still
        // creates a distinct shared-production decision.
        editor.BeginLogicalFrame(101u, 1u, true, true);
        editor.BeginLogicalFrame(102u, 0u, true, true);
        Expect(
            editor.GetSharedSimulationSelectionCount() == 3u,
            "Frame-slot wrap was confused with LogicalFrameId reuse.");

        editor.BeginLogicalFrame(103u, 1u, false, true);
        const auto sceneFallbackPlan = editor.BuildViewPlan(true);
        Expect(
            sceneFallbackPlan.count == 1u
                && sceneFallbackPlan.order[0]
                    == Prism::Renderer::RenderViewKind::Scene
                && !sceneFallbackPlan.gameProducesSharedSimulation
                && sceneFallbackPlan.sceneProducesSharedSimulation
                && sceneFallbackPlan.sharedSimulationProducer
                    == Prism::Renderer::RenderViewKind::Scene,
            "Unavailable preferred view did not fall back to the active Scene consumer.");

        editor.BeginLogicalFrame(104u, 0u, false, false);
        const auto suspendedPlan = editor.BuildViewPlan(false);
        Expect(
            suspendedPlan.count == 0u
                && !suspendedPlan.sharedSimulationProducer.has_value()
                && editor.GetSharedSimulationProducerCount() == 0u,
            "A frame without consumers retained a shared simulation producer.");

        editor.BeginLogicalFrame(105u, 1u, true, false);
        Expect(
            editor.BuildViewPlan(false).count == 1u,
            "Hidden Scene view changed the Game consumer plan.");
        editor.BeginLogicalFrame(106u, 0u, true, true);
        const auto recoveredPlan = editor.BuildViewPlan(true);
        Expect(
            recoveredPlan.count == 2u
                && editor.GetSharedSimulationProducerCount() == 1u
                && recoveredPlan.gameProducesSharedSimulation
                && !recoveredPlan.sceneProducesSharedSimulation,
            "Scene view recovery duplicated shared simulation production.");

        editor.SetFrameDiagnosticsEnabled(true);
        editor.SetProfilingMode(
            Prism::RHI::GpuProfiler::SamplingMode::Off);
        editor.Shutdown();
        editor.Shutdown();
        Expect(
            editor.GetSharedSimulationProducerCount() == 0,
            "Coordinator repeated shutdown retained a simulation producer.");

        std::cout << "RenderFrameCoordinator tests passed.\n";
        return EXIT_SUCCESS;
    }
    catch (const std::exception& error)
    {
        std::cerr << "RenderFrameCoordinator tests failed: "
                  << error.what() << '\n';
        return EXIT_FAILURE;
    }
}
