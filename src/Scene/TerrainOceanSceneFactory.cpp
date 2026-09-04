#include "Scene/TerrainOceanSceneFactory.h"

#include "Asset/AssetRegistry.h"
#include "Asset/Mesh.h"
#include "Asset/MeshAsset.h"
#include "Core/Environment.h"
#include "RHI/IGraphicsDevice.h"
#include "Scene/DemoSceneBuilder.h"
#include "Scene/RenderObject.h"
#include "Scene/RenderScene.h"

#include <DirectXMath.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <functional>
#include <limits>
#include <iterator>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace Prism::Scene
{
using namespace DirectX;

namespace
{
constexpr float TerrainWorldSize = 4096.0f;
constexpr std::uint32_t TerrainRootTileCount = 4;
// 16 roots plus four children per root produce 80 GPU candidates. Multiple
// roots model a tiled large map while staying inside the descriptor budget.
constexpr std::uint32_t TerrainMaxLevel = 1;
// A 129x129 leaf patch samples the 512x512 shared heightfield at roughly one
// vertex per two texels. This avoids turning narrow erosion gullies into the
// isolated, many-metre spikes produced by the former 65x65 CPU sampling.
constexpr std::uint32_t PatchResolution = 129;
constexpr float TerrainMinimumHeight = -600.0f;
constexpr float TerrainMaximumHeight = 750.0f;

std::shared_ptr<Asset::MeshAsset> CreateTerrainPatchAsset(
    const float centerX,
    const float centerZ,
    const float halfExtent,
    const std::uint32_t level,
    float& outMinimumHeight,
    float& outMaximumHeight)
{
    std::vector<Asset::MeshVertex> vertices;
    std::vector<std::uint16_t> indices;
    vertices.reserve(PatchResolution * PatchResolution + PatchResolution * 4u);
    outMinimumHeight = TerrainMinimumHeight;
    outMaximumHeight = TerrainMaximumHeight;
    for (std::uint32_t z = 0; z < PatchResolution; ++z)
    {
        for (std::uint32_t x = 0; x < PatchResolution; ++x)
        {
            const float u = static_cast<float>(x)
                / static_cast<float>(PatchResolution - 1u);
            const float v = static_cast<float>(z)
                / static_cast<float>(PatchResolution - 1u);
            const float worldX = centerX + (u * 2.0f - 1.0f) * halfExtent;
            const float worldZ = centerZ + (v * 2.0f - 1.0f) * halfExtent;
            Asset::MeshVertex vertex{};
            // Height, slope and ridge values are supplied by the shared GPU
            // heightfield in Mesh.hlsl. Keep only a regular world-space grid
            // on the CPU so editing never requires rebuilding mesh buffers.
            vertex.position = {worldX, 0.0f, worldZ};
            vertex.normal = {0.0f, 1.0f, 0.0f};
            vertex.color = {0.5f, 0.0f, 1.0f, 1.0f};
            vertex.texCoord = {
                worldX / TerrainWorldSize + 0.5f,
                worldZ / TerrainWorldSize + 0.5f};
            vertex.tangent = {1.0f, 0.0f, 0.0f, 1.0f};
            vertices.push_back(vertex);
        }
    }
    for (std::uint32_t z = 0; z + 1u < PatchResolution; ++z)
    {
        for (std::uint32_t x = 0; x + 1u < PatchResolution; ++x)
        {
            const std::uint16_t first = static_cast<std::uint16_t>(
                z * PatchResolution + x);
            const std::uint16_t nextRow = static_cast<std::uint16_t>(
                first + PatchResolution);
            indices.insert(indices.end(), {
                first, nextRow, static_cast<std::uint16_t>(first + 1u),
                static_cast<std::uint16_t>(first + 1u), nextRow,
                static_cast<std::uint16_t>(nextRow + 1u)});
        }
    }

    const auto appendSkirt = [&vertices, &indices](
        const std::vector<std::uint16_t>& edge)
    {
        std::vector<std::uint16_t> skirt;
        skirt.reserve(edge.size());
        for (const std::uint16_t source : edge)
        {
            Asset::MeshVertex vertex = vertices[source];
            vertex.position.y -= 5.0f;
            skirt.push_back(static_cast<std::uint16_t>(vertices.size()));
            vertices.push_back(vertex);
        }
        for (std::size_t index = 0; index + 1u < edge.size(); ++index)
        {
            indices.insert(indices.end(), {
                edge[index], skirt[index], edge[index + 1u],
                edge[index + 1u], skirt[index], skirt[index + 1u]});
        }
    };
    std::vector<std::uint16_t> north;
    std::vector<std::uint16_t> south;
    std::vector<std::uint16_t> west;
    std::vector<std::uint16_t> east;
    for (std::uint32_t index = 0; index < PatchResolution; ++index)
    {
        north.push_back(static_cast<std::uint16_t>(index));
        south.push_back(static_cast<std::uint16_t>(
            (PatchResolution - 1u) * PatchResolution + index));
        west.push_back(static_cast<std::uint16_t>(index * PatchResolution));
        east.push_back(static_cast<std::uint16_t>(
            index * PatchResolution + PatchResolution - 1u));
    }
    appendSkirt(north);
    appendSkirt(south);
    appendSkirt(west);
    appendSkirt(east);

    auto asset = std::make_shared<Asset::MeshAsset>();
    asset->SetName("TerrainPatchL" + std::to_string(level));
    const float boundsRadius = std::sqrt(
        halfExtent * halfExtent * 2.0f
        + std::max(
            std::abs(outMinimumHeight),
            std::abs(outMaximumHeight))
            * std::max(
                std::abs(outMinimumHeight),
                std::abs(outMaximumHeight)));
    asset->SetGeometry(
        std::move(vertices),
        std::move(indices),
        {{centerX - halfExtent, outMinimumHeight - 5.0f, centerZ - halfExtent},
         {centerX + halfExtent, outMaximumHeight, centerZ + halfExtent},
         boundsRadius});
    return asset;
}

std::shared_ptr<Asset::MeshAsset> CreateOceanClipmapAsset(
    const float outerHalfExtent,
    const float innerHalfExtent,
    const float simulationPatchLength,
    const std::uint32_t resolution,
    const std::string& name)
{
    constexpr std::uint32_t MaximumResolution = 255;
    const std::uint32_t clampedResolution = std::clamp(
        resolution,
        3u,
        MaximumResolution);
    std::vector<Asset::MeshVertex> vertices;
    std::vector<std::uint16_t> indices;
    vertices.reserve(clampedResolution * clampedResolution);
    const float surfaceSize = outerHalfExtent * 2.0f;
    const float textureRepeat = surfaceSize
        / std::max(simulationPatchLength, 1.0f);
    for (std::uint32_t z = 0; z < clampedResolution; ++z)
    {
        for (std::uint32_t x = 0; x < clampedResolution; ++x)
        {
            const float u = static_cast<float>(x)
                / static_cast<float>(clampedResolution - 1u);
            const float v = static_cast<float>(z)
                / static_cast<float>(clampedResolution - 1u);
            Asset::MeshVertex vertex{};
            vertex.position = {
                (u * 2.0f - 1.0f) * outerHalfExtent,
                0.0f,
                (v * 2.0f - 1.0f) * outerHalfExtent};
            vertex.normal = {0.0f, 1.0f, 0.0f};
            vertex.color = {1.0f, 1.0f, 1.0f, 1.0f};
            vertex.texCoord = {
                u * textureRepeat,
                v * textureRepeat};
            vertex.tangent = {1.0f, 0.0f, 0.0f, 1.0f};
            vertices.push_back(vertex);
        }
    }
    for (std::uint32_t z = 0; z + 1u < clampedResolution; ++z)
    {
        for (std::uint32_t x = 0; x + 1u < clampedResolution; ++x)
        {
            const float centerU =
                (static_cast<float>(x) + 0.5f)
                / static_cast<float>(clampedResolution - 1u);
            const float centerV =
                (static_cast<float>(z) + 0.5f)
                / static_cast<float>(clampedResolution - 1u);
            const float cellCenterX =
                (centerU * 2.0f - 1.0f) * outerHalfExtent;
            const float cellCenterZ =
                (centerV * 2.0f - 1.0f) * outerHalfExtent;
            if (innerHalfExtent > 0.0f
                && std::abs(cellCenterX) < innerHalfExtent
                && std::abs(cellCenterZ) < innerHalfExtent)
            {
                continue;
            }
            const std::uint16_t first = static_cast<std::uint16_t>(
                z * clampedResolution + x);
            const std::uint16_t next = static_cast<std::uint16_t>(
                first + clampedResolution);
            indices.insert(indices.end(), {
                first, next, static_cast<std::uint16_t>(first + 1u),
                static_cast<std::uint16_t>(first + 1u), next,
                static_cast<std::uint16_t>(next + 1u)});
        }
    }
    auto asset = std::make_shared<Asset::MeshAsset>();
    asset->SetName(name);
    asset->SetGeometry(
        std::move(vertices), std::move(indices),
        {{-outerHalfExtent, -24.0f, -outerHalfExtent},
         {outerHalfExtent, 24.0f, outerHalfExtent},
         std::sqrt(
             outerHalfExtent * outerHalfExtent * 2.0f)});
    return asset;
}
} // namespace

TerrainOceanSceneSummary TerrainOceanSceneFactory::PopulateTerrain(
    Asset::AssetRegistry& assetRegistry,
    RHI::IGraphicsDevice& device,
    RenderScene& scene)
{
    constexpr const char* Root = "builtin://terrain-vt-lab/";
    scene.ClearRenderObjects();
    DemoSceneBuilder::ResetLights(scene);
    const DemoMaterialResources terrainMaterial =
        DemoSceneBuilder::CreateMaterial(
            assetRegistry,
            device,
            Root,
            {
                .name = "TerrainSurface",
                .albedo = {0.24f, 0.42f, 0.16f, 1.0f},
                .metallic = 0.02f,
                .roughness = 0.82f,
                // A displaced heightfield can expose triangles whose winding
                // faces away from a grazing camera. Keep terrain two-sided so
                // those facets and the patch skirts never become sky holes.
                .doubleSided = true});

    std::uint32_t patchCount = 0;
    const std::function<void(float, float, float, std::uint32_t, std::uint32_t, std::string)>
        addNode = [&](const float centerX,
                      const float centerZ,
                      const float halfExtent,
                      const std::uint32_t level,
                      const std::uint32_t parentObjectIndex,
                      std::string parentPatchName)
    {
        const std::uint32_t objectIndex =
            static_cast<std::uint32_t>(scene.GetRenderObjects().size());
        float minimumHeight = 0.0f;
        float maximumHeight = 0.0f;
        std::shared_ptr<Asset::MeshAsset> meshAsset =
            CreateTerrainPatchAsset(
                centerX,
                centerZ,
                halfExtent,
                level,
                minimumHeight,
                maximumHeight);
        const std::string suffix =
            "L" + std::to_string(level)
            + "_" + std::to_string(objectIndex);
        const Asset::MeshHandle meshHandle =
            assetRegistry.RegisterMeshAsset(
                std::string(Root) + "meshes/" + suffix,
                meshAsset);
        std::shared_ptr<Asset::Mesh> mesh =
            Asset::Mesh::CreateFromAsset(device, *meshAsset);
        assetRegistry.SetRuntimeMesh(meshHandle, mesh);
        const std::string objectName =
            "TerrainPatch_" + suffix;
        RenderObject& object = DemoSceneBuilder::AddObject(
            scene,
            objectName,
            meshHandle,
            mesh,
            terrainMaterial,
            {},
            {1.0f, 1.0f, 1.0f});
        object.surfaceType = RenderSurfaceType::VirtualTerrain;
        const float heightCenter =
            (minimumHeight + maximumHeight) * 0.5f;
        const float heightHalf =
            (maximumHeight - minimumHeight) * 0.5f + 5.0f;
        object.quadtreePatch = {
            true,
            parentObjectIndex,
            std::move(parentPatchName),
            level,
            TerrainMaxLevel,
            {centerX, heightCenter, centerZ},
            std::sqrt(
                halfExtent * halfExtent * 2.0f
                + heightHalf * heightHalf),
            halfExtent,
            145.0f};
        ++patchCount;

        if (level >= TerrainMaxLevel)
        {
            return;
        }
        const float childHalf = halfExtent * 0.5f;
        addNode(centerX - childHalf, centerZ - childHalf,
                childHalf, level + 1u, objectIndex, objectName);
        addNode(centerX + childHalf, centerZ - childHalf,
                childHalf, level + 1u, objectIndex, objectName);
        addNode(centerX - childHalf, centerZ + childHalf,
                childHalf, level + 1u, objectIndex, objectName);
        addNode(centerX + childHalf, centerZ + childHalf,
                childHalf, level + 1u, objectIndex, objectName);
    };
    const float rootTileSize = TerrainWorldSize
        / static_cast<float>(TerrainRootTileCount);
    const float rootHalfExtent = rootTileSize * 0.5f;
    const float mapMinimum = -TerrainWorldSize * 0.5f;
    for (std::uint32_t tileZ = 0;
         tileZ < TerrainRootTileCount;
         ++tileZ)
    {
        for (std::uint32_t tileX = 0;
             tileX < TerrainRootTileCount;
             ++tileX)
        {
            addNode(
                mapMinimum + rootHalfExtent
                    + static_cast<float>(tileX) * rootTileSize,
                mapMinimum + rootHalfExtent
                    + static_cast<float>(tileZ) * rootTileSize,
                rootHalfExtent,
                0u,
                GpuQuadtreePatch::InvalidParent,
                {});
        }
    }

    return {
        static_cast<std::uint32_t>(scene.GetRenderObjects().size()),
        patchCount,
        TerrainMaxLevel};
}

TerrainOceanSceneSummary TerrainOceanSceneFactory::PopulateOcean(
    Asset::AssetRegistry& assetRegistry,
    RHI::IGraphicsDevice& device,
    RenderScene& scene,
    const OceanLabSceneKind kind)
{
    const bool referenceGeometry =
        kind != OceanLabSceneKind::LegacyFft;
    const bool hpWaterReference =
        kind == OceanLabSceneKind::HpWaterReference;
    const char* const root = hpWaterReference
        ? "builtin://hpwater-ocean-lab/"
        : (referenceGeometry
            ? "builtin://waveworks-ocean-lab/"
            : "builtin://fft-ocean-lab/");
    const float oceanSimulationSize = referenceGeometry
        ? 1000.0f
        : 320.0f;
    const XMFLOAT4 surfaceColor = referenceGeometry
        ? XMFLOAT4{0.0f, 0.2f, 0.4f, 1.0f}
        : XMFLOAT4{0.004f, 0.018f, 0.028f, 1.0f};
    const XMFLOAT4 underlayColor = referenceGeometry
        ? XMFLOAT4{0.0f, 0.08f, 0.16f, 1.0f}
        : XMFLOAT4{0.008f, 0.035f, 0.050f, 1.0f};
    scene.ClearRenderObjects();
    DemoSceneBuilder::ResetLights(scene);
    const DemoMaterialResources oceanMaterial =
        DemoSceneBuilder::CreateMaterial(
            assetRegistry,
            device,
            root,
            {"OceanSurface",
             surfaceColor,
             0.0f,
             0.08f,
             {},
             1.0f,
             true});
    const DemoMaterialResources oceanUnderlayMaterial =
        DemoSceneBuilder::CreateMaterial(
            assetRegistry,
            device,
            root,
            {"OceanUnderlay", underlayColor, 0.0f, 0.90f});
    struct OceanRing
    {
        float outerHalfExtent;
        float innerHalfExtent;
        std::uint32_t meshResolution;
        const char* name;
    };
    // Exact 4:1 edge correspondence keeps displaced boundaries watertight:
    // inner 3.125 m cells -> middle 12.5 m -> outer 50 m.  The previous
    // 270/250 and 1050/1000 overlaps plus per-ring Y offsets produced visible
    // dark steps and double-rendered ridges in the reference camera.
    constexpr OceanRing Rings[] = {
        {250.0f, 0.0f, 161u, "Inner"},
        {1000.0f, 250.0f, 161u, "Middle"},
        {3200.0f, 1000.0f, 129u, "Outer"}};
    for (std::uint32_t ringIndex = 0;
         ringIndex < std::size(Rings);
         ++ringIndex)
    {
        const OceanRing& ring = Rings[ringIndex];
        const std::string ringName =
            std::string(hpWaterReference
                    ? "HpWaterOcean_"
                    : (referenceGeometry ? "WaveWorksOcean_" : "FftOcean_"))
                + ring.name + "Ring";
        std::shared_ptr<Asset::MeshAsset> oceanAsset =
            CreateOceanClipmapAsset(
                ring.outerHalfExtent,
                ring.innerHalfExtent,
                oceanSimulationSize,
                ring.meshResolution,
                ringName);
        const Asset::MeshHandle oceanHandle =
            assetRegistry.RegisterMeshAsset(
                std::string(root) + "meshes/"
                    + ring.name + "-ring",
                oceanAsset);
        std::shared_ptr<Asset::Mesh> oceanMesh =
            Asset::Mesh::CreateFromAsset(
                device,
                *oceanAsset);
        assetRegistry.SetRuntimeMesh(
            oceanHandle,
            oceanMesh);
        RenderObject& ocean = DemoSceneBuilder::AddObject(
            scene,
            ringName,
            oceanHandle,
            oceanMesh,
            oceanMaterial,
            {0.0f, 0.0f, 0.0f},
            {1.0f, 1.0f, 1.0f});
        ocean.surfaceType = RenderSurfaceType::FftOcean;
        ocean.surfaceLodLevel = ringIndex;
    }

    // A matte surface a few metres below the displaced FFT mesh models the
    // low-frequency body color of deep water. It prevents downward-facing wave
    // facets from exposing the clear/sky background while the FFT surface
    // itself remains responsible for highlights, displacement, normals and
    // foam.
    std::shared_ptr<Asset::MeshAsset> oceanUnderlayAsset =
        CreateOceanClipmapAsset(
            3200.0f,
            0.0f,
            oceanSimulationSize,
            129u,
            "OceanUnderlay");
    const Asset::MeshHandle oceanUnderlayHandle =
        assetRegistry.RegisterMeshAsset(
            std::string(root) + "meshes/underlay",
            oceanUnderlayAsset);
    std::shared_ptr<Asset::Mesh> oceanUnderlayMesh =
        Asset::Mesh::CreateFromAsset(
            device,
            *oceanUnderlayAsset);
    assetRegistry.SetRuntimeMesh(
        oceanUnderlayHandle,
        oceanUnderlayMesh);
    DemoSceneBuilder::AddObject(
        scene,
        "Ocean_Underlay",
        oceanUnderlayHandle,
        oceanUnderlayMesh,
        oceanUnderlayMaterial,
        {0.0f, -10.0f, 0.0f},
        {1.0f, 1.0f, 1.0f});

    // Opt-in equal-input fixtures make refraction, receiver caustics and
    // waterline edges observable without changing either Lab's default scene.
    const auto validationCamera = Core::ReadEnvironmentVariableValue("PRISM_RENDER_WATER_CAMERA");
    if (referenceGeometry && !validationCamera.empty() && validationCamera != "default")
    {
        // The diagnostic receivers must be closed from either viewing side,
        // including cameras below the surface. Keep two-sided index coverage
        // local to these fixtures; do not alter ordinary mesh PSOs or assets.
        const auto receiverAsset = Asset::MeshAsset::CreateCube();
        auto receiverIndices = receiverAsset->GetIndices();
        const auto indexCount = receiverIndices.size();
        for (std::size_t i = 0; i < indexCount; i += 3)
            receiverIndices.insert(receiverIndices.end(),
                {receiverIndices[i], receiverIndices[i + 2], receiverIndices[i + 1]});
        receiverAsset->SetGeometry(receiverAsset->GetVertices(), std::move(receiverIndices), receiverAsset->GetBounds());
        receiverAsset->SetName("WaterValidationReceiver");
        const auto receiverHandle = assetRegistry.RegisterMeshAsset(std::string(root) + "meshes/water-validation-receiver", receiverAsset);
        const auto receiverMesh = Asset::Mesh::CreateFromAsset(device, *receiverAsset);
        assetRegistry.SetRuntimeMesh(receiverHandle, receiverMesh);
        DemoMaterialDescription receiverDescription{};
        receiverDescription.name = "WaterValidationReceiver";
        receiverDescription.albedo = {0.65f, 0.58f, 0.39f, 1.0f};
        receiverDescription.roughness = 0.8f;
        receiverDescription.checkerTileCount = 8u;
        receiverDescription.checkerAlbedo = {0.15f, 0.23f, 0.25f, 1.0f};
        const auto receiver = DemoSceneBuilder::CreateMaterial(assetRegistry, device, root, receiverDescription);
        // Cube coordinates span [-1,1], so these scales are half-extents.
        DemoSceneBuilder::AddObject(scene, "WaterValidation_Floor", receiverHandle, receiverMesh,
            receiver, {-3.0f, -8.5f, 39.0f}, {25.0f, 0.5f, 20.0f});
        DemoSceneBuilder::AddObject(scene, "WaterValidation_Submerged", receiverHandle, receiverMesh,
            receiver, {-13.0f, -4.0f, 36.0f}, {1.5f, 2.5f, 1.5f});
        DemoSceneBuilder::AddObject(scene, "WaterValidation_Waterline", receiverHandle, receiverMesh,
            receiver, {-6.0f, -1.0f, 45.0f}, {1.5f, 6.0f, 1.5f});
    }

    return {
        static_cast<std::uint32_t>(scene.GetRenderObjects().size()),
        static_cast<std::uint32_t>(std::size(Rings)),
        0u};
}

void TerrainOceanSceneFactory::ConfigureTerrainWorld(
    RenderScene& scene,
    const float cameraAspectRatio)
{
    // Scene View camera: freely controlled by the editor.
    scene.GetCamera().SetPerspective(
        XMConvertToRadians(58.0f),
        cameraAspectRatio,
        0.5f,
        9000.0f);
    scene.GetCamera().SetLookAt(
        {-3350.0f, 2050.0f, -3450.0f},
        {0.0f, 80.0f, 0.0f},
        {0.0f, 1.0f, 0.0f});

    // Game/Final Output camera: also drives that view's GPU quadtree LOD and
    // frustum culling.
    scene.GetGameCamera().SetPerspective(
        XMConvertToRadians(38.0f),
        cameraAspectRatio,
        45.0f,
        3000.0f);
    scene.GetGameCamera().SetLookAt(
        {-1450.0f, 480.0f, -1550.0f},
        {320.0f, 100.0f, 260.0f},
        {0.0f, 1.0f, 0.0f});
    DirectionalLight& sun = scene.GetDirectionalLight();
    sun.direction = {-0.42f, -0.76f, 0.34f};
    sun.intensity = 2.25f;
    sun.color = {1.0f, 0.89f, 0.72f};
}

void TerrainOceanSceneFactory::ConfigureOceanWorld(
    RenderScene& scene,
    const float cameraAspectRatio,
    const OceanLabSceneKind kind)
{
    if (kind != OceanLabSceneKind::LegacyFft)
    {
        scene.GetCamera().SetPerspective(
            XMConvertToRadians(60.0f),
            cameraAspectRatio,
            0.1f,
            100000.0f);
        scene.GetCamera().SetLookAt(
            {-30.0f, 10.0f, 36.0625f},
            {0.0f, 5.0f, 39.0625f},
            {0.0f, 1.0f, 0.0f});
        // Shared named views keep WaveWorks/HPWater comparisons equal-input.
        const std::string validationCamera =
            Core::ReadEnvironmentVariableValue("PRISM_RENDER_WATER_CAMERA");
        if (validationCamera == "underwater")
            scene.GetCamera().SetLookAt({-30.0f, -3.0f, 36.0625f},
                {0.0f, -4.0f, 39.0625f}, {0.0f, 1.0f, 0.0f});
        else if (validationCamera == "waterline")
            scene.GetCamera().SetLookAt({-30.0f, -0.1f, 36.0625f},
                {0.0f, -0.1f, 39.0625f}, {0.0f, 1.0f, 0.0f});
        else if (validationCamera == "near" || validationCamera == "foam")
            scene.GetCamera().SetLookAt({-20.0f, 5.0f, 36.0625f},
                {0.0f, -3.0f, 39.0625f}, {0.0f, 1.0f, 0.0f});
        else if (validationCamera == "refraction")
            scene.GetCamera().SetLookAt({-22.0f, 3.0f, 28.0f},
                {-8.0f, -3.0f, 40.0f}, {0.0f, 1.0f, 0.0f});
        else if (validationCamera == "caustics")
            scene.GetCamera().SetLookAt({-19.0f, 10.0f, 31.0f},
                {-3.0f, -8.0f, 39.0f}, {0.0f, 1.0f, 0.0f});
        else if (validationCamera == "horizon")
            scene.GetCamera().SetLookAt({-30.0f, 10.0f, 36.0625f},
                {270.0f, 8.0f, 66.0625f}, {0.0f, 1.0f, 0.0f});
        scene.GetGameCamera() = scene.GetCamera();
        DirectionalLight& sun = scene.GetDirectionalLight();
        // WaveWorks exposes a 20-degree sun elevation. Prism stores the
        // direction of incoming rays in its XZ-horizontal/Y-up convention.
        // Keep the WaveWorks 20-degree elevation but align the sun azimuth
        // with the reference camera. This creates the characteristic central
        // reflection path instead of placing the disk at the upper-left.
        sun.direction = {-0.9350f, -0.34202f, -0.0935f};
        sun.intensity = 4.0f;
        sun.color = {1.0f, 0.93f, 0.78f};

        auto& auxiliaryLights = scene.GetAuxiliaryDirectionalLights();
        // A warmer, higher light preserves the reference sun azimuth while
        // revealing wave-face curvature outside the narrow primary glint.
        auxiliaryLights[0].direction = {
            -0.7589f, -0.5485f, 0.3495f};
        auxiliaryLights[0].intensity = 0.32f;
        auxiliaryLights[0].color = {1.0f, 0.82f, 0.64f};

        // Incoming rays roughly follow the Game camera and mirror the main
        // sun horizontally. This cool fill separates back-facing wavelets
        // without creating another visible sun disk or shadow cascade.
        auxiliaryLights[1].direction = {
            0.8190f, -0.2800f, -0.4990f};
        auxiliaryLights[1].intensity = 0.20f;
        auxiliaryLights[1].color = {0.58f, 0.72f, 0.90f};
        return;
    }
    scene.GetCamera().SetPerspective(
        XMConvertToRadians(58.0f),
        cameraAspectRatio,
        0.2f,
        2800.0f);
    scene.GetCamera().SetLookAt(
        {0.0f, 14.0f, -42.0f},
        {0.0f, 0.0f, 118.0f},
        {0.0f, 1.0f, 0.0f});
    scene.GetGameCamera() = scene.GetCamera();
    DirectionalLight& sun = scene.GetDirectionalLight();
    // Direction points along the incoming rays. The inverse therefore places
    // the visible sun directly ahead, 1.5 degrees above the sea horizon.
    sun.direction = {0.0f, -0.026177f, -0.999657f};
    sun.intensity = 3.35f;
    sun.color = {1.0f, 0.72f, 0.42f};
}
} // namespace Prism::Scene
