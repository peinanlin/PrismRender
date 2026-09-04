#include "Engine/World.h"

#include <stdexcept>
#include <utility>

namespace Prism::Engine
{
World::World(const std::uint64_t idSeed)
    : m_idGenerator(idSeed)
{
}

EntityRecord& World::CreateEntity(std::string name, const std::optional<EntityId> requestedId)
{
    EntityId id = requestedId.value_or(EntityId{});
    if (requestedId.has_value() && (!id.IsValid() || m_entities.contains(id)))
    {
        throw std::runtime_error("The requested entity UUID is invalid or already exists.");
    }
    while (!id.IsValid() || m_entities.contains(id))
    {
        id = m_idGenerator.Generate();
    }

    EntityRecord entity{};
    entity.id = id;
    entity.name.value = std::move(name);
    return m_entities.emplace(id, std::move(entity)).first->second;
}

bool World::DestroyEntity(const EntityId id)
{
    if (m_entities.erase(id) == 0)
    {
        return false;
    }
    for (auto& [childId, entity] : m_entities)
    {
        (void)childId;
        if (entity.hierarchy.has_value() && entity.hierarchy->parent == id)
        {
            entity.hierarchy->parent.reset();
        }
    }
    return true;
}

void World::Clear()
{
    m_entities.clear();
}

EntityRecord* World::FindEntity(const EntityId id)
{
    const auto found = m_entities.find(id);
    return found == m_entities.end() ? nullptr : &found->second;
}

const EntityRecord* World::FindEntity(const EntityId id) const
{
    const auto found = m_entities.find(id);
    return found == m_entities.end() ? nullptr : &found->second;
}

const std::map<EntityId, EntityRecord>& World::GetEntities() const { return m_entities; }
std::size_t World::GetEntityCount() const { return m_entities.size(); }

void World::RestoreIdGenerator(const std::uint64_t seed, const std::uint64_t counter)
{
    m_idGenerator.Reset(seed, counter);
}

std::uint64_t World::GetIdSeed() const { return m_idGenerator.GetSeed(); }
std::uint64_t World::GetIdCounter() const { return m_idGenerator.GetCounter(); }
} // namespace Prism::Engine
