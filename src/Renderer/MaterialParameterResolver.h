#pragma once

#include "Renderer/SharedRenderData.h"

#include <cstddef>
#include <cstdint>

namespace Prism::Scene
{
struct RenderObject;
class RenderSceneView;
}

namespace Prism::Renderer
{
enum class MaterialRenderQueue
{
    Opaque,
    AlphaMask,
    Transparent
};

[[nodiscard]] SharedMaterialConstants ResolveMaterialConstants(
    const Scene::RenderObject& renderObject);
// Derive batching/resource identity from the live override values. Factory
// objects do not pass through World synchronization before first render.
[[nodiscard]] std::uint64_t ResolveMaterialOverrideSignature(
    const Scene::RenderObject& renderObject);
[[nodiscard]] MaterialRenderQueue ResolveMaterialRenderQueue(
    const Scene::RenderObject& renderObject);
[[nodiscard]] bool IsDoubleSidedMaterial(
    const Scene::RenderObject& renderObject);
[[nodiscard]] SharedMaterialConstants ResolveMaterialConstants(
    const Scene::RenderSceneView& scene,
    std::size_t objectIndex);
[[nodiscard]] MaterialRenderQueue ResolveMaterialRenderQueue(
    const Scene::RenderSceneView& scene,
    std::size_t objectIndex);
[[nodiscard]] bool IsDoubleSidedMaterial(
    const Scene::RenderSceneView& scene,
    std::size_t objectIndex);
}
