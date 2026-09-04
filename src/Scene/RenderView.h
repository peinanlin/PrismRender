#pragma once

#include "Scene/Camera.h"

#include <compare>
#include <cstdint>
#include <memory>
#include <vector>

namespace Prism::Scene
{
struct RenderViewId
{
    std::uint64_t value = 0;
    [[nodiscard]] explicit operator bool() const
    {
        return value != 0;
    }
    auto operator<=>(const RenderViewId&) const = default;
};

struct RenderViewSelection
{
    std::uint64_t revision = 0;
    // Indices refer to immutable RenderSceneData storage. Filtering a view
    // never erases shared objects or changes parent/instance indices.
    std::vector<std::uint32_t> objectIndices;
};

inline constexpr RenderViewId GameRenderViewId{1};
inline constexpr RenderViewId SceneRenderViewId{2};

enum class RenderViewHistoryInvalidation : std::uint32_t
{
    None = 0,
    Initialization = 1u << 0u,
    Extent = 1u << 1u,
    Settings = 1u << 2u,
    CameraCut = 1u << 3u,
    Scene = 1u << 4u
};

constexpr RenderViewHistoryInvalidation operator|(
    const RenderViewHistoryInvalidation left,
    const RenderViewHistoryInvalidation right) noexcept
{
    return static_cast<RenderViewHistoryInvalidation>(
        static_cast<std::uint32_t>(left)
        | static_cast<std::uint32_t>(right));
}

constexpr bool HasRenderViewHistoryInvalidation(
    const RenderViewHistoryInvalidation value,
    const RenderViewHistoryInvalidation flag) noexcept
{
    return (static_cast<std::uint32_t>(value)
        & static_cast<std::uint32_t>(flag)) != 0;
}

// Per-view dynamic values. Cameras and history revisions are deliberately not
// stored in RenderSceneData, so Game and Scene views cannot overwrite each
// other's temporal state.
struct RenderView
{
    RenderViewId id;
    Camera camera;
    Camera previousCamera;
    bool previousCameraValid = false;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint64_t cameraRevision = 0;
    std::uint64_t historyRevision = 0;
    std::uint64_t cameraCutRevision = 0;
    RenderViewHistoryInvalidation historyInvalidation =
        RenderViewHistoryInvalidation::None;
    std::shared_ptr<const RenderViewSelection> selection;
};
} // namespace Prism::Scene
