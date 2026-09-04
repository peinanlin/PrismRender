#pragma once

#include "Engine/Components.h"

#include <map>
#include <optional>
#include <string>

namespace Prism::Engine
{
struct EntityRecord
{
    EntityId id;
    NameComponent name;
    TransformComponent transform;
    std::optional<HierarchyComponent> hierarchy;
    std::optional<MeshRendererComponent> meshRenderer;
    std::optional<MaterialOverrideComponent> materialOverride;
    std::optional<CameraComponent> camera;
    std::optional<DirectionalLightComponent> directionalLight;
    std::optional<PointLightComponent> pointLight;
    std::optional<SpotLightComponent> spotLight;
};

class World
{
public:
    explicit World(std::uint64_t idSeed = 0x505249534d454e47ull);

    EntityRecord& CreateEntity(
        std::string name = "Entity",
        std::optional<EntityId> requestedId = std::nullopt);
    bool DestroyEntity(EntityId id);
    void Clear();

    [[nodiscard]] EntityRecord* FindEntity(EntityId id);
    [[nodiscard]] const EntityRecord* FindEntity(EntityId id) const;
    [[nodiscard]] const std::map<EntityId, EntityRecord>& GetEntities() const;
    [[nodiscard]] std::size_t GetEntityCount() const;

    void RestoreIdGenerator(std::uint64_t seed, std::uint64_t counter);
    [[nodiscard]] std::uint64_t GetIdSeed() const;
    [[nodiscard]] std::uint64_t GetIdCounter() const;

private:
    EntityIdGenerator m_idGenerator;
    std::map<EntityId, EntityRecord> m_entities;
};
} // namespace Prism::Engine
