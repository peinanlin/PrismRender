#include "Scene/AssetStreamingSceneBridge.h"

#include "Asset/AssetRegistry.h"
#include "Asset/AssetStreamingManager.h"
#include "Scene/RenderObject.h"
#include "Scene/RenderScene.h"

#include <fstream>
#include <stdexcept>
#include <vector>

#include <json.hpp>

namespace Prism::Scene
{
AssetStreamingSceneActivationResult
AssetStreamingSceneBridge::Activate(
    const std::string_view sceneAssetIdOrPath,
    Asset::AssetStreamingManager& manager,
    Asset::AssetRegistry& registry,
    RenderScene& scene,
    const bool clearExistingObjects,
    const DirectX::XMFLOAT3& rootOffset)
{
    AssetStreamingSceneActivationResult result{};
    const auto sceneEntry =
        manager.Find(sceneAssetIdOrPath);
    if (!sceneEntry.has_value())
    {
        result.success = false;
        result.errorCode =
            "streaming_scene_not_found";
        result.errorMessage =
            "The requested streaming Scene is not in the Asset Manifest.";
        return result;
    }
    if (sceneEntry->state
        != Asset::AssetResidencyState::Resident)
    {
        result.ready = false;
        return result;
    }

    std::vector<Asset::AssetStreamingSceneInstance>
        instances;
    std::string queryError;
    if (!manager.GetResidentSceneInstances(
            sceneAssetIdOrPath,
            instances,
            &queryError))
    {
        result.success = false;
        result.errorCode =
            "streaming_scene_metadata_invalid";
        result.errorMessage =
            queryError.empty()
            ? "The resident streaming Scene could not be resolved."
            : queryError;
        return result;
    }

    std::vector<RenderObject> objects;
    objects.reserve(instances.size());
    for (const Asset::AssetStreamingSceneInstance&
             instance : instances)
    {
        const Asset::MeshHandle meshHandle =
            registry.FindMeshByPath(
                instance.meshAssetPath);
        const Asset::MaterialHandle materialHandle =
            registry.FindMaterialByPath(
                instance.materialAssetPath);
        std::shared_ptr<Asset::Mesh> mesh =
            registry.GetRuntimeMesh(meshHandle);
        std::shared_ptr<Asset::Material> material =
            registry.GetRuntimeMaterial(
                materialHandle);
        if (!meshHandle.IsValid()
            || !materialHandle.IsValid()
            || mesh == nullptr
            || material == nullptr)
        {
            result.success = false;
            result.errorCode =
                "streaming_scene_dependency_unavailable";
            result.errorMessage =
                "A resident Scene instance does not have a runtime Mesh or Material.";
            return result;
        }

        RenderObject object{};
        object.name = instance.name;
        object.meshHandle = meshHandle;
        object.materialHandle = materialHandle;
        object.mesh = std::move(mesh);
        object.material = std::move(material);
        const DirectX::XMFLOAT4X4 worldMatrix{
            instance.worldMatrix[0],
            instance.worldMatrix[1],
            instance.worldMatrix[2],
            instance.worldMatrix[3],
            instance.worldMatrix[4],
            instance.worldMatrix[5],
            instance.worldMatrix[6],
            instance.worldMatrix[7],
            instance.worldMatrix[8],
            instance.worldMatrix[9],
            instance.worldMatrix[10],
            instance.worldMatrix[11],
            instance.worldMatrix[12],
            instance.worldMatrix[13],
            instance.worldMatrix[14],
            instance.worldMatrix[15]};
        if (!object.transform.SetWorldMatrix(
                worldMatrix))
        {
            result.success = false;
            result.errorCode =
                "streaming_scene_transform_invalid";
            result.errorMessage =
                "A streamed Scene instance contains a non-decomposable world matrix.";
            return result;
        }
        DirectX::XMFLOAT3 position =
            object.transform.GetPosition();
        position.x += rootOffset.x;
        position.y += rootOffset.y;
        position.z += rootOffset.z;
        object.transform.SetPosition(position);
        objects.push_back(std::move(object));
    }

    if (clearExistingObjects)
    {
        scene.ClearRenderObjects();
    }
    for (RenderObject& object : objects)
    {
        scene.AddRenderObject(std::move(object));
    }
    result.ready = true;
    result.topologyChanged = true;
    result.renderObjectCount =
        objects.size();
    return result;
}

bool AssetStreamingSceneBridge::WriteReport(
    const std::filesystem::path& path,
    const std::string_view graphicsApi,
    const std::string_view sceneAssetIdOrPath,
    const bool activationAttempted,
    const bool activationSucceeded,
    const std::size_t activatedObjectCount,
    const std::string_view errorCode,
    const std::string_view errorMessage,
    const Asset::AssetRegistry& registry,
    const RenderScene& scene,
    std::string* outError)
{
    if (path.empty())
    {
        if (outError != nullptr)
        {
            *outError =
                "Streaming Scene report path is empty.";
        }
        return false;
    }

    try
    {
        nlohmann::json objects =
            nlohmann::json::array();
        std::size_t streamedBindingCount = 0;
        for (const RenderObject& object :
             scene.GetRenderObjects())
        {
            const std::string meshPath =
                object.meshHandle.IsValid()
                ? registry.GetMeshPath(
                    object.meshHandle)
                : std::string{};
            const std::string materialPath =
                object.materialHandle.IsValid()
                ? registry.GetMaterialPath(
                    object.materialHandle)
                : std::string{};
            const bool streamedBinding =
                meshPath.starts_with(
                    "prism-asset://")
                && materialPath.starts_with(
                    "prism-asset://");
            if (streamedBinding)
            {
                ++streamedBindingCount;
            }
            objects.push_back({
                {"name", object.name},
                {"meshPath", meshPath},
                {"materialPath", materialPath},
                {"meshRuntimeReady",
                 object.mesh != nullptr},
                {"materialRuntimeReady",
                 object.material != nullptr},
                {"streamedBinding",
                 streamedBinding}});
        }

        const nlohmann::json report = {
            {"format",
             "PrismAssetStreamingSceneReport"},
            {"version", 1},
            {"graphicsApi", graphicsApi},
            {"sceneAsset", sceneAssetIdOrPath},
            {"activationAttempted",
             activationAttempted},
            {"activationSucceeded",
             activationSucceeded},
            {"activatedObjectCount",
             activatedObjectCount},
            {"renderObjectCount",
             scene.GetRenderObjects().size()},
            {"streamedBindingCount",
             streamedBindingCount},
            {"error", {
                {"code", errorCode},
                {"message", errorMessage}}},
            {"objects", std::move(objects)}};

        if (!path.parent_path().empty())
        {
            std::filesystem::create_directories(
                path.parent_path());
        }
        std::ofstream stream(
            path,
            std::ios::binary | std::ios::trunc);
        if (!stream)
        {
            throw std::runtime_error(
                "Could not open Streaming Scene report.");
        }
        stream << report.dump(2) << '\n';
        if (!stream)
        {
            throw std::runtime_error(
                "Could not write Streaming Scene report.");
        }
        return true;
    }
    catch (const std::exception& exception)
    {
        if (outError != nullptr)
        {
            *outError = exception.what();
        }
        return false;
    }
}
} // namespace Prism::Scene
