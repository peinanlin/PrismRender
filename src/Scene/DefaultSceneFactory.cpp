#include "Scene/DefaultSceneFactory.h"

#include "Asset/AssetRegistry.h"
#include "Asset/Material.h"
#include "Asset/MaterialAsset.h"
#include "Asset/Mesh.h"
#include "Asset/MeshAsset.h"
#include "Asset/TextureAsset.h"
#include "Asset/Texture.h"
#include "RHI/IGraphicsDevice.h"
#include "Scene/RenderObject.h"
#include "Scene/RenderScene.h"

#include <DirectXMath.h>

#include <array>
#include <memory>
#include <string>
#include <utility>

namespace Prism::Scene
{
using namespace DirectX;

namespace
{
struct RuntimeMaterial
{
    Asset::MaterialHandle handle;
    std::shared_ptr<Asset::Material> material;
};

struct PreviewMaterialDescription
{
    const char* name;
    XMFLOAT4 albedo;
    XMFLOAT3 specular;
    float shininess;
    float roughness;
};

enum class PreviewMesh
{
    Cube,
    Sphere
};

struct PreviewObjectDescription
{
    const char* name;
    PreviewMesh mesh;
    std::size_t material;
    XMFLOAT3 position;
    XMFLOAT3 scale;
    XMFLOAT3 rotation;
};

const std::array<PreviewMaterialDescription, 3> PreviewMaterials = {{
    {"Preview_Neutral", {0.72f, 0.75f, 0.78f, 1.0f}, {0.40f, 0.42f, 0.44f}, 64.0f, 0.45f},
    {"Preview_Warm", {0.90f, 0.46f, 0.22f, 1.0f}, {0.75f, 0.45f, 0.28f}, 72.0f, 0.38f},
    {"Preview_Cool", {0.25f, 0.56f, 0.92f, 1.0f}, {0.35f, 0.56f, 0.92f}, 80.0f, 0.34f},
}};

const std::array<PreviewObjectDescription, 5> PreviewObjects = {{
    {"Origin_Platform", PreviewMesh::Cube, 0, {0.0f, 0.16f, 0.0f}, {1.45f, 0.16f, 1.45f}, {}},
    {"Preview_Sphere", PreviewMesh::Sphere, 2, {0.0f, 1.48f, 0.0f}, {1.12f, 1.12f, 1.12f}, {}},
    {"Preview_Cube", PreviewMesh::Cube, 1, {-4.0f, 1.0f, 2.0f}, {1.0f, 1.0f, 1.0f}, {0.0f, 0.45f, 0.0f}},
    {"Preview_Tall_Block", PreviewMesh::Cube, 0, {4.2f, 1.7f, -2.5f}, {0.72f, 1.7f, 0.72f}, {}},
    {"Preview_Ramp", PreviewMesh::Cube, 1, {-1.4f, 0.42f, 5.0f}, {2.2f, 0.22f, 0.95f}, {-0.34f, 0.0f, 0.0f}},
}};


void AddObject(
    RenderScene& scene,
    const std::string& name,
    const Asset::MeshHandle meshHandle,
    const std::shared_ptr<Asset::Mesh>& mesh,
    const RuntimeMaterial& material,
    const XMFLOAT3& position,
    const XMFLOAT3& scale,
    const XMFLOAT3 rotation = {0.0f, 0.0f, 0.0f})
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
    scene.AddRenderObject(std::move(renderObject));
}

std::shared_ptr<Asset::Material> CreateRhiMaterial(
    RHI::IGraphicsDevice& device,
    const PreviewMaterialDescription& description)
{
    const auto createTexture = [&](const XMFLOAT4& color)
    {
        auto texture = std::make_shared<Asset::Texture>();
        texture->InitializeSolidColor(device, color);
        return texture;
    };
    const std::shared_ptr<Asset::Texture> albedo = createTexture(description.albedo);
    const std::shared_ptr<Asset::Texture> metallicRoughness = createTexture({0.0f, description.roughness, 0.0f, 1.0f});
    const std::shared_ptr<Asset::Texture> normal = createTexture({0.5f, 0.5f, 1.0f, 1.0f});
    const std::shared_ptr<Asset::Texture> occlusion = createTexture({1.0f, 1.0f, 1.0f, 1.0f});
    const std::shared_ptr<Asset::Texture> emissive = createTexture({0.0f, 0.0f, 0.0f, 1.0f});

    Asset::Material::Parameters parameters{};
    parameters.albedoColor = {1.0f, 1.0f, 1.0f, 1.0f};
    parameters.specularColor = description.specular;
    parameters.shininess = description.shininess;
    parameters.roughness = description.roughness;
    auto material = std::make_shared<Asset::Material>();
    material->Initialize(parameters, albedo, metallicRoughness, normal, occlusion, emissive);
    return material;
}

RuntimeMaterial CreateRegisteredRhiMaterial(
    Asset::AssetRegistry& assetRegistry,
    RHI::IGraphicsDevice& device,
    const PreviewMaterialDescription& description)
{
    auto albedoAsset = std::make_shared<Asset::TextureAsset>();
    albedoAsset->SetName(std::string(description.name) + "_BaseColor");
    albedoAsset->SetSolidColor(description.albedo);
    const Asset::TextureHandle albedoHandle = assetRegistry.RegisterTextureAsset(
        "builtin://editor-preview/textures/" + std::string(description.name),
        albedoAsset);

    auto albedo = std::make_shared<Asset::Texture>();
    albedo->InitializeSolidColor(device, description.albedo);
    assetRegistry.SetRuntimeTexture(albedoHandle, albedo);

    const auto createTexture = [&](const XMFLOAT4& color)
    {
        auto texture = std::make_shared<Asset::Texture>();
        texture->InitializeSolidColor(device, color);
        return texture;
    };
    const std::shared_ptr<Asset::Texture> metallicRoughness =
        createTexture({0.0f, description.roughness, 0.0f, 1.0f});
    const std::shared_ptr<Asset::Texture> normal =
        createTexture({0.5f, 0.5f, 1.0f, 1.0f});
    const std::shared_ptr<Asset::Texture> occlusion =
        createTexture({1.0f, 1.0f, 1.0f, 1.0f});
    const std::shared_ptr<Asset::Texture> emissive =
        createTexture({0.0f, 0.0f, 0.0f, 1.0f});

    auto materialAsset = std::make_shared<Asset::MaterialAsset>();
    materialAsset->SetName(description.name);
    materialAsset->SetAlbedoColor(description.albedo);
    materialAsset->SetSpecularColor(description.specular);
    materialAsset->SetShininess(description.shininess);
    materialAsset->SetRoughness(description.roughness);
    materialAsset->SetBaseColorTexture(albedoHandle);
    materialAsset->SetUseAlbedoTexture(true);
    const Asset::MaterialHandle materialHandle = assetRegistry.RegisterMaterialAsset(
        "builtin://editor-preview/materials/" + std::string(description.name),
        materialAsset);

    Asset::Material::Parameters parameters{};
    parameters.albedoColor = {1.0f, 1.0f, 1.0f, 1.0f};
    parameters.specularColor = description.specular;
    parameters.shininess = description.shininess;
    parameters.roughness = description.roughness;
    auto material = std::make_shared<Asset::Material>();
    material->Initialize(
        parameters,
        albedo,
        metallicRoughness,
        normal,
        occlusion,
        emissive);
    assetRegistry.SetRuntimeMaterial(materialHandle, material);
    return {materialHandle, std::move(material)};
}
} // namespace


void DefaultSceneFactory::PopulateEditorPreviewScene(
    Asset::AssetRegistry& assetRegistry,
    RHI::IGraphicsDevice& device,
    RenderScene& scene)
{
    scene.ClearRenderObjects();

    const std::shared_ptr<Asset::MeshAsset> cubeMeshAsset = Asset::MeshAsset::CreateCube();
    const Asset::MeshHandle cubeMeshHandle = assetRegistry.RegisterMeshAsset(
        "builtin://editor-preview/meshes/cube",
        cubeMeshAsset);
    const std::shared_ptr<Asset::Mesh> cubeMesh =
        Asset::Mesh::CreateFromAsset(device, *cubeMeshAsset);
    assetRegistry.SetRuntimeMesh(cubeMeshHandle, cubeMesh);

    const std::shared_ptr<Asset::MeshAsset> sphereMeshAsset =
        Asset::MeshAsset::CreateUvSphere(18, 36);
    const Asset::MeshHandle sphereMeshHandle = assetRegistry.RegisterMeshAsset(
        "builtin://editor-preview/meshes/uv-sphere",
        sphereMeshAsset);
    const std::shared_ptr<Asset::Mesh> sphereMesh =
        Asset::Mesh::CreateFromAsset(device, *sphereMeshAsset);
    assetRegistry.SetRuntimeMesh(sphereMeshHandle, sphereMesh);

    std::array<RuntimeMaterial, PreviewMaterials.size()> materials;
    for (std::size_t index = 0; index < PreviewMaterials.size(); ++index)
    {
        materials[index] = CreateRegisteredRhiMaterial(
            assetRegistry,
            device,
            PreviewMaterials[index]);
    }
    for (const PreviewObjectDescription& object : PreviewObjects)
    {
        const bool cube = object.mesh == PreviewMesh::Cube;
        AddObject(
            scene,
            object.name,
            cube ? cubeMeshHandle : sphereMeshHandle,
            cube ? cubeMesh : sphereMesh,
            materials[object.material],
            object.position,
            object.scale,
            object.rotation);
    }
}

void DefaultSceneFactory::PopulateEditorPreviewScene(
    RHI::IGraphicsDevice& device,
    RenderScene& scene)
{
    scene.ClearRenderObjects();
    const std::shared_ptr<Asset::Mesh> cube = Asset::Mesh::CreateCube(device);
    const std::shared_ptr<Asset::MeshAsset> sphereAsset = Asset::MeshAsset::CreateUvSphere(18, 36);
    const std::shared_ptr<Asset::Mesh> sphere = Asset::Mesh::CreateFromAsset(device, *sphereAsset);
    std::array<std::shared_ptr<Asset::Material>, PreviewMaterials.size()> materials;
    for (std::size_t index = 0; index < PreviewMaterials.size(); ++index)
    {
        materials[index] = CreateRhiMaterial(device, PreviewMaterials[index]);
    }
    for (const PreviewObjectDescription& object : PreviewObjects)
    {
        RenderObject renderObject{};
        renderObject.name = object.name;
        renderObject.mesh = object.mesh == PreviewMesh::Cube ? cube : sphere;
        renderObject.material = materials[object.material];
        renderObject.transform.SetPosition(object.position);
        renderObject.transform.SetScale(object.scale);
        renderObject.transform.SetRotationEulerRadians(object.rotation);
        scene.AddRenderObject(std::move(renderObject));
    }
}

void DefaultSceneFactory::ConfigureEditorPreviewWorld(
    RenderScene& scene,
    const float cameraAspectRatio)
{
    Camera& camera = scene.GetCamera();
    camera.SetPerspective(XM_PIDIV4, cameraAspectRatio, 0.1f, 250.0f);
    camera.SetLookAt({8.0f, 5.4f, -12.0f}, {0.0f, 0.9f, 0.0f}, {0.0f, 1.0f, 0.0f});
    scene.GetGameCamera() = camera;

    scene.GetDirectionalLight().direction = {-0.30f, -0.12f, -0.95f};
    scene.GetDirectionalLight().color = {1.0f, 0.94f, 0.84f};
    scene.GetDirectionalLight().intensity = 1.35f;
    scene.SetActivePointLightCount(2);
    scene.GetPointLights()[0].position = {-3.6f, 3.0f, 3.0f};
    scene.GetPointLights()[0].color = {1.0f, 0.55f, 0.40f};
    scene.GetPointLights()[0].intensity = 1.2f;
    scene.GetPointLights()[0].range = 8.0f;
    scene.GetPointLights()[0].castsShadow = true;
    scene.GetPointLights()[1].position = {4.0f, 2.6f, -4.0f};
    scene.GetPointLights()[1].color = {0.35f, 0.55f, 1.0f};
    scene.GetPointLights()[1].intensity = 0.9f;
    scene.GetPointLights()[1].range = 8.0f;
    scene.GetPointLights()[1].castsShadow = true;
    scene.SetActiveSpotLightCount(1);
    scene.GetSpotLights()[0].position =
        {0.0f, 7.0f, -5.0f};
    scene.GetSpotLights()[0].direction =
        {0.0f, -0.82f, 0.57f};
    scene.GetSpotLights()[0].color =
        {1.0f, 0.92f, 0.76f};
    scene.GetSpotLights()[0].intensity = 3.5f;
    scene.GetSpotLights()[0].range = 18.0f;
    scene.GetSpotLights()[0].castsShadow = true;
}
} // namespace Prism::Scene
