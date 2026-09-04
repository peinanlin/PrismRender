#include "Asset/AssetRegistry.h"
#include "Asset/MeshAsset.h"
#include "Engine/World.h"
#include "Renderer/DemoSceneSettings.h"
#include "Renderer/MaterialParameterResolver.h"
#include "Renderer/RenderSettings.h"
#include "Scene/DemoSceneCatalog.h"
#include "Scene/CameraController.h"
#include "Scene/DefaultSceneFactory.h"
#include "Scene/FeatureLabSceneFactory.h"
#include "Scene/FluidLabSceneFactory.h"
#include "Scene/LightingShowcaseSceneFactory.h"
#include "Scene/RenderScene.h"
#include "Scene/ShadowShowcaseSceneFactory.h"
#include "Scene/ShowcaseSceneFactory.h"
#include "Scene/TerrainOceanSceneFactory.h"
#include "Scene/Transform.h"
#include "Scene/WorldRenderSceneBridge.h"

#include <cmath>
#include <cstdlib>
#include <iostream>
#include <stdexcept>

namespace
{
void Expect(const bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
}

bool Near(const float left, const float right)
{
    return std::abs(left - right) < 0.0001f;
}

bool NearDouble(const double left, const double right)
{
    return std::abs(left - right) < 0.000000001;
}
} // namespace

int main()
{
    try
    {
        Prism::Asset::AssetRegistry assets;
        Prism::Scene::Camera terrainEditorCamera;
        terrainEditorCamera.SetWorldPosition(
            {-3350.0, 2050.0, -3450.0});
        Prism::Scene::CameraController terrainController;
        terrainController.SynchronizeOrbitPivot(
            terrainEditorCamera,
            {0.0, 80.0, 0.0});
        Expect(
            terrainController.GetOrbitDistance() > 4000.0f
                && terrainController.GetOrbitDistance() < 6000.0f,
            "Scene navigation did not adopt the active Lab's world scale.");

        const std::shared_ptr<Prism::Asset::MeshAsset>
            builtinCube = Prism::Asset::MeshAsset::CreateCube();
        for (const Prism::Asset::MeshVertex& vertex :
             builtinCube->GetVertices())
        {
            Expect(
                Near(vertex.color.x, 1.0f)
                    && Near(vertex.color.y, 1.0f)
                    && Near(vertex.color.z, 1.0f)
                    && Near(vertex.color.w, 1.0f),
                "The built-in cube default vertex color is not neutral white.");
        }
        const Prism::Asset::MeshHandle mesh =
            assets.RegisterMeshAsset("assets/meshes/test.mesh", nullptr);
        const Prism::Asset::MaterialHandle material =
            assets.RegisterMaterialAsset("assets/materials/test.material", nullptr);

        Prism::Scene::RenderScene scene;
        scene.GetCamera().SetPerspective(0.9f, 16.0f / 9.0f, 0.2f, 750.0f);
        scene.GetCamera().SetPosition({1.0f, 2.0f, -6.0f});
        scene.GetCamera().SetRotation(0.15f, 0.35f);
        scene.GetGameCamera().SetPosition(
            {-12.0f, 8.0f, -24.0f});
        scene.GetGameCamera().SetRotation(
            -0.2f, 0.65f);
        Expect(
            !Near(
                scene.GetCamera().GetPosition().x,
                scene.GetGameCamera().GetPosition().x),
            "Render and culling cameras unexpectedly share state.");
        scene.GetDirectionalLight().direction = {0.1f, -0.9f, 0.2f};
        scene.GetDirectionalLight().intensity = 3.0f;
        scene.GetAuxiliaryDirectionalLights()[0].direction =
            {-0.7f, -0.6f, 0.3f};
        scene.GetAuxiliaryDirectionalLights()[0].color =
            {1.0f, 0.8f, 0.6f};
        scene.GetAuxiliaryDirectionalLights()[0].intensity = 0.35f;
        scene.GetAuxiliaryDirectionalLights()[1].direction =
            {0.8f, -0.3f, -0.5f};
        scene.GetAuxiliaryDirectionalLights()[1].color =
            {0.6f, 0.7f, 0.9f};
        scene.GetAuxiliaryDirectionalLights()[1].intensity = 0.2f;
        scene.GetPointLights()[0].position = {2.0f, 4.0f, 1.0f};
        scene.GetPointLights()[0].range = 14.0f;
        scene.GetPointLights()[0].castsShadow =
            false;
        scene.SetActivePointLightCount(1);
        scene.GetSpotLights()[0].position =
            {-2.0f, 6.0f, -3.0f};
        scene.GetSpotLights()[0].direction =
            {0.2f, -0.9f, 0.3f};
        scene.GetSpotLights()[0]
            .outerAngleRadians = 0.6f;
        scene.SetActiveSpotLightCount(1);

        Prism::Scene::RenderObject object{};
        object.name = "BridgeCube";
        object.meshHandle = mesh;
        object.materialHandle = material;
        object.transform.SetPosition({3.0f, 1.0f, 2.0f});
        object.hasMaterialOverride = true;
        object.materialOverride.albedoColor =
            {0.42f, 0.18f, 0.08f, 1.0f};
        object.materialOverride.metallic = 0.35f;
        object.materialOverride.roughness = 0.64f;
        Prism::Scene::RenderObject secondOverride = object;
        secondOverride.materialOverride.albedoColor =
            {0.08f, 0.46f, 0.82f, 1.0f};
        Expect(
            Prism::Renderer::ResolveMaterialOverrideSignature(object) != 0
                && Prism::Renderer::ResolveMaterialOverrideSignature(object)
                    != Prism::Renderer::ResolveMaterialOverrideSignature(
                        secondOverride),
            "Distinct live material overrides collapsed to one renderer cache key.");
        scene.AddRenderObject(std::move(object));
        Prism::Scene::RenderObject importedEditorHelper{};
        importedEditorHelper.name = "GameCameraFrustum";
        importedEditorHelper.editorOnly = true;
        importedEditorHelper.surfaceType =
            Prism::Scene::RenderSurfaceType::EditorDebugLine;
        scene.AddRenderObject(std::move(importedEditorHelper));

        Prism::Engine::World world;
        Prism::Scene::WorldRenderSceneBridge::ImportRenderScene(scene, assets, world, 99);
        Expect(world.GetEntityCount() == 7,
               "RenderScene import lost a camera, directional fill, or object entity.");
        Expect(scene.GetRenderObjects()[0].entityId.IsValid(), "Render object did not receive an Entity UUID.");
        Expect(!scene.GetRenderObjects()[1].entityId.IsValid(),
               "Editor-only renderer helper leaked into the authoring World.");

        Prism::Engine::EntityRecord* objectEntity =
            world.FindEntity(scene.GetRenderObjects()[0].entityId);
        Expect(objectEntity != nullptr && objectEntity->meshRenderer.has_value(),
               "Imported render object is missing MeshRenderer data.");
        Expect(objectEntity->meshRenderer->meshAsset == "assets/meshes/test.mesh"
                   && objectEntity->meshRenderer->materialAsset == "assets/materials/test.material",
               "Imported render object lost its stable asset paths.");
        Expect(objectEntity->materialOverride.has_value()
                   && Near(objectEntity->materialOverride->albedoColor.x, 0.42f)
                   && Near(objectEntity->materialOverride->roughness, 0.64f),
               "RenderScene MaterialOverride did not reach Engine::World.");
        objectEntity->transform.position = {8.0f, 2.0f, -1.0f};
        objectEntity->meshRenderer->visible = false;
        objectEntity->materialOverride =
            Prism::Engine::MaterialOverrideComponent{};
        objectEntity->materialOverride->albedoColor =
            {0.1f, 0.3f, 0.7f, 1.0f};
        objectEntity->materialOverride->metallic = 0.8f;
        objectEntity->materialOverride->roughness = 0.12f;

        for (const auto& [id, entity] : world.GetEntities())
        {
            if (entity.camera.has_value())
            {
                Prism::Engine::EntityRecord* camera = world.FindEntity(id);
                camera->transform.position = {4.0f, 5.0f, -9.0f};
                camera->camera->farPlane = 1200.0f;
            }
            if (entity.directionalLight.has_value())
            {
                Prism::Engine::EntityRecord* light = world.FindEntity(id);
                if (entity.name.value == "DirectionalLight")
                {
                    light->directionalLight->intensity = 5.5f;
                }
                else if (entity.name.value
                         == "DirectionalLight_HighFill")
                {
                    light->directionalLight->intensity = 0.75f;
                }
                else if (entity.name.value
                         == "DirectionalLight_MirrorFill")
                {
                    light->directionalLight->intensity = 0.45f;
                }
            }
            if (entity.spotLight.has_value())
            {
                Prism::Engine::EntityRecord* light =
                    world.FindEntity(id);
                light->spotLight->intensity = 7.0f;
                light->spotLight->castsShadow =
                    false;
            }
        }

        const Prism::Scene::WorldRenderSyncResult result =
            Prism::Scene::WorldRenderSceneBridge::SynchronizeToRenderScene(
                world,
                assets,
                scene,
                {true,
                 true,
                 {42,
                  Prism::Engine::SceneChangeCategory::Transform
                      | Prism::Engine::SceneChangeCategory::Visibility,
                  2}});
        Expect(result.renderObjectCount == 2 && scene.GetRenderObjects().size() == 2,
               "World synchronization produced the wrong render object count.");
        Expect(result.appliedChanges.revision == 42
                   && result.appliedChanges.mutationCount == 2
                   && Prism::Engine::HasSceneChange(
                       result.appliedChanges.categories,
                       Prism::Engine::SceneChangeCategory::Visibility),
               "World bridge did not preserve the committed change identity.");
        Expect(result.unresolvedAssets.size() == 2
                   && result.unresolvedAssets[0].entityId == objectEntity->id.ToString()
                   && result.unresolvedAssets[0].assetType == "mesh"
                   && result.unresolvedAssets[1].assetType == "material",
               "Unresolved render assets were not reported structurally.");
        Expect(scene.GetRenderObjects()[0].entityId == objectEntity->id
                   && !scene.GetRenderObjects()[0].visible,
               "World synchronization lost object identity or visibility.");
        Expect(Near(scene.GetRenderObjects()[0].transform.GetPosition().x, 8.0f),
               "World transform did not reach RenderScene.");
        Expect(scene.GetRenderObjects()[0].hasMaterialOverride
                   && scene.GetRenderObjects()[0]
                          .materialOverrideSignature != 0
                   && Near(scene.GetRenderObjects()[0]
                               .materialOverride.metallic,
                           0.8f)
                   && Near(scene.GetRenderObjects()[0]
                               .materialOverride.albedoColor.z,
                           0.7f),
               "World MaterialOverride did not reach RenderScene.");
        Expect(scene.GetRenderObjects()[1].editorOnly
                   && scene.GetRenderObjects()[1].surfaceType
                       == Prism::Scene::RenderSurfaceType::EditorDebugLine
                   && scene.GetRenderObjects()[1].name == "GameCameraFrustum",
               "World synchronization discarded renderer-only editor metadata.");
        Expect(Near(scene.GetCamera().GetPosition().y, 5.0f)
                   && Near(scene.GetCamera().GetFarPlane(), 1200.0f),
               "World camera did not reach RenderScene.");
        Expect(Near(scene.GetDirectionalLight().intensity, 5.5f),
               "World directional light did not reach RenderScene.");
        Expect(
            Near(scene.GetAuxiliaryDirectionalLights()[0].intensity,
                 0.75f)
                && Near(
                    scene.GetAuxiliaryDirectionalLights()[1].intensity,
                    0.45f),
            "World directional fill lights did not retain their renderer slots.");
        Expect(scene.GetActivePointLightCount() == 1
                   && Near(scene.GetPointLights()[0].range, 14.0f)
                   && !scene.GetPointLights()[0].castsShadow,
               "World point lights did not reach RenderScene.");
        Expect(
            scene.GetActiveSpotLightCount() == 1
                && Near(
                    scene.GetSpotLights()[0]
                        .intensity,
                    7.0f)
                && !scene.GetSpotLights()[0]
                         .castsShadow,
            "World spot lights did not reach RenderScene.");

        constexpr float CameraAspectRatio = 16.0f / 9.0f;
        const auto expectMirroredGameCamera =
            [](const Prism::Scene::RenderScene& configuredScene,
               const char* message)
        {
            Expect(
                Near(configuredScene.GetCamera().GetPosition().x,
                     configuredScene.GetGameCamera().GetPosition().x)
                    && Near(configuredScene.GetCamera().GetPosition().y,
                            configuredScene.GetGameCamera().GetPosition().y)
                    && Near(configuredScene.GetCamera().GetYaw(),
                            configuredScene.GetGameCamera().GetYaw()),
                message);
        };
        Prism::Scene::RenderScene configuredScene;
        Prism::Scene::DefaultSceneFactory::ConfigureEditorPreviewWorld(
            configuredScene, CameraAspectRatio);
        expectMirroredGameCamera(
            configuredScene,
            "Editor Preview did not initialize its Game Camera.");
        Prism::Scene::ShowcaseSceneFactory::ConfigureWorld(
            configuredScene, CameraAspectRatio);
        expectMirroredGameCamera(
            configuredScene,
            "Showcase did not initialize its Game Camera.");
        Prism::Scene::ShadowShowcaseSceneFactory::ConfigureWorld(
            configuredScene, CameraAspectRatio);
        expectMirroredGameCamera(
            configuredScene,
            "Shadow Lab did not initialize its Game Camera.");
        Prism::Scene::LightingShowcaseSceneFactory::ConfigureWorld(
            configuredScene, CameraAspectRatio);
        expectMirroredGameCamera(
            configuredScene,
            "Lighting Lab did not initialize its Game Camera.");
        Prism::Scene::FeatureLabSceneFactory::ConfigureWorld(
            Prism::Scene::FeatureLabSceneKind::Material,
            configuredScene,
            CameraAspectRatio);
        expectMirroredGameCamera(
            configuredScene,
            "Feature Lab did not initialize its Game Camera.");
        Prism::Scene::TerrainOceanSceneFactory::ConfigureOceanWorld(
            configuredScene, CameraAspectRatio);
        expectMirroredGameCamera(
            configuredScene,
            "Ocean Lab did not initialize its Game Camera.");
        const DirectX::XMFLOAT3 oceanLightDirection =
            configuredScene.GetDirectionalLight().direction;
        Expect(
            std::abs(oceanLightDirection.x) < 0.01f
                && oceanLightDirection.y < 0.0f
                && oceanLightDirection.y > -0.06f
                && oceanLightDirection.z < -0.98f,
            "Ocean Lab sun is not aligned just above the forward horizon.");
        Prism::Scene::RenderScene waveWorksConfiguredScene;
        Prism::Scene::RenderScene hpWaterConfiguredScene;
        Prism::Scene::TerrainOceanSceneFactory::ConfigureOceanWorld(
            waveWorksConfiguredScene,
            CameraAspectRatio,
            Prism::Scene::OceanLabSceneKind::WaveWorksReference);
        Prism::Scene::TerrainOceanSceneFactory::ConfigureOceanWorld(
            hpWaterConfiguredScene,
            CameraAspectRatio,
            Prism::Scene::OceanLabSceneKind::HpWaterReference);
        Expect(
            Near(waveWorksConfiguredScene.GetCamera().GetPosition().x,
                hpWaterConfiguredScene.GetCamera().GetPosition().x)
                && Near(waveWorksConfiguredScene.GetCamera().GetPosition().y,
                    hpWaterConfiguredScene.GetCamera().GetPosition().y)
                && Near(waveWorksConfiguredScene.GetCamera().GetPosition().z,
                    hpWaterConfiguredScene.GetCamera().GetPosition().z)
                && Near(waveWorksConfiguredScene.GetCamera().GetYaw(),
                    hpWaterConfiguredScene.GetCamera().GetYaw())
                && Near(waveWorksConfiguredScene.GetDirectionalLight().direction.x,
                    hpWaterConfiguredScene.GetDirectionalLight().direction.x)
                && Near(waveWorksConfiguredScene.GetDirectionalLight().direction.y,
                    hpWaterConfiguredScene.GetDirectionalLight().direction.y)
                && Near(waveWorksConfiguredScene.GetDirectionalLight().intensity,
                    hpWaterConfiguredScene.GetDirectionalLight().intensity),
            "HPWater and WaveWorks Labs must share camera and light inputs.");
        Prism::Scene::FluidLabSceneFactory::ConfigureWorld(
            configuredScene,
            CameraAspectRatio);
        expectMirroredGameCamera(
            configuredScene,
            "Fluid Lab did not initialize its Game Camera.");
        Prism::Scene::TerrainOceanSceneFactory::ConfigureTerrainWorld(
            configuredScene, CameraAspectRatio);
        Expect(
            !Near(configuredScene.GetCamera().GetPosition().x,
                  configuredScene.GetGameCamera().GetPosition().x),
            "Terrain Lab lost its intentional independent Game Camera.");

        Prism::Scene::RenderScene capacityScene;
        capacityScene.SetActivePointLightCount(96);
        Expect(
            capacityScene.GetActivePointLightCount()
                == 96,
            "RenderScene still applies the legacy four-light limit.");
        capacityScene.SetActivePointLightCount(999);
        Expect(
            capacityScene.GetActivePointLightCount()
                == Prism::Scene::RenderScene::
                    MaxPointLights,
            "RenderScene did not clamp lights to the clustered-light capacity.");
        capacityScene.SetActiveSpotLightCount(999);
        Expect(
            capacityScene.GetActiveSpotLightCount()
                == Prism::Scene::RenderScene::
                    MaxSpotLights,
            "RenderScene did not clamp spot lights to the local-light capacity.");

        Prism::Scene::RenderScene viewScene;
        viewScene.GetCamera().SetPosition({2.0f, 3.0f, -8.0f});
        viewScene.GetGameCamera().SetPosition({40.0f, 12.0f, -25.0f});
        Prism::Scene::RenderObject editorHelper{};
        editorHelper.name = "GameCameraFrustum";
        editorHelper.editorOnly = true;
        editorHelper.surfaceType =
            Prism::Scene::RenderSurfaceType::EditorDebugLine;
        viewScene.AddRenderObject(std::move(editorHelper));
        Prism::Scene::RenderObject rootTile{};
        rootTile.name = "TerrainRoot";
        rootTile.quadtreePatch.enabled = true;
        rootTile.quadtreePatch.level = 0;
        rootTile.quadtreePatch.maxLevel = 1;
        rootTile.quadtreePatch.halfExtent = 128.0f;
        rootTile.gpuVisibilityReason =
            Prism::Scene::GpuVisibilityReason::LodRejected;
        viewScene.AddRenderObject(std::move(rootTile));
        Prism::Scene::RenderObject childTile{};
        childTile.name = "TerrainChild";
        childTile.quadtreePatch.enabled = true;
        childTile.quadtreePatch.level = 1;
        childTile.quadtreePatch.maxLevel = 1;
        childTile.quadtreePatch.halfExtent = 64.0f;
        childTile.quadtreePatch.parentPatchName =
            "TerrainRoot";
        childTile.gpuVisibilityReason =
            Prism::Scene::GpuVisibilityReason::Visible;
        viewScene.AddRenderObject(std::move(childTile));

        const Prism::Scene::RenderScene gameView =
            viewScene.CreateGameView();
        Expect(
            Near(gameView.GetCamera().GetPosition().x, 40.0f)
                && !gameView.GetRenderObjects()[0].visible,
            "Game View did not activate the game camera or hide editor helpers.");
        const Prism::Scene::RenderScene editorView =
            viewScene.CreateEditorView();
        Expect(
            Near(editorView.GetCamera().GetPosition().x, 2.0f)
                && editorView.GetRenderObjects().size() == 2
                && editorView.GetRenderObjects()[0].visible
                && Near(
                    editorView.GetRenderObjects()[0]
                        .transform.GetPosition().x,
                    40.0f)
                && !editorView.GetRenderObjects()[1]
                        .quadtreePatch.enabled
                && Near(
                    editorView.GetRenderObjects()[1]
                        .quadtreePatch.halfExtent,
                    64.0f)
                && editorView.GetRenderObjects()[1]
                        .gpuVisibilityReason
                    == Prism::Scene::GpuVisibilityReason::Visible,
            "Scene View did not retain editor helpers or select the GPU terrain LOD nodes.");
        const Prism::Scene::RenderScene cpuTerrainView =
            viewScene.CreateGameView(true, false);
        Expect(
            cpuTerrainView.GetRenderObjects().size() == 2
                && cpuTerrainView.GetRenderObjects()[0].editorOnly
                && !cpuTerrainView.GetRenderObjects()[0].visible
                && cpuTerrainView.GetRenderObjects()[1].name
                    == "TerrainChild"
                && !cpuTerrainView.GetRenderObjects()[1]
                        .quadtreePatch.enabled,
            "CPU terrain fallback did not select one non-overlapping leaf LOD.");

        const Prism::Scene::DemoSceneId
            atmosphereScene =
                Prism::Scene::DemoSceneCatalog::Parse(
                    "atmosphere");
        Prism::Renderer::RenderSettings
            atmosphereSettings{};
        Prism::Renderer::ApplyDemoSceneSettings(
            atmosphereScene,
            atmosphereSettings);
        Expect(
            atmosphereScene
                    == Prism::Scene::DemoSceneId::
                        AtmosphereLab
                && atmosphereSettings
                       .physicalAtmosphereEnabled
                && !atmosphereSettings
                        .editorGridEnabled
                && Near(
                    atmosphereSettings
                        .atmosphereMieAnisotropy,
                    0.82f),
            "Physical Atmosphere Lab did not resolve its required render settings.");

        for (std::uint32_t sceneIndex = 0;
             sceneIndex < static_cast<std::uint32_t>(
                 Prism::Scene::DemoSceneId::Count);
             ++sceneIndex)
        {
            const auto sceneId =
                static_cast<Prism::Scene::DemoSceneId>(sceneIndex);
            Prism::Renderer::RenderSettings sceneSettings{};
            Prism::Renderer::ApplyDemoSceneSettings(
                sceneId,
                sceneSettings);
            const bool terrainOwner = sceneId
                == Prism::Scene::DemoSceneId::
                    TerrainVirtualTextureLab;
            Expect(
                sceneSettings.interactiveTerrainEnabled
                        == terrainOwner
                    && sceneSettings.terrainErosionEnabled
                        == terrainOwner,
                "A built-in scene leaked or lost default Terrain ownership.");
        }

        constexpr Prism::Core::Double3 worldAnchor{
            6'378'137.125,
            1'000'000.0625,
            -4'200'000.25};
        Prism::Scene::Transform precisionTransform;
        precisionTransform.SetWorldPosition(
            worldAnchor
            + Prism::Core::Double3{
                0.375,
                0.03125,
                -0.125});
        DirectX::XMFLOAT4X4 relativeWorld{};
        DirectX::XMStoreFloat4x4(
            &relativeWorld,
            precisionTransform.GetRelativeWorldMatrix(
                worldAnchor));
        Expect(
            Near(relativeWorld._41, 0.375f)
                && Near(relativeWorld._42, 0.03125f)
                && Near(relativeWorld._43, -0.125f),
            "Relative-to-Eye transform lost sub-metre offsets at planetary coordinates.");

        Prism::Scene::RenderScene precisionScene;
        precisionScene.GetCamera().SetWorldPosition(
            worldAnchor
            + Prism::Core::Double3{
                -12.375,
                8.125,
                -18.75});
        Prism::Scene::RenderObject precisionObject{};
        precisionObject.name = "PrecisionObject";
        precisionObject.meshHandle = mesh;
        precisionObject.materialHandle = material;
        precisionObject.transform.SetWorldPosition(
            worldAnchor
            + Prism::Core::Double3{
                0.375,
                1.03125,
                8.125});
        precisionScene.AddRenderObject(
            std::move(precisionObject));
        Prism::Engine::World precisionWorld;
        Prism::Scene::WorldRenderSceneBridge::ImportRenderScene(
            precisionScene,
            assets,
            precisionWorld,
            100);
        Prism::Scene::RenderScene synchronizedPrecisionScene;
        const Prism::Scene::WorldRenderSyncResult precisionSync =
            Prism::Scene::WorldRenderSceneBridge::SynchronizeToRenderScene(
                precisionWorld,
                assets,
                synchronizedPrecisionScene);
        const Prism::Core::Double3 synchronizedPosition =
            synchronizedPrecisionScene.GetRenderObjects()[0]
                .transform.GetWorldPosition();
        Expect(
            precisionSync.renderObjectCount == 1
                && NearDouble(
                synchronizedPosition.x,
                worldAnchor.x + 0.375)
                && NearDouble(
                    synchronizedPosition.y,
                    worldAnchor.y + 1.03125)
                && NearDouble(
                    synchronizedPosition.z,
                    worldAnchor.z + 8.125),
            "World-to-render synchronization truncated double-precision positions.");

        const Prism::Scene::DemoSceneId largeWorldScene =
            Prism::Scene::DemoSceneCatalog::Parse("rte");
        Prism::Renderer::RenderSettings largeWorldSettings{};
        Prism::Renderer::ApplyDemoSceneSettings(
            largeWorldScene,
            largeWorldSettings);
        Expect(
            largeWorldScene
                    == Prism::Scene::DemoSceneId::LargeWorldLab
                && largeWorldSettings.relativeToEyeEnabled
                && !largeWorldSettings.shadowsEnabled
                && !largeWorldSettings.gpuDrivenEnabled
                && !largeWorldSettings.clusteredLightingEnabled,
            "Large World Lab did not isolate the Relative-to-Eye render path.");

        const Prism::Scene::DemoSceneId terrainScene =
            Prism::Scene::DemoSceneCatalog::Parse("terrain-vt");
        Prism::Renderer::RenderSettings terrainSettings{};
        Prism::Renderer::ApplyDemoSceneSettings(
            terrainScene,
            terrainSettings);
        Expect(
            terrainScene
                    == Prism::Scene::DemoSceneId::
                        TerrainVirtualTextureLab
                && terrainSettings.virtualTerrainEnabled
                && !terrainSettings.terrainVirtualTextureEnabled
                && terrainSettings.terrainMaterialColorsEnabled
                && !terrainSettings.terrainTileDebugEnabled
                && terrainSettings.terrainWorldSize == 4096.0f
                && terrainSettings.terrainTileBorderWidth == 2.0f
                && terrainSettings.atmosphereBrightness > 1.0f
                && terrainSettings.gameCameraEnabled
                && terrainSettings.occlusionCullingEnabled
                && !terrainSettings.gpuDrivenEnabled
                && terrainSettings.frustumCullingEnabled
                && !terrainSettings.editorGridEnabled,
            "Terrain Lab did not enable GPU quadtree terrain and virtual-texture residency.");

        const Prism::Scene::DemoSceneId oceanScene =
            Prism::Scene::DemoSceneCatalog::Parse("fft-ocean");
        Prism::Renderer::RenderSettings oceanSettings{};
        Prism::Renderer::ApplyDemoSceneSettings(
            oceanScene,
            oceanSettings);
        Expect(
            oceanScene == Prism::Scene::DemoSceneId::OceanLab
                && oceanSettings.fftOceanEnabled
                && oceanSettings.ocean.implementation
                    == Prism::Renderer::OceanImplementation::LegacyFft
                && oceanSettings.oceanCameraFollowEnabled
                && oceanSettings.atmosphereBrightness > 1.0f
                && oceanSettings.physicalAtmosphereEnabled
                && !oceanSettings.deferredRenderingEnabled
                && oceanSettings.iblEnabled
                && oceanSettings.iblSplitSumEnabled
                && oceanSettings.iblSpecularStrength > 1.0f
                && oceanSettings.sunAngularRadiusDegrees < 0.5f
                && !oceanSettings.screenSpaceReflectionsEnabled
                && !oceanSettings.editorGridEnabled,
            "Ocean Lab did not enable the forward FFT and IBL reflection path.");

        const Prism::Scene::DemoSceneId waveWorksScene =
            Prism::Scene::DemoSceneCatalog::Parse("waveworks-ocean");
        Prism::Renderer::RenderSettings waveWorksSettings{};
        Prism::Renderer::ApplyDemoSceneSettings(
            waveWorksScene,
            waveWorksSettings);
        Expect(
            waveWorksScene
                    == Prism::Scene::DemoSceneId::WaveWorksLab
                && Prism::Scene::DemoSceneCatalog::Parse("waveworks")
                    == waveWorksScene
                && Prism::Scene::DemoSceneCatalog::GetDescription(
                       waveWorksScene)
                       .editorSceneViewUpdatePolicy
                    == Prism::Scene::EditorSceneViewUpdatePolicy::
                        OnInteraction
                && Prism::Scene::DemoSceneCatalog::GetDescription(
                       oceanScene)
                       .editorSceneViewUpdatePolicy
                    == Prism::Scene::EditorSceneViewUpdatePolicy::
                        Continuous
                && waveWorksSettings.fftOceanEnabled
                && waveWorksSettings.ocean.implementation
                    == Prism::Renderer::OceanImplementation::SpectralOcean
                && Near(
                    waveWorksSettings.ocean.simulationPeriodMeters,
                    1000.0f)
                && Near(waveWorksSettings.ocean.baseWind.speed, 9.9f)
                && Near(
                    waveWorksSettings.ocean.baseWind.fetchKilometers,
                    0.001f)
                && Near(waveWorksSettings.ocean.swell.speed, 29.62f)
                && Near(
                    waveWorksSettings.ocean.swell.fetchKilometers,
                    9.502f)
                && Near(
                    waveWorksSettings.ocean.shading.deepWaterColor.y,
                    0.2f)
                && Near(
                    waveWorksSettings.ocean.shading.scatteringColor.y,
                    160.0f / 255.0f)
                && waveWorksSettings.ocean.query.readbackFifoEntries
                    == 30u
                && Near(waveWorksSettings.oceanPatchLength, 1000.0f)
                && Near(waveWorksSettings.oceanWindSpeed, 9.9f)
                && !waveWorksSettings.deferredRenderingEnabled
                && waveWorksSettings.physicalAtmosphereEnabled
                && !waveWorksSettings.interactiveTerrainEnabled
                && !waveWorksSettings.terrainSculptEnabled
                && !waveWorksSettings.terrainErosionEnabled,
            "WaveWorks Ocean Lab did not load the isolated reference spectral defaults.");

        const Prism::Scene::DemoSceneId hpWaterScene =
            Prism::Scene::DemoSceneCatalog::Parse("hpwater-ocean");
        Prism::Renderer::RenderSettings hpWaterSettings{};
        Prism::Renderer::ApplyDemoSceneSettings(
            hpWaterScene,
            hpWaterSettings);
        Expect(
            hpWaterScene == Prism::Scene::DemoSceneId::HpWaterOceanLab
                && Prism::Scene::DemoSceneCatalog::Parse("hpwater")
                    == hpWaterScene
                && hpWaterSettings.fftOceanEnabled
                && hpWaterSettings.ocean.implementation
                    == Prism::Renderer::OceanImplementation::SpectralOcean
                && hpWaterSettings.ocean.opticsModel
                    == Prism::Renderer::OceanOpticsModel::HpWater
                && Near(hpWaterSettings.ocean.simulationPeriodMeters,
                    waveWorksSettings.ocean.simulationPeriodMeters)
                && Near(hpWaterSettings.ocean.baseWind.speed,
                    waveWorksSettings.ocean.baseWind.speed)
                && Near(hpWaterSettings.ocean.swell.speed,
                    waveWorksSettings.ocean.swell.speed)
                && hpWaterSettings.ocean.geometry.maximumLod
                    == waveWorksSettings.ocean.geometry.maximumLod
                && hpWaterSettings.ocean.local.gridSize
                    == waveWorksSettings.ocean.local.gridSize,
            "HPWater Ocean Lab did not isolate optics over the shared large-area wave inputs.");

        Prism::Renderer::ApplyDemoSceneSettings(
            oceanScene,
            waveWorksSettings);
        Expect(
            waveWorksSettings.ocean.implementation
                    == Prism::Renderer::OceanImplementation::LegacyFft
                && Near(waveWorksSettings.oceanPatchLength, 320.0f)
                && Near(waveWorksSettings.oceanWindSpeed, 15.0f)
                && Near(
                    waveWorksSettings.ocean.shading.deepWaterColor.y,
                    0.018f),
            "Switching from WaveWorks Lab leaked reference settings into the legacy FFT Lab.");

        const Prism::Scene::DemoSceneId pbfScene =
            Prism::Scene::DemoSceneCatalog::Parse("pbf");
        Prism::Renderer::RenderSettings pbfSettings{};
        Prism::Renderer::ApplyDemoSceneSettings(
            pbfScene,
            pbfSettings);
        Expect(
            pbfScene == Prism::Scene::DemoSceneId::PbfLab
                && pbfSettings.fluid.enabled
                && pbfSettings.fluid.particleCount == 32768u
                && pbfSettings.fluid.renderMode
                    == Prism::Renderer::FluidRenderMode::Particles
                && pbfSettings.fluid.demoPipeline
                    == Prism::Renderer::FluidDemoPipeline::PbfParticles
                && Near(
                    pbfSettings.fluid.minimumRenderDensityRatio,
                    0.20f)
                && Near(
                    pbfSettings.fluid.splashDensityRatioThreshold,
                    0.45f)
                && !pbfSettings.fluid.causticsEnabled
                && !pbfSettings.fluid.toonEnabled
                && !pbfSettings.fluid.foamEnabled,
            "PBF Demo did not select the simulation and particle-preview path.");

        const Prism::Scene::DemoSceneId fluidRenderScene =
            Prism::Scene::DemoSceneCatalog::Parse("fluid-render");
        Prism::Renderer::RenderSettings fluidRenderSettings{};
        Prism::Renderer::ApplyDemoSceneSettings(
            fluidRenderScene,
            fluidRenderSettings);
        Expect(
            fluidRenderScene
                    == Prism::Scene::DemoSceneId::FluidRenderLab
                && Prism::Scene::DemoSceneCatalog::Parse("fluid")
                    == fluidRenderScene
                && fluidRenderSettings.fluid.particleCount == 65536u
                && Near(fluidRenderSettings.fluid.particleMass, 0.2916f)
                && Near(fluidRenderSettings.fluid.domainMin.x, -3.0f)
                && Near(fluidRenderSettings.fluid.domainMax.x, 3.0f)
                && fluidRenderSettings.fluid.spawnLayout
                    == Prism::Renderer::FluidSpawnLayout::Block
                && Near(fluidRenderSettings.fluid.spawnMin.x, -2.75f)
                && Near(fluidRenderSettings.fluid.spawnMin.y, 0.08f)
                && Near(fluidRenderSettings.fluid.spawnMin.z, -2.75f)
                && Near(fluidRenderSettings.fluid.spawnMax.x, -0.05f)
                && Near(fluidRenderSettings.fluid.spawnMax.y, 2.78f)
                && Near(fluidRenderSettings.fluid.spawnMax.z, -0.05f)
                && Near(
                    fluidRenderSettings.fluid.boundaryCornerDamping,
                    0.08f)
                && Near(
                    fluidRenderSettings.fluid
                        .boundaryCornerUpwardVelocityLimit,
                    0.0f)
                && Near(
                    fluidRenderSettings.fluid.maxPositionCorrection,
                    0.03f)
                && Near(fluidRenderSettings.fluid.viscosity, 0.05f)
                && Near(fluidRenderSettings.fluid.maxVelocity, 5.0f)
                && Near(
                    fluidRenderSettings.fluid
                        .minimumRenderDensityRatio,
                    0.20f)
                && fluidRenderSettings.fluid
                        .minimumSplashNeighborCount == 10u
                && fluidRenderSettings.fluid
                        .bilateralIterations == 8u
                && fluidRenderSettings.fluid
                        .bilateralRadius == 15u
                && fluidRenderSettings.fluid
                        .normalSmoothingRadius == 6u
                && fluidRenderSettings.fluid
                        .silhouetteSmoothingRadius == 6u
                && fluidRenderSettings.fluid.renderMode
                    == Prism::Renderer::FluidRenderMode::Realistic
                && !fluidRenderSettings.fluid.causticsEnabled
                && !fluidRenderSettings.fluid.toonEnabled
                && !fluidRenderSettings.fluid.foamEnabled,
            "Screen-Space Fluid Demo did not isolate realistic reconstruction.");

        const Prism::Scene::DemoSceneId causticsScene =
            Prism::Scene::DemoSceneCatalog::Parse("fluid-caustics");
        Prism::Renderer::RenderSettings causticsSettings{};
        Prism::Renderer::ApplyDemoSceneSettings(
            causticsScene,
            causticsSettings);
        Expect(
            causticsScene
                    == Prism::Scene::DemoSceneId::FluidCausticsLab
                && causticsSettings.fluid.causticsEnabled
                && causticsSettings.fluid.renderMode
                    == Prism::Renderer::FluidRenderMode::Realistic
                && !causticsSettings.fluid.toonEnabled
                && !causticsSettings.fluid.foamEnabled,
            "Fluid Caustics Demo did not isolate realistic water and caustics.");

        const Prism::Scene::DemoSceneId toonScene =
            Prism::Scene::DemoSceneCatalog::Parse("fluid-toon");
        Prism::Renderer::RenderSettings toonSettings{};
        Prism::Renderer::ApplyDemoSceneSettings(
            toonScene,
            toonSettings);
        Expect(
            toonScene == Prism::Scene::DemoSceneId::FluidToonLab
                && toonSettings.fluid.renderMode
                    == Prism::Renderer::FluidRenderMode::Toon
                && toonSettings.fluid.toonEnabled
                && toonSettings.fluid.foamEnabled
                && !toonSettings.fluid.causticsEnabled,
            "Toon Fluid Demo did not isolate toon shading and density foam.");

        Prism::Renderer::RenderSettings sceneSwitchSettings{};
        Prism::Renderer::ApplyDemoSceneSettings(
            fluidRenderScene,
            sceneSwitchSettings);
        Prism::Renderer::ApplyDemoSceneSettings(
            atmosphereScene,
            sceneSwitchSettings);
        Expect(
            !sceneSwitchSettings.fluid.enabled
                && sceneSwitchSettings.physicalAtmosphereEnabled
                && !sceneSwitchSettings.editorGridEnabled
                && Near(sceneSwitchSettings.atmosphereMieAnisotropy, 0.82f),
            "Switching away from a fluid demo retained fluid-only render settings.");

        std::cout << "World to RenderScene bridge tests passed.\n";
        return EXIT_SUCCESS;
    }
    catch (const std::exception& exception)
    {
        std::cerr << "World to RenderScene bridge test failed: " << exception.what() << '\n';
        return EXIT_FAILURE;
    }
}
