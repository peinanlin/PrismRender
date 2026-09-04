#include "Renderer/ShadowCascades.h"

#include "Renderer/RenderSettings.h"
#include "Scene/Camera.h"
#include "Scene/RenderScene.h"
#include "Scene/RenderSceneView.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>

namespace Prism::Renderer
{
using namespace DirectX;

CascadeShadowData BuildCascadeShadowData(
    const Scene::RenderSceneView& scene,
    const RenderSettings& settings,
    const std::uint32_t shadowMapResolution)
{
    const Scene::Camera& camera = scene.GetCamera();
    const float nearPlane = std::max(camera.GetNearPlane(), 0.01f);
    const float farPlane = std::max(
        nearPlane + 1.0f, std::min(camera.GetFarPlane(), settings.shadowDistance));
    const float splitLambda = std::clamp(settings.cascadeSplitLambda, 0.0f, 1.0f);

    std::array<float, SharedShadowCascadeCount> splitDistances{};
    for (std::uint32_t cascadeIndex = 0; cascadeIndex < SharedShadowCascadeCount; ++cascadeIndex)
    {
        const float fraction = static_cast<float>(cascadeIndex + 1u)
            / static_cast<float>(SharedShadowCascadeCount);
        const float logarithmic = nearPlane * std::pow(farPlane / nearPlane, fraction);
        const float uniform = nearPlane + (farPlane - nearPlane) * fraction;
        splitDistances[cascadeIndex] = std::lerp(uniform, logarithmic, splitLambda);
    }
    if (!settings.cascadeShadowsEnabled)
    {
        splitDistances.fill(farPlane);
    }

    CascadeShadowData result{};
    result.splitDistances = {
        splitDistances[0], splitDistances[1], splitDistances[2], farPlane};

    const XMVECTOR cameraPosition = XMLoadFloat3(&camera.GetPosition());
    const XMFLOAT3 cameraForwardValue = camera.GetForwardVector();
    const XMFLOAT3 cameraRightValue = camera.GetRightVector();
    const XMFLOAT3 cameraUpValue = camera.GetUpVector();
    const XMVECTOR cameraForward = XMLoadFloat3(&cameraForwardValue);
    const XMVECTOR cameraRight = XMLoadFloat3(&cameraRightValue);
    const XMVECTOR cameraUp = XMLoadFloat3(&cameraUpValue);
    const float tanHalfFov = std::tan(camera.GetFieldOfViewYRadians() * 0.5f);

    const XMVECTOR lightDirection = XMVector3Normalize(
        XMLoadFloat3(&scene.GetDirectionalLight().direction));
    const float lightUpDot = std::abs(XMVectorGetX(XMVector3Dot(
        lightDirection, XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f))));
    const XMVECTOR lightUp = lightUpDot > 0.95f
        ? XMVectorSet(0.0f, 0.0f, 1.0f, 0.0f)
        : XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f);
    const XMVECTOR lightRight = XMVector3Normalize(
        XMVector3Cross(lightUp, lightDirection));
    const XMVECTOR lightOrthoUp = XMVector3Normalize(
        XMVector3Cross(lightDirection, lightRight));

    float cascadeNear = nearPlane;
    const std::uint32_t cascadeCount = settings.cascadeShadowsEnabled
        ? SharedShadowCascadeCount
        : 1u;
    for (std::uint32_t cascadeIndex = 0; cascadeIndex < cascadeCount; ++cascadeIndex)
    {
        const float cascadeFar = splitDistances[cascadeIndex];
        const float nearHalfHeight = tanHalfFov * cascadeNear;
        const float nearHalfWidth = nearHalfHeight * camera.GetAspectRatio();
        const float farHalfHeight = tanHalfFov * cascadeFar;
        const float farHalfWidth = farHalfHeight * camera.GetAspectRatio();
        const XMVECTOR nearCenter = cameraPosition + cameraForward * cascadeNear;
        const XMVECTOR farCenter = cameraPosition + cameraForward * cascadeFar;

        const std::array<XMVECTOR, 8> corners = {
            nearCenter - cameraRight * nearHalfWidth - cameraUp * nearHalfHeight,
            nearCenter - cameraRight * nearHalfWidth + cameraUp * nearHalfHeight,
            nearCenter + cameraRight * nearHalfWidth - cameraUp * nearHalfHeight,
            nearCenter + cameraRight * nearHalfWidth + cameraUp * nearHalfHeight,
            farCenter - cameraRight * farHalfWidth - cameraUp * farHalfHeight,
            farCenter - cameraRight * farHalfWidth + cameraUp * farHalfHeight,
            farCenter + cameraRight * farHalfWidth - cameraUp * farHalfHeight,
            farCenter + cameraRight * farHalfWidth + cameraUp * farHalfHeight};

        XMVECTOR cascadeCenter = XMVectorZero();
        for (const XMVECTOR corner : corners)
        {
            cascadeCenter += corner;
        }
        cascadeCenter /= static_cast<float>(corners.size());

        float radius = 0.0f;
        for (const XMVECTOR corner : corners)
        {
            radius = std::max(
                radius, XMVectorGetX(XMVector3Length(corner - cascadeCenter)));
        }
        radius = std::ceil(std::max(radius, 1.0f) * 16.0f) / 16.0f;

        // Snap the cascade center in the fixed world-space light basis. The
        // previous implementation transformed cascadeCenter by a view matrix
        // that was already looking at cascadeCenter, producing x/y ~= 0 and
        // therefore making the snapping operation a no-op.
        const float unsnappedTexelSize =
            (radius * 2.0f)
            / static_cast<float>(shadowMapResolution);
        const float projectionRadius =
            radius + unsnappedTexelSize;
        const float texelSize =
            (projectionRadius * 2.0f)
            / static_cast<float>(shadowMapResolution);
        const float centerX = XMVectorGetX(
            XMVector3Dot(cascadeCenter, lightRight));
        const float centerY = XMVectorGetX(
            XMVector3Dot(cascadeCenter, lightOrthoUp));
        const float snappedX = std::floor(
            centerX / texelSize + 0.5f)
            * texelSize;
        const float snappedY = std::floor(
            centerY / texelSize + 0.5f)
            * texelSize;
        const XMVECTOR stableCenter = cascadeCenter
            + lightRight * (snappedX - centerX)
            + lightOrthoUp * (snappedY - centerY);
        const XMVECTOR lightPosition = stableCenter
            - lightDirection
                * (projectionRadius * 2.0f + 50.0f);
        const XMMATRIX lightView = XMMatrixLookAtLH(
            lightPosition,
            stableCenter,
            lightUp);
        float minimumZ = std::numeric_limits<float>::max();
        float maximumZ = -std::numeric_limits<float>::max();
        for (const XMVECTOR corner : corners)
        {
            const XMVECTOR lightSpaceCorner = XMVector3TransformCoord(corner, lightView);
            minimumZ = std::min(minimumZ, XMVectorGetZ(lightSpaceCorner));
            maximumZ = std::max(maximumZ, XMVectorGetZ(lightSpaceCorner));
        }

        const float nearZ = std::max(0.1f, minimumZ - radius * 2.0f);
        const float farZ = std::max(nearZ + 1.0f, maximumZ + radius * 2.0f);
        const XMMATRIX lightProjection = XMMatrixOrthographicOffCenterLH(
            -projectionRadius,
            projectionRadius,
            -projectionRadius,
            projectionRadius,
            nearZ,
            farZ);
        result.viewProjections[cascadeIndex] = lightView * lightProjection;
        cascadeNear = cascadeFar;
    }

    for (std::uint32_t cascadeIndex = cascadeCount;
         cascadeIndex < SharedShadowCascadeCount;
         ++cascadeIndex)
    {
        result.viewProjections[cascadeIndex] = result.viewProjections[0];
    }
    return result;
}
} // namespace Prism::Renderer
