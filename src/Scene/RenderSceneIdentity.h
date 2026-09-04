#pragma once

#include "Engine/EntityId.h"

#include <compare>
#include <cstddef>
#include <cstdint>

namespace Prism::Scene
{
struct SceneGeneration
{
    std::uint64_t value = 0;
    [[nodiscard]] explicit operator bool() const
    {
        return value != 0;
    }
    auto operator<=>(const SceneGeneration&) const = default;
};

struct RenderSceneDataRevision
{
    std::uint64_t value = 0;
    [[nodiscard]] explicit operator bool() const
    {
        return value != 0;
    }
    auto operator<=>(const RenderSceneDataRevision&) const = default;
};

enum class RenderObjectIdDomain : std::uint8_t
{
    WorldEntity,
    Programmatic
};

// IDs are stable within a SceneGeneration. World IDs preserve EntityId;
// programmatic IDs use the generation plus a registry-assigned counter.
struct RenderObjectId
{
    RenderObjectIdDomain domain =
        RenderObjectIdDomain::Programmatic;
    std::uint64_t high = 0;
    std::uint64_t low = 0;

    [[nodiscard]] bool IsValid() const
    {
        return high != 0 || low != 0;
    }
    auto operator<=>(const RenderObjectId&) const = default;

    [[nodiscard]] static RenderObjectId FromEntityId(
        const Engine::EntityId& entityId)
    {
        return {
            RenderObjectIdDomain::WorldEntity,
            entityId.high,
            entityId.low};
    }
};

struct RenderObjectIdHash
{
    [[nodiscard]] std::size_t operator()(
        const RenderObjectId& id) const noexcept;
};
} // namespace Prism::Scene
