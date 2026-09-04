#include "Scene/ShadowShowcaseSceneFactory.h"

#include "Scene/DemoSceneBuilder.h"
#include "Scene/RenderScene.h"

#include <DirectXMath.h>

#include <array>
#include <cmath>
#include <string>

namespace Prism::Scene
{
using namespace DirectX;

namespace
{
constexpr const char* ShadowRoot = "builtin://shadow-lab/";
constexpr std::uint32_t DirectionalSubjectCount = 12;
constexpr std::uint32_t PointLightCount = 2;
constexpr std::uint32_t SpotLightCount = 2;
}

ShadowShowcaseSceneSummary ShadowShowcaseSceneFactory::Populate(
    Asset::AssetRegistry& assetRegistry,
    RHI::IGraphicsDevice& device,
    RenderScene& scene)
{
    scene.ClearRenderObjects();
    DemoSceneBuilder::ResetLights(scene);

    const DemoMeshResources meshes =
        DemoSceneBuilder::CreateMeshes(
            assetRegistry,
            device,
            ShadowRoot,
            "ShadowLab");
    const DemoMaterialResources floor =
        DemoSceneBuilder::CreateMaterial(
            assetRegistry,
            device,
            ShadowRoot,
            {"Floor", {0.52f, 0.55f, 0.60f, 1.0f}, 0.0f, 0.78f});
    const DemoMaterialResources neutral =
        DemoSceneBuilder::CreateMaterial(
            assetRegistry,
            device,
            ShadowRoot,
            {"Neutral", {0.62f, 0.66f, 0.72f, 1.0f}, 0.08f, 0.42f});
    const DemoMaterialResources dark =
        DemoSceneBuilder::CreateMaterial(
            assetRegistry,
            device,
            ShadowRoot,
            {"Dark", {0.08f, 0.10f, 0.14f, 1.0f}, 0.42f, 0.34f});
    const DemoMaterialResources warm =
        DemoSceneBuilder::CreateMaterial(
            assetRegistry,
            device,
            ShadowRoot,
            {"Warm", {0.88f, 0.32f, 0.09f, 1.0f}, 0.12f, 0.36f});
    const DemoMaterialResources cool =
        DemoSceneBuilder::CreateMaterial(
            assetRegistry,
            device,
            ShadowRoot,
            {"Cool", {0.08f, 0.34f, 0.92f, 1.0f}, 0.28f, 0.30f});
    const DemoMaterialResources warmEmitter =
        DemoSceneBuilder::CreateMaterial(
            assetRegistry,
            device,
            ShadowRoot,
            {"WarmEmitter", {1.0f, 0.36f, 0.06f, 1.0f}, 0.0f, 0.24f,
             {1.0f, 0.16f, 0.02f}, 9.0f});
    const DemoMaterialResources coolEmitter =
        DemoSceneBuilder::CreateMaterial(
            assetRegistry,
            device,
            ShadowRoot,
            {"CoolEmitter", {0.05f, 0.42f, 1.0f, 1.0f}, 0.0f, 0.24f,
             {0.02f, 0.24f, 1.0f}, 9.0f});

    DemoSceneBuilder::AddObject(
        scene, "ShadowLab_Floor",
        meshes.cubeHandle, meshes.cube, floor,
        {0.0f, -0.25f, 21.0f},
        {22.0f, 0.25f, 34.0f});
    DemoSceneBuilder::AddObject(
        scene, "ShadowLab_Backdrop",
        meshes.cubeHandle, meshes.cube, dark,
        {0.0f, 5.0f, 54.5f},
        {22.0f, 5.0f, 0.35f});

    // The center lane deliberately crosses all three directional-light cascades.
    for (std::uint32_t index = 0;
         index < DirectionalSubjectCount;
         ++index)
    {
        const float z = -5.0f + static_cast<float>(index) * 4.8f;
        const float x = index % 2 == 0 ? -2.2f : 2.2f;
        const float height = 0.8f + static_cast<float>(index % 4) * 0.42f;
        const bool sphere = index % 3 == 1;
        DemoSceneBuilder::AddObject(
            scene,
            "CSM_Subject_" + std::to_string(index),
            sphere ? meshes.sphereHandle : meshes.cubeHandle,
            sphere ? meshes.sphere : meshes.cube,
            index % 2 == 0 ? warm : neutral,
            {x, height, z},
            sphere
                ? XMFLOAT3{height, height, height}
                : XMFLOAT3{0.85f, height, 0.85f},
            {0.0f, 0.18f * static_cast<float>(index), 0.0f});
    }

    // Left bay: two cones make spot-shadow coverage and penumbra boundaries easy to read.
    for (std::uint32_t index = 0; index < 5; ++index)
    {
        const float z = 5.0f + static_cast<float>(index) * 3.4f;
        DemoSceneBuilder::AddObject(
            scene,
            "SpotShadow_Subject_" + std::to_string(index),
            index % 2 == 0 ? meshes.sphereHandle : meshes.cubeHandle,
            index % 2 == 0 ? meshes.sphere : meshes.cube,
            neutral,
            {-11.0f + static_cast<float>(index % 2) * 2.2f,
             1.0f + static_cast<float>(index % 3) * 0.25f,
             z},
            {1.0f, 1.0f + static_cast<float>(index % 3) * 0.25f, 1.0f});
    }

    // Right bay: blockers surround an omnidirectional shadow-casting point light.
    constexpr float TwoPi = XM_2PI;
    for (std::uint32_t index = 0; index < 8; ++index)
    {
        const float angle = TwoPi * static_cast<float>(index) / 8.0f;
        const float radius = index % 2 == 0 ? 4.2f : 6.0f;
        const float height = 0.8f + static_cast<float>(index % 3) * 0.48f;
        DemoSceneBuilder::AddObject(
            scene,
            "PointShadow_Blocker_" + std::to_string(index),
            meshes.cubeHandle,
            meshes.cube,
            index % 2 == 0 ? cool : neutral,
            {11.0f + std::cos(angle) * radius,
             height,
             11.0f + std::sin(angle) * radius},
            {0.72f, height, 0.72f},
            {0.0f, -angle, 0.0f});
    }

    const std::array<XMFLOAT3, PointLightCount> pointPositions{{
        {11.0f, 3.2f, 11.0f},
        {9.0f, 2.8f, 29.0f}}};
    const std::array<XMFLOAT3, PointLightCount> pointColors{{
        {0.12f, 0.42f, 1.0f},
        {1.0f, 0.26f, 0.06f}}};
    for (std::uint32_t index = 0; index < PointLightCount; ++index)
    {
        PointLight& light = scene.GetPointLights()[index];
        light.position = pointPositions[index];
        light.color = pointColors[index];
        light.intensity = index == 0 ? 8.0f : 4.5f;
        light.range = index == 0 ? 12.0f : 9.0f;
        light.castsShadow = index == 0;
        DemoSceneBuilder::AddObject(
            scene,
            "PointLight_Emitter_" + std::to_string(index),
            meshes.sphereHandle,
            meshes.sphere,
            index == 0 ? coolEmitter : warmEmitter,
            light.position,
            {0.24f, 0.24f, 0.24f});
    }
    scene.SetActivePointLightCount(PointLightCount);

    for (std::uint32_t index = 0; index < SpotLightCount; ++index)
    {
        SpotLight& light = scene.GetSpotLights()[index];
        light.position = {
            -13.0f + static_cast<float>(index) * 4.0f,
            8.0f,
            2.0f + static_cast<float>(index) * 10.0f};
        light.direction = {
            0.16f - static_cast<float>(index) * 0.30f,
            -0.94f,
            0.22f};
        light.color = index == 0
            ? XMFLOAT3{1.0f, 0.38f, 0.08f}
            : XMFLOAT3{0.12f, 0.42f, 1.0f};
        light.intensity = 7.0f;
        light.range = 23.0f;
        light.innerAngleRadians = XMConvertToRadians(15.0f);
        light.outerAngleRadians = XMConvertToRadians(27.0f);
        light.castsShadow = true;
        DemoSceneBuilder::AddObject(
            scene,
            "SpotLight_Emitter_" + std::to_string(index),
            meshes.sphereHandle,
            meshes.sphere,
            index == 0 ? warmEmitter : coolEmitter,
            light.position,
            {0.22f, 0.22f, 0.22f});
    }
    scene.SetActiveSpotLightCount(SpotLightCount);

    return {
        static_cast<std::uint32_t>(scene.GetRenderObjects().size()),
        DirectionalSubjectCount,
        PointLightCount,
        SpotLightCount};
}

void ShadowShowcaseSceneFactory::ConfigureWorld(
    RenderScene& scene,
    const float cameraAspectRatio)
{
    Camera& camera = scene.GetCamera();
    camera.SetPerspective(
        XMConvertToRadians(48.0f),
        cameraAspectRatio,
        0.1f,
        180.0f);
    camera.SetLookAt(
        {24.0f, 14.0f, -25.0f},
        {0.0f, 2.0f, 18.0f},
        {0.0f, 1.0f, 0.0f});
    scene.GetGameCamera() = camera;

    DirectionalLight& sun = scene.GetDirectionalLight();
    sun.direction = {-0.46f, -0.78f, 0.32f};
    sun.color = {1.0f, 0.90f, 0.76f};
    sun.intensity = 2.0f;
}
} // namespace Prism::Scene
