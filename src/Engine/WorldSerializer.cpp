#include "Engine/WorldSerializer.h"

#include <fstream>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <set>
#include <utility>

namespace Prism::Engine
{
namespace
{
using json = nlohmann::json;

json ToJson(const Float3& value)
{
    return json::array({value.x, value.y, value.z});
}

json ToJson(const Core::Double3& value)
{
    return json::array({value.x, value.y, value.z});
}

json ToJson(const Float4& value)
{
    return json::array({value.x, value.y, value.z, value.w});
}

Float3 ReadFloat3(const json& value, const char* field)
{
    if (!value.is_array() || value.size() != 3)
    {
        throw std::runtime_error(std::string(field) + " must contain exactly three numbers.");
    }
    return {value.at(0).get<float>(), value.at(1).get<float>(), value.at(2).get<float>()};
}

Core::Double3 ReadDouble3(
    const json& value,
    const char* field)
{
    if (!value.is_array() || value.size() != 3)
    {
        throw std::runtime_error(
            std::string(field)
            + " must contain three numbers.");
    }
    return {
        value.at(0).get<double>(),
        value.at(1).get<double>(),
        value.at(2).get<double>()};
}

Float4 ReadFloat4(const json& value, const char* field)
{
    if (!value.is_array() || value.size() != 4)
    {
        throw std::runtime_error(std::string(field) + " must contain exactly four numbers.");
    }
    return {
        value.at(0).get<float>(), value.at(1).get<float>(),
        value.at(2).get<float>(), value.at(3).get<float>()};
}

EntityId ReadEntityId(const json& value, const char* field)
{
    if (!value.is_string())
    {
        throw std::runtime_error(std::string(field) + " must be a UUID string.");
    }
    const std::optional<EntityId> id = EntityId::Parse(value.get<std::string>());
    if (!id.has_value())
    {
        throw std::runtime_error(std::string(field) + " is not a valid UUID.");
    }
    return *id;
}

json MigrateVersion1(const json& source)
{
    json migrated;
    migrated["format"] = "PrismEngineWorld";
    migrated["version"] = WorldSerializer::CurrentVersion;
    migrated["idGenerator"] = source.value(
        "idGenerator",
        json{{"seed", 0x505249534d454e47ull}, {"counter", 0ull}});
    migrated["entities"] = json::array();

    for (const json& legacy : source.at("entities"))
    {
        json components;
        components["Name"] = {{"value", legacy.value("name", std::string("Entity"))}};
        components["Transform"] = {
            {"position", legacy.value("position", json::array({0.0f, 0.0f, 0.0f}))},
            {"rotation", legacy.value("rotation", json::array({0.0f, 0.0f, 0.0f}))},
            {"scale", legacy.value("scale", json::array({1.0f, 1.0f, 1.0f}))}};
        if (legacy.contains("parent") || legacy.contains("mesh") || legacy.contains("material"))
        {
            if (legacy.contains("parent"))
            {
                components["Hierarchy"] = {{"parent", legacy.at("parent")}};
            }
            if (legacy.contains("mesh") || legacy.contains("material"))
            {
                components["MeshRenderer"] = {
                    {"meshAsset", legacy.value("mesh", std::string{})},
                    {"materialAsset", legacy.value("material", std::string{})},
                    {"visible", legacy.value("visible", true)}};
            }
        }
        migrated["entities"].push_back({{"id", legacy.at("id")}, {"components", components}});
    }
    return migrated;
}

std::string ReadLegacyAssetPath(const json& reference, const char* type)
{
    const std::string path = reference.value("path", std::string{});
    if (!path.empty())
    {
        return path;
    }
    return std::string("prism-handle://") + type + '/' + std::to_string(reference.value("handle", 0u));
}

json MigrateRenderSceneVersion1(const json& source)
{
    constexpr std::uint64_t MigrationSeed = 0x505249534d53434eull;
    EntityIdGenerator ids(MigrationSeed);
    json migrated;
    migrated["format"] = "PrismEngineWorld";
    migrated["version"] = WorldSerializer::CurrentVersion;
    migrated["entities"] = json::array();

    const auto appendEntity = [&](std::string name, json components)
    {
        components["Name"] = {{"value", std::move(name)}};
        if (!components.contains("Transform"))
        {
            components["Transform"] = {
                {"position", json::array({0.0f, 0.0f, 0.0f})},
                {"rotation", json::array({0.0f, 0.0f, 0.0f})},
                {"scale", json::array({1.0f, 1.0f, 1.0f})}};
        }
        migrated["entities"].push_back({
            {"id", ids.Generate().ToString()},
            {"components", std::move(components)}});
    };

    if (source.contains("camera"))
    {
        const json& camera = source.at("camera");
        appendEntity("EditorCamera", {
            {"Transform", {
                {"position", camera.at("position")},
                {"rotation", json::array({camera.value("pitch", 0.0f), camera.value("yaw", 0.0f), 0.0f})},
                {"scale", json::array({1.0f, 1.0f, 1.0f})}}},
            {"Camera", {
                {"fieldOfViewY", camera.value("fieldOfViewY", 0.785398163f)},
                {"nearPlane", camera.value("nearPlane", 0.1f)},
                {"farPlane", camera.value("farPlane", 1000.0f)}}}});
    }

    if (source.contains("directionalLight"))
    {
        const json& light = source.at("directionalLight");
        appendEntity("DirectionalLight", {
            {"DirectionalLight", {
                {"direction", light.at("direction")},
                {"color", light.at("color")},
                {"intensity", light.value("intensity", 4.0f)}}}});
    }

    std::size_t pointLightIndex = 0;
    for (const json& light : source.value("pointLights", json::array()))
    {
        appendEntity("PointLight" + std::to_string(pointLightIndex++), {
            {"Transform", {
                {"position", light.at("position")},
                {"rotation", json::array({0.0f, 0.0f, 0.0f})},
                {"scale", json::array({1.0f, 1.0f, 1.0f})}}},
            {"PointLight", {
                {"color", light.at("color")},
                {"intensity", light.value("intensity", 1.0f)},
                {"range", light.value("range", 10.0f)}}}});
    }

    for (const json& object : source.value("objects", json::array()))
    {
        const json& transform = object.at("transform");
        appendEntity(object.value("name", std::string("RenderObject")), {
            {"Transform", {
                {"position", transform.at("position")},
                {"rotation", transform.at("rotation")},
                {"scale", transform.at("scale")}}},
            {"MeshRenderer", {
                {"meshAsset", ReadLegacyAssetPath(object.at("mesh"), "mesh")},
                {"materialAsset", ReadLegacyAssetPath(object.at("material"), "material")},
                {"visible", object.value("visible", true)}}}});
    }

    migrated["idGenerator"] = {{"seed", MigrationSeed}, {"counter", ids.GetCounter()}};
    return migrated;
}
} // namespace

nlohmann::json WorldSerializer::Serialize(const World& world)
{
    json root;
    root["format"] = "PrismEngineWorld";
    root["version"] = CurrentVersion;
    root["idGenerator"] = {{"seed", world.GetIdSeed()}, {"counter", world.GetIdCounter()}};
    root["entities"] = json::array();

    for (const auto& [id, entity] : world.GetEntities())
    {
        json components;
        components["Name"] = {{"value", entity.name.value}};
        components["Transform"] = {
            {"position", ToJson(entity.transform.position)},
            {"rotation", ToJson(entity.transform.rotation)},
            {"scale", ToJson(entity.transform.scale)}};
        if (entity.hierarchy.has_value())
        {
            components["Hierarchy"] = {
                {"parent", entity.hierarchy->parent.has_value()
                     ? json(entity.hierarchy->parent->ToString())
                     : json(nullptr)}};
        }
        if (entity.meshRenderer.has_value())
        {
            components["MeshRenderer"] = {
                {"meshAsset", entity.meshRenderer->meshAsset},
                {"materialAsset", entity.meshRenderer->materialAsset},
                {"visible", entity.meshRenderer->visible}};
        }
        if (entity.materialOverride.has_value())
        {
            const MaterialOverrideComponent& material = *entity.materialOverride;
            components["MaterialOverride"] = {
                {"albedoColor", ToJson(material.albedoColor)},
                {"emissiveColor", ToJson(material.emissiveColor)},
                {"metallic", material.metallic},
                {"roughness", material.roughness},
                {"occlusionStrength", material.occlusionStrength},
                {"normalScale", material.normalScale},
                {"emissiveStrength", material.emissiveStrength},
                {"alphaCutoff", material.alphaCutoff},
                {"alphaMode", material.alphaMode},
                {"useAlbedoTexture", material.useAlbedoTexture},
                {"useMetallicRoughnessTexture", material.useMetallicRoughnessTexture},
                {"useNormalTexture", material.useNormalTexture},
                {"useOcclusionTexture", material.useOcclusionTexture},
                {"useEmissiveTexture", material.useEmissiveTexture}};
        }
        if (entity.camera.has_value())
        {
            components["Camera"] = {
                {"fieldOfViewY", entity.camera->fieldOfViewY},
                {"nearPlane", entity.camera->nearPlane},
                {"farPlane", entity.camera->farPlane}};
        }
        if (entity.directionalLight.has_value())
        {
            components["DirectionalLight"] = {
                {"direction", ToJson(entity.directionalLight->direction)},
                {"color", ToJson(entity.directionalLight->color)},
                {"intensity", entity.directionalLight->intensity}};
        }
        if (entity.pointLight.has_value())
        {
            components["PointLight"] = {
                {"color", ToJson(entity.pointLight->color)},
                {"intensity", entity.pointLight->intensity},
                {"range", entity.pointLight->range},
                {"castsShadow", entity.pointLight->castsShadow}};
        }
        if (entity.spotLight.has_value())
        {
            components["SpotLight"] = {
                {"direction", ToJson(entity.spotLight->direction)},
                {"color", ToJson(entity.spotLight->color)},
                {"intensity", entity.spotLight->intensity},
                {"range", entity.spotLight->range},
                {"innerAngleRadians", entity.spotLight->innerAngleRadians},
                {"outerAngleRadians", entity.spotLight->outerAngleRadians},
                {"castsShadow", entity.spotLight->castsShadow}};
        }
        root["entities"].push_back({{"id", id.ToString()}, {"components", std::move(components)}});
    }
    return root;
}

void WorldSerializer::Deserialize(const nlohmann::json& document, World& world)
{
    const std::string format = document.value("format", std::string{});
    json root = document;
    if (format == "PrismRenderScene" && document.value("version", 0u) == 1u)
    {
        root = MigrateRenderSceneVersion1(document);
    }
    else if (format != "PrismEngineWorld")
    {
        throw std::runtime_error("The document is not a PrismEngine world.");
    }

    const std::uint32_t version = root.value("version", 0u);
    if (version == 1)
    {
        root = MigrateVersion1(root);
    }
    else if (version != CurrentVersion)
    {
        throw std::runtime_error("The PrismEngine world version is not supported.");
    }

    World loaded(root.at("idGenerator").value("seed", 0x505249534d454e47ull));
    for (const json& entityJson : root.at("entities"))
    {
        const EntityId id = ReadEntityId(entityJson.at("id"), "entity.id");
        const json& components = entityJson.at("components");
        EntityRecord& entity = loaded.CreateEntity(
            components.at("Name").value("value", std::string("Entity")), id);

        const json& transform = components.at("Transform");
        entity.transform.position = ReadDouble3(
            transform.at("position"),
            "Transform.position");
        entity.transform.rotation = ReadFloat3(transform.at("rotation"), "Transform.rotation");
        entity.transform.scale = ReadFloat3(transform.at("scale"), "Transform.scale");

        if (components.contains("Hierarchy"))
        {
            entity.hierarchy.emplace();
            const json& parent = components.at("Hierarchy").at("parent");
            if (!parent.is_null())
            {
                entity.hierarchy->parent = ReadEntityId(parent, "Hierarchy.parent");
            }
        }
        if (components.contains("MeshRenderer"))
        {
            const json& renderer = components.at("MeshRenderer");
            entity.meshRenderer = MeshRendererComponent{
                renderer.value("meshAsset", std::string{}),
                renderer.value("materialAsset", std::string{}),
                renderer.value("visible", true)};
        }
        if (components.contains("MaterialOverride"))
        {
            const json& material = components.at("MaterialOverride");
            entity.materialOverride = MaterialOverrideComponent{
                ReadFloat4(material.value("albedoColor", json::array({1.0f, 1.0f, 1.0f, 1.0f})), "MaterialOverride.albedoColor"),
                ReadFloat3(material.value("emissiveColor", json::array({0.0f, 0.0f, 0.0f})), "MaterialOverride.emissiveColor"),
                material.value("metallic", 0.0f),
                material.value("roughness", 0.5f),
                material.value("occlusionStrength", 1.0f),
                material.value("normalScale", 1.0f),
                material.value("emissiveStrength", 1.0f),
                material.value("alphaCutoff", 0.5f),
                material.value("alphaMode", 0.0f),
                material.value("useAlbedoTexture", true),
                material.value("useMetallicRoughnessTexture", false),
                material.value("useNormalTexture", false),
                material.value("useOcclusionTexture", false),
                material.value("useEmissiveTexture", false)};
        }
        if (components.contains("Camera"))
        {
            const json& camera = components.at("Camera");
            entity.camera = CameraComponent{
                camera.value("fieldOfViewY", 0.785398163f),
                camera.value("nearPlane", 0.1f),
                camera.value("farPlane", 1000.0f)};
        }
        if (components.contains("DirectionalLight"))
        {
            const json& light = components.at("DirectionalLight");
            entity.directionalLight = DirectionalLightComponent{
                ReadFloat3(light.at("direction"), "DirectionalLight.direction"),
                ReadFloat3(light.at("color"), "DirectionalLight.color"),
                light.value("intensity", 4.0f)};
        }
        if (components.contains("PointLight"))
        {
            const json& light = components.at("PointLight");
            entity.pointLight = PointLightComponent{
                ReadFloat3(light.at("color"), "PointLight.color"),
                light.value("intensity", 1.0f),
                light.value("range", 10.0f),
                light.value("castsShadow", true)};
        }
        if (components.contains("SpotLight"))
        {
            const json& light = components.at("SpotLight");
            entity.spotLight = SpotLightComponent{
                ReadFloat3(light.at("direction"), "SpotLight.direction"),
                ReadFloat3(light.at("color"), "SpotLight.color"),
                light.value("intensity", 1.0f),
                light.value("range", 10.0f),
                light.value("innerAngleRadians", 0.31415927f),
                light.value("outerAngleRadians", 0.48869219f),
                light.value("castsShadow", true)};
        }
    }

    for (const auto& [id, entity] : loaded.GetEntities())
    {
        if (entity.hierarchy.has_value() && entity.hierarchy->parent.has_value()
            && loaded.FindEntity(*entity.hierarchy->parent) == nullptr)
        {
            throw std::runtime_error("A Hierarchy component references a missing parent entity.");
        }
        std::set<EntityId> ancestors{id};
        std::optional<EntityId> parent = entity.hierarchy.has_value()
            ? entity.hierarchy->parent
            : std::nullopt;
        while (parent.has_value())
        {
            if (!ancestors.insert(*parent).second)
            {
                throw std::runtime_error("The world contains a Hierarchy cycle.");
            }
            const EntityRecord* parentEntity = loaded.FindEntity(*parent);
            parent = parentEntity != nullptr && parentEntity->hierarchy.has_value()
                ? parentEntity->hierarchy->parent
                : std::nullopt;
        }
    }
    loaded.RestoreIdGenerator(
        root.at("idGenerator").value("seed", 0x505249534d454e47ull),
        root.at("idGenerator").value("counter", 0ull));
    world = std::move(loaded);
}

bool WorldSerializer::Save(
    const std::filesystem::path& path,
    const World& world,
    std::string* outError)
{
    try
    {
        if (!path.parent_path().empty())
        {
            std::filesystem::create_directories(path.parent_path());
        }
        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        if (!output)
        {
            throw std::runtime_error("Could not open the world file for writing.");
        }
        output << Serialize(world).dump(2) << '\n';
        if (!output)
        {
            throw std::runtime_error("Failed while writing the world file.");
        }
        return true;
    }
    catch (const std::exception& exception)
    {
        if (outError != nullptr) *outError = exception.what();
        return false;
    }
}

bool WorldSerializer::Load(
    const std::filesystem::path& path,
    World& world,
    std::string* outError)
{
    try
    {
        std::ifstream input(path, std::ios::binary);
        if (!input)
        {
            throw std::runtime_error("Could not open the world file for reading.");
        }
        json document;
        input >> document;
        Deserialize(document, world);
        return true;
    }
    catch (const std::exception& exception)
    {
        if (outError != nullptr) *outError = exception.what();
        return false;
    }
}

std::string WorldSerializer::ComputeHash(const nlohmann::json& document)
{
    const std::string canonical = document.dump();
    std::uint64_t hash = 14695981039346656037ull;
    for (const unsigned char byte : canonical)
    {
        hash ^= byte;
        hash *= 1099511628211ull;
    }
    std::ostringstream output;
    output << std::hex << std::setfill('0') << std::setw(16) << hash;
    return output.str();
}

std::string WorldSerializer::ComputeHash(const World& world)
{
    return ComputeHash(Serialize(world));
}
} // namespace Prism::Engine
