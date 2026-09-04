#include "Scene/WorldRenderSceneBridge.h"

#include "Asset/AssetRegistry.h"
#include "Engine/World.h"
#include "Scene/Camera.h"
#include "Scene/RenderObject.h"
#include "Scene/RenderScene.h"

#include <DirectXMath.h>

#include <algorithm>
#include <bit>
#include <charconv>
#include <cmath>
#include <iterator>
#include <map>
#include <optional>
#include <string_view>
#include <utility>

namespace Prism::Scene
{
namespace
{
using namespace DirectX;

constexpr std::string_view PrimaryDirectionalLightName =
    "DirectionalLight";
constexpr std::array<std::string_view,
    RenderScene::MaxAuxiliaryDirectionalLights>
    AuxiliaryDirectionalLightNames{
        "DirectionalLight_HighFill",
        "DirectionalLight_MirrorFill"};

Engine::Float3 ToEngine(const XMFLOAT3& value)
{
    return {value.x, value.y, value.z};
}

Core::Double3 ToWorldPosition(
    const XMFLOAT3& value)
{
    return {value.x, value.y, value.z};
}

XMFLOAT3 ToRender(const Engine::Float3& value)
{
    return {value.x, value.y, value.z};
}

XMFLOAT3 ToRender(const Core::Double3& value)
{
    return {
        static_cast<float>(value.x),
        static_cast<float>(value.y),
        static_cast<float>(value.z)};
}

Engine::Float3 RotationFromDirection(
    const XMFLOAT3& value)
{
    XMFLOAT3 direction{};
    XMStoreFloat3(
        &direction,
        XMVector3Normalize(
            XMLoadFloat3(&value)));
    return {
        std::asin(std::clamp(
            direction.y,
            -1.0f,
            1.0f)),
        std::atan2(
            direction.x,
            direction.z),
        0.0f};
}

std::string MakeLegacyHandlePath(const char* type, const std::uint32_t handle)
{
    return std::string("prism-handle://") + type + '/' + std::to_string(handle);
}

std::optional<std::uint32_t> ParseLegacyHandlePath(
    const std::string_view path,
    const std::string_view type)
{
    const std::string prefix = "prism-handle://" + std::string(type) + '/';
    if (!path.starts_with(prefix))
    {
        return std::nullopt;
    }
    std::uint32_t value = 0;
    const std::string_view number = path.substr(prefix.size());
    const auto [end, error] = std::from_chars(number.data(), number.data() + number.size(), value);
    return error == std::errc{} && end == number.data() + number.size()
        ? std::optional<std::uint32_t>(value)
        : std::nullopt;
}

std::uint64_t HashMaterialOverride(
    const Engine::MaterialOverrideComponent& material)
{
    std::uint64_t hash = 14695981039346656037ull;
    const auto appendFloat = [&](const float value)
    {
        hash ^= std::bit_cast<std::uint32_t>(value);
        hash *= 1099511628211ull;
    };
    const auto appendBool = [&](const bool value)
    {
        hash ^= value ? 1ull : 0ull;
        hash *= 1099511628211ull;
    };
    appendFloat(material.albedoColor.x);
    appendFloat(material.albedoColor.y);
    appendFloat(material.albedoColor.z);
    appendFloat(material.albedoColor.w);
    appendFloat(material.emissiveColor.x);
    appendFloat(material.emissiveColor.y);
    appendFloat(material.emissiveColor.z);
    appendFloat(material.metallic);
    appendFloat(material.roughness);
    appendFloat(material.occlusionStrength);
    appendFloat(material.normalScale);
    appendFloat(material.emissiveStrength);
    appendFloat(material.alphaCutoff);
    appendFloat(material.alphaMode);
    appendBool(material.useAlbedoTexture);
    appendBool(material.useMetallicRoughnessTexture);
    appendBool(material.useNormalTexture);
    appendBool(material.useOcclusionTexture);
    appendBool(material.useEmissiveTexture);
    return hash;
}
} // namespace

void WorldRenderSceneBridge::ImportRenderScene(
    RenderScene& scene,
    const Asset::AssetRegistry& assetRegistry,
    Engine::World& world,
    const std::uint64_t idSeed)
{
    world = Engine::World(idSeed);

    const Camera& camera = scene.GetCamera();
    Engine::EntityRecord& cameraEntity = world.CreateEntity("EditorCamera");
    cameraEntity.transform.position =
        camera.GetWorldPosition();
    cameraEntity.transform.rotation = {camera.GetPitch(), camera.GetYaw(), 0.0f};
    cameraEntity.camera = Engine::CameraComponent{
        camera.GetFieldOfViewYRadians(), camera.GetNearPlane(), camera.GetFarPlane()};

    const DirectionalLight& directionalLight = scene.GetDirectionalLight();
    Engine::EntityRecord& directionalEntity = world.CreateEntity(
        std::string(PrimaryDirectionalLightName));
    directionalEntity.transform.rotation =
        RotationFromDirection(
            directionalLight.direction);
    directionalEntity.directionalLight = Engine::DirectionalLightComponent{
        ToEngine(directionalLight.direction),
        ToEngine(directionalLight.color),
        directionalLight.intensity};

    const auto& auxiliaryDirectionalLights =
        scene.GetAuxiliaryDirectionalLights();
    for (std::size_t index = 0;
         index < auxiliaryDirectionalLights.size();
         ++index)
    {
        const DirectionalLight& auxiliaryLight =
            auxiliaryDirectionalLights[index];
        if (auxiliaryLight.intensity <= 0.0f)
        {
            continue;
        }
        Engine::EntityRecord& auxiliaryEntity = world.CreateEntity(
            std::string(AuxiliaryDirectionalLightNames[index]));
        auxiliaryEntity.transform.rotation =
            RotationFromDirection(auxiliaryLight.direction);
        auxiliaryEntity.directionalLight =
            Engine::DirectionalLightComponent{
                ToEngine(auxiliaryLight.direction),
                ToEngine(auxiliaryLight.color),
                auxiliaryLight.intensity};
    }

    for (std::uint32_t index = 0; index < scene.GetActivePointLightCount(); ++index)
    {
        const PointLight& pointLight = scene.GetPointLights()[index];
        Engine::EntityRecord& pointEntity = world.CreateEntity("PointLight" + std::to_string(index));
        pointEntity.transform.position =
            ToWorldPosition(pointLight.position);
        pointEntity.pointLight = Engine::PointLightComponent{
            ToEngine(pointLight.color),
            pointLight.intensity,
            pointLight.range,
            pointLight.castsShadow};
    }

    for (std::uint32_t index = 0;
         index < scene.GetActiveSpotLightCount();
         ++index)
    {
        const SpotLight& spotLight =
            scene.GetSpotLights()[index];
        Engine::EntityRecord& spotEntity =
            world.CreateEntity(
                "SpotLight"
                + std::to_string(index));
        spotEntity.transform.position =
            ToWorldPosition(spotLight.position);
        spotEntity.transform.rotation =
            RotationFromDirection(
                spotLight.direction);
        spotEntity.spotLight =
            Engine::SpotLightComponent{
                ToEngine(spotLight.direction),
                ToEngine(spotLight.color),
                spotLight.intensity,
                spotLight.range,
                spotLight.innerAngleRadians,
                spotLight.outerAngleRadians,
                spotLight.castsShadow};
    }

    for (RenderObject& renderObject :
         scene.EditRenderObjectsForFullRebuild())
    {
        // Renderer-owned editor helpers are intentionally absent from the
        // authoring World. Importing them makes them selectable in Hierarchy,
        // which in turn draws the transform gizmo over diagnostic geometry.
        if (renderObject.editorOnly)
        {
            renderObject.entityId = {};
            continue;
        }

        Engine::EntityRecord& entity = world.CreateEntity(
            renderObject.name.empty() ? "RenderObject" : renderObject.name);
        renderObject.entityId = entity.id;
        entity.transform.position =
            renderObject.transform.GetWorldPosition();
        entity.transform.rotation = ToEngine(renderObject.transform.GetRotationEulerRadians());
        entity.transform.scale = ToEngine(renderObject.transform.GetScale());

        const std::string& meshPath = assetRegistry.GetMeshPath(renderObject.meshHandle);
        const std::string& materialPath = assetRegistry.GetMaterialPath(renderObject.materialHandle);
        entity.meshRenderer = Engine::MeshRendererComponent{
            meshPath.empty() ? MakeLegacyHandlePath("mesh", renderObject.meshHandle.Value()) : meshPath,
            materialPath.empty() ? MakeLegacyHandlePath("material", renderObject.materialHandle.Value()) : materialPath,
            renderObject.visible};
        if (renderObject.hasMaterialOverride)
        {
            const MaterialParameterOverride& material =
                renderObject.materialOverride;
            entity.materialOverride =
                Engine::MaterialOverrideComponent{
                    {material.albedoColor.x,
                     material.albedoColor.y,
                     material.albedoColor.z,
                     material.albedoColor.w},
                    ToEngine(material.emissiveColor),
                    material.metallic,
                    material.roughness,
                    material.occlusionStrength,
                    material.normalScale,
                    material.emissiveStrength,
                    material.alphaCutoff,
                    material.alphaMode,
                    material.useAlbedoTexture,
                    material.useMetallicRoughnessTexture,
                    material.useNormalTexture,
                    material.useOcclusionTexture,
                    material.useEmissiveTexture};
        }
    }
}

WorldRenderSyncResult WorldRenderSceneBridge::SynchronizeToRenderScene(
    const Engine::World& world,
    const Asset::AssetRegistry& assetRegistry,
    RenderScene& scene,
    const WorldRenderSyncOptions& options)
{
    WorldRenderSyncResult result{};
    result.appliedChanges = options.committedChanges;
    std::map<Engine::EntityId, RenderObject> existingObjects;
    std::vector<RenderObject> editorOnlyObjects;
    for (const RenderObject& object : scene.GetRenderObjects())
    {
        if (object.editorOnly)
        {
            editorOnlyObjects.push_back(object);
            continue;
        }
        if (object.entityId.IsValid())
        {
            existingObjects.emplace(object.entityId, object);
        }
    }

    std::vector<RenderObject> synchronizedObjects;
    std::array<PointLight, RenderScene::MaxPointLights> synchronizedPointLights{};
    std::uint32_t pointLightCount = 0;
    std::array<
        SpotLight,
        RenderScene::MaxSpotLights>
        synchronizedSpotLights{};
    std::uint32_t spotLightCount = 0;
    bool cameraSynchronized = false;
    bool directionalLightSynchronized = false;
    std::optional<DirectionalLight> fallbackDirectionalLight;
    std::array<DirectionalLight,
        RenderScene::MaxAuxiliaryDirectionalLights>
        synchronizedAuxiliaryDirectionalLights{};
    for (DirectionalLight& light :
         synchronizedAuxiliaryDirectionalLights)
    {
        light.intensity = 0.0f;
    }

    for (const auto& [id, entity] : world.GetEntities())
    {
        if (options.synchronizeCamera && !cameraSynchronized && entity.camera.has_value())
        {
            Camera& camera = scene.GetCamera();
            camera.SetPerspective(
                entity.camera->fieldOfViewY,
                camera.GetAspectRatio(),
                entity.camera->nearPlane,
                entity.camera->farPlane);
            camera.SetWorldPosition(
                entity.transform.position);
            camera.SetRotation(entity.transform.rotation.x, entity.transform.rotation.y);
            cameraSynchronized = true;
        }
        if (options.synchronizeLights
            && entity.directionalLight.has_value())
        {
            DirectionalLight light{};
            light.direction = ToRender(entity.directionalLight->direction);
            light.color = ToRender(entity.directionalLight->color);
            light.intensity = entity.directionalLight->intensity;
            const std::string_view entityName = entity.name.value;
            bool auxiliaryMatched = false;
            for (std::size_t auxiliaryIndex = 0;
                 auxiliaryIndex < AuxiliaryDirectionalLightNames.size();
                 ++auxiliaryIndex)
            {
                if (entityName
                    == AuxiliaryDirectionalLightNames[auxiliaryIndex])
                {
                    synchronizedAuxiliaryDirectionalLights[
                        auxiliaryIndex] = light;
                    auxiliaryMatched = true;
                    break;
                }
            }
            if (!auxiliaryMatched
                && entityName == PrimaryDirectionalLightName)
            {
                scene.GetDirectionalLight() = light;
                directionalLightSynchronized = true;
            }
            else if (!auxiliaryMatched
                     && !fallbackDirectionalLight.has_value())
            {
                // Legacy worlds may contain a differently named primary
                // directional light. Preserve that compatibility without
                // allowing the two named fill lights to replace the sun.
                fallbackDirectionalLight = light;
            }
        }
        if (options.synchronizeLights && entity.pointLight.has_value()
            && pointLightCount < RenderScene::MaxPointLights)
        {
            PointLight light{};
            light.position = ToRender(entity.transform.position);
            light.color = ToRender(entity.pointLight->color);
            light.intensity = entity.pointLight->intensity;
            light.range = entity.pointLight->range;
            light.castsShadow =
                entity.pointLight->castsShadow;
            synchronizedPointLights[pointLightCount++] = light;
        }
        if (options.synchronizeLights
            && entity.spotLight.has_value()
            && spotLightCount
                < RenderScene::MaxSpotLights)
        {
            SpotLight light{};
            light.position =
                ToRender(entity.transform.position);
            light.direction =
                ToRender(entity.spotLight->direction);
            light.color =
                ToRender(entity.spotLight->color);
            light.intensity =
                entity.spotLight->intensity;
            light.range = entity.spotLight->range;
            light.innerAngleRadians =
                entity.spotLight
                    ->innerAngleRadians;
            light.outerAngleRadians =
                entity.spotLight
                    ->outerAngleRadians;
            light.castsShadow =
                entity.spotLight->castsShadow;
            synchronizedSpotLights[
                spotLightCount++] = light;
        }
        if (!entity.meshRenderer.has_value())
        {
            continue;
        }

        RenderObject renderObject{};
        renderObject.entityId = id;
        renderObject.name = entity.name.value;
        renderObject.visible = entity.meshRenderer->visible;
        renderObject.transform.SetWorldPosition(
            entity.transform.position);
        renderObject.transform.SetRotationEulerRadians(ToRender(entity.transform.rotation));
        renderObject.transform.SetScale(ToRender(entity.transform.scale));

        if (entity.materialOverride.has_value())
        {
            const Engine::MaterialOverrideComponent& material =
                *entity.materialOverride;
            renderObject.hasMaterialOverride = true;
            renderObject.materialOverride = {
                {material.albedoColor.x, material.albedoColor.y,
                 material.albedoColor.z, material.albedoColor.w},
                ToRender(material.emissiveColor),
                material.metallic,
                material.roughness,
                material.occlusionStrength,
                material.normalScale,
                material.emissiveStrength,
                material.alphaCutoff,
                material.alphaMode,
                material.useAlbedoTexture,
                material.useMetallicRoughnessTexture,
                material.useNormalTexture,
                material.useOcclusionTexture,
                material.useEmissiveTexture};
            renderObject.materialOverrideSignature =
                HashMaterialOverride(material);
        }

        renderObject.meshHandle = assetRegistry.FindMeshByPath(entity.meshRenderer->meshAsset);
        renderObject.materialHandle = assetRegistry.FindMaterialByPath(entity.meshRenderer->materialAsset);
        if (!renderObject.meshHandle.IsValid())
        {
            if (const auto legacy = ParseLegacyHandlePath(entity.meshRenderer->meshAsset, "mesh"))
            {
                renderObject.meshHandle = Asset::MeshHandle(*legacy);
            }
        }
        if (!renderObject.materialHandle.IsValid())
        {
            if (const auto legacy = ParseLegacyHandlePath(entity.meshRenderer->materialAsset, "material"))
            {
                renderObject.materialHandle = Asset::MaterialHandle(*legacy);
            }
        }

        const auto existing = existingObjects.find(id);
        renderObject.mesh = assetRegistry.GetRuntimeMesh(renderObject.meshHandle);
        renderObject.material = assetRegistry.GetRuntimeMaterial(renderObject.materialHandle);
        if (renderObject.mesh == nullptr && existing != existingObjects.end())
        {
            renderObject.mesh = existing->second.mesh;
            renderObject.meshHandle = existing->second.meshHandle;
        }
        if (renderObject.material == nullptr && existing != existingObjects.end())
        {
            renderObject.material = existing->second.material;
            renderObject.materialHandle = existing->second.materialHandle;
        }
        if (existing != existingObjects.end())
        {
            // Built-in feature Labs attach renderer-only metadata. Preserve it
            // while their objects are synchronized through Engine::World.
            renderObject.surfaceType =
                existing->second.surfaceType;
            renderObject.surfaceLodLevel =
                existing->second.surfaceLodLevel;
            renderObject.quadtreePatch =
                existing->second.quadtreePatch;
            renderObject.gpuVisibilityReason =
                existing->second.gpuVisibilityReason;
            renderObject.editorOnly =
                existing->second.editorOnly;
        }
        if (renderObject.mesh == nullptr)
        {
            result.unresolvedAssets.push_back({
                id.ToString(),
                entity.name.value,
                "mesh",
                entity.meshRenderer->meshAsset});
        }
        if (renderObject.material == nullptr)
        {
            result.unresolvedAssets.push_back({
                id.ToString(),
                entity.name.value,
                "material",
                entity.meshRenderer->materialAsset});
        }
        synchronizedObjects.push_back(std::move(renderObject));
    }

    // Helpers have no Engine entity by design, but remain stable across every
    // World -> RenderScene synchronization for Scene View diagnostics.
    synchronizedObjects.insert(
        synchronizedObjects.end(),
        std::make_move_iterator(editorOnlyObjects.begin()),
        std::make_move_iterator(editorOnlyObjects.end()));

    scene.ReplaceRenderObjects(std::move(synchronizedObjects));
    if (options.synchronizeLights)
    {
        if (!directionalLightSynchronized
            && fallbackDirectionalLight.has_value())
        {
            scene.GetDirectionalLight() =
                *fallbackDirectionalLight;
        }
        scene.GetAuxiliaryDirectionalLights() =
            synchronizedAuxiliaryDirectionalLights;
        scene.GetPointLights() = synchronizedPointLights;
        scene.SetActivePointLightCount(pointLightCount);
        scene.GetSpotLights() =
            synchronizedSpotLights;
        scene.SetActiveSpotLightCount(
            spotLightCount);
    }
    result.renderObjectCount = scene.GetRenderObjects().size();
    return result;
}
} // namespace Prism::Scene
