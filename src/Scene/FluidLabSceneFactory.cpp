#include "Scene/FluidLabSceneFactory.h"

#include "Scene/DemoSceneBuilder.h"
#include "Scene/RenderScene.h"

#include <DirectXMath.h>

namespace Prism::Scene
{
using namespace DirectX;

namespace
{
struct FluidLabEnvironmentPreset
{
    XMFLOAT4 basinAlbedo{};
    float basinMetallic = 0.0f;
    float basinRoughness = 0.5f;
    XMFLOAT4 rimAlbedo{};
    float rimMetallic = 0.0f;
    float rimRoughness = 0.5f;
    XMFLOAT4 receiverAlbedo{};
    XMFLOAT4 receiverCheckerAlbedo{};
    float receiverRoughness = 0.8f;
    std::uint32_t receiverTileCount = 12u;
    float sunIntensity = 2.0f;
};

FluidLabEnvironmentPreset GetEnvironmentPreset(
    const FluidLabSceneKind kind)
{
    switch (kind)
    {
    case FluidLabSceneKind::Caustics:
        // Warm limestone keeps the RGB caustic dispersion visible without
        // turning the receiver into a self-luminous white card.
        return {
            {0.46f, 0.52f, 0.56f, 1.0f}, 0.06f, 0.44f,
            {0.70f, 0.74f, 0.72f, 1.0f}, 0.20f, 0.30f,
            {0.64f, 0.60f, 0.50f, 1.0f},
            {0.42f, 0.47f, 0.48f, 1.0f},
            0.90f, 18u, 2.25f};

    case FluidLabSceneKind::ToonFoam:
        // A cool pastel surround preserves the black silhouette and white
        // foam while remaining deliberately flatter than the realistic Lab.
        return {
            {0.56f, 0.64f, 0.76f, 1.0f}, 0.02f, 0.52f,
            {0.76f, 0.82f, 0.90f, 1.0f}, 0.06f, 0.38f,
            {0.57f, 0.67f, 0.80f, 1.0f},
            {0.38f, 0.49f, 0.66f, 1.0f},
            0.86f, 12u, 1.90f};

    case FluidLabSceneKind::PbfParticles:
        return {
            {0.38f, 0.45f, 0.52f, 1.0f}, 0.04f, 0.50f,
            {0.64f, 0.70f, 0.76f, 1.0f}, 0.12f, 0.34f,
            {0.48f, 0.53f, 0.58f, 1.0f},
            {0.32f, 0.38f, 0.43f, 1.0f},
            0.88f, 12u, 1.75f};

    case FluidLabSceneKind::ScreenSpace:
    default:
        // Muted sunlit tile gives transparent water both warm and cool
        // refraction cues. Neither checker cell approaches black.
        return {
            {0.42f, 0.50f, 0.57f, 1.0f}, 0.08f, 0.42f,
            {0.68f, 0.74f, 0.80f, 1.0f}, 0.24f, 0.27f,
            {0.58f, 0.45f, 0.32f, 1.0f},
            {0.35f, 0.42f, 0.44f, 1.0f},
            0.84f, 14u, 1.95f};
    }
}
} // namespace

FluidLabSceneSummary FluidLabSceneFactory::Populate(
    Asset::AssetRegistry& assetRegistry,
    RHI::IGraphicsDevice& device,
    RenderScene& scene,
    const FluidLabSceneKind kind)
{
    constexpr const char* Root = "builtin://fluid-lab/";
    const bool emphasizeReceiver =
        kind == FluidLabSceneKind::Caustics;
    const FluidLabEnvironmentPreset environment =
        GetEnvironmentPreset(kind);
    scene.ClearRenderObjects();
    DemoSceneBuilder::ResetLights(scene);

    const DemoMeshResources meshes = DemoSceneBuilder::CreateMeshes(
        assetRegistry,
        device,
        Root,
        "FluidLab",
        20u,
        40u);
    DemoMaterialDescription receiverDescription{
        "CausticReceiver",
        environment.receiverAlbedo,
        0.0f,
        environment.receiverRoughness};
    receiverDescription.checkerTileCount =
        environment.receiverTileCount;
    receiverDescription.checkerAlbedo =
        environment.receiverCheckerAlbedo;
    const DemoMaterialResources backdrop =
        DemoSceneBuilder::CreateMaterial(
            assetRegistry,
            device,
            Root,
            receiverDescription);

    // Keep a single compact ground receiver. The left, right, and back wall
    // meshes/rims remain removed; simulation-only boundaries coincide with
    // the receiver edge so the liquid cannot spread into an undersampled film.
    DemoSceneBuilder::AddObject(
        scene,
        "FluidLab_Ground",
        meshes.cubeHandle,
        meshes.cube,
        backdrop,
        {0.0f, -0.12f, 0.0f},
        {emphasizeReceiver ? 3.5f : 3.25f,
         0.12f,
         emphasizeReceiver ? 3.5f : 3.25f});

    DirectionalLight& sun = scene.GetDirectionalLight();
    sun.direction = {-0.38f, -0.82f, 0.42f};
    sun.color = {1.0f, 0.97f, 0.90f};
    sun.intensity = environment.sunIntensity;

    scene.SetActivePointLightCount(2u);
    PointLight& warm = scene.GetPointLights()[0];
    warm.position = {-3.8f, 4.8f, -2.8f};
    warm.color = {1.0f, 0.82f, 0.62f};
    warm.intensity = emphasizeReceiver ? 3.35f : 3.05f;
    warm.range = 10.0f;
    warm.castsShadow = false;
    PointLight& cool = scene.GetPointLights()[1];
    cool.position = {3.6f, 3.8f, 1.8f};
    cool.color = {0.48f, 0.68f, 1.0f};
    cool.intensity = emphasizeReceiver ? 2.85f : 2.55f;
    cool.range = 9.0f;
    cool.castsShadow = false;

    return {
        static_cast<std::uint32_t>(
            scene.GetRenderObjects().size()),
        1u};
}

void FluidLabSceneFactory::ConfigureWorld(
    RenderScene& scene,
    const float cameraAspectRatio,
    const FluidLabSceneKind kind)
{
    Camera& camera = scene.GetCamera();
    camera.SetPerspective(
        XMConvertToRadians(43.0f),
        cameraAspectRatio,
        0.05f,
        80.0f);
    if (kind == FluidLabSceneKind::Caustics)
    {
        camera.SetLookAt(
            {5.15f, 7.35f, -10.15f},
            {0.0f, 0.48f, 0.20f},
            {0.0f, 1.0f, 0.0f});
    }
    else
    {
        camera.SetLookAt(
            {5.8f, 3.35f, -11.2f},
            {0.0f, 2.55f, 0.10f},
            {0.0f, 1.0f, 0.0f});
    }
    scene.GetGameCamera() = camera;
}
} // namespace Prism::Scene
