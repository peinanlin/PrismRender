#include "Scene/SceneLoader.h"

#include "Asset/AssetRegistry.h"
#include "Asset/GltfLoader.h"
#include "Asset/Material.h"
#include "Asset/MaterialAsset.h"
#include "Asset/Mesh.h"
#include "Asset/MeshAsset.h"
#include "Asset/Texture.h"
#include "Asset/TextureAsset.h"
#include "Core/CpuTrace.h"
#include "RHI/IGraphicsDevice.h"
#include "Scene/RenderObject.h"
#include "Scene/RenderScene.h"

#include <DirectXMath.h>

#include <cmath>
#include <limits>
#include <string>

using namespace DirectX;

namespace Prism::Scene
{
namespace
{
XMFLOAT3 QuaternionToEuler(const XMVECTOR& quaternion)
{
    XMFLOAT4 q{};
    XMStoreFloat4(&q, XMQuaternionNormalize(quaternion));

    const float sinrCosp = 2.0f * (q.w * q.x + q.y * q.z);
    const float cosrCosp = 1.0f - 2.0f * (q.x * q.x + q.y * q.y);
    const float pitch = std::atan2(sinrCosp, cosrCosp);

    const float sinp = 2.0f * (q.w * q.y - q.z * q.x);
    const float yaw = std::abs(sinp) >= 1.0f ? std::copysign(XM_PIDIV2, sinp) : std::asin(sinp);

    const float sinyCosp = 2.0f * (q.w * q.z + q.x * q.y);
    const float cosyCosp = 1.0f - 2.0f * (q.y * q.y + q.z * q.z);
    const float roll = std::atan2(sinyCosp, cosyCosp);
    return {pitch, yaw, roll};
}

void ApplyWorldMatrix(const XMFLOAT4X4& worldMatrix, Transform& transform)
{
    XMVECTOR scale{};
    XMVECTOR rotation{};
    XMVECTOR translation{};
    if (!XMMatrixDecompose(&scale, &rotation, &translation, XMLoadFloat4x4(&worldMatrix)))
    {
        return;
    }

    XMFLOAT3 scaleValue{};
    XMFLOAT3 translationValue{};
    XMStoreFloat3(&scaleValue, scale);
    XMStoreFloat3(&translationValue, translation);
    transform.SetPosition(translationValue);
    transform.SetScale(scaleValue);
    transform.SetRotationEulerRadians(QuaternionToEuler(rotation));
}

std::shared_ptr<Asset::TextureAsset> ResolveTextureAsset(
    std::shared_ptr<Asset::TextureAsset> textureAsset,
    const std::string& name,
    const XMFLOAT4& fallbackColor)
{
    if (textureAsset != nullptr)
    {
        return textureAsset;
    }
    textureAsset = std::make_shared<Asset::TextureAsset>();
    textureAsset->SetName(name);
    textureAsset->SetSolidColor(fallbackColor);
    return textureAsset;
}

std::shared_ptr<Asset::Texture> CreateRhiTexture(
    RHI::IGraphicsDevice& device,
    const std::shared_ptr<Asset::TextureAsset>& textureAsset)
{
    auto texture = std::make_shared<Asset::Texture>();
    if (textureAsset->HasImageData())
    {
        texture->InitializeRgba8(
            device,
            textureAsset->GetWidth(),
            textureAsset->GetHeight(),
            textureAsset->GetImageData().data());
    }
    else
    {
        texture->InitializeSolidColor(device, textureAsset->GetSolidColor());
    }
    return texture;
}

Asset::Material::Parameters CreateMaterialParameters(const Asset::MaterialAsset& materialAsset)
{
    Asset::Material::Parameters parameters{};
    parameters.albedoColor = materialAsset.GetAlbedoColor();
    parameters.specularColor = materialAsset.GetSpecularColor();
    parameters.emissiveColor = materialAsset.GetEmissiveColor();
    parameters.metallic = materialAsset.GetMetallic();
    parameters.roughness = materialAsset.GetRoughness();
    parameters.shininess = materialAsset.GetShininess();
    parameters.occlusionStrength = materialAsset.GetOcclusionStrength();
    parameters.normalScale = materialAsset.GetNormalScale();
    parameters.emissiveStrength = materialAsset.GetEmissiveStrength();
    parameters.alphaCutoff = materialAsset.GetAlphaCutoff();
    parameters.alphaMode = materialAsset.GetAlphaMode();
    parameters.doubleSided = materialAsset.GetDoubleSided();
    parameters.useAlbedoTexture = materialAsset.GetUseAlbedoTexture();
    parameters.useMetallicRoughnessTexture = materialAsset.GetUseMetallicRoughnessTexture();
    parameters.useNormalTexture = materialAsset.GetUseNormalTexture();
    parameters.useOcclusionTexture = materialAsset.GetUseOcclusionTexture();
    parameters.useEmissiveTexture = materialAsset.GetUseEmissiveTexture();
    return parameters;
}
} // namespace


bool SceneLoader::LoadFromGltf(
    const std::string& path,
    RHI::IGraphicsDevice& device,
    Asset::AssetRegistry& assetRegistry,
    RenderScene& scene,
    std::string* outErrorMessage,
    const bool clearExistingObjects,
    const XMFLOAT3 rootOffset) const
{
    Core::CpuTraceSpan traceSpan(
        "GltfSceneLoad",
        "asset");
    Asset::GltfLoader gltfLoader;
    Asset::GltfLoader::SceneData sceneData;
    if (!gltfLoader.LoadScene(path, sceneData, outErrorMessage))
    {
        return false;
    }

    if (clearExistingObjects)
    {
        scene.ClearRenderObjects();
    }

    std::uint32_t meshCounter = assetRegistry.GetMeshAssetCount();
    std::uint32_t textureCounter = assetRegistry.GetTextureAssetCount();
    std::uint32_t materialCounter = assetRegistry.GetMaterialAssetCount();
    std::size_t loadedObjectCount = 0;
    for (const Asset::GltfLoader::MeshInstanceRecord& instance : sceneData.instances)
    {
        if (instance.mesh == nullptr || instance.material == nullptr)
        {
            continue;
        }

        const Asset::MeshHandle meshHandle = assetRegistry.RegisterMeshAsset(
            "gltf://meshes/" + std::to_string(++meshCounter) + "/" + instance.name,
            instance.mesh);
        std::shared_ptr<Asset::Mesh> runtimeMesh = assetRegistry.GetRuntimeMesh(meshHandle);
        if (runtimeMesh == nullptr)
        {
            runtimeMesh = Asset::Mesh::CreateFromAsset(device, *instance.mesh);
            assetRegistry.SetRuntimeMesh(meshHandle, runtimeMesh);
        }

        const std::shared_ptr<Asset::TextureAsset> albedoAsset = ResolveTextureAsset(
            instance.baseColorTexture,
            instance.material->GetName() + "_BaseColor",
            instance.material->GetAlbedoColor());
        const std::shared_ptr<Asset::TextureAsset> metallicRoughnessAsset = ResolveTextureAsset(
            instance.metallicRoughnessTexture,
            instance.material->GetName() + "_MetallicRoughness",
            {0.0f, instance.material->GetRoughness(), instance.material->GetMetallic(), 1.0f});
        const std::shared_ptr<Asset::TextureAsset> normalAsset = ResolveTextureAsset(
            instance.normalTexture,
            instance.material->GetName() + "_Normal",
            {0.5f, 0.5f, 1.0f, 1.0f});
        const std::shared_ptr<Asset::TextureAsset> occlusionAsset = ResolveTextureAsset(
            instance.occlusionTexture,
            instance.material->GetName() + "_Occlusion",
            {1.0f, 1.0f, 1.0f, 1.0f});
        const std::shared_ptr<Asset::TextureAsset> emissiveAsset = ResolveTextureAsset(
            instance.emissiveTexture,
            instance.material->GetName() + "_Emissive",
            {0.0f, 0.0f, 0.0f, 1.0f});

        const Asset::TextureHandle albedoHandle = assetRegistry.RegisterTextureAsset(
            "gltf://textures/" + std::to_string(++textureCounter) + "/" + instance.name,
            albedoAsset);
        const Asset::TextureHandle metallicRoughnessHandle = assetRegistry.RegisterTextureAsset(
            "gltf://textures/" + std::to_string(++textureCounter) + "/" + instance.name + "_metallicRoughness",
            metallicRoughnessAsset);
        const Asset::TextureHandle normalHandle = assetRegistry.RegisterTextureAsset(
            "gltf://textures/" + std::to_string(++textureCounter) + "/" + instance.name + "_normal",
            normalAsset);
        const Asset::TextureHandle occlusionHandle = assetRegistry.RegisterTextureAsset(
            "gltf://textures/" + std::to_string(++textureCounter) + "/" + instance.name + "_occlusion",
            occlusionAsset);
        const Asset::TextureHandle emissiveHandle = assetRegistry.RegisterTextureAsset(
            "gltf://textures/" + std::to_string(++textureCounter) + "/" + instance.name + "_emissive",
            emissiveAsset);

        const auto resolveRuntimeTexture = [&](const Asset::TextureHandle handle,
                                               const std::shared_ptr<Asset::TextureAsset>& asset)
        {
            std::shared_ptr<Asset::Texture> texture = assetRegistry.GetRuntimeTexture(handle);
            if (texture == nullptr)
            {
                texture = CreateRhiTexture(device, asset);
                assetRegistry.SetRuntimeTexture(handle, texture);
            }
            return texture;
        };
        const std::shared_ptr<Asset::Texture> albedo =
            resolveRuntimeTexture(albedoHandle, albedoAsset);
        const std::shared_ptr<Asset::Texture> metallicRoughness =
            resolveRuntimeTexture(metallicRoughnessHandle, metallicRoughnessAsset);
        const std::shared_ptr<Asset::Texture> normal =
            resolveRuntimeTexture(normalHandle, normalAsset);
        const std::shared_ptr<Asset::Texture> occlusion =
            resolveRuntimeTexture(occlusionHandle, occlusionAsset);
        const std::shared_ptr<Asset::Texture> emissive =
            resolveRuntimeTexture(emissiveHandle, emissiveAsset);

        auto materialAsset = std::make_shared<Asset::MaterialAsset>(*instance.material);
        materialAsset->SetBaseColorTexture(albedoHandle);
        materialAsset->SetMetallicRoughnessTexture(metallicRoughnessHandle);
        materialAsset->SetNormalTexture(normalHandle);
        materialAsset->SetOcclusionTexture(occlusionHandle);
        materialAsset->SetEmissiveTexture(emissiveHandle);
        const Asset::MaterialHandle materialHandle = assetRegistry.RegisterMaterialAsset(
            "gltf://materials/" + std::to_string(++materialCounter) + "/" + instance.name,
            materialAsset);
        std::shared_ptr<Asset::Material> runtimeMaterial =
            assetRegistry.GetRuntimeMaterial(materialHandle);
        if (runtimeMaterial == nullptr)
        {
            runtimeMaterial = std::make_shared<Asset::Material>();
            runtimeMaterial->Initialize(
                CreateMaterialParameters(*materialAsset),
                albedo,
                metallicRoughness,
                normal,
                occlusion,
                emissive);
            assetRegistry.SetRuntimeMaterial(materialHandle, runtimeMaterial);
        }

        RenderObject renderObject{};
        renderObject.name = instance.name;
        renderObject.meshHandle = meshHandle;
        renderObject.mesh = std::move(runtimeMesh);
        renderObject.materialHandle = materialHandle;
        renderObject.material = std::move(runtimeMaterial);
        ApplyWorldMatrix(instance.worldMatrix, renderObject.transform);
        XMFLOAT3 importedPosition = renderObject.transform.GetPosition();
        importedPosition.x += rootOffset.x;
        importedPosition.y += rootOffset.y;
        importedPosition.z += rootOffset.z;
        renderObject.transform.SetPosition(importedPosition);
        scene.AddRenderObject(std::move(renderObject));
        ++loadedObjectCount;
    }

    if (loadedObjectCount == 0 && outErrorMessage != nullptr && outErrorMessage->empty())
    {
        *outErrorMessage = "The glTF scene did not contain any renderable mesh instances.";
    }
    return loadedObjectCount > 0;
}

bool SceneLoader::LoadFromGltf(
    const std::string& path,
    RHI::IGraphicsDevice& device,
    RenderScene& scene,
    std::string* outErrorMessage,
    const bool clearExistingObjects,
    const XMFLOAT3 rootOffset) const
{
    Core::CpuTraceSpan traceSpan(
        "GltfSceneLoad",
        "asset");
    Asset::GltfLoader gltfLoader;
    Asset::GltfLoader::SceneData sceneData;
    if (!gltfLoader.LoadScene(path, sceneData, outErrorMessage))
    {
        return false;
    }

    if (clearExistingObjects)
    {
        scene.ClearRenderObjects();
    }

    std::size_t loadedObjectCount = 0;
    for (const Asset::GltfLoader::MeshInstanceRecord& instance : sceneData.instances)
    {
        if (instance.mesh == nullptr || instance.material == nullptr)
        {
            continue;
        }

        const std::shared_ptr<Asset::TextureAsset> albedoAsset = ResolveTextureAsset(
            instance.baseColorTexture,
            instance.material->GetName() + "_BaseColor",
            instance.material->GetAlbedoColor());
        const std::shared_ptr<Asset::TextureAsset> metallicRoughnessAsset = ResolveTextureAsset(
            instance.metallicRoughnessTexture,
            instance.material->GetName() + "_MetallicRoughness",
            {0.0f, instance.material->GetRoughness(), instance.material->GetMetallic(), 1.0f});
        const std::shared_ptr<Asset::TextureAsset> normalAsset = ResolveTextureAsset(
            instance.normalTexture,
            instance.material->GetName() + "_Normal",
            {0.5f, 0.5f, 1.0f, 1.0f});
        const std::shared_ptr<Asset::TextureAsset> occlusionAsset = ResolveTextureAsset(
            instance.occlusionTexture,
            instance.material->GetName() + "_Occlusion",
            {1.0f, 1.0f, 1.0f, 1.0f});
        const std::shared_ptr<Asset::TextureAsset> emissiveAsset = ResolveTextureAsset(
            instance.emissiveTexture,
            instance.material->GetName() + "_Emissive",
            {0.0f, 0.0f, 0.0f, 1.0f});

        auto material = std::make_shared<Asset::Material>();
        material->Initialize(
            CreateMaterialParameters(*instance.material),
            CreateRhiTexture(device, albedoAsset),
            CreateRhiTexture(device, metallicRoughnessAsset),
            CreateRhiTexture(device, normalAsset),
            CreateRhiTexture(device, occlusionAsset),
            CreateRhiTexture(device, emissiveAsset));

        RenderObject renderObject{};
        renderObject.name = instance.name;
        renderObject.mesh = Asset::Mesh::CreateFromAsset(device, *instance.mesh);
        renderObject.material = std::move(material);
        ApplyWorldMatrix(instance.worldMatrix, renderObject.transform);
        XMFLOAT3 importedPosition = renderObject.transform.GetPosition();
        importedPosition.x += rootOffset.x;
        importedPosition.y += rootOffset.y;
        importedPosition.z += rootOffset.z;
        renderObject.transform.SetPosition(importedPosition);
        scene.AddRenderObject(renderObject);
        ++loadedObjectCount;
    }

    if (loadedObjectCount == 0 && outErrorMessage != nullptr && outErrorMessage->empty())
    {
        *outErrorMessage = "The glTF scene did not contain any renderable mesh instances.";
    }
    return loadedObjectCount > 0;
}
} // namespace Prism::Scene
