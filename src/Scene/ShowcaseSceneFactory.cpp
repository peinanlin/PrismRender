#include "Scene/ShowcaseSceneFactory.h"

#include "Asset/AssetRegistry.h"
#include "Asset/Material.h"
#include "Asset/MaterialAsset.h"
#include "Asset/Mesh.h"
#include "Asset/MeshAsset.h"
#include "Asset/Texture.h"
#include "Asset/TextureAsset.h"
#include "RHI/IGraphicsDevice.h"
#include "Scene/RenderObject.h"
#include "Scene/RenderScene.h"

#include <DirectXMath.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>

namespace Prism::Scene
{
using namespace DirectX;

namespace
{
constexpr std::uint32_t ShowcaseRenderObjectBudget = 128;
constexpr std::uint32_t MaterialSampleRows = 3;
constexpr std::uint32_t MaterialSampleColumns = 5;
constexpr std::uint32_t InstanceRows = 8;
constexpr std::uint32_t InstanceColumns = 6;
constexpr std::uint32_t ShowcasePointLightCount = 8;
constexpr std::uint32_t ShowcaseSpotLightCount = 2;

struct RuntimeMaterial
{
    Asset::MaterialHandle handle;
    std::shared_ptr<Asset::Material> material;
};

struct MaterialDescription
{
    const char* name;
    XMFLOAT4 albedo;
    float metallic;
    float roughness;
    XMFLOAT3 emissive;
    float emissiveStrength;
};

enum class ShowcaseMaterial : std::size_t
{
    Floor,
    ArchitectureDark,
    ArchitectureLight,
    Gold,
    GalleryBase,
    Instance,
    EmissiveWarm,
    EmissiveCool,
    Count
};

constexpr std::array<
    MaterialDescription,
    static_cast<std::size_t>(ShowcaseMaterial::Count)>
    MaterialDescriptions{{
        {"Floor", {0.22f, 0.25f, 0.30f, 1.0f}, 0.28f, 0.36f, {}, 1.0f},
        {"ArchitectureDark", {0.10f, 0.12f, 0.16f, 1.0f}, 0.35f, 0.46f, {}, 1.0f},
        {"ArchitectureLight", {0.46f, 0.52f, 0.60f, 1.0f}, 0.10f, 0.58f, {}, 1.0f},
        {"HeroGold", {0.96f, 0.58f, 0.16f, 1.0f}, 0.92f, 0.18f, {}, 1.0f},
        {"GalleryBase", {1.0f, 1.0f, 1.0f, 1.0f}, 0.0f, 0.5f, {}, 1.0f},
        {"Instance", {0.13f, 0.31f, 0.52f, 1.0f}, 0.55f, 0.34f, {}, 1.0f},
        {"EmissiveWarm", {0.95f, 0.32f, 0.06f, 1.0f}, 0.10f, 0.28f, {1.0f, 0.18f, 0.025f}, 8.0f},
        {"EmissiveCool", {0.08f, 0.38f, 0.95f, 1.0f}, 0.10f, 0.28f, {0.025f, 0.22f, 1.0f}, 8.0f},
    }};

RuntimeMaterial CreateRegisteredMaterial(
    Asset::AssetRegistry& assetRegistry,
    RHI::IGraphicsDevice& device,
    const MaterialDescription& description)
{
    const std::string name(description.name);
    const std::string rootPath =
        "builtin://showcase/";

    auto albedoAsset =
        std::make_shared<Asset::TextureAsset>();
    albedoAsset->SetName(name + "_BaseColor");
    albedoAsset->SetSolidColor(description.albedo);
    const Asset::TextureHandle albedoHandle =
        assetRegistry.RegisterTextureAsset(
            rootPath + "textures/" + name,
            albedoAsset);

    auto albedoTexture = std::make_shared<Asset::Texture>();
    albedoTexture->InitializeSolidColor(
        device,
        description.albedo);
    albedoTexture->SetDebugName(
        "Showcase." + name + ".BaseColor");
    assetRegistry.SetRuntimeTexture(
        albedoHandle,
        albedoTexture);

    const auto createTexture =
        [&device, &name](
            const XMFLOAT4& color,
            const std::string& channel)
    {
        auto texture = std::make_shared<Asset::Texture>();
        texture->InitializeSolidColor(device, color);
        texture->SetDebugName(
            "Showcase." + name + '.' + channel);
        return texture;
    };
    const std::shared_ptr<Asset::Texture>
        metallicRoughnessTexture = createTexture({
            0.0f,
            description.roughness,
            description.metallic,
            1.0f},
            "MetallicRoughness");
    const std::shared_ptr<Asset::Texture> normalTexture =
        createTexture(
            {0.5f, 0.5f, 1.0f, 1.0f},
            "Normal");
    const std::shared_ptr<Asset::Texture> occlusionTexture =
        createTexture(
            {1.0f, 1.0f, 1.0f, 1.0f},
            "Occlusion");
    const std::shared_ptr<Asset::Texture> emissiveTexture =
        createTexture(
            {1.0f, 1.0f, 1.0f, 1.0f},
            "Emissive");

    auto materialAsset =
        std::make_shared<Asset::MaterialAsset>();
    materialAsset->SetName(name);
    materialAsset->SetAlbedoColor(description.albedo);
    materialAsset->SetMetallic(description.metallic);
    materialAsset->SetRoughness(description.roughness);
    materialAsset->SetShininess(
        std::max(
            4.0f,
            (1.0f - description.roughness) * 128.0f));
    materialAsset->SetEmissiveColor(description.emissive);
    materialAsset->SetEmissiveStrength(
        description.emissiveStrength);
    materialAsset->SetBaseColorTexture(albedoHandle);
    materialAsset->SetUseAlbedoTexture(true);
    materialAsset->SetUseMetallicRoughnessTexture(false);
    materialAsset->SetUseNormalTexture(false);
    materialAsset->SetUseOcclusionTexture(false);
    materialAsset->SetUseEmissiveTexture(false);
    const Asset::MaterialHandle materialHandle =
        assetRegistry.RegisterMaterialAsset(
            rootPath + "materials/" + name,
            materialAsset);

    Asset::Material::Parameters parameters{};
    parameters.albedoColor =
        {1.0f, 1.0f, 1.0f, 1.0f};
    parameters.specularColor =
        {0.72f, 0.72f, 0.72f};
    parameters.metallic = description.metallic;
    parameters.roughness = description.roughness;
    parameters.shininess = materialAsset->GetShininess();
    parameters.emissiveColor = description.emissive;
    parameters.emissiveStrength =
        description.emissiveStrength;
    parameters.useAlbedoTexture = true;

    auto material = std::make_shared<Asset::Material>();
    material->Initialize(
        parameters,
        albedoTexture,
        metallicRoughnessTexture,
        normalTexture,
        occlusionTexture,
        emissiveTexture);
    assetRegistry.SetRuntimeMaterial(
        materialHandle,
        material);
    return {materialHandle, std::move(material)};
}

RenderObject& AddObject(
    RenderScene& scene,
    const std::string& name,
    const Asset::MeshHandle meshHandle,
    const std::shared_ptr<Asset::Mesh>& mesh,
    const RuntimeMaterial& material,
    const XMFLOAT3& position,
    const XMFLOAT3& scale,
    const XMFLOAT3& rotation = {})
{
    RenderObject renderObject{};
    renderObject.name = name;
    renderObject.meshHandle = meshHandle;
    renderObject.mesh = mesh;
    renderObject.materialHandle = material.handle;
    renderObject.material = material.material;
    renderObject.transform.SetPosition(position);
    renderObject.transform.SetScale(scale);
    renderObject.transform.SetRotationEulerRadians(rotation);
    return scene.AddRenderObject(std::move(renderObject));
}

void ApplyMaterialSampleOverride(
    RenderObject& renderObject,
    const float metallic,
    const float roughness)
{
    renderObject.hasMaterialOverride = true;
    renderObject.materialOverride.albedoColor =
        {0.92f, 0.42f, 0.10f, 1.0f};
    renderObject.materialOverride.metallic = metallic;
    renderObject.materialOverride.roughness = roughness;
    renderObject.materialOverride.useAlbedoTexture = false;
    renderObject.materialOverride.useMetallicRoughnessTexture =
        false;
    renderObject.materialOverride.useNormalTexture = false;
    renderObject.materialOverride.useOcclusionTexture = false;
    renderObject.materialOverride.useEmissiveTexture = false;
}

std::string MakeIndexedName(
    const std::string& prefix,
    const std::uint32_t first,
    const std::uint32_t second)
{
    return prefix
        + '_' + std::to_string(first)
        + '_' + std::to_string(second);
}
} // namespace

ShowcaseSceneSummary ShowcaseSceneFactory::Populate(
    Asset::AssetRegistry& assetRegistry,
    RHI::IGraphicsDevice& device,
    RenderScene& scene)
{
    scene.ClearRenderObjects();

    const std::shared_ptr<Asset::MeshAsset> cubeAsset =
        Asset::MeshAsset::CreateCube();
    cubeAsset->SetName("ShowcaseCube");
    const Asset::MeshHandle cubeHandle =
        assetRegistry.RegisterMeshAsset(
            "builtin://showcase/meshes/cube",
            cubeAsset);
    const std::shared_ptr<Asset::Mesh> cube =
        Asset::Mesh::CreateFromAsset(device, *cubeAsset);
    assetRegistry.SetRuntimeMesh(cubeHandle, cube);

    const std::shared_ptr<Asset::MeshAsset> sphereAsset =
        Asset::MeshAsset::CreateUvSphere(24, 48);
    sphereAsset->SetName("ShowcaseSphere");
    const Asset::MeshHandle sphereHandle =
        assetRegistry.RegisterMeshAsset(
            "builtin://showcase/meshes/sphere",
            sphereAsset);
    const std::shared_ptr<Asset::Mesh> sphere =
        Asset::Mesh::CreateFromAsset(device, *sphereAsset);
    assetRegistry.SetRuntimeMesh(sphereHandle, sphere);

    std::array<
        RuntimeMaterial,
        static_cast<std::size_t>(ShowcaseMaterial::Count)>
        materials;
    for (std::size_t index = 0;
         index < MaterialDescriptions.size();
         ++index)
    {
        materials[index] = CreateRegisteredMaterial(
            assetRegistry,
            device,
            MaterialDescriptions[index]);
    }
    const auto material =
        [&materials](const ShowcaseMaterial value)
            -> const RuntimeMaterial&
    {
        return materials[static_cast<std::size_t>(value)];
    };

    AddObject(
        scene, "Showcase_Floor", cubeHandle, cube,
        material(ShowcaseMaterial::Floor),
        {0.0f, -0.25f, 12.0f},
        {20.0f, 0.25f, 30.0f});
    AddObject(
        scene, "Showcase_BackWall", cubeHandle, cube,
        material(ShowcaseMaterial::ArchitectureDark),
        {0.0f, 4.5f, 30.0f},
        {20.0f, 4.5f, 0.30f});
    AddObject(
        scene, "Showcase_LeftWall", cubeHandle, cube,
        material(ShowcaseMaterial::ArchitectureDark),
        {-20.0f, 4.5f, 19.0f},
        {0.30f, 4.5f, 11.0f});
    AddObject(
        scene, "Showcase_RightWall", cubeHandle, cube,
        material(ShowcaseMaterial::ArchitectureDark),
        {20.0f, 4.5f, 19.0f},
        {0.30f, 4.5f, 11.0f});
    AddObject(
        scene, "Showcase_Runway", cubeHandle, cube,
        material(ShowcaseMaterial::ArchitectureLight),
        {0.0f, 0.08f, 10.0f},
        {3.4f, 0.08f, 17.0f});

    AddObject(
        scene, "Hero_Platform_Lower", cubeHandle, cube,
        material(ShowcaseMaterial::ArchitectureDark),
        {0.0f, 0.35f, 1.5f},
        {3.1f, 0.35f, 3.1f});
    AddObject(
        scene, "Hero_Platform_Upper", cubeHandle, cube,
        material(ShowcaseMaterial::ArchitectureLight),
        {0.0f, 0.82f, 1.5f},
        {2.25f, 0.14f, 2.25f});
    AddObject(
        scene, "Hero_GoldSphere", sphereHandle, sphere,
        material(ShowcaseMaterial::Gold),
        {0.0f, 2.45f, 1.5f},
        {1.48f, 1.48f, 1.48f});

    constexpr std::array<float, 4> ArchDepths{
        0.0f, 8.0f, 16.0f, 24.0f};
    for (std::uint32_t index = 0;
         index < ArchDepths.size();
         ++index)
    {
        const float z = ArchDepths[index];
        AddObject(
            scene,
            "Arch_Left_" + std::to_string(index),
            cubeHandle, cube,
            material(ShowcaseMaterial::ArchitectureLight),
            {-6.2f, 3.0f, z},
            {0.36f, 3.0f, 0.36f});
        AddObject(
            scene,
            "Arch_Right_" + std::to_string(index),
            cubeHandle, cube,
            material(ShowcaseMaterial::ArchitectureLight),
            {6.2f, 3.0f, z},
            {0.36f, 3.0f, 0.36f});
        AddObject(
            scene,
            "Arch_Beam_" + std::to_string(index),
            cubeHandle, cube,
            material(ShowcaseMaterial::ArchitectureLight),
            {0.0f, 6.0f, z},
            {6.55f, 0.28f, 0.36f});
    }

    for (std::uint32_t index = 0; index < 3; ++index)
    {
        const float width = 4.2f - index * 0.55f;
        AddObject(
            scene,
            "Hero_Step_" + std::to_string(index),
            cubeHandle, cube,
            material(ShowcaseMaterial::ArchitectureLight),
            {0.0f, 0.08f + index * 0.13f,
             -2.8f + index * 0.55f},
            {width, 0.08f + index * 0.05f, 0.42f});
    }

    AddObject(
        scene, "Gallery_Backdrop", cubeHandle, cube,
        material(ShowcaseMaterial::ArchitectureDark),
        {-11.0f, 3.3f, 26.7f},
        {6.8f, 3.3f, 0.24f});
    for (std::uint32_t row = 0;
         row < MaterialSampleRows;
         ++row)
    {
        AddObject(
            scene,
            "Gallery_Shelf_" + std::to_string(row),
            cubeHandle, cube,
            material(ShowcaseMaterial::ArchitectureLight),
            {-11.0f, 0.44f + row * 2.05f, 25.9f},
            {6.6f, 0.10f, 0.65f});
    }

    constexpr std::array<float, MaterialSampleRows>
        MetallicValues{0.0f, 0.5f, 1.0f};
    constexpr std::array<float, MaterialSampleColumns>
        RoughnessValues{0.08f, 0.24f, 0.44f, 0.68f, 0.90f};
    for (std::uint32_t row = 0;
         row < MaterialSampleRows;
         ++row)
    {
        for (std::uint32_t column = 0;
             column < MaterialSampleColumns;
             ++column)
        {
            RenderObject& sample = AddObject(
                scene,
                MakeIndexedName(
                    "MaterialSample", row, column),
                sphereHandle, sphere,
                material(ShowcaseMaterial::GalleryBase),
                {-15.6f + column * 2.30f,
                 1.35f + row * 2.05f,
                 25.15f},
                {0.72f, 0.72f, 0.72f});
            ApplyMaterialSampleOverride(
                sample,
                MetallicValues[row],
                RoughnessValues[column]);
        }
    }

    for (std::uint32_t row = 0;
         row < InstanceRows;
         ++row)
    {
        for (std::uint32_t column = 0;
             column < InstanceColumns;
             ++column)
        {
            const float height =
                0.45f
                + static_cast<float>((row + column) % 4)
                    * 0.18f;
            AddObject(
                scene,
                MakeIndexedName(
                    "Instance", row, column),
                cubeHandle, cube,
                material(ShowcaseMaterial::Instance),
                {8.0f + column * 1.75f,
                 height,
                 6.0f + row * 2.45f},
                {0.55f, height, 0.55f},
                {0.0f,
                 0.16f * static_cast<float>(row + column),
                 0.0f});
        }
    }
    AddObject(
        scene, "Stress_Occluder_Near", cubeHandle, cube,
        material(ShowcaseMaterial::ArchitectureDark),
        {12.3f, 1.25f, 12.0f},
        {6.2f, 1.25f, 0.32f});
    AddObject(
        scene, "Stress_Occluder_Far", cubeHandle, cube,
        material(ShowcaseMaterial::ArchitectureDark),
        {12.3f, 0.95f, 20.0f},
        {6.2f, 0.95f, 0.32f});

    constexpr std::array<XMFLOAT3, ShowcasePointLightCount>
        LightColors{{
            {1.0f, 0.18f, 0.04f},
            {0.06f, 0.28f, 1.0f},
            {1.0f, 0.55f, 0.08f},
            {0.10f, 0.75f, 1.0f},
            {1.0f, 0.12f, 0.35f},
            {0.18f, 0.35f, 1.0f},
            {0.95f, 0.36f, 0.05f},
            {0.08f, 0.65f, 1.0f},
        }};
    for (std::uint32_t index = 0;
         index < ShowcasePointLightCount;
         ++index)
    {
        const bool left = (index % 2) == 0;
        const float z = 4.0f
            + static_cast<float>(index / 2) * 5.0f;
        const RuntimeMaterial& orbMaterial = material(
            left
                ? ShowcaseMaterial::EmissiveWarm
                : ShowcaseMaterial::EmissiveCool);
        AddObject(
            scene,
            "LightOrb_" + std::to_string(index),
            sphereHandle, sphere,
            orbMaterial,
            {left ? -4.45f : 4.45f, 1.05f, z},
            {0.24f, 0.24f, 0.24f});

        PointLight& light = scene.GetPointLights()[index];
        light.position =
            {left ? -4.45f : 4.45f, 1.40f, z};
        light.color = LightColors[index];
        light.intensity = 4.5f;
        light.range = 8.5f;
        light.castsShadow = index < 4;
    }
    scene.SetActivePointLightCount(
        ShowcasePointLightCount);

    for (std::uint32_t index = 0;
         index < ShowcaseSpotLightCount;
         ++index)
    {
        SpotLight& light = scene.GetSpotLights()[index];
        light.position =
            {index == 0 ? -5.0f : 5.0f, 7.6f, -2.5f};
        light.direction =
            {index == 0 ? 0.42f : -0.42f, -0.78f, 0.46f};
        light.color =
            index == 0
                ? XMFLOAT3{1.0f, 0.48f, 0.18f}
                : XMFLOAT3{0.20f, 0.42f, 1.0f};
        light.intensity = 7.0f;
        light.range = 22.0f;
        light.innerAngleRadians = XMConvertToRadians(16.0f);
        light.outerAngleRadians = XMConvertToRadians(27.0f);
        light.castsShadow = true;
    }
    scene.SetActiveSpotLightCount(
        ShowcaseSpotLightCount);

    if (scene.GetRenderObjects().size()
        > ShowcaseRenderObjectBudget)
    {
        throw std::runtime_error(
            "Showcase scene exceeded the current renderer object budget.");
    }

    return {
        static_cast<std::uint32_t>(
            scene.GetRenderObjects().size()),
        MaterialSampleRows * MaterialSampleColumns,
        InstanceRows * InstanceColumns,
        ShowcasePointLightCount,
        ShowcaseSpotLightCount};
}

void ShowcaseSceneFactory::ConfigureWorld(
    RenderScene& scene,
    const float cameraAspectRatio)
{
    Camera& camera = scene.GetCamera();
    camera.SetPerspective(
        XMConvertToRadians(46.0f),
        cameraAspectRatio,
        0.1f,
        220.0f);
    camera.SetLookAt(
        {12.5f, 7.0f, -18.5f},
        {0.0f, 2.2f, 8.0f},
        {0.0f, 1.0f, 0.0f});
    scene.GetGameCamera() = camera;

    DirectionalLight& sun = scene.GetDirectionalLight();
    sun.direction = {-0.42f, -0.72f, 0.34f};
    sun.color = {1.0f, 0.90f, 0.76f};
    sun.intensity = 2.15f;
}
} // namespace Prism::Scene
