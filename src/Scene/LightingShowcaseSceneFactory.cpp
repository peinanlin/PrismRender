#include "Scene/LightingShowcaseSceneFactory.h"

#include "Scene/DemoSceneBuilder.h"
#include "Scene/RenderObject.h"
#include "Scene/RenderScene.h"

#include <DirectXMath.h>

#include <algorithm>
#include <cmath>
#include <string>

namespace Prism::Scene
{
using namespace DirectX;

namespace
{
constexpr const char* LightingRoot = "builtin://lighting-lab/";
constexpr std::uint32_t SubjectRows = 6;
constexpr std::uint32_t SubjectColumns = 8;
constexpr std::uint32_t LightRows = 4;
constexpr std::uint32_t LightColumns = 8;
constexpr std::uint32_t SubjectCount = SubjectRows * SubjectColumns;
constexpr std::uint32_t LightCount = LightRows * LightColumns;

XMFLOAT3 HsvToRgb(const float hue)
{
    const float scaled = hue * 6.0f;
    const int sector = static_cast<int>(std::floor(scaled)) % 6;
    const float fraction = scaled - std::floor(scaled);
    const float low = 0.12f;
    const float falling = 1.0f - fraction * (1.0f - low);
    const float rising = low + fraction * (1.0f - low);
    switch (sector)
    {
    case 0: return {1.0f, rising, low};
    case 1: return {falling, 1.0f, low};
    case 2: return {low, 1.0f, rising};
    case 3: return {low, falling, 1.0f};
    case 4: return {rising, low, 1.0f};
    default: return {1.0f, low, falling};
    }
}
} // namespace

LightingShowcaseSceneSummary LightingShowcaseSceneFactory::Populate(
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
            LightingRoot,
            "LightingLab",
            18,
            36);
    const DemoMaterialResources floor =
        DemoSceneBuilder::CreateMaterial(
            assetRegistry,
            device,
            LightingRoot,
            {"Floor", {0.26f, 0.29f, 0.34f, 1.0f}, 0.0f, 0.72f});
    const DemoMaterialResources subjectMaterial =
        DemoSceneBuilder::CreateMaterial(
            assetRegistry,
            device,
            LightingRoot,
            {"LightingSubject", {0.68f, 0.70f, 0.74f, 1.0f}, 0.12f, 0.36f});
    const DemoMaterialResources wall =
        DemoSceneBuilder::CreateMaterial(
            assetRegistry,
            device,
            LightingRoot,
            {"Wall", {0.11f, 0.13f, 0.18f, 1.0f}, 0.20f, 0.52f});
    const DemoMaterialResources emitter =
        DemoSceneBuilder::CreateMaterial(
            assetRegistry,
            device,
            LightingRoot,
            {"Emitter", {1.0f, 1.0f, 1.0f, 1.0f}, 0.0f, 0.20f,
             {1.0f, 1.0f, 1.0f}, 6.0f});

    DemoSceneBuilder::AddObject(
        scene,
        "LightingLab_Floor",
        meshes.cubeHandle,
        meshes.cube,
        floor,
        {0.0f, -0.25f, 14.0f},
        {20.0f, 0.25f, 24.0f});
    DemoSceneBuilder::AddObject(
        scene,
        "LightingLab_BackWall",
        meshes.cubeHandle,
        meshes.cube,
        wall,
        {0.0f, 5.0f, 38.0f},
        {20.0f, 5.0f, 0.30f});
    DemoSceneBuilder::AddObject(
        scene,
        "LightingLab_LeftWall",
        meshes.cubeHandle,
        meshes.cube,
        wall,
        {-20.0f, 5.0f, 14.0f},
        {0.30f, 5.0f, 24.0f});
    DemoSceneBuilder::AddObject(
        scene,
        "LightingLab_RightWall",
        meshes.cubeHandle,
        meshes.cube,
        wall,
        {20.0f, 5.0f, 14.0f},
        {0.30f, 5.0f, 24.0f});

    for (std::uint32_t row = 0; row < SubjectRows; ++row)
    {
        for (std::uint32_t column = 0;
             column < SubjectColumns;
             ++column)
        {
            const std::uint32_t index = row * SubjectColumns + column;
            const bool sphere = (row + column) % 2 == 0;
            RenderObject& subject = DemoSceneBuilder::AddObject(
                scene,
                "LightingSubject_" + std::to_string(row)
                    + '_' + std::to_string(column),
                sphere ? meshes.sphereHandle : meshes.cubeHandle,
                sphere ? meshes.sphere : meshes.cube,
                subjectMaterial,
                {-14.0f + static_cast<float>(column) * 4.0f,
                 0.72f + static_cast<float>(index % 3) * 0.18f,
                 -1.0f + static_cast<float>(row) * 6.0f},
                sphere
                    ? XMFLOAT3{0.72f, 0.72f, 0.72f}
                    : XMFLOAT3{0.66f, 0.66f, 0.66f},
                {0.0f, 0.22f * static_cast<float>(index), 0.0f});
            subject.hasMaterialOverride = true;
            subject.materialOverride.albedoColor = {
                0.34f + 0.07f * static_cast<float>(column),
                0.30f + 0.08f * static_cast<float>(row),
                0.70f - 0.05f * static_cast<float>(row),
                1.0f};
            subject.materialOverride.metallic =
                static_cast<float>(row) /
                static_cast<float>(SubjectRows - 1);
            subject.materialOverride.roughness =
                0.08f + 0.11f * static_cast<float>(column);
            subject.materialOverride.useAlbedoTexture = false;
            subject.materialOverride.useMetallicRoughnessTexture = false;
            subject.materialOverride.useNormalTexture = false;
            subject.materialOverride.useOcclusionTexture = false;
            subject.materialOverride.useEmissiveTexture = false;
        }
    }

    for (std::uint32_t row = 0; row < LightRows; ++row)
    {
        for (std::uint32_t column = 0;
             column < LightColumns;
             ++column)
        {
            const std::uint32_t index = row * LightColumns + column;
            PointLight& light = scene.GetPointLights()[index];
            light.position = {
                -14.0f + static_cast<float>(column) * 4.0f,
                3.0f + static_cast<float>((row + column) % 3) * 0.65f,
                2.0f + static_cast<float>(row) * 9.0f};
            light.color = HsvToRgb(
                static_cast<float>(index) /
                static_cast<float>(LightCount));
            light.range = 8.5f;
            light.intensity = 4.8f;
            light.castsShadow = index < 4;

            RenderObject& marker = DemoSceneBuilder::AddObject(
                scene,
                "PointLightMarker_" + std::to_string(index),
                meshes.sphereHandle,
                meshes.sphere,
                emitter,
                light.position,
                {0.15f, 0.15f, 0.15f});
            marker.hasMaterialOverride = true;
            marker.materialOverride.albedoColor = {
                light.color.x,
                light.color.y,
                light.color.z,
                1.0f};
            marker.materialOverride.emissiveColor = light.color;
            marker.materialOverride.emissiveStrength = 7.0f;
            marker.materialOverride.roughness = 0.2f;
            marker.materialOverride.useAlbedoTexture = false;
            marker.materialOverride.useMetallicRoughnessTexture = false;
            marker.materialOverride.useNormalTexture = false;
            marker.materialOverride.useOcclusionTexture = false;
            marker.materialOverride.useEmissiveTexture = false;
        }
    }
    scene.SetActivePointLightCount(LightCount);
    scene.SetActiveSpotLightCount(0);

    return {
        static_cast<std::uint32_t>(scene.GetRenderObjects().size()),
        SubjectCount,
        LightCount};
}

void LightingShowcaseSceneFactory::ConfigureWorld(
    RenderScene& scene,
    const float cameraAspectRatio)
{
    Camera& camera = scene.GetCamera();
    camera.SetPerspective(
        XMConvertToRadians(49.0f),
        cameraAspectRatio,
        0.1f,
        160.0f);
    camera.SetLookAt(
        {23.0f, 15.0f, -25.0f},
        {0.0f, 2.0f, 14.0f},
        {0.0f, 1.0f, 0.0f});
    scene.GetGameCamera() = camera;

    DirectionalLight& sun = scene.GetDirectionalLight();
    sun.direction = {-0.36f, -0.80f, 0.28f};
    sun.color = {0.72f, 0.78f, 1.0f};
    sun.intensity = 0.18f;
}
} // namespace Prism::Scene
