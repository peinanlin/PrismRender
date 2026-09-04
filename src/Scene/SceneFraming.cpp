#include "Scene/SceneFraming.h"

#include "Asset/Mesh.h"
#include "Scene/RenderScene.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace Prism::Scene
{
std::optional<RenderSceneBounds> CalculateRenderObjectBounds(
    const RenderScene& scene)
{
    const std::vector<RenderObject>& renderObjects =
        scene.GetRenderObjects();
    const Core::Double3 renderOrigin =
        scene.GetCamera().GetWorldPosition();
    Core::Double3 boundsMin{
        std::numeric_limits<double>::max(),
        std::numeric_limits<double>::max(),
        std::numeric_limits<double>::max()};
    Core::Double3 boundsMax{
        std::numeric_limits<double>::lowest(),
        std::numeric_limits<double>::lowest(),
        std::numeric_limits<double>::lowest()};
    bool hasBounds = false;

    for (const RenderObject& renderObject :
         renderObjects)
    {
        if (!renderObject.visible
            || renderObject.editorOnly
            || renderObject.mesh == nullptr)
        {
            continue;
        }
        DirectX::XMFLOAT3 relativePosition{};
        DirectX::XMStoreFloat3(
            &relativePosition,
            DirectX::XMVector3TransformCoord(
                DirectX::XMLoadFloat3(
                    &renderObject.mesh->GetBoundsCenter()),
                renderObject.transform.GetRelativeWorldMatrix(
                    renderOrigin)));
        const Core::Double3 position{
            renderOrigin.x + relativePosition.x,
            renderOrigin.y + relativePosition.y,
            renderOrigin.z + relativePosition.z};
        const DirectX::XMFLOAT3& scale =
            renderObject.transform.GetScale();
        const float maxScale = std::max({
            std::abs(scale.x),
            std::abs(scale.y),
            std::abs(scale.z)});
        const float radius = std::max(
            0.1f,
            renderObject.mesh->GetBoundsRadius()
                * maxScale);
        boundsMin.x = std::min(
            boundsMin.x,
            position.x - radius);
        boundsMin.y = std::min(
            boundsMin.y,
            position.y - radius);
        boundsMin.z = std::min(
            boundsMin.z,
            position.z - radius);
        boundsMax.x = std::max(
            boundsMax.x,
            position.x + radius);
        boundsMax.y = std::max(
            boundsMax.y,
            position.y + radius);
        boundsMax.z = std::max(
            boundsMax.z,
            position.z + radius);
        hasBounds = true;
    }
    if (!hasBounds)
    {
        return std::nullopt;
    }

    const Core::Double3 center{
        (boundsMin.x + boundsMax.x) * 0.5,
        (boundsMin.y + boundsMax.y) * 0.5,
        (boundsMin.z + boundsMax.z) * 0.5};
    const double extentX =
        boundsMax.x - boundsMin.x;
    const double extentY =
        boundsMax.y - boundsMin.y;
    const double extentZ =
        boundsMax.z - boundsMin.z;
    const float radius = std::max(
        0.5f,
        static_cast<float>(
            std::max({extentX, extentY, extentZ})
            * 0.5));
    return RenderSceneBounds{center, radius};
}

bool FrameCameraToRenderObjects(RenderScene& scene)
{
    const std::optional<RenderSceneBounds> bounds =
        CalculateRenderObjectBounds(scene);
    if (!bounds.has_value())
    {
        return false;
    }

    Camera& camera = scene.GetCamera();
    const float distance =
        bounds->radius
            / std::tan(
                camera.GetFieldOfViewYRadians()
                * 0.5f)
        + bounds->radius * 1.75f;
    const Core::Double3 eye{
        bounds->center.x,
        bounds->center.y + bounds->radius * 0.35,
        bounds->center.z - distance};
    camera.SetWorldLookAt(
        eye,
        bounds->center,
        {0.0f, 1.0f, 0.0f});
    return true;
}
} // namespace Prism::Scene
