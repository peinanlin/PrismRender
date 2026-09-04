#include "Scene/DemoSceneBuilder.h"

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

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <utility>
#include <vector>

namespace Prism::Scene
{
namespace
{
float LinearToSrgb(const float value)
{
    const float clamped = std::clamp(value, 0.0f, 1.0f);
    return clamped <= 0.0031308f
        ? clamped * 12.92f
        : 1.055f * std::pow(clamped, 1.0f / 2.4f) - 0.055f;
}

DirectX::XMFLOAT4 EncodeSrgb(
    const DirectX::XMFLOAT4& linear)
{
    return {
        LinearToSrgb(linear.x),
        LinearToSrgb(linear.y),
        LinearToSrgb(linear.z),
        linear.w};
}

std::uint8_t ToByte(const float value)
{
    return static_cast<std::uint8_t>(
        std::clamp(value, 0.0f, 1.0f) * 255.0f + 0.5f);
}
} // namespace

DemoMeshResources DemoSceneBuilder::CreateMeshes(
    Asset::AssetRegistry& assetRegistry,
    RHI::IGraphicsDevice& device,
    const std::string& rootPath,
    const std::string& namePrefix,
    const std::uint32_t sphereLatitudeSegments,
    const std::uint32_t sphereLongitudeSegments)
{
    const std::shared_ptr<Asset::MeshAsset> cubeAsset =
        Asset::MeshAsset::CreateCube();
    cubeAsset->SetName(namePrefix + "Cube");
    const Asset::MeshHandle cubeHandle =
        assetRegistry.RegisterMeshAsset(
            rootPath + "meshes/cube",
            cubeAsset);
    const std::shared_ptr<Asset::Mesh> cube =
        Asset::Mesh::CreateFromAsset(device, *cubeAsset);
    assetRegistry.SetRuntimeMesh(cubeHandle, cube);

    const std::shared_ptr<Asset::MeshAsset> sphereAsset =
        Asset::MeshAsset::CreateUvSphere(
            sphereLatitudeSegments,
            sphereLongitudeSegments);
    sphereAsset->SetName(namePrefix + "Sphere");
    const Asset::MeshHandle sphereHandle =
        assetRegistry.RegisterMeshAsset(
            rootPath + "meshes/sphere",
            sphereAsset);
    const std::shared_ptr<Asset::Mesh> sphere =
        Asset::Mesh::CreateFromAsset(device, *sphereAsset);
    assetRegistry.SetRuntimeMesh(sphereHandle, sphere);

    return {
        cubeHandle,
        cube,
        sphereHandle,
        sphere};
}

DemoMaterialResources DemoSceneBuilder::CreateMaterial(
    Asset::AssetRegistry& assetRegistry,
    RHI::IGraphicsDevice& device,
    const std::string& rootPath,
    const DemoMaterialDescription& description)
{
    auto albedoAsset =
        std::make_shared<Asset::TextureAsset>();
    albedoAsset->SetName(description.name + "_BaseColor");
    const DirectX::XMFLOAT4 encodedAlbedo =
        EncodeSrgb(description.albedo);
    albedoAsset->SetSolidColor(encodedAlbedo);
    const Asset::TextureHandle albedoHandle =
        assetRegistry.RegisterTextureAsset(
            rootPath + "textures/" + description.name,
            albedoAsset);

    auto albedoTexture = std::make_shared<Asset::Texture>();
    if (description.checkerTileCount > 0u)
    {
        constexpr std::uint32_t CheckerResolution = 128u;
        const DirectX::XMFLOAT4 encodedChecker =
            EncodeSrgb(description.checkerAlbedo);
        std::vector<std::uint8_t> checkerPixels(
            static_cast<std::size_t>(CheckerResolution)
                * CheckerResolution * 4u,
            255u);
        for (std::uint32_t y = 0u; y < CheckerResolution; ++y)
        {
            for (std::uint32_t x = 0u; x < CheckerResolution; ++x)
            {
                const std::uint32_t tileX =
                    x * description.checkerTileCount
                    / CheckerResolution;
                const std::uint32_t tileY =
                    y * description.checkerTileCount
                    / CheckerResolution;
                const DirectX::XMFLOAT4& color =
                    ((tileX + tileY) & 1u) == 0u
                    ? encodedAlbedo
                    : encodedChecker;
                const std::size_t index =
                    (static_cast<std::size_t>(y)
                         * CheckerResolution
                     + x)
                    * 4u;
                checkerPixels[index + 0u] = ToByte(color.x);
                checkerPixels[index + 1u] = ToByte(color.y);
                checkerPixels[index + 2u] = ToByte(color.z);
                checkerPixels[index + 3u] = ToByte(color.w);
            }
        }
        albedoTexture->InitializeRgba8(
            device,
            CheckerResolution,
            CheckerResolution,
            checkerPixels.data());
    }
    else
    {
        albedoTexture->InitializeSolidColor(
            device,
            encodedAlbedo);
    }
    assetRegistry.SetRuntimeTexture(
        albedoHandle,
        albedoTexture);

    const auto createTexture =
        [&device](const DirectX::XMFLOAT4& color)
    {
        auto texture = std::make_shared<Asset::Texture>();
        texture->InitializeSolidColor(device, color);
        return texture;
    };
    const std::shared_ptr<Asset::Texture>
        metallicRoughnessTexture = createTexture({
            0.0f,
            description.roughness,
            description.metallic,
            1.0f});
    const std::shared_ptr<Asset::Texture> normalTexture =
        createTexture({0.5f, 0.5f, 1.0f, 1.0f});
    const std::shared_ptr<Asset::Texture> occlusionTexture =
        createTexture({1.0f, 1.0f, 1.0f, 1.0f});
    const std::shared_ptr<Asset::Texture> emissiveTexture =
        createTexture({1.0f, 1.0f, 1.0f, 1.0f});

    auto materialAsset =
        std::make_shared<Asset::MaterialAsset>();
    materialAsset->SetName(description.name);
    materialAsset->SetAlbedoColor(description.albedo);
    materialAsset->SetMetallic(description.metallic);
    materialAsset->SetRoughness(description.roughness);
    materialAsset->SetShininess(std::max(
        4.0f,
        (1.0f - description.roughness) * 128.0f));
    materialAsset->SetEmissiveColor(description.emissive);
    materialAsset->SetEmissiveStrength(
        description.emissiveStrength);
    materialAsset->SetDoubleSided(
        description.doubleSided);
    materialAsset->SetBaseColorTexture(albedoHandle);
    materialAsset->SetUseAlbedoTexture(true);
    materialAsset->SetUseMetallicRoughnessTexture(false);
    materialAsset->SetUseNormalTexture(false);
    materialAsset->SetUseOcclusionTexture(false);
    materialAsset->SetUseEmissiveTexture(false);
    const Asset::MaterialHandle materialHandle =
        assetRegistry.RegisterMaterialAsset(
            rootPath + "materials/" + description.name,
            materialAsset);

    Asset::Material::Parameters parameters{};
    parameters.albedoColor = {1.0f, 1.0f, 1.0f, 1.0f};
    parameters.specularColor = {0.72f, 0.72f, 0.72f};
    parameters.metallic = description.metallic;
    parameters.roughness = description.roughness;
    parameters.shininess = materialAsset->GetShininess();
    parameters.emissiveColor = description.emissive;
    parameters.emissiveStrength = description.emissiveStrength;
    parameters.doubleSided = description.doubleSided;
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

RenderObject& DemoSceneBuilder::AddObject(
    RenderScene& scene,
    const std::string& name,
    const Asset::MeshHandle meshHandle,
    const std::shared_ptr<Asset::Mesh>& mesh,
    const DemoMaterialResources& material,
    const DirectX::XMFLOAT3& position,
    const DirectX::XMFLOAT3& scale,
    const DirectX::XMFLOAT3& rotation)
{
    RenderObject object{};
    object.name = name;
    object.meshHandle = meshHandle;
    object.mesh = mesh;
    object.materialHandle = material.handle;
    object.material = material.material;
    object.transform.SetPosition(position);
    object.transform.SetScale(scale);
    object.transform.SetRotationEulerRadians(rotation);
    return scene.AddRenderObject(std::move(object));
}

void DemoSceneBuilder::ResetLights(RenderScene& scene)
{
    for (DirectionalLight& light :
         scene.GetAuxiliaryDirectionalLights())
    {
        light = {};
        light.direction = {0.0f, -1.0f, 0.0f};
        light.intensity = 0.0f;
    }
    for (PointLight& light : scene.GetPointLights())
    {
        light = {};
        light.castsShadow = false;
    }
    for (SpotLight& light : scene.GetSpotLights())
    {
        light = {};
        light.castsShadow = false;
    }
    scene.SetActivePointLightCount(0);
    scene.SetActiveSpotLightCount(0);
}
} // namespace Prism::Scene
