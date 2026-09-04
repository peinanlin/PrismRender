#include "Scene/FeatureLabSceneFactory.h"

#include "Scene/DemoSceneBuilder.h"
#include "Scene/RenderObject.h"
#include "Scene/RenderScene.h"

#include <DirectXMath.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <string>

namespace Prism::Scene
{
using namespace DirectX;

namespace
{
XMFLOAT3 ColorWheel(const float value)
{
    const float angle = value * XM_2PI;
    return {
        0.55f + 0.45f * std::sin(angle),
        0.55f + 0.45f * std::sin(angle + 2.094f),
        0.55f + 0.45f * std::sin(angle + 4.188f)};
}

void ApplyOverride(
    RenderObject& object,
    const XMFLOAT4& albedo,
    const float metallic,
    const float roughness,
    const XMFLOAT3& emissive = {},
    const float emissiveStrength = 1.0f)
{
    object.hasMaterialOverride = true;
    object.materialOverride.albedoColor = albedo;
    object.materialOverride.metallic = metallic;
    object.materialOverride.roughness = roughness;
    object.materialOverride.emissiveColor = emissive;
    object.materialOverride.emissiveStrength = emissiveStrength;
    object.materialOverride.useAlbedoTexture = false;
    object.materialOverride.useMetallicRoughnessTexture = false;
    object.materialOverride.useNormalTexture = false;
    object.materialOverride.useOcclusionTexture = false;
    object.materialOverride.useEmissiveTexture = false;
}

FeatureLabSceneSummary PopulateGpuDriven(
    Asset::AssetRegistry& assetRegistry,
    RHI::IGraphicsDevice& device,
    RenderScene& scene)
{
    constexpr const char* Root = "builtin://gpu-driven-lab/";
    const DemoMeshResources meshes = DemoSceneBuilder::CreateMeshes(
        assetRegistry, device, Root, "GpuDriven", 12, 24);
    const DemoMaterialResources floor = DemoSceneBuilder::CreateMaterial(
        assetRegistry, device, Root,
        {"Floor", {0.25f, 0.28f, 0.33f, 1.0f}, 0.0f, 0.72f});
    const DemoMaterialResources instance = DemoSceneBuilder::CreateMaterial(
        assetRegistry, device, Root,
        {"Instance", {0.24f, 0.48f, 0.92f, 1.0f}, 0.35f, 0.30f});
    const DemoMaterialResources occluder = DemoSceneBuilder::CreateMaterial(
        assetRegistry, device, Root,
        {"Occluder", {0.035f, 0.04f, 0.055f, 1.0f}, 0.1f, 0.72f});

    DemoSceneBuilder::AddObject(
        scene, "GpuStress_Floor", meshes.cubeHandle, meshes.cube,
        floor, {0.0f, -0.25f, 22.0f}, {24.0f, 0.25f, 34.0f});
    DemoSceneBuilder::AddObject(
        scene, "GpuStress_OccluderA", meshes.cubeHandle, meshes.cube,
        occluder, {-7.0f, 3.0f, 8.0f}, {5.8f, 3.0f, 0.55f});
    DemoSceneBuilder::AddObject(
        scene, "GpuStress_OccluderB", meshes.cubeHandle, meshes.cube,
        occluder, {8.0f, 2.4f, 22.0f}, {6.0f, 2.4f, 0.55f});

    constexpr std::uint32_t Rows = 10;
    constexpr std::uint32_t Columns = 12;
    for (std::uint32_t row = 0; row < Rows; ++row)
    {
        for (std::uint32_t column = 0; column < Columns; ++column)
        {
            const std::uint32_t index = row * Columns + column;
            RenderObject& object = DemoSceneBuilder::AddObject(
                scene,
                "GpuInstance_" + std::to_string(index),
                meshes.cubeHandle,
                meshes.cube,
                instance,
                {-16.5f + column * 3.0f,
                 0.55f + 0.18f * static_cast<float>(index % 4u),
                 -2.0f + row * 5.0f},
                {0.52f, 0.52f + 0.18f * static_cast<float>(index % 4u), 0.52f},
                {0.0f, 0.13f * static_cast<float>(index), 0.0f});
            ApplyOverride(
                object,
                {0.14f + 0.05f * static_cast<float>(column),
                 0.22f + 0.045f * static_cast<float>(row),
                 0.82f,
                 1.0f},
                0.25f,
                0.24f + 0.04f * static_cast<float>(index % 5u));
        }
    }

    scene.SetActivePointLightCount(4);
    for (std::uint32_t index = 0; index < 4; ++index)
    {
        PointLight& light = scene.GetPointLights()[index];
        light.position = {
            index % 2u == 0u ? -10.0f : 10.0f,
            5.0f,
            5.0f + static_cast<float>(index / 2u) * 28.0f};
        light.color = ColorWheel(static_cast<float>(index) / 4.0f);
        light.intensity = 5.0f;
        light.range = 18.0f;
        light.castsShadow = false;
    }
    return {
        static_cast<std::uint32_t>(scene.GetRenderObjects().size()),
        Rows * Columns,
        4u};
}

FeatureLabSceneSummary PopulatePostProcess(
    Asset::AssetRegistry& assetRegistry,
    RHI::IGraphicsDevice& device,
    RenderScene& scene,
    const bool renderGraphVariant)
{
    const std::string root = renderGraphVariant
        ? "builtin://render-graph-lab/"
        : "builtin://post-process-lab/";
    const DemoMeshResources meshes = DemoSceneBuilder::CreateMeshes(
        assetRegistry, device, root,
        renderGraphVariant ? "RenderGraph" : "PostProcess", 18, 36);
    const DemoMaterialResources floor = DemoSceneBuilder::CreateMaterial(
        assetRegistry, device, root,
        {"Floor", {0.24f, 0.27f, 0.32f, 1.0f}, 0.0f, 0.74f});
    const DemoMaterialResources dark = DemoSceneBuilder::CreateMaterial(
        assetRegistry, device, root,
        {"Dark", {0.025f, 0.03f, 0.045f, 1.0f}, 0.15f, 0.62f});
    const DemoMaterialResources neutral = DemoSceneBuilder::CreateMaterial(
        assetRegistry, device, root,
        {"Neutral", {0.38f, 0.42f, 0.48f, 1.0f}, 0.15f, 0.36f});
    const DemoMaterialResources emitter = DemoSceneBuilder::CreateMaterial(
        assetRegistry, device, root,
        {"Emitter", {1.0f, 1.0f, 1.0f, 1.0f}, 0.0f, 0.18f,
         {1.0f, 1.0f, 1.0f}, 8.0f});

    DemoSceneBuilder::AddObject(
        scene, "Post_Floor", meshes.cubeHandle, meshes.cube,
        floor, {0.0f, -0.25f, 10.0f}, {15.0f, 0.25f, 18.0f});
    DemoSceneBuilder::AddObject(
        scene, "Post_BackWall", meshes.cubeHandle, meshes.cube,
        dark, {0.0f, 5.5f, 27.5f}, {15.0f, 5.5f, 0.25f});
    DemoSceneBuilder::AddObject(
        scene, "Post_Center", meshes.sphereHandle, meshes.sphere,
        neutral, {0.0f, 2.4f, 8.0f}, {2.0f, 2.0f, 2.0f});

    constexpr std::uint32_t EmitterCount = 14;
    for (std::uint32_t index = 0; index < EmitterCount; ++index)
    {
        const float side = index % 2u == 0u ? -1.0f : 1.0f;
        const float z = -1.0f + static_cast<float>(index / 2u) * 4.0f;
        const XMFLOAT3 color = ColorWheel(
            static_cast<float>(index) / static_cast<float>(EmitterCount));
        RenderObject& orb = DemoSceneBuilder::AddObject(
            scene,
            "Post_Emitter_" + std::to_string(index),
            meshes.sphereHandle,
            meshes.sphere,
            emitter,
            {side * (4.0f + 0.4f * static_cast<float>(index % 3u)),
             1.1f + 0.55f * static_cast<float>(index % 3u),
             z},
            {0.28f, 0.28f, 0.28f});
        ApplyOverride(
            orb,
            {color.x, color.y, color.z, 1.0f},
            0.0f,
            0.16f,
            color,
            2.0f + static_cast<float>(index) * 0.85f);

        if (index < 8u)
        {
            PointLight& light = scene.GetPointLights()[index];
            light.position = orb.transform.GetPosition();
            light.position.y += 0.35f;
            light.color = color;
            light.intensity = 3.8f;
            light.range = 7.5f;
            light.castsShadow = renderGraphVariant && index < 2u;
        }
    }
    scene.SetActivePointLightCount(8);
    return {
        static_cast<std::uint32_t>(scene.GetRenderObjects().size()),
        EmitterCount,
        8u};
}

FeatureLabSceneSummary PopulateMaterial(
    Asset::AssetRegistry& assetRegistry,
    RHI::IGraphicsDevice& device,
    RenderScene& scene)
{
    constexpr const char* Root = "builtin://material-lab/";
    const DemoMeshResources meshes = DemoSceneBuilder::CreateMeshes(
        assetRegistry, device, Root, "Material", 24, 48);
    const DemoMaterialResources floor = DemoSceneBuilder::CreateMaterial(
        assetRegistry, device, Root,
        {"Floor", {0.25f, 0.27f, 0.31f, 1.0f}, 0.0f, 0.72f});
    const DemoMaterialResources sample = DemoSceneBuilder::CreateMaterial(
        assetRegistry, device, Root,
        {"Sample", {0.72f, 0.36f, 0.12f, 1.0f}, 0.0f, 0.5f});
    DemoSceneBuilder::AddObject(
        scene, "Material_Floor", meshes.cubeHandle, meshes.cube,
        floor, {0.0f, -0.25f, 8.0f}, {14.0f, 0.25f, 14.0f});

    constexpr std::uint32_t Rows = 5;
    constexpr std::uint32_t Columns = 5;
    for (std::uint32_t row = 0; row < Rows; ++row)
    {
        for (std::uint32_t column = 0; column < Columns; ++column)
        {
            RenderObject& sphere = DemoSceneBuilder::AddObject(
                scene,
                "Material_M" + std::to_string(row)
                    + "_R" + std::to_string(column),
                meshes.sphereHandle,
                meshes.sphere,
                sample,
                {-6.0f + column * 3.0f,
                 1.0f,
                 2.0f + row * 3.0f},
                {0.82f, 0.82f, 0.82f});
            const XMFLOAT3 tint = ColorWheel(
                static_cast<float>(row * Columns + column)
                / static_cast<float>(Rows * Columns));
            ApplyOverride(
                sphere,
                {0.25f + tint.x * 0.65f,
                 0.25f + tint.y * 0.65f,
                 0.25f + tint.z * 0.65f,
                 1.0f},
                static_cast<float>(row) / static_cast<float>(Rows - 1u),
                0.08f + 0.22f * static_cast<float>(column));
        }
    }
    scene.SetActivePointLightCount(3);
    const XMFLOAT3 lightPositions[3] = {
        {-8.0f, 6.0f, 1.0f},
        {8.0f, 7.0f, 8.0f},
        {0.0f, 5.0f, 18.0f}};
    for (std::uint32_t index = 0; index < 3; ++index)
    {
        PointLight& light = scene.GetPointLights()[index];
        light.position = lightPositions[index];
        light.color = index == 0u
            ? XMFLOAT3{1.0f, 0.55f, 0.28f}
            : (index == 1u
                ? XMFLOAT3{0.28f, 0.48f, 1.0f}
                : XMFLOAT3{0.45f, 1.0f, 0.62f});
        light.intensity = 5.0f;
        light.range = 17.0f;
        light.castsShadow = index == 0u;
    }
    return {
        static_cast<std::uint32_t>(scene.GetRenderObjects().size()),
        Rows * Columns,
        3u};
}

FeatureLabSceneSummary PopulateStreaming(
    Asset::AssetRegistry& assetRegistry,
    RHI::IGraphicsDevice& device,
    RenderScene& scene)
{
    constexpr const char* Root = "builtin://asset-streaming-lab/";
    const DemoMeshResources meshes = DemoSceneBuilder::CreateMeshes(
        assetRegistry, device, Root, "Streaming", 16, 32);
    const DemoMaterialResources floor = DemoSceneBuilder::CreateMaterial(
        assetRegistry, device, Root,
        {"Floor", {0.23f, 0.26f, 0.32f, 1.0f}, 0.0f, 0.74f});
    const DemoMaterialResources tile = DemoSceneBuilder::CreateMaterial(
        assetRegistry, device, Root,
        {"ResidencyTile", {0.18f, 0.52f, 0.88f, 1.0f}, 0.2f, 0.32f});
    DemoSceneBuilder::AddObject(
        scene, "Streaming_Floor", meshes.cubeHandle, meshes.cube,
        floor, {0.0f, -0.25f, 8.0f}, {13.0f, 0.25f, 14.0f});
    constexpr std::uint32_t TileCount = 18;
    for (std::uint32_t index = 0; index < TileCount; ++index)
    {
        const std::uint32_t row = index / 6u;
        const std::uint32_t column = index % 6u;
        RenderObject& object = DemoSceneBuilder::AddObject(
            scene,
            "Streaming_Residency_" + std::to_string(index),
            index % 2u == 0u ? meshes.cubeHandle : meshes.sphereHandle,
            index % 2u == 0u ? meshes.cube : meshes.sphere,
            tile,
            {-7.5f + column * 3.0f,
             0.6f + 0.35f * static_cast<float>(index % 3u),
             2.0f + row * 5.0f},
            {0.62f, 0.62f, 0.62f});
        const XMFLOAT3 color = ColorWheel(
            static_cast<float>(index) / static_cast<float>(TileCount));
        ApplyOverride(
            object,
            {color.x, color.y, color.z, 1.0f},
            0.15f + 0.1f * static_cast<float>(row),
            0.24f + 0.08f * static_cast<float>(column));
    }
    scene.SetActivePointLightCount(2);
    scene.GetPointLights()[0] = {
        {-7.0f, 5.0f, 1.0f}, 16.0f,
        {1.0f, 0.48f, 0.20f}, 4.5f, false};
    scene.GetPointLights()[1] = {
        {7.0f, 5.0f, 14.0f}, 16.0f,
        {0.20f, 0.48f, 1.0f}, 4.5f, false};
    return {
        static_cast<std::uint32_t>(scene.GetRenderObjects().size()),
        TileCount,
        2u};
}

FeatureLabSceneSummary PopulateAtmosphere(
    Asset::AssetRegistry& assetRegistry,
    RHI::IGraphicsDevice& device,
    RenderScene& scene)
{
    constexpr const char* Root = "builtin://atmosphere-lab/";
    const DemoMeshResources meshes =
        DemoSceneBuilder::CreateMeshes(
            assetRegistry,
            device,
            Root,
            "Atmosphere",
            24,
            48);
    const DemoMaterialResources ground =
        DemoSceneBuilder::CreateMaterial(
            assetRegistry,
            device,
            Root,
            {"Ground", {0.26f, 0.22f, 0.18f, 1.0f}, 0.0f, 0.88f});
    const DemoMaterialResources stone =
        DemoSceneBuilder::CreateMaterial(
            assetRegistry,
            device,
            Root,
            {"WarmStone", {0.38f, 0.23f, 0.12f, 1.0f}, 0.05f, 0.62f});
    const DemoMaterialResources metal =
        DemoSceneBuilder::CreateMaterial(
            assetRegistry,
            device,
            Root,
            {"SunMetal", {0.72f, 0.45f, 0.16f, 1.0f}, 0.86f, 0.24f});
    const DemoMaterialResources silhouette =
        DemoSceneBuilder::CreateMaterial(
            assetRegistry,
            device,
            Root,
            {"Silhouette", {0.018f, 0.020f, 0.024f, 1.0f}, 0.1f, 0.78f});

    DemoSceneBuilder::AddObject(
        scene,
        "Atmosphere_Ground",
        meshes.cubeHandle,
        meshes.cube,
        ground,
        {0.0f, -0.25f, 38.0f},
        {52.0f, 0.25f, 72.0f});
    DemoSceneBuilder::AddObject(
        scene,
        "Atmosphere_ObservatoryBase",
        meshes.cubeHandle,
        meshes.cube,
        stone,
        {0.0f, 0.35f, 15.0f},
        {7.5f, 0.35f, 7.5f});
    DemoSceneBuilder::AddObject(
        scene,
        "Atmosphere_ObservatoryStep",
        meshes.cubeHandle,
        meshes.cube,
        stone,
        {0.0f, 0.82f, 15.0f},
        {5.6f, 0.14f, 5.6f});
    DemoSceneBuilder::AddObject(
        scene,
        "Atmosphere_SunMarker",
        meshes.sphereHandle,
        meshes.sphere,
        metal,
        {0.0f, 2.65f, 15.0f},
        {1.55f, 1.55f, 1.55f});

    constexpr std::array<float, 4> ObeliskX{
        -22.0f,
        -10.0f,
        11.0f,
        24.0f};
    for (std::uint32_t index = 0;
         index < static_cast<std::uint32_t>(
             ObeliskX.size());
         ++index)
    {
        const float height =
            3.8f + static_cast<float>(index % 3u) * 1.4f;
        DemoSceneBuilder::AddObject(
            scene,
            "Atmosphere_Obelisk_"
                + std::to_string(index),
            meshes.cubeHandle,
            meshes.cube,
            silhouette,
            {ObeliskX[index], height, 39.0f
                + static_cast<float>(index) * 5.0f},
            {0.75f, height, 0.75f},
            {0.0f,
             0.12f * static_cast<float>(index),
             0.0f});
    }

    DemoSceneBuilder::AddObject(
        scene,
        "Atmosphere_ArchLeft",
        meshes.cubeHandle,
        meshes.cube,
        silhouette,
        {-7.0f, 4.5f, 55.0f},
        {0.65f, 4.5f, 0.65f});
    DemoSceneBuilder::AddObject(
        scene,
        "Atmosphere_ArchRight",
        meshes.cubeHandle,
        meshes.cube,
        silhouette,
        {7.0f, 4.5f, 55.0f},
        {0.65f, 4.5f, 0.65f});
    DemoSceneBuilder::AddObject(
        scene,
        "Atmosphere_ArchBeam",
        meshes.cubeHandle,
        meshes.cube,
        silhouette,
        {0.0f, 8.7f, 55.0f},
        {7.65f, 0.55f, 0.65f});

    constexpr std::uint32_t HorizonMarkerCount = 9;
    for (std::uint32_t index = 0;
         index < HorizonMarkerCount;
         ++index)
    {
        const float normalized =
            static_cast<float>(index)
            / static_cast<float>(HorizonMarkerCount - 1u);
        const float height =
            0.8f + 1.6f
                * std::sin(normalized * XM_PI);
        DemoSceneBuilder::AddObject(
            scene,
            "Atmosphere_Horizon_"
                + std::to_string(index),
            meshes.cubeHandle,
            meshes.cube,
            silhouette,
            {-44.0f + normalized * 88.0f,
             height,
             92.0f},
            {5.8f, height, 2.4f});
    }

    scene.SetActivePointLightCount(0);
    scene.SetActiveSpotLightCount(0);
    return {
        static_cast<std::uint32_t>(
            scene.GetRenderObjects().size()),
        19u,
        0u};
}

FeatureLabSceneSummary PopulateLargeWorld(
    Asset::AssetRegistry& assetRegistry,
    RHI::IGraphicsDevice& device,
    RenderScene& scene)
{
    constexpr const char* Root = "builtin://large-world-lab/";
    constexpr Core::Double3 WorldAnchor{
        6'378'137.125,
        1'000'000.0625,
        -4'200'000.25};
    const DemoMeshResources meshes = DemoSceneBuilder::CreateMeshes(
        assetRegistry, device, Root, "LargeWorld", 24, 48);
    const DemoMaterialResources floor = DemoSceneBuilder::CreateMaterial(
        assetRegistry, device, Root,
        {"SurveyFloor", {0.19f, 0.24f, 0.31f, 1.0f}, 0.0f, 0.78f});
    const DemoMaterialResources marker = DemoSceneBuilder::CreateMaterial(
        assetRegistry, device, Root,
        {"PrecisionMarker", {0.12f, 0.62f, 1.0f, 1.0f}, 0.72f, 0.18f});
    const DemoMaterialResources warm = DemoSceneBuilder::CreateMaterial(
        assetRegistry, device, Root,
        {"WarmReference", {0.95f, 0.30f, 0.08f, 1.0f}, 0.18f, 0.30f});

    DemoSceneBuilder::AddObject(
        scene, "RTE_SurveyFloor", meshes.cubeHandle, meshes.cube,
        floor, {0.125f, -0.1875f, 8.25f}, {18.0f, 0.1875f, 20.0f});
    DemoSceneBuilder::AddObject(
        scene, "RTE_CenterPlinth", meshes.cubeHandle, meshes.cube,
        floor, {0.125f, 0.3125f, 8.25f}, {2.25f, 0.3125f, 2.25f});

    // At this absolute X coordinate a 32-bit float has roughly half-metre
    // spacing. These 0.375 m calibration markers collapse or jump if their
    // absolute positions are converted to float before origin subtraction.
    constexpr std::array<float, 5> CalibrationOffsets{
        -0.75f, -0.375f, 0.0f, 0.375f, 0.75f};
    for (std::uint32_t index = 0;
         index < CalibrationOffsets.size();
         ++index)
    {
        RenderObject& bead = DemoSceneBuilder::AddObject(
            scene,
            "RTE_SubMeterMarker_" + std::to_string(index),
            meshes.sphereHandle,
            meshes.sphere,
            marker,
            {0.125f + CalibrationOffsets[index], 1.10f, 8.25f},
            {0.13f, 0.13f, 0.13f});
        const XMFLOAT3 tint = ColorWheel(
            static_cast<float>(index)
            / static_cast<float>(CalibrationOffsets.size()));
        ApplyOverride(
            bead,
            {tint.x, tint.y, tint.z, 1.0f},
            0.65f,
            0.16f);
    }

    constexpr std::array<XMFLOAT3, 4> PillarPositions{{
        {-8.375f, 2.0f, 2.125f},
        {8.625f, 2.0f, 2.375f},
        {-8.125f, 2.0f, 15.875f},
        {8.375f, 2.0f, 16.125f}}};
    for (std::uint32_t index = 0;
         index < PillarPositions.size();
         ++index)
    {
        DemoSceneBuilder::AddObject(
            scene,
            "RTE_ReferencePillar_" + std::to_string(index),
            meshes.cubeHandle,
            meshes.cube,
            warm,
            PillarPositions[index],
            {0.45f, 2.0f, 0.45f});
    }

    for (RenderObject& object :
         scene.EditRenderObjectsForFullRebuild())
    {
        const Core::Double3 local = object.transform.GetWorldPosition();
        object.transform.SetWorldPosition(WorldAnchor + local);
    }
    scene.SetActivePointLightCount(0);
    scene.SetActiveSpotLightCount(0);
    return {
        static_cast<std::uint32_t>(scene.GetRenderObjects().size()),
        static_cast<std::uint32_t>(CalibrationOffsets.size()),
        0u};
}
} // namespace

FeatureLabSceneSummary FeatureLabSceneFactory::Populate(
    const FeatureLabSceneKind kind,
    Asset::AssetRegistry& assetRegistry,
    RHI::IGraphicsDevice& device,
    RenderScene& scene)
{
    scene.ClearRenderObjects();
    DemoSceneBuilder::ResetLights(scene);
    switch (kind)
    {
    case FeatureLabSceneKind::GpuDriven:
        return PopulateGpuDriven(assetRegistry, device, scene);
    case FeatureLabSceneKind::PostProcess:
        return PopulatePostProcess(assetRegistry, device, scene, false);
    case FeatureLabSceneKind::RenderGraph:
        return PopulatePostProcess(assetRegistry, device, scene, true);
    case FeatureLabSceneKind::Material:
        return PopulateMaterial(assetRegistry, device, scene);
    case FeatureLabSceneKind::AssetStreaming:
        return PopulateStreaming(assetRegistry, device, scene);
    case FeatureLabSceneKind::Atmosphere:
        return PopulateAtmosphere(assetRegistry, device, scene);
    case FeatureLabSceneKind::LargeWorld:
        return PopulateLargeWorld(assetRegistry, device, scene);
    }
    return {};
}

void FeatureLabSceneFactory::ConfigureWorld(
    const FeatureLabSceneKind kind,
    RenderScene& scene,
    const float cameraAspectRatio)
{
    Camera& camera = scene.GetCamera();
    camera.SetPerspective(
        XMConvertToRadians(
            kind == FeatureLabSceneKind::GpuDriven
                ? 52.0f
                : (kind == FeatureLabSceneKind::Atmosphere
                    ? 58.0f
                    : 47.0f)),
        cameraAspectRatio,
        0.1f,
        kind == FeatureLabSceneKind::GpuDriven
            || kind == FeatureLabSceneKind::Atmosphere
            || kind == FeatureLabSceneKind::LargeWorld
        ? 220.0f
        : 140.0f);
    switch (kind)
    {
    case FeatureLabSceneKind::GpuDriven:
        camera.SetLookAt(
            {24.0f, 18.0f, -30.0f},
            {0.0f, 1.5f, 20.0f},
            {0.0f, 1.0f, 0.0f});
        break;
    case FeatureLabSceneKind::Material:
        camera.SetLookAt(
            {15.0f, 11.0f, -14.0f},
            {0.0f, 1.0f, 8.0f},
            {0.0f, 1.0f, 0.0f});
        break;
    case FeatureLabSceneKind::Atmosphere:
        camera.SetLookAt(
            {0.0f, 4.2f, -18.0f},
            {0.0f, 5.2f, 36.0f},
            {0.0f, 1.0f, 0.0f});
        break;
    case FeatureLabSceneKind::LargeWorld:
    {
        constexpr Core::Double3 WorldAnchor{
            6'378'137.125,
            1'000'000.0625,
            -4'200'000.25};
        camera.SetWorldLookAt(
            WorldAnchor + Core::Double3{-12.375, 8.125, -18.75},
            WorldAnchor + Core::Double3{0.125, 1.125, 8.25},
            {0.0f, 1.0f, 0.0f});
        break;
    }
    default:
        camera.SetLookAt(
            {13.0f, 8.5f, -13.0f},
            {0.0f, 1.8f, 8.0f},
            {0.0f, 1.0f, 0.0f});
        break;
    }
    scene.GetGameCamera() = camera;

    DirectionalLight& sun = scene.GetDirectionalLight();
    if (kind == FeatureLabSceneKind::Atmosphere)
    {
        // The renderer interprets the negated light direction as the vector
        // toward the sun. This places it just above the right-hand horizon.
        sun.direction = {-0.45f, -0.16f, -0.88f};
        sun.color = {1.0f, 0.78f, 0.56f};
        sun.intensity = 2.4f;
    }
    else
    {
        sun.direction = {-0.38f, -0.78f, 0.34f};
        sun.color = {0.92f, 0.94f, 1.0f};
        sun.intensity = kind == FeatureLabSceneKind::PostProcess
            || kind == FeatureLabSceneKind::RenderGraph
            ? 0.22f
            : 1.25f;
    }
}
} // namespace Prism::Scene
