#include "Asset/AssetRegistry.h"
#include "Asset/Material.h"
#include "Asset/MaterialAsset.h"
#include "Asset/Mesh.h"
#include "Asset/Texture.h"
#include "Asset/TextureAsset.h"
#include "Scene/RenderFramePacket.h"
#include "Scene/RenderDynamicInputState.h"
#include "Scene/RenderScene.h"
#include "Scene/RenderSceneData.h"
#include "Scene/RenderSceneExtractor.h"
#include "Scene/RenderSceneMailbox.h"
#include "Scene/RenderSceneView.h"
#include "Scene/RenderView.h"
#include "Renderer/RenderHistorySettingsTracker.h"

#include <cmath>
#include <functional>
#include <iostream>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace
{
void Expect(const bool condition, const char* const message)
{
    if (!condition) throw std::runtime_error(message);
}

void ExpectRejected(
    const std::function<void()>& action,
    const char* const message)
{
    try
    {
        action();
    }
    catch (const std::invalid_argument&)
    {
        return;
    }
    throw std::runtime_error(message);
}

Prism::Scene::RenderView MakeView(
    const std::uint64_t id,
    const std::uint32_t width = 1280,
    const std::uint32_t height = 800)
{
    Prism::Scene::RenderView view{};
    view.id = Prism::Scene::RenderViewId{id};
    view.width = width;
    view.height = height;
    return view;
}

Prism::Scene::RenderSceneObjectSource MakeProgrammaticObject(
    std::string key,
    std::string name,
    const bool editorOnly = false,
    std::string parentKey = {})
{
    Prism::Scene::RenderSceneObjectSource source{};
    source.object.name = std::move(name);
    source.object.editorOnly = editorOnly;
    source.programmaticKey = std::move(key);
    source.parentProgrammaticKey = std::move(parentKey);
    return source;
}

Prism::Scene::RenderSceneObjectSource MakeWorldObject(
    const Prism::Engine::EntityId id,
    std::string name,
    const std::optional<Prism::Engine::EntityId> parent =
        std::nullopt)
{
    Prism::Scene::RenderSceneObjectSource source{};
    source.object.entityId = id;
    source.object.name = std::move(name);
    source.parentEntityId = parent;
    return source;
}

std::shared_ptr<Prism::Asset::Material> MakeMaterial(
    const float roughness,
    const std::shared_ptr<Prism::Asset::Texture>& texture)
{
    Prism::Asset::Material::Parameters parameters{};
    parameters.roughness = roughness;
    auto material = std::make_shared<Prism::Asset::Material>();
    material->Initialize(
        parameters,
        texture,
        texture,
        texture,
        texture,
        texture);
    return material;
}
} // namespace

int main()
{
    using namespace Prism::Scene;
    static_assert(!std::is_convertible_v<
        SceneGeneration,
        RenderSceneDataRevision>);
    static_assert(!std::is_convertible_v<
        RenderViewId,
        LogicalFrameId>);
    static_assert(sizeof(Prism::Asset::MeshHandle)
        == sizeof(std::uint32_t));
    static_assert(sizeof(Prism::Asset::TextureHandle)
        == sizeof(std::uint32_t));
    static_assert(sizeof(Prism::Asset::MaterialHandle)
        == sizeof(std::uint32_t));

    try
    {
        RenderObject object{};
        object.name = "ImmutableObject";
        std::vector<RenderObject> sourceObjects{object};
        RenderObjectIdentityRegistry initialIdentities(
            SceneGeneration{3});
        std::vector<RenderSceneObjectSource> initialSources;
        initialSources.push_back(
            MakeProgrammaticObject(
                "immutable-object",
                sourceObjects[0].name));
        const auto sceneData = std::make_shared<RenderSceneData>(
            BuildRenderSceneData(
                initialIdentities,
                RenderSceneDataRevision{7},
                std::move(initialSources)));
        sourceObjects[0].name = "MutatedSource";
        Expect(
            sceneData->GetRenderObjects()[0].name
                == "ImmutableObject",
            "RenderSceneData aliases mutable source object values.");
        Expect(
            !sceneData->GetFeatureUsage().hasOpaqueGeometry
                && !sceneData->GetFeatureUsage().hasTransparentGeometry
                && !sceneData->GetFeatureUsage().hasShadowCaster
                && !sceneData->GetFeatureUsage().hasTerrainSurface
                && !sceneData->GetFeatureUsage().hasGpuDrivenCandidate,
            "Invalid render bindings contributed static feature usage.");

        const auto featureMesh =
            std::make_shared<Prism::Asset::Mesh>();
        const auto featureTexture =
            std::make_shared<Prism::Asset::Texture>();
        const auto featureMaterial =
            MakeMaterial(0.5f, featureTexture);
        const auto makeRenderableSource = [&featureMesh, &featureMaterial](
                                              const char* key,
                                              const char* name)
        {
            RenderSceneObjectSource source =
                MakeProgrammaticObject(key, name);
            source.object.mesh = featureMesh;
            source.object.material = featureMaterial;
            return source;
        };
        std::vector<RenderSceneObjectSource> featureSources;
        featureSources.push_back(
            makeRenderableSource("opaque", "Opaque"));
        featureSources.push_back(
            makeRenderableSource("transparent", "Transparent"));
        featureSources.back().object.hasMaterialOverride = true;
        featureSources.back().object.materialOverride.alphaMode = 2.0f;
        featureSources.push_back(
            makeRenderableSource("terrain", "Terrain"));
        featureSources.back().object.surfaceType =
            RenderSurfaceType::VirtualTerrain;
        featureSources.push_back(
            makeRenderableSource("hidden", "Hidden"));
        featureSources.back().object.visible = false;
        featureSources.push_back(
            makeRenderableSource("editor", "EditorOnly"));
        featureSources.back().object.editorOnly = true;
        RenderObjectIdentityRegistry featureIdentities(
            SceneGeneration{30});
        const auto featureData =
            std::make_shared<const RenderSceneData>(
                BuildRenderSceneData(
                    featureIdentities,
                    RenderSceneDataRevision{1},
                    std::move(featureSources)));
        const RenderSceneFeatureUsage& staticUsage =
            featureData->GetFeatureUsage();
        Expect(
            staticUsage.hasOpaqueGeometry
                && staticUsage.hasTransparentGeometry
                && staticUsage.hasShadowCaster
                && staticUsage.hasTerrainSurface
                && staticUsage.hasGpuDrivenCandidate,
            "Renderable objects produced incomplete static feature usage.");

        auto gameSelection =
            std::make_shared<RenderViewSelection>();
        gameSelection->revision = 11;
        gameSelection->objectIndices = {0};
        RenderView gameView = MakeView(1);
        gameView.historyRevision = 13;
        gameView.cameraCutRevision = 2;
        gameView.selection = gameSelection;
        RenderView sceneView = MakeView(2, 960, 600);
        sceneView.historyRevision = 21;

        RenderFrameDynamicData dynamicData{};
        dynamicData.activePointLightCount = 1;
        dynamicData.pointLights[0].intensity = 3.0f;
        dynamicData.activeSpotLightCount = 1;
        dynamicData.spotLights[0].intensity = 2.0f;
        dynamicData.spotLights[0].castsShadow = true;
        RenderFramePacket packet(
            LogicalFrameId{19},
            2.5,
            sceneData,
            dynamicData,
            {gameView, sceneView});

        Expect(
            packet.GetSceneData().get() == sceneData.get(),
            "RenderFramePacket copied immutable scene data.");
        Expect(
            packet.GetLogicalFrameId() == LogicalFrameId{19}
                && packet.GetSimulationTimeSeconds() == 2.5,
            "RenderFramePacket lost logical-frame dynamic values.");
        Expect(
            packet.GetDynamicData().activePointLightCount == 1
                && packet.GetDynamicData().pointLights[0].intensity
                    == 3.0f,
            "RenderFramePacket lost frame-shared light values.");
        const RenderFrameFeatureUsage& frameUsage =
            packet.GetFeatureUsage();
        Expect(
            frameUsage.hasDirectionalLight
                && frameUsage.hasPointLights
                && frameUsage.hasSpotLights
                && frameUsage.hasShadowedPointLights
                && frameUsage.hasShadowedSpotLights,
            "RenderFramePacket produced incomplete dynamic light usage.");
        const RenderView* const retainedGame =
            packet.FindView(RenderViewId{1});
        const RenderView* const retainedScene =
            packet.FindView(RenderViewId{2});
        Expect(
            retainedGame != nullptr && retainedScene != nullptr
                && retainedGame->historyRevision == 13
                && retainedScene->historyRevision == 21
                && retainedGame->selection->objectIndices[0] == 0,
            "RenderFramePacket did not preserve isolated per-view values.");
        Expect(
            packet.FindView(RenderViewId{99}) == nullptr,
            "RenderFramePacket resolved an unknown view ID.");

        auto retainedViewPacket =
            std::make_shared<const RenderFramePacket>(
                LogicalFrameId{20},
                2.75,
                sceneData,
                dynamicData,
                std::vector<RenderView>{gameView, sceneView});
        std::weak_ptr<const RenderFramePacket> retainedViewPacketWeak =
            retainedViewPacket;
        {
            RenderSceneView packetView(
                retainedViewPacket,
                RenderViewId{1});
            retainedViewPacket.reset();
            Expect(
                !retainedViewPacketWeak.expired()
                    && packetView.GetPacket() != nullptr,
                "RenderSceneView did not retain its frame packet.");
            Expect(
                packetView.GetRenderObjects().data()
                    == sceneData->GetRenderObjects().data()
                    && &packetView.GetFeatureUsage()
                        == &sceneData->GetFeatureUsage()
                    && packetView.IsObjectSelected(0)
                    && packetView.GetActivePointLightCount() == 1,
                "RenderSceneView copied or mismatched packet scene/view data.");
        }
        Expect(
            retainedViewPacketWeak.expired(),
            "RenderSceneView retained a packet after the view was released.");

        RenderFrameDynamicData neutralLights{};
        neutralLights.directionalLight.intensity = 0.0f;
        neutralLights.activePointLightCount = 1;
        neutralLights.pointLights[0].intensity = 0.0f;
        neutralLights.activeSpotLightCount = 1;
        neutralLights.spotLights[0].range = 0.0f;
        const RenderFramePacket neutralPacket(
            LogicalFrameId{22},
            3.0,
            sceneData,
            neutralLights,
            {gameView});
        const RenderFrameFeatureUsage& neutralUsage =
            neutralPacket.GetFeatureUsage();
        Expect(
            !neutralUsage.hasDirectionalLight
                && !neutralUsage.hasPointLights
                && !neutralUsage.hasSpotLights
                && !neutralUsage.hasShadowedPointLights
                && !neutralUsage.hasShadowedSpotLights,
            "Zero-energy lights contributed dynamic feature usage.");

        RenderObject rootTerrain{};
        rootTerrain.name = "TerrainRoot";
        rootTerrain.quadtreePatch.enabled = true;
        rootTerrain.quadtreePatch.level = 0;
        rootTerrain.quadtreePatch.maxLevel = 1;
        RenderObject childTerrain{};
        childTerrain.name = "TerrainChild";
        childTerrain.quadtreePatch.enabled = true;
        childTerrain.quadtreePatch.level = 1;
        childTerrain.quadtreePatch.maxLevel = 1;
        RenderObjectIdentityRegistry terrainIdentities(
            SceneGeneration{4});
        std::vector<RenderSceneObjectSource> terrainSources;
        terrainSources.push_back(MakeProgrammaticObject(
            "terrain-root", rootTerrain.name));
        terrainSources.back().object = rootTerrain;
        terrainSources.push_back(MakeProgrammaticObject(
            "terrain-child", childTerrain.name));
        terrainSources.back().object = childTerrain;
        terrainSources.back().parentProgrammaticKey =
            "terrain-root";
        RenderObject gameFrustum{};
        gameFrustum.name = "GameCameraFrustum";
        gameFrustum.editorOnly = true;
        gameFrustum.surfaceType = RenderSurfaceType::EditorDebugLine;
        terrainSources.push_back(MakeProgrammaticObject(
            "game-camera-frustum", gameFrustum.name));
        terrainSources.back().object = gameFrustum;
        auto terrainData = std::make_shared<const RenderSceneData>(
            BuildRenderSceneData(
                terrainIdentities,
                RenderSceneDataRevision{1},
                std::move(terrainSources)));
        RenderView terrainView = MakeView(1);
        terrainView.selection = BuildRenderViewSelection(
            *terrainData,
            1,
            [](const RenderObject&,
                const RenderSceneObjectMetadata&)
            {
                return true;
            });
        RenderView terrainSceneView = MakeView(2);
        terrainSceneView.selection = terrainView.selection;
        auto terrainPacket = std::make_shared<const RenderFramePacket>(
            LogicalFrameId{21},
            3.0,
            terrainData,
            RenderFrameDynamicData{},
            std::vector<RenderView>{terrainView, terrainSceneView});
        const RenderSceneView leafTerrainView(
            terrainPacket,
            RenderViewId{1},
            RenderTerrainSelectionPolicy::LeafOnly);
        const RenderSceneView rootTerrainView(
            terrainPacket,
            RenderViewId{1},
            RenderTerrainSelectionPolicy::RootOnly);
        Expect(
            !leafTerrainView.IsObjectSelected(0)
                && leafTerrainView.IsObjectSelected(1)
                && rootTerrainView.IsObjectSelected(0)
                && !rootTerrainView.IsObjectSelected(1),
            "RenderSceneView terrain selection changed stable object indices.");

        const std::vector<GpuVisibilityReason> terrainReasons{
            GpuVisibilityReason::LodRejected,
            GpuVisibilityReason::Visible,
            GpuVisibilityReason::Unknown};
        Camera feedbackCamera = terrainView.camera;
        feedbackCamera.SetWorldPosition({11.0, 12.0, 13.0});
        feedbackCamera.SetRotation(0.25f, -0.5f);
        const auto makeFeedback =
            [&](const SceneGeneration generation,
                const RenderSceneDataRevision revision,
                const RenderViewId viewId)
            {
                return std::make_shared<const RenderViewFeedback>(
                    RenderViewFeedbackIdentity{
                        generation,
                        revision,
                        viewId,
                        LogicalFrameId{20}},
                    terrainReasons,
                    feedbackCamera);
            };
        const auto validTerrainFeedback = makeFeedback(
            SceneGeneration{4},
            RenderSceneDataRevision{1},
            GameRenderViewId);
        Expect(
            validTerrainFeedback->Matches(
                *terrainPacket, GameRenderViewId),
            "Matching terrain visibility feedback identity was rejected.");
        const RenderSceneView feedbackTerrainView(
            terrainPacket,
            SceneRenderViewId,
            RenderTerrainSelectionPolicy::RootOnly,
            validTerrainFeedback,
            GameRenderViewId);
        Expect(
            feedbackTerrainView.GetVisibilityFeedback() != nullptr,
            "Matching terrain visibility feedback was not retained by the view.");
        Expect(
            !feedbackTerrainView.IsObjectSelected(0),
            "Matching terrain visibility feedback did not reject a split root.");
        Expect(
            feedbackTerrainView.IsObjectSelected(1),
            "Matching terrain visibility feedback did not select the split child.");
        Expect(
            feedbackTerrainView.GetGpuVisibilityReason(0)
                    == GpuVisibilityReason::LodRejected
                && terrainData->GetRenderObjects()[0]
                       .gpuVisibilityReason
                    == GpuVisibilityReason::Unknown,
            "Versioned visibility feedback did not select the matching terrain hierarchy.");
        DirectX::XMFLOAT4X4 frustumWorld{};
        DirectX::XMStoreFloat4x4(
            &frustumWorld,
            feedbackTerrainView.GetObjectWorldMatrix(2));
        Expect(
            std::abs(frustumWorld._41 - 11.0f) < 0.0001f
                && std::abs(frustumWorld._42 - 12.0f) < 0.0001f
                && std::abs(frustumWorld._43 - 13.0f) < 0.0001f,
            "Editor game-camera frustum ignored the matched culling camera feedback.");

        const RenderSceneView staleSceneFeedbackView(
            terrainPacket,
            SceneRenderViewId,
            RenderTerrainSelectionPolicy::RootOnly,
            makeFeedback(
                SceneGeneration{5},
                RenderSceneDataRevision{1},
                GameRenderViewId));
        const RenderSceneView staleDataFeedbackView(
            terrainPacket,
            SceneRenderViewId,
            RenderTerrainSelectionPolicy::RootOnly,
            makeFeedback(
                SceneGeneration{4},
                RenderSceneDataRevision{2},
                GameRenderViewId));
        const RenderSceneView wrongViewFeedbackView(
            terrainPacket,
            SceneRenderViewId,
            RenderTerrainSelectionPolicy::RootOnly,
            makeFeedback(
                SceneGeneration{4},
                RenderSceneDataRevision{1},
                SceneRenderViewId));
        Expect(
            staleSceneFeedbackView.GetVisibilityFeedback() == nullptr
                && staleDataFeedbackView.GetVisibilityFeedback() == nullptr
                && wrongViewFeedbackView.GetVisibilityFeedback() == nullptr
                && staleSceneFeedbackView.IsObjectSelected(0)
                && !staleSceneFeedbackView.IsObjectSelected(1),
            "A stale or wrong-view GPU feedback mapping reached the current scene view.");

        const auto replacementData =
            std::make_shared<RenderSceneData>(
                BuildRenderSceneData(
                    initialIdentities,
                    RenderSceneDataRevision{8},
                    {}));
        RenderFramePacket nextPacket(
            LogicalFrameId{20},
            2.6,
            replacementData,
            RenderFrameDynamicData{},
            {MakeView(1)});
        Expect(
            packet.GetSceneData()->GetDataRevision()
                    == RenderSceneDataRevision{7}
                && nextPacket.GetSceneData()->GetDataRevision()
                    == RenderSceneDataRevision{8},
            "Publishing a new data revision mutated an old packet.");

        ExpectRejected(
            []
            {
                (void)RenderSceneData(
                    SceneGeneration{},
                    RenderSceneDataRevision{1},
                    {},
                    {},
                    {});
            },
            "RenderSceneData accepted a zero scene generation.");
        ExpectRejected(
            [sceneData]
            {
                (void)RenderFramePacket(
                    LogicalFrameId{},
                    0.0,
                    sceneData,
                    RenderFrameDynamicData{},
                    {MakeView(1)});
            },
            "RenderFramePacket accepted a zero logical-frame ID.");
        ExpectRejected(
            []
            {
                (void)RenderFramePacket(
                    LogicalFrameId{1},
                    0.0,
                    nullptr,
                    RenderFrameDynamicData{},
                    {MakeView(1)});
            },
            "RenderFramePacket accepted null scene data.");
        ExpectRejected(
            [sceneData]
            {
                (void)RenderFramePacket(
                    LogicalFrameId{1},
                    std::numeric_limits<double>::infinity(),
                    sceneData,
                    RenderFrameDynamicData{},
                    {MakeView(1)});
            },
            "RenderFramePacket accepted non-finite simulation time.");
        ExpectRejected(
            [sceneData]
            {
                (void)RenderFramePacket(
                    LogicalFrameId{1},
                    0.0,
                    sceneData,
                    RenderFrameDynamicData{},
                    {MakeView(1), MakeView(1)});
            },
            "RenderFramePacket accepted duplicate view IDs.");
        ExpectRejected(
            [sceneData]
            {
                RenderView invalid = MakeView(1, 0, 800);
                (void)RenderFramePacket(
                    LogicalFrameId{1},
                    0.0,
                    sceneData,
                    RenderFrameDynamicData{},
                    {invalid});
            },
            "RenderFramePacket accepted a zero-sized view.");
        ExpectRejected(
            [sceneData]
            {
                auto invalidSelection =
                    std::make_shared<RenderViewSelection>();
                invalidSelection->objectIndices = {1};
                RenderView invalid = MakeView(1);
                invalid.selection = invalidSelection;
                (void)RenderFramePacket(
                    LogicalFrameId{1},
                    0.0,
                    sceneData,
                    RenderFrameDynamicData{},
                    {invalid});
            },
            "RenderFramePacket accepted an out-of-range object selection.");

        RenderObjectIdentityRegistry topologyIdentities(
            SceneGeneration{100});
        const Prism::Engine::EntityId worldRootId{10, 1};
        const Prism::Engine::EntityId worldChildId{10, 2};
        std::vector<RenderSceneObjectSource> topologySources;
        topologySources.push_back(MakeProgrammaticObject(
            "terrain-root", "TerrainRoot"));
        topologySources.push_back(MakeWorldObject(
            worldRootId, "WorldRoot"));
        topologySources.push_back(MakeProgrammaticObject(
            "terrain-child", "TerrainChild", false, "terrain-root"));
        topologySources.push_back(MakeWorldObject(
            worldChildId, "WorldChild", worldRootId));
        topologySources.push_back(MakeProgrammaticObject(
            "editor-helper", "EditorHelper", true));
        const RenderSceneData topologyV1 = BuildRenderSceneData(
            topologyIdentities,
            RenderSceneDataRevision{1},
            topologySources);
        const RenderObjectId terrainRootObjectId =
            topologyV1.GetObjectMetadata()[0].id;
        const RenderObjectId terrainChildObjectId =
            topologyV1.GetObjectMetadata()[2].id;
        const RenderObjectId worldRootObjectId =
            topologyV1.GetObjectMetadata()[1].id;
        const std::uint32_t terrainRootInstance =
            topologyV1.GetObjectMetadata()[0].firstInstance;
        const std::uint32_t terrainChildInstance =
            topologyV1.GetObjectMetadata()[2].firstInstance;
        Expect(
            topologyV1.GetObjectMetadata()[2].parentIndex == 0
                && topologyV1.GetObjectMetadata()[3].parentIndex == 1,
            "RenderSceneData did not resolve programmatic/World parents.");

        std::vector<RenderSceneObjectSource> changedSources;
        changedSources.push_back(MakeProgrammaticObject(
            "new-object", "NewObject"));
        changedSources.push_back(MakeWorldObject(
            worldRootId, "WorldRoot"));
        changedSources.push_back(MakeProgrammaticObject(
            "terrain-child", "TerrainChild", false, "terrain-root"));
        changedSources.push_back(MakeProgrammaticObject(
            "terrain-root", "TerrainRoot"));
        changedSources.push_back(MakeWorldObject(
            worldChildId, "WorldChild", worldRootId));
        const RenderSceneData topologyV2 = BuildRenderSceneData(
            topologyIdentities,
            RenderSceneDataRevision{2},
            std::move(changedSources));
        const std::uint32_t terrainRootIndexV2 =
            *topologyV2.FindObjectIndex(terrainRootObjectId);
        const std::uint32_t terrainChildIndexV2 =
            *topologyV2.FindObjectIndex(terrainChildObjectId);
        Expect(
            topologyV2.GetObjectMetadata()[terrainRootIndexV2]
                    .firstInstance == terrainRootInstance
                && topologyV2.GetObjectMetadata()[terrainChildIndexV2]
                    .firstInstance == terrainChildInstance,
            "Add/remove/reorder changed stable instance indices.");
        Expect(
            topologyV2.GetObjectMetadata()[terrainChildIndexV2]
                    .parentIndex == terrainRootIndexV2
                && topologyV2.FindObjectIndex(worldRootObjectId)
                    .has_value(),
            "Data revision rebuild did not remap parents by stable identity.");

        const std::vector<RenderSceneObjectMetadata>
            metadataBeforeSelection =
                topologyV1.GetObjectMetadata();
        const auto gameTopologySelection = BuildRenderViewSelection(
            topologyV1,
            1,
            [](const RenderObject& selected,
               const RenderSceneObjectMetadata&)
            {
                return selected.visible && !selected.editorOnly;
            });
        const auto editorTopologySelection = BuildRenderViewSelection(
            topologyV1,
            2,
            [](const RenderObject& selected,
               const RenderSceneObjectMetadata&)
            {
                return selected.visible;
            });
        Expect(
            gameTopologySelection->objectIndices.size() == 4
                && editorTopologySelection->objectIndices.size() == 5
                && topologyV1.GetObjectMetadata()
                    == metadataBeforeSelection,
            "View filtering mutated or compacted shared object metadata.");
        Expect(
            topologyV1.GetObjectMetadata()[2].parentIndex == 0
                && topologyV1.GetObjectMetadata()[2].firstInstance
                    == terrainChildInstance,
            "View filtering changed parentIndex or firstInstance.");

        topologyIdentities.ResetTopology(SceneGeneration{101});
        const RenderSceneData rebuiltTopology = BuildRenderSceneData(
            topologyIdentities,
            RenderSceneDataRevision{1},
            {MakeProgrammaticObject("terrain-root", "TerrainRoot"),
             MakeWorldObject(worldRootId, "WorldRoot")});
        Expect(
            rebuiltTopology.GetObjectMetadata()[0].id
                    != terrainRootObjectId
                && rebuiltTopology.GetObjectMetadata()[1].id
                    == worldRootObjectId,
            "Topology generation did not renew programmatic identity or preserve World identity.");

        Prism::Asset::AssetRegistry assetRegistry;
        auto textureAsset =
            std::make_shared<Prism::Asset::TextureAsset>();
        const Prism::Asset::TextureHandle textureHandle =
            assetRegistry.RegisterTextureAsset(
                "snapshot-texture",
                textureAsset);
        auto materialAsset =
            std::make_shared<Prism::Asset::MaterialAsset>();
        materialAsset->SetBaseColorTexture(textureHandle);
        materialAsset->SetMetallicRoughnessTexture(textureHandle);
        materialAsset->SetNormalTexture(textureHandle);
        materialAsset->SetOcclusionTexture(textureHandle);
        materialAsset->SetEmissiveTexture(textureHandle);
        const Prism::Asset::MaterialHandle materialHandle =
            assetRegistry.RegisterMaterialAsset(
                "snapshot-material",
                materialAsset);
        const Prism::Asset::MeshHandle meshHandle{17};
        const auto meshA = std::make_shared<Prism::Asset::Mesh>();
        const auto meshB = std::make_shared<Prism::Asset::Mesh>();
        const auto textureA =
            std::make_shared<Prism::Asset::Texture>();
        const auto textureB =
            std::make_shared<Prism::Asset::Texture>();
        const auto materialA = MakeMaterial(0.2f, textureA);
        const auto materialB = MakeMaterial(0.8f, textureB);
        assetRegistry.SetRuntimeMesh(meshHandle, meshA);
        assetRegistry.SetRuntimeTexture(textureHandle, textureA);
        assetRegistry.SetRuntimeMaterial(materialHandle, materialA);

        RenderObjectIdentityRegistry bindingIdentities(
            SceneGeneration{200});
        RenderSceneObjectSource boundSource =
            MakeProgrammaticObject("bound-object", "BoundObject");
        boundSource.object.meshHandle = meshHandle;
        boundSource.object.materialHandle = materialHandle;
        boundSource.object.mesh = meshA;
        boundSource.object.material = materialA;
        const RenderSceneData bindingV1 = BuildRenderSceneData(
            bindingIdentities,
            RenderSceneDataRevision{1},
            {boundSource},
            &assetRegistry);
        const RenderSceneObjectAssetBindings oldBindings =
            bindingV1.GetObjectBindings()[0];
        Expect(
            oldBindings.mesh.get() == meshA.get()
                && oldBindings.material.hasParameters
                && oldBindings.material.parameters.roughness == 0.2f
                && oldBindings.material.textures[0].resource.get()
                    == textureA.get(),
            "Initial runtime asset bindings were not captured by value/version.");
        Expect(
            bindingV1.GetRenderObjects()[0].mesh == nullptr
                && bindingV1.GetRenderObjects()[0].material == nullptr,
            "Published object values expose mutable runtime asset pointers.");

        assetRegistry.SetRuntimeMesh(meshHandle, meshB);
        assetRegistry.SetRuntimeTexture(textureHandle, textureB);
        assetRegistry.SetRuntimeMaterial(materialHandle, materialB);
        const RenderSceneData bindingV2 = BuildRenderSceneData(
            bindingIdentities,
            RenderSceneDataRevision{2},
            {boundSource},
            &assetRegistry);
        const RenderSceneObjectAssetBindings& newBindings =
            bindingV2.GetObjectBindings()[0];
        Expect(
            newBindings.mesh.get() == meshB.get()
                && newBindings.material.parameters.roughness == 0.8f
                && newBindings.material.textures[0].resource.get()
                    == textureB.get(),
            "Replacement runtime asset bindings were not published.");
        Expect(
            newBindings.meshRevision != oldBindings.meshRevision
                && newBindings.material.revision
                    != oldBindings.material.revision
                && newBindings.material.textures[0].revision
                    != oldBindings.material.textures[0].revision,
            "Runtime asset replacement did not advance binding revisions.");
        Expect(
            oldBindings.mesh.get() == meshA.get()
                && oldBindings.material.parameters.roughness == 0.2f
                && oldBindings.material.textures[0].resource.get()
                    == textureA.get(),
            "Runtime asset replacement mutated an old scene binding.");

        materialA->Initialize(
            materialB->GetParameters(),
            textureB,
            textureB,
            textureB,
            textureB,
            textureB);
        Expect(
            oldBindings.material.parameters.roughness == 0.2f,
            "Mutable source material changed published CPU parameters.");
        Expect(
            oldBindings.meshHandle.Value() == meshHandle.Value()
                && oldBindings.material.handle.Value()
                    == materialHandle.Value()
                && oldBindings.material.textures[0].handle.Value()
                    == textureHandle.Value(),
            "Runtime binding versioning changed serialized handle values.");

        assetRegistry.SetRuntimeTexture(textureHandle, nullptr);
        const Prism::Asset::RuntimeTextureBinding evictedBinding =
            assetRegistry.GetRuntimeTextureBinding(textureHandle);
        Expect(
            evictedBinding.revision
                && evictedBinding.resource == nullptr
                && oldBindings.material.textures[0].resource.get()
                    == textureA.get(),
            "Eviction did not publish a new empty binding or retain the old lease.");

        // A packet is the ownership boundary for old GPU-resource bindings.
        // Replacing registry bindings may publish the next version, but the
        // resources referenced by an in-flight packet remain alive until its
        // final view/consumer releases that packet.
        std::weak_ptr<Prism::Asset::Mesh> oldFrameMeshWeak;
        std::weak_ptr<Prism::Asset::Texture> oldFrameTextureWeak;
        std::weak_ptr<const RenderFramePacket> oldFramePacketWeak;
        {
            auto oldFrameTextureAsset =
                std::make_shared<Prism::Asset::TextureAsset>();
            const Prism::Asset::TextureHandle oldFrameTextureHandle =
                assetRegistry.RegisterTextureAsset(
                    "old-frame-texture",
                    oldFrameTextureAsset);
            auto oldFrameMaterialAsset =
                std::make_shared<Prism::Asset::MaterialAsset>();
            oldFrameMaterialAsset->SetBaseColorTexture(
                oldFrameTextureHandle);
            const Prism::Asset::MaterialHandle oldFrameMaterialHandle =
                assetRegistry.RegisterMaterialAsset(
                    "old-frame-material",
                    oldFrameMaterialAsset);
            const Prism::Asset::MeshHandle oldFrameMeshHandle{701};
            auto oldFrameMesh =
                std::make_shared<Prism::Asset::Mesh>();
            auto oldFrameTexture =
                std::make_shared<Prism::Asset::Texture>();
            auto oldFrameMaterial =
                MakeMaterial(0.35f, oldFrameTexture);
            oldFrameMeshWeak = oldFrameMesh;
            oldFrameTextureWeak = oldFrameTexture;
            assetRegistry.SetRuntimeMesh(
                oldFrameMeshHandle, oldFrameMesh);
            assetRegistry.SetRuntimeTexture(
                oldFrameTextureHandle, oldFrameTexture);
            assetRegistry.SetRuntimeMaterial(
                oldFrameMaterialHandle, oldFrameMaterial);

            RenderSceneObjectSource oldFrameSource =
                MakeProgrammaticObject(
                    "old-frame-object", "OldFrameObject");
            oldFrameSource.object.meshHandle = oldFrameMeshHandle;
            oldFrameSource.object.materialHandle =
                oldFrameMaterialHandle;
            RenderObjectIdentityRegistry oldFrameIdentities(
                SceneGeneration{250});
            auto oldFrameData =
                std::make_shared<const RenderSceneData>(
                    BuildRenderSceneData(
                        oldFrameIdentities,
                        RenderSceneDataRevision{1},
                        {oldFrameSource},
                        &assetRegistry));
            auto oldFramePacket =
                std::make_shared<const RenderFramePacket>(
                    LogicalFrameId{30},
                    0.5,
                    oldFrameData,
                    RenderFrameDynamicData{},
                    std::vector<RenderView>{MakeView(1)});
            oldFramePacketWeak = oldFramePacket;
            RenderSceneView oldFrameView(
                oldFramePacket, GameRenderViewId);
            oldFrameData.reset();

            auto replacementTexture =
                std::make_shared<Prism::Asset::Texture>();
            auto replacementMaterial =
                MakeMaterial(0.9f, replacementTexture);
            assetRegistry.SetRuntimeMesh(
                oldFrameMeshHandle,
                std::make_shared<Prism::Asset::Mesh>());
            assetRegistry.SetRuntimeTexture(
                oldFrameTextureHandle, replacementTexture);
            assetRegistry.SetRuntimeMaterial(
                oldFrameMaterialHandle, replacementMaterial);
            oldFrameMesh.reset();
            oldFrameTexture.reset();
            oldFrameMaterial.reset();
            oldFramePacket.reset();

            Expect(
                !oldFramePacketWeak.expired()
                    && !oldFrameMeshWeak.expired()
                    && !oldFrameTextureWeak.expired()
                    && oldFrameView.GetMesh(0)
                        == oldFrameMeshWeak.lock().get()
                    && oldFrameView.GetMaterialTexture(
                        0, MaterialTextureSlot::Albedo)
                        == oldFrameTextureWeak.lock().get(),
                "An old frame/view lost its replaced runtime resource lease.");
        }
        Expect(
            oldFramePacketWeak.expired()
                && oldFrameMeshWeak.expired()
                && oldFrameTextureWeak.expired(),
            "Released old frame resources remained retained without a consumer.");

        ExpectRejected(
            []
            {
                RenderObjectIdentityRegistry identities(
                    SceneGeneration{1});
                (void)BuildRenderSceneData(
                    identities,
                    RenderSceneDataRevision{1},
                    {MakeProgrammaticObject("same", "A"),
                     MakeProgrammaticObject("same", "B")});
            },
            "RenderSceneData accepted duplicate programmatic keys.");
        ExpectRejected(
            []
            {
                RenderObjectIdentityRegistry identities(
                    SceneGeneration{1});
                (void)BuildRenderSceneData(
                    identities,
                    RenderSceneDataRevision{1},
                    {MakeProgrammaticObject(
                        "child", "Child", false, "missing")});
            },
            "RenderSceneData accepted a missing parent.");
        ExpectRejected(
            []
            {
                RenderObjectIdentityRegistry identities(
                    SceneGeneration{1});
                (void)BuildRenderSceneData(
                    identities,
                    RenderSceneDataRevision{1},
                    {MakeProgrammaticObject("a", "A", false, "b"),
                     MakeProgrammaticObject("b", "B", false, "a")});
            },
            "RenderSceneData accepted a parent cycle.");

        Prism::Scene::RenderScene dynamicScene;
        dynamicScene.GetGameCamera().SetPosition(
            {10.0f, 4.0f, -8.0f});
        dynamicScene.GetCamera().SetPosition(
            {-6.0f, 3.0f, -5.0f});
        Prism::Scene::RenderDynamicInputState dynamicInputs;
        dynamicInputs.ResetForScene(SceneGeneration{300});
        Prism::Renderer::RenderSettings gameSettings{};
        Prism::Renderer::RenderSettings sceneSettings{};
        Prism::Renderer::RenderHistorySettingsTracker
            gameSettingsTracker;
        Prism::Renderer::RenderHistorySettingsTracker
            sceneSettingsTracker;
        const auto gameSettingsV1 =
            gameSettingsTracker.Observe(gameSettings);
        const auto sceneSettingsV1 =
            sceneSettingsTracker.Observe(sceneSettings);
        dynamicInputs.BeginFrame(LogicalFrameId{1}, 0.0);
        Expect(dynamicInputs.SynchronizeLights(dynamicScene),
            "Initial dynamic lights were not published.");
        Expect(
            dynamicInputs.GetDynamicData().activePointLightCount
                    <= dynamicInputs.GetDynamicData().pointLights.size()
                && dynamicInputs.GetDynamicData().activeSpotLightCount
                    <= dynamicInputs.GetDynamicData().spotLights.size(),
            "Dynamic light synchronization exceeded packet capacity.");
        Expect(
            dynamicInputs.SynchronizeView(
                GameRenderViewId,
                dynamicScene.GetGameCamera(),
                1280,
                800,
                gameSettingsV1.revision)
                && dynamicInputs.SynchronizeView(
                    SceneRenderViewId,
                    dynamicScene.GetCamera(),
                    900,
                    700,
                    sceneSettingsV1.revision),
            "Initial dynamic views were not published.");
        const std::uint64_t initialLightRevision =
            dynamicInputs.GetDynamicData().revision;
        const std::uint64_t initialGameHistory =
            dynamicInputs.FindView(GameRenderViewId)
                ->historyRevision;
        const std::uint64_t initialSceneHistory =
            dynamicInputs.FindView(SceneRenderViewId)
                ->historyRevision;

        dynamicScene.GetGameCamera().SetPosition(
            {11.0f, -0.25f, -8.0f});
        dynamicInputs.BeginFrame(LogicalFrameId{2}, 0.5);
        Expect(
            dynamicInputs.SynchronizeView(
                GameRenderViewId,
                dynamicScene.GetGameCamera(),
                1280,
                800,
                gameSettingsV1.revision)
                && !dynamicInputs.SynchronizeView(
                    SceneRenderViewId,
                    dynamicScene.GetCamera(),
                    900,
                    700,
                    sceneSettingsV1.revision),
            "Camera-only input did not isolate the changed view.");
        const RenderView* cameraMovedGame =
            dynamicInputs.FindView(GameRenderViewId);
        const RenderView* unchangedScene =
            dynamicInputs.FindView(SceneRenderViewId);
        Expect(
            cameraMovedGame->cameraRevision == 2
                && cameraMovedGame->historyRevision
                    == initialGameHistory
                && cameraMovedGame->camera.GetPosition().y < 0.0f
                && unchangedScene->cameraRevision == 1
                && unchangedScene->historyRevision
                    == initialSceneHistory
                && dynamicInputs.GetSimulationTimeSeconds() == 0.5,
            "Camera navigation changed history/static state, crossed views, or froze dynamic time.");

        gameSettings.ocean.optics.material.absorption.x *= 0.5f;
        const auto gameSettingsV2 =
            gameSettingsTracker.Observe(gameSettings);
        Expect(
            gameSettingsV2.historyInvalidated
                && gameSettingsV2.revision
                    == gameSettingsV1.revision + 1,
            "A water-history setting did not publish a settings revision.");
        Expect(
            dynamicInputs.SynchronizeView(
                GameRenderViewId,
                dynamicScene.GetGameCamera(),
                1280,
                800,
                gameSettingsV2.revision),
            "A history-affecting settings revision was not consumed.");
        Expect(
            dynamicInputs.FindView(GameRenderViewId)
                    ->historyRevision
                == initialGameHistory + 1
                && HasRenderViewHistoryInvalidation(
                    dynamicInputs.FindView(GameRenderViewId)
                        ->historyInvalidation,
                    RenderViewHistoryInvalidation::Settings)
                && dynamicInputs.FindView(SceneRenderViewId)
                        ->historyRevision
                    == initialSceneHistory,
            "Settings invalidation did not remain isolated to its view.");

        dynamicScene.GetPointLights()[0].intensity += 1.0f;
        Expect(
            dynamicInputs.SynchronizeLights(dynamicScene)
                && dynamicInputs.GetDynamicData().revision
                    == initialLightRevision + 1,
            "Light edits did not advance only the small dynamic block.");
        dynamicInputs.NotifyHistoryInvalidation(
            GameRenderViewId,
            RenderViewHistoryInvalidation::CameraCut);
        Expect(
            HasRenderViewHistoryInvalidation(
                dynamicInputs.FindView(GameRenderViewId)
                    ->historyInvalidation,
                RenderViewHistoryInvalidation::CameraCut)
                && dynamicInputs.FindView(GameRenderViewId)
                        ->cameraCutRevision
                    == 1,
            "Explicit validation camera cuts did not publish a view event.");

        Expect(
            ParseRenderScenePublicationMode("")
                    == RenderScenePublicationMode::Versioned
                && ParseRenderScenePublicationMode("full-rebuild")
                    == RenderScenePublicationMode::FullRebuild
                && ParseRenderScenePublicationMode("versioned")
                    == RenderScenePublicationMode::Versioned,
            "RenderScene publication mode parsing lost its versioned default or diagnostic fallback.");
        ExpectRejected(
            []
            {
                (void)ParseRenderScenePublicationMode("incremental-ish");
            },
            "RenderScene publication mode accepted an invalid value.");

        Prism::Scene::RenderScene extractionScene;
        RenderObject extractedObject{};
        extractedObject.name = "ExtractedObject";
        extractedObject.meshHandle = meshHandle;
        extractedObject.materialHandle = materialHandle;
        extractedObject.transform.SetPosition({1.0f, 2.0f, 3.0f});
        extractionScene.AddRenderObject(std::move(extractedObject));
        RenderSceneExtractor fullExtractor(
            assetRegistry,
            RenderScenePublicationMode::FullRebuild);
        RenderSceneExtractor defaultExtractor(assetRegistry);
        RenderSceneExtractor versionedExtractor(
            assetRegistry,
            RenderScenePublicationMode::Versioned);
        Expect(
            defaultExtractor.GetMode()
                == RenderScenePublicationMode::Versioned,
            "RenderSceneExtractor did not default to versioned publication.");
        const auto MakeExtractionRequest = [&extractionScene](
            const std::uint64_t dataRevision,
            const std::uint64_t bindingRevision,
            const Prism::Engine::SceneChangeCategory category)
        {
            Prism::Engine::SceneChangeSet changes{};
            changes.revision = dataRevision;
            changes.categories = category;
            changes.mutationCount =
                category == Prism::Engine::SceneChangeCategory::None
                ? 0u : 1u;
            return RenderSceneExtractionRequest{
                extractionScene,
                SceneGeneration{400},
                RenderSceneDataRevision{dataRevision},
                bindingRevision,
                changes};
        };
        const auto ExpectEquivalentExtraction = [](
            const RenderSceneExtractionResult& left,
            const RenderSceneExtractionResult& right)
        {
            const auto& leftObjects =
                left.sceneData->GetRenderObjects();
            const auto& rightObjects =
                right.sceneData->GetRenderObjects();
            const auto& leftMetadata =
                left.sceneData->GetObjectMetadata();
            const auto& rightMetadata =
                right.sceneData->GetObjectMetadata();
            const auto& leftBindings =
                left.sceneData->GetObjectBindings();
            const auto& rightBindings =
                right.sceneData->GetObjectBindings();
            Expect(
                leftObjects.size() == rightObjects.size()
                    && leftObjects[0].name == rightObjects[0].name
                    && leftObjects[0].transform.GetPosition().x
                        == rightObjects[0].transform.GetPosition().x
                    && leftMetadata == rightMetadata
                    && leftBindings[0].meshRevision
                        == rightBindings[0].meshRevision
                    && leftBindings[0].material.revision
                        == rightBindings[0].material.revision,
                "Full-rebuild and versioned extraction produced different values or bindings.");
        };

        const RenderSceneExtractionResult fullV1 =
            fullExtractor.Extract(MakeExtractionRequest(
                1,
                1,
                Prism::Engine::SceneChangeCategory::EntityTopology));
        const RenderSceneExtractionResult defaultV1 =
            defaultExtractor.Extract(MakeExtractionRequest(
                1,
                1,
                Prism::Engine::SceneChangeCategory::EntityTopology));
        const RenderSceneExtractionResult versionedV1 =
            versionedExtractor.Extract(MakeExtractionRequest(
                1,
                1,
                Prism::Engine::SceneChangeCategory::EntityTopology));
        Expect(
            fullV1.sourceObjectVisitCount == 1
                && versionedV1.sourceObjectVisitCount == 1,
            "A rebuilt extraction did not report its source object visits.");
        ExpectEquivalentExtraction(fullV1, versionedV1);
        const RenderSceneExtractionResult defaultV1Again =
            defaultExtractor.Extract(MakeExtractionRequest(
                1,
                1,
                Prism::Engine::SceneChangeCategory::None));
        Expect(
            defaultV1.rebuilt
                && defaultV1Again.reused
                && defaultV1Again.sceneData.get()
                    == defaultV1.sceneData.get(),
            "Default publication did not rebuild once then reuse unchanged data.");
        const RenderSceneExtractionResult fullV1Again =
            fullExtractor.Extract(MakeExtractionRequest(
                1,
                1,
                Prism::Engine::SceneChangeCategory::None));
        const RenderSceneExtractionResult versionedV1Again =
            versionedExtractor.Extract(MakeExtractionRequest(
                1,
                1,
                Prism::Engine::SceneChangeCategory::None));
        Expect(
            fullV1Again.rebuilt && !fullV1Again.reused
                && fullV1Again.sceneData.get()
                    != fullV1.sceneData.get()
                && versionedV1Again.reused
                && !versionedV1Again.rebuilt
                && versionedV1Again.sourceObjectVisitCount == 0
                && versionedV1Again.sceneData.get()
                    == versionedV1.sceneData.get(),
            "Extraction strategies did not preserve full rebuild or versioned reuse semantics.");

        extractionScene.EditRenderObjectsForFullRebuild()[0]
            .transform.SetPosition({7.0f, 2.0f, 3.0f});
        const RenderSceneExtractionResult fullV2 =
            fullExtractor.Extract(MakeExtractionRequest(
                2,
                1,
                Prism::Engine::SceneChangeCategory::Transform));
        const RenderSceneExtractionResult versionedV2 =
            versionedExtractor.Extract(MakeExtractionRequest(
                2,
                1,
                Prism::Engine::SceneChangeCategory::Transform));
        Expect(
            fullV2.rebuilt && versionedV2.rebuilt,
            "A versioned object mutation did not rebuild once.");
        ExpectEquivalentExtraction(fullV2, versionedV2);

        const auto meshC = std::make_shared<Prism::Asset::Mesh>();
        assetRegistry.SetRuntimeMesh(meshHandle, meshC);
        const RenderSceneExtractionResult fullBindingV2 =
            fullExtractor.Extract(MakeExtractionRequest(
                2,
                2,
                Prism::Engine::SceneChangeCategory::None));
        const RenderSceneExtractionResult versionedBindingV2 =
            versionedExtractor.Extract(MakeExtractionRequest(
                2,
                2,
                Prism::Engine::SceneChangeCategory::None));
        ExpectEquivalentExtraction(fullBindingV2, versionedBindingV2);
        Expect(
            versionedBindingV2.rebuilt
                && versionedBindingV2.sceneData
                    ->GetObjectBindings()[0].mesh.get()
                    == meshC.get(),
            "Binding revision did not rebuild the versioned extraction result.");

        const RenderSceneExtractionResult unknownFallback =
            versionedExtractor.Extract(MakeExtractionRequest(
                2,
                2,
                Prism::Engine::SceneChangeCategory::FullRebuild));
        Expect(
            unknownFallback.rebuilt
                && unknownFallback.conservativeFallback
                && unknownFallback.effectiveMode
                    == RenderScenePublicationMode::FullRebuild
                && unknownFallback.reason
                    == "unknown-write-full-rebuild",
            "An unknown mutable write did not produce a visible full-rebuild fallback.");

        // D5 mutation matrix: committed static writes rebuild once, dynamic
        // inputs reuse the immutable scene, and missing commit metadata takes
        // an explicit conservative path. Reuse reports zero source visits,
        // proving it does not scan/hash the object array every frame.
        RenderScene matrixScene;
        RenderObject matrixRoot{};
        matrixRoot.name = "MatrixRoot";
        matrixScene.AddRenderObject(std::move(matrixRoot));
        RenderSceneExtractor matrixExtractor(
            assetRegistry,
            RenderScenePublicationMode::Versioned);
        const auto MakeMatrixRequest = [&matrixScene](
            const std::uint64_t generation,
            const std::uint64_t dataRevision,
            const Prism::Engine::SceneChangeCategory category)
        {
            Prism::Engine::SceneChangeSet changes{};
            changes.revision = dataRevision;
            changes.categories = category;
            changes.mutationCount =
                category == Prism::Engine::SceneChangeCategory::None
                ? 0u : 1u;
            return RenderSceneExtractionRequest{
                matrixScene,
                SceneGeneration{generation},
                RenderSceneDataRevision{dataRevision},
                0,
                changes};
        };
        std::uint64_t matrixRevision = 1;
        RenderSceneExtractionResult matrixResult =
            matrixExtractor.Extract(MakeMatrixRequest(
                500,
                matrixRevision,
                Prism::Engine::SceneChangeCategory::EntityTopology));
        Expect(
            matrixResult.rebuilt
                && matrixResult.sourceObjectVisitCount == 1,
            "Initial mutation-matrix extraction did not rebuild.");

        struct StaticMutationCase
        {
            Prism::Engine::SceneChangeCategory category;
            std::function<void()> mutate;
        };
        std::vector<StaticMutationCase> staticMutationCases;
        staticMutationCases.push_back({
            Prism::Engine::SceneChangeCategory::EntityTopology,
            [&matrixScene]
            {
                RenderObject child{};
                child.name = "MatrixChild";
                matrixScene.AddRenderObject(std::move(child));
            }});
        staticMutationCases.push_back({
            Prism::Engine::SceneChangeCategory::Hierarchy,
            [&matrixScene]
            {
                matrixScene.EditRenderObjectsForFullRebuild()[1]
                    .quadtreePatch.parentPatchName = "MatrixRoot";
            }});
        staticMutationCases.push_back({
            Prism::Engine::SceneChangeCategory::Transform,
            [&matrixScene]
            {
                matrixScene.EditRenderObjectsForFullRebuild()[0]
                    .transform.SetPosition({3.0f, 4.0f, 5.0f});
            }});
        staticMutationCases.push_back({
            Prism::Engine::SceneChangeCategory::Visibility,
            [&matrixScene]
            {
                auto& objects =
                    matrixScene.EditRenderObjectsForFullRebuild();
                objects[0].visible = false;
                objects[1].editorOnly = true;
            }});
        staticMutationCases.push_back({
            Prism::Engine::SceneChangeCategory::Material,
            [&matrixScene, meshHandle, materialHandle]
            {
                auto& object =
                    matrixScene.EditRenderObjectsForFullRebuild()[0];
                object.meshHandle = meshHandle;
                object.materialHandle = materialHandle;
                object.hasMaterialOverride = true;
                object.materialOverride.roughness = 0.45f;
            }});

        for (const StaticMutationCase& mutation :
             staticMutationCases)
        {
            mutation.mutate();
            ++matrixRevision;
            const RenderSceneExtractionResult changed =
                matrixExtractor.Extract(MakeMatrixRequest(
                    500, matrixRevision, mutation.category));
            const RenderSceneExtractionResult unchanged =
                matrixExtractor.Extract(MakeMatrixRequest(
                    500,
                    matrixRevision,
                    Prism::Engine::SceneChangeCategory::None));
            Expect(
                changed.rebuilt
                    && !changed.conservativeFallback
                    && changed.sourceObjectVisitCount
                        == matrixScene.GetRenderObjects().size()
                    && unchanged.reused
                    && unchanged.sceneData.get()
                        == changed.sceneData.get()
                    && unchanged.sourceObjectVisitCount == 0,
                "A committed D5 static mutation did not rebuild exactly once.");
        }

        for (const Prism::Engine::SceneChangeCategory dynamicCategory :
             {Prism::Engine::SceneChangeCategory::Camera,
              Prism::Engine::SceneChangeCategory::Lighting})
        {
            const RenderSceneExtractionResult dynamicOnly =
                matrixExtractor.Extract(MakeMatrixRequest(
                    500, matrixRevision, dynamicCategory));
            Expect(
                dynamicOnly.reused
                    && dynamicOnly.sourceObjectVisitCount == 0,
                "A camera/light-only D5 mutation rebuilt static scene data.");
        }

        constexpr std::uint64_t CameraOnlyAcceptanceFrameCount = 120;
        std::shared_ptr<const RenderSceneData> cameraOnlySceneData =
            matrixExtractor.Extract(MakeMatrixRequest(
                500,
                matrixRevision,
                Prism::Engine::SceneChangeCategory::None))
                .sceneData;
        std::uint64_t cameraOnlyBuildCount = 0;
        std::uint64_t cameraOnlyReuseCount = 0;
        std::uint64_t cameraOnlySourceObjectVisitCount = 0;
        for (std::uint64_t frame = 0;
             frame < CameraOnlyAcceptanceFrameCount;
             ++frame)
        {
            // The changing camera value lives in the view/dynamic path; the
            // extractor receives only the categorized commit identity.
            matrixScene.GetGameCamera().SetPosition(
                {static_cast<float>(frame) * 0.01f, 1.0f, -5.0f});
            const RenderSceneExtractionResult cameraOnly =
                matrixExtractor.Extract(MakeMatrixRequest(
                    500,
                    matrixRevision,
                    Prism::Engine::SceneChangeCategory::Camera));
            cameraOnlyBuildCount += cameraOnly.rebuilt ? 1u : 0u;
            cameraOnlyReuseCount += cameraOnly.reused ? 1u : 0u;
            cameraOnlySourceObjectVisitCount +=
                cameraOnly.sourceObjectVisitCount;
            Expect(
                cameraOnly.sceneData.get()
                    == cameraOnlySceneData.get(),
                "A camera-only frame replaced immutable scene data.");
        }
        Expect(
            cameraOnlyBuildCount == 0
                && cameraOnlyReuseCount
                    == CameraOnlyAcceptanceFrameCount
                && cameraOnlySourceObjectVisitCount == 0,
            "Camera-only acceptance rebuilt or scanned static objects.");

        // Continuous editing acceptance stays at the extractor boundary so
        // it measures publication semantics without paying the unrelated
        // undo/journal serialization cost of 120 UI commands. The command,
        // transaction, rollback and bridge paths are covered separately by
        // SceneSessionTests.
        constexpr std::uint64_t ContinuousEditBatchCount = 120;
        std::uint64_t continuousEditBuildCount = 0;
        std::uint64_t continuousEditReuseCount = 0;
        for (std::uint64_t batch = 0;
             batch < ContinuousEditBatchCount;
             ++batch)
        {
            matrixScene.EditRenderObjectsForFullRebuild()[0]
                .transform.SetPosition(
                    {3.0f + static_cast<float>(batch) * 0.01f,
                     4.0f,
                     5.0f});
            ++matrixRevision;
            const RenderSceneExtractionResult changed =
                matrixExtractor.Extract(MakeMatrixRequest(
                    500,
                    matrixRevision,
                    Prism::Engine::SceneChangeCategory::Transform));
            const RenderSceneExtractionResult unchanged =
                matrixExtractor.Extract(MakeMatrixRequest(
                    500,
                    matrixRevision,
                    Prism::Engine::SceneChangeCategory::None));
            continuousEditBuildCount += changed.rebuilt ? 1u : 0u;
            continuousEditBuildCount += unchanged.rebuilt ? 1u : 0u;
            continuousEditReuseCount += changed.reused ? 1u : 0u;
            continuousEditReuseCount += unchanged.reused ? 1u : 0u;
            Expect(
                changed.rebuilt
                    && !changed.reused
                    && !changed.conservativeFallback
                    && changed.sourceObjectVisitCount
                        == matrixScene.GetRenderObjects().size()
                    && unchanged.reused
                    && !unchanged.rebuilt
                    && unchanged.sourceObjectVisitCount == 0
                    && unchanged.sceneData.get()
                        == changed.sceneData.get(),
                "A continuous object-edit batch was missed, rebuilt twice, or falsely reused.");
        }
        Expect(
            continuousEditBuildCount == ContinuousEditBatchCount
                && continuousEditReuseCount
                    == ContinuousEditBatchCount,
            "Continuous object edits did not rebuild exactly once per batch.");

        const RenderSceneExtractionResult switchedGeneration =
            matrixExtractor.Extract(MakeMatrixRequest(
                501,
                1,
                Prism::Engine::SceneChangeCategory::EntityTopology));
        Expect(
            switchedGeneration.rebuilt
                && switchedGeneration.sceneData->GetSceneGeneration()
                    == SceneGeneration{501},
            "Scene activation did not rebuild generation-bound mappings.");

        RenderObject untrackedObject{};
        untrackedObject.name = "UntrackedTopology";
        matrixScene.AddRenderObject(std::move(untrackedObject));
        const RenderSceneExtractionResult untrackedTopology =
            matrixExtractor.Extract(MakeMatrixRequest(
                501,
                1,
                Prism::Engine::SceneChangeCategory::None));
        Expect(
            untrackedTopology.conservativeFallback
                && untrackedTopology.effectiveMode
                    == RenderScenePublicationMode::FullRebuild
                && untrackedTopology.reason
                    == "untracked-topology-full-rebuild",
            "An uncommitted topology write was silently reused.");

        matrixScene.EditRenderObjectsForFullRebuild()[0].visible = true;
        const RenderSceneExtractionResult untrackedRawWrite =
            matrixExtractor.Extract(MakeMatrixRequest(
                501,
                1,
                Prism::Engine::SceneChangeCategory::None));
        Expect(
            untrackedRawWrite.conservativeFallback
                && untrackedRawWrite.effectiveMode
                    == RenderScenePublicationMode::FullRebuild
                && untrackedRawWrite.reason
                    == "raw-mutable-write-full-rebuild",
            "A raw mutable value write was silently reused.");

        Expect(
            dynamicInputs.GetDynamicData().activePointLightCount
                    <= dynamicInputs.GetDynamicData().pointLights.size()
                && dynamicInputs.GetDynamicData().activeSpotLightCount
                    <= dynamicInputs.GetDynamicData().spotLights.size(),
            "Extraction acceptance corrupted dynamic light packet data.");

        Prism::Scene::RenderSceneMailbox packetMailbox;
        std::shared_ptr<const RenderSceneData> retainedPacketData =
            std::make_shared<const RenderSceneData>(
                *versionedBindingV2.sceneData);
        std::shared_ptr<const RenderFramePacket> retainedPacket =
            std::make_shared<const RenderFramePacket>(
                LogicalFrameId{10},
                1.0,
                retainedPacketData,
                dynamicInputs.GetDynamicData(),
                std::vector<RenderView>{MakeView(1)});
        std::weak_ptr<const RenderFramePacket> firstPacketWeak =
            retainedPacket;
        std::weak_ptr<const RenderSceneData> firstPacketDataWeak =
            retainedPacketData;
        (void)packetMailbox.PublishFramePacket(retainedPacket);
        std::shared_ptr<const RenderFramePacket> retainedConsumer =
            packetMailbox.AcquireLatestPacket();

        RenderObjectIdentityRegistry switchedIdentities(
            SceneGeneration{401});
        RenderSceneObjectSource switchedSource =
            MakeProgrammaticObject(
                "switched-object",
                "SwitchedObject");
        auto switchedData = std::make_shared<const RenderSceneData>(
            BuildRenderSceneData(
                switchedIdentities,
                RenderSceneDataRevision{1},
                {switchedSource},
                &assetRegistry));
        auto switchedPacket =
            std::make_shared<const RenderFramePacket>(
                LogicalFrameId{11},
                2.0,
                switchedData,
                dynamicInputs.GetDynamicData(),
                std::vector<RenderView>{MakeView(1)});
        (void)packetMailbox.PublishFramePacket(switchedPacket);
        Expect(
            packetMailbox.AcquireLatestPacket().get()
                    == switchedPacket.get()
                && packetMailbox.AcquireLatestPacket()
                    ->GetSceneData()->GetSceneGeneration()
                    == SceneGeneration{401}
                && retainedConsumer.get() == retainedPacket.get()
                && retainedConsumer->GetSceneData().get()
                    == retainedPacketData.get(),
            "FramePacket publication was not self-consistent across continuous publish or scene switch.");
        retainedPacket.reset();
        retainedPacketData.reset();
        Expect(
            !firstPacketWeak.expired()
                && !firstPacketDataWeak.expired(),
            "An acquired packet consumer did not retain its packet/data.");
        retainedConsumer.reset();
        Expect(
            firstPacketWeak.expired()
                && firstPacketDataWeak.expired(),
            "Mailbox retained an unbounded history after all old consumers released it.");

        std::weak_ptr<const RenderFramePacket> priorPacket;
        for (std::uint64_t frame = 12; frame < 28; ++frame)
        {
            auto loopPacket =
                std::make_shared<const RenderFramePacket>(
                    LogicalFrameId{frame},
                    static_cast<double>(frame) / 60.0,
                    switchedData,
                    dynamicInputs.GetDynamicData(),
                    std::vector<RenderView>{MakeView(1)});
            (void)packetMailbox.PublishFramePacket(loopPacket);
            if (frame > 12)
            {
                Expect(
                    priorPacket.expired(),
                    "Mailbox retained more than its latest unobserved packet.");
            }
            priorPacket = loopPacket;
        }

        std::cout << "RenderScene publication type tests passed.\n";
        return 0;
    }
    catch (const std::exception& exception)
    {
        std::cerr << "RenderScene publication type test failed: "
                  << exception.what() << '\n';
        return 1;
    }
}
