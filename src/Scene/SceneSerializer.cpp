#include "Scene/SceneSerializer.h"

#include "Asset/AssetRegistry.h"
#include "Scene/Camera.h"
#include "Scene/RenderObject.h"
#include "Scene/RenderScene.h"

#include <DirectXMath.h>

#include <algorithm>
#include <fstream>
#include <stdexcept>
#include <utility>
#include <vector>

#include <json.hpp>

namespace Prism::Scene
{
using json = nlohmann::json;
using namespace DirectX;

namespace
{
constexpr std::uint32_t CurrentSceneVersion = 1;

json ToJson(const XMFLOAT3& value)
{
    return json::array({value.x, value.y, value.z});
}

json ToJson(const Core::Double3& value)
{
    return json::array({value.x, value.y, value.z});
}

XMFLOAT3 ReadFloat3(const json& value, const char* fieldName)
{
    if (!value.is_array() || value.size() != 3)
    {
        throw std::runtime_error(std::string(fieldName) + " must contain exactly three numbers.");
    }
    return {
        value.at(0).get<float>(),
        value.at(1).get<float>(),
        value.at(2).get<float>()};
}

Core::Double3 ReadDouble3(
    const json& value,
    const char* fieldName)
{
    if (!value.is_array() || value.size() != 3)
    {
        throw std::runtime_error(
            std::string(fieldName)
            + " must contain exactly three numbers.");
    }
    return {
        value.at(0).get<double>(),
        value.at(1).get<double>(),
        value.at(2).get<double>()};
}

json SerializeAssetReference(const std::string& path, const std::uint32_t handle)
{
    return {{"path", path}, {"handle", handle}};
}

Asset::MeshHandle ResolveMesh(const json& reference, const Asset::AssetRegistry& registry)
{
    const std::string path = reference.value("path", std::string{});
    Asset::MeshHandle handle = path.empty() ? Asset::MeshHandle{} : registry.FindMeshByPath(path);
    if (!handle.IsValid())
    {
        handle = Asset::MeshHandle(reference.value("handle", 0u));
    }
    return handle;
}

Asset::MaterialHandle ResolveMaterial(const json& reference, const Asset::AssetRegistry& registry)
{
    const std::string path = reference.value("path", std::string{});
    Asset::MaterialHandle handle = path.empty() ? Asset::MaterialHandle{} : registry.FindMaterialByPath(path);
    if (!handle.IsValid())
    {
        handle = Asset::MaterialHandle(reference.value("handle", 0u));
    }
    return handle;
}
} // namespace

bool SceneSerializer::Save(
    const std::filesystem::path& path,
    const RenderScene& scene,
    const Asset::AssetRegistry& assetRegistry,
    std::string* outMessage)
{
    try
    {
        json root;
        root["format"] = "PrismRenderScene";
        root["version"] = CurrentSceneVersion;

        const Camera& camera = scene.GetCamera();
        root["camera"] = {
            {"position", ToJson(camera.GetWorldPosition())},
            {"pitch", camera.GetPitch()},
            {"yaw", camera.GetYaw()},
            {"fieldOfViewY", camera.GetFieldOfViewYRadians()},
            {"nearPlane", camera.GetNearPlane()},
            {"farPlane", camera.GetFarPlane()}};

        const DirectionalLight& directionalLight = scene.GetDirectionalLight();
        root["directionalLight"] = {
            {"direction", ToJson(directionalLight.direction)},
            {"color", ToJson(directionalLight.color)},
            {"intensity", directionalLight.intensity}};

        json pointLights = json::array();
        for (std::uint32_t lightIndex = 0; lightIndex < scene.GetActivePointLightCount(); ++lightIndex)
        {
            const PointLight& light = scene.GetPointLights()[lightIndex];
            pointLights.push_back({
                {"position", ToJson(light.position)},
                {"color", ToJson(light.color)},
                {"intensity", light.intensity},
                {"range", light.range}});
        }
        root["pointLights"] = std::move(pointLights);

        json objects = json::array();
        for (const RenderObject& object : scene.GetRenderObjects())
        {
            objects.push_back({
                {"name", object.name},
                {"visible", object.visible},
                {"mesh", SerializeAssetReference(assetRegistry.GetMeshPath(object.meshHandle), object.meshHandle.Value())},
                {"material", SerializeAssetReference(assetRegistry.GetMaterialPath(object.materialHandle), object.materialHandle.Value())},
                {"transform", {
                    {"position", ToJson(object.transform.GetWorldPosition())},
                    {"rotation", ToJson(object.transform.GetRotationEulerRadians())},
                    {"scale", ToJson(object.transform.GetScale())}}}});
        }
        root["objects"] = std::move(objects);

        if (!path.parent_path().empty())
        {
            std::filesystem::create_directories(path.parent_path());
        }
        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        if (!output)
        {
            throw std::runtime_error("Could not open the scene file for writing.");
        }
        output << root.dump(2) << '\n';
        if (!output)
        {
            throw std::runtime_error("Failed while writing the scene file.");
        }

        if (outMessage != nullptr)
        {
            *outMessage = "Scene saved: " + path.string();
        }
        return true;
    }
    catch (const std::exception& exception)
    {
        if (outMessage != nullptr)
        {
            *outMessage = "Scene save failed: " + std::string(exception.what());
        }
        return false;
    }
}

bool SceneSerializer::Load(
    const std::filesystem::path& path,
    RenderScene& scene,
    const Asset::AssetRegistry& assetRegistry,
    std::string* outMessage)
{
    try
    {
        std::ifstream input(path, std::ios::binary);
        if (!input)
        {
            throw std::runtime_error("Could not open the scene file for reading.");
        }

        json root;
        input >> root;
        if (root.value("format", std::string{}) != "PrismRenderScene")
        {
            throw std::runtime_error("The file is not a PrismRender scene.");
        }
        if (root.value("version", 0u) != CurrentSceneVersion)
        {
            throw std::runtime_error("The scene version is not supported.");
        }

        const json& cameraJson = root.at("camera");
        const Core::Double3 cameraPosition =
            ReadDouble3(
                cameraJson.at("position"),
                "camera.position");
        const float cameraPitch = cameraJson.at("pitch").get<float>();
        const float cameraYaw = cameraJson.at("yaw").get<float>();
        const float fieldOfViewY = cameraJson.at("fieldOfViewY").get<float>();
        const float nearPlane = cameraJson.at("nearPlane").get<float>();
        const float farPlane = cameraJson.at("farPlane").get<float>();

        const json& directionalJson = root.at("directionalLight");
        DirectionalLight directionalLight{};
        directionalLight.direction = ReadFloat3(directionalJson.at("direction"), "directionalLight.direction");
        directionalLight.color = ReadFloat3(directionalJson.at("color"), "directionalLight.color");
        directionalLight.intensity = directionalJson.at("intensity").get<float>();

        std::vector<PointLight> pointLights;
        for (const json& pointLightJson : root.at("pointLights"))
        {
            if (pointLights.size() >= RenderScene::MaxPointLights)
            {
                break;
            }
            PointLight light{};
            light.position = ReadFloat3(pointLightJson.at("position"), "pointLight.position");
            light.color = ReadFloat3(pointLightJson.at("color"), "pointLight.color");
            light.intensity = pointLightJson.at("intensity").get<float>();
            light.range = pointLightJson.at("range").get<float>();
            pointLights.push_back(light);
        }

        std::vector<RenderObject> objects;
        for (const json& objectJson : root.at("objects"))
        {
            RenderObject object{};
            object.name = objectJson.at("name").get<std::string>();
            object.visible = objectJson.value("visible", true);
            object.meshHandle = ResolveMesh(objectJson.at("mesh"), assetRegistry);
            object.materialHandle = ResolveMaterial(objectJson.at("material"), assetRegistry);
            object.mesh = assetRegistry.GetRuntimeMesh(object.meshHandle);
            object.material = assetRegistry.GetRuntimeMaterial(object.materialHandle);
            if (object.mesh == nullptr || object.material == nullptr)
            {
                throw std::runtime_error("Scene object references an unavailable asset: " + object.name);
            }

            const json& transformJson = objectJson.at("transform");
            object.transform.SetWorldPosition(
                ReadDouble3(
                    transformJson.at("position"),
                    "transform.position"));
            object.transform.SetRotationEulerRadians(ReadFloat3(transformJson.at("rotation"), "transform.rotation"));
            object.transform.SetScale(ReadFloat3(transformJson.at("scale"), "transform.scale"));
            objects.push_back(std::move(object));
        }

        Camera& camera = scene.GetCamera();
        camera.SetPerspective(fieldOfViewY, camera.GetAspectRatio(), nearPlane, farPlane);
        camera.SetWorldPosition(cameraPosition);
        camera.SetRotation(cameraPitch, cameraYaw);
        scene.GetDirectionalLight() = directionalLight;
        scene.SetActivePointLightCount(static_cast<std::uint32_t>(pointLights.size()));
        for (std::size_t lightIndex = 0; lightIndex < pointLights.size(); ++lightIndex)
        {
            scene.GetPointLights()[lightIndex] = pointLights[lightIndex];
        }
        scene.ReplaceRenderObjects(std::move(objects));

        if (outMessage != nullptr)
        {
            *outMessage = "Scene loaded: " + path.string();
        }
        return true;
    }
    catch (const std::exception& exception)
    {
        if (outMessage != nullptr)
        {
            *outMessage = "Scene load failed: " + std::string(exception.what());
        }
        return false;
    }
}
} // namespace Prism::Scene
