#pragma once

#include "Asset/AssetRegistry.h"
#include "Engine/SceneChangeTracker.h"
#include "Scene/RenderDynamicInputState.h"
#include "Scene/RenderFramePacket.h"
#include "Scene/RenderScene.h"
#include "Scene/RenderSceneExtractor.h"

#include <cstdint>
#include <memory>
#include <vector>

namespace Prism::Tests
{
// GPU fixtures use the same immutable frame input as the application. This
// intentionally replaces the removed RenderScene-per-frame compatibility
// overload; initialization and scene-change notifications remain separate.
class RenderFramePacketFixture final
{
public:
    explicit RenderFramePacketFixture(
        const Asset::AssetRegistry& assetRegistry)
        : m_extractor(
              assetRegistry,
              Scene::RenderScenePublicationMode::Versioned)
    {
    }

    [[nodiscard]] std::shared_ptr<const Scene::RenderFramePacket> Build(
        const Scene::RenderScene& scene,
        const double simulationTimeSeconds,
        const std::uint32_t width,
        const std::uint32_t height,
        const Scene::RenderViewId viewId = Scene::GameRenderViewId)
    {
        if (m_activeScene != &scene)
        {
            ActivateScene(scene);
        }

        const Scene::LogicalFrameId logicalFrameId{++m_logicalFrameId};
        m_dynamicInputs.BeginFrame(
            logicalFrameId,
            simulationTimeSeconds);
        (void)m_dynamicInputs.SynchronizeLights(scene);
        (void)m_dynamicInputs.SynchronizeView(
            viewId,
            scene.GetCamera(),
            width,
            height,
            1);

        Engine::SceneChangeSet changes{};
        if (m_activationPending)
        {
            changes.revision = 1;
            changes.categories =
                Engine::SceneChangeCategory::EntityTopology;
            changes.mutationCount = 1;
        }
        const Scene::RenderSceneExtractionResult extraction =
            m_extractor.Extract({
                scene,
                Scene::SceneGeneration{m_sceneGeneration},
                Scene::RenderSceneDataRevision{1},
                0,
                changes});
        m_activationPending = false;

        return std::make_shared<const Scene::RenderFramePacket>(
            logicalFrameId,
            simulationTimeSeconds,
            extraction.sceneData,
            m_dynamicInputs.GetDynamicData(),
            std::vector<Scene::RenderView>{
                *m_dynamicInputs.FindView(viewId)});
    }

private:
    void ActivateScene(const Scene::RenderScene& scene)
    {
        m_activeScene = &scene;
        ++m_sceneGeneration;
        m_extractor.Reset();
        m_dynamicInputs.ResetForScene(
            Scene::SceneGeneration{m_sceneGeneration});
        m_activationPending = true;
    }

    Scene::RenderSceneExtractor m_extractor;
    Scene::RenderDynamicInputState m_dynamicInputs;
    const Scene::RenderScene* m_activeScene = nullptr;
    std::uint64_t m_sceneGeneration = 0;
    std::uint64_t m_logicalFrameId = 0;
    bool m_activationPending = false;
};
} // namespace Prism::Tests
