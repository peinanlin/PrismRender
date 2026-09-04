#include "Scene/CameraController.h"

#include "Platform/Window.h"
#include "Scene/Camera.h"

#include <GLFW/glfw3.h>

#include <algorithm>
#include <cmath>
#include <limits>

using namespace DirectX;

namespace Prism::Scene
{
void CameraController::Update(Camera& camera, Platform::Window& window, const float deltaTime, const bool inputEnabled)
{
    double cursorX = 0.0;
    double cursorY = 0.0;
    window.GetCursorPosition(cursorX, cursorY);

    const double scrollDelta = window.ConsumeScrollDelta();
    const bool altDown = window.IsKeyDown(GLFW_KEY_LEFT_ALT) || window.IsKeyDown(GLFW_KEY_RIGHT_ALT);
    const bool lookActive = inputEnabled && window.IsMouseButtonDown(GLFW_MOUSE_BUTTON_RIGHT);
    const bool orbitActive = inputEnabled && !lookActive && altDown && window.IsMouseButtonDown(GLFW_MOUSE_BUTTON_LEFT);
    const bool panActive = inputEnabled && !lookActive && !orbitActive && window.IsMouseButtonDown(GLFW_MOUSE_BUTTON_MIDDLE);
    if (lookActive != m_isLooking)
    {
        window.SetCursorCaptured(lookActive);
        m_lastCursorX = cursorX;
        m_lastCursorY = cursorY;
    }

    if ((panActive && !m_isPanning) || (orbitActive && !m_isOrbiting))
    {
        m_lastCursorX = cursorX;
        m_lastCursorY = cursorY;
    }

    const float deltaX = static_cast<float>(cursorX - m_lastCursorX);
    const float deltaY = static_cast<float>(cursorY - m_lastCursorY);
    Core::Double3 position =
        camera.GetWorldPosition();

    if (inputEnabled && std::abs(scrollDelta) > 0.0001)
    {
        if (lookActive)
        {
            // Unity/UE-style fly-camera speed adjustment while RMB is held.
            m_moveSpeed = std::clamp(
                m_moveSpeed
                    * std::pow(
                        1.25f,
                        static_cast<float>(scrollDelta)),
                0.1f,
                10000.0f);
        }
        else
        {
            const XMFLOAT3 forward = camera.GetForwardVector();
            const float dollyDistance =
                std::max(0.25f, m_orbitDistance * 0.12f)
                * static_cast<float>(scrollDelta);
            position.x += forward.x * dollyDistance;
            position.y += forward.y * dollyDistance;
            position.z += forward.z * dollyDistance;
            m_orbitDistance = std::max(
                0.25f,
                m_orbitDistance - dollyDistance);
        }
    }

    if (lookActive)
    {
        camera.SetRotation(camera.GetPitch() - deltaY * m_lookSensitivity, camera.GetYaw() + deltaX * m_lookSensitivity);

        const XMFLOAT3 forward = camera.GetForwardVector();
        const XMFLOAT3 right = camera.GetRightVector();
        const bool fastMove = window.IsKeyDown(GLFW_KEY_LEFT_SHIFT) || window.IsKeyDown(GLFW_KEY_RIGHT_SHIFT);
        const bool slowMove = window.IsKeyDown(GLFW_KEY_LEFT_CONTROL) || window.IsKeyDown(GLFW_KEY_RIGHT_CONTROL);
        const float speedScale = fastMove ? 3.0f : (slowMove ? 0.35f : 1.0f);
        const float travelDistance = m_moveSpeed * speedScale * deltaTime;

        auto translate = [&position, travelDistance](const XMFLOAT3& axis, const float direction)
        {
            position.x += axis.x * travelDistance * direction;
            position.y += axis.y * travelDistance * direction;
            position.z += axis.z * travelDistance * direction;
        };

        if (window.IsKeyDown(GLFW_KEY_W))
        {
            translate(forward, 1.0f);
        }
        if (window.IsKeyDown(GLFW_KEY_S))
        {
            translate(forward, -1.0f);
        }
        if (window.IsKeyDown(GLFW_KEY_D))
        {
            translate(right, 1.0f);
        }
        if (window.IsKeyDown(GLFW_KEY_A))
        {
            translate(right, -1.0f);
        }
        if (window.IsKeyDown(GLFW_KEY_E))
        {
            position.y += travelDistance;
        }
        if (window.IsKeyDown(GLFW_KEY_Q))
        {
            position.y -= travelDistance;
        }

        const XMFLOAT3 updatedForward = camera.GetForwardVector();
        m_orbitPivot = {
            position.x + updatedForward.x * m_orbitDistance,
            position.y + updatedForward.y * m_orbitDistance,
            position.z + updatedForward.z * m_orbitDistance};
    }
    else if (orbitActive)
    {
        camera.SetRotation(camera.GetPitch() - deltaY * m_lookSensitivity, camera.GetYaw() + deltaX * m_lookSensitivity);
        const XMFLOAT3 forward = camera.GetForwardVector();
        position = {
            m_orbitPivot.x - forward.x * m_orbitDistance,
            m_orbitPivot.y - forward.y * m_orbitDistance,
            m_orbitPivot.z - forward.z * m_orbitDistance};
    }
    else if (panActive)
    {
        const XMFLOAT3 right = camera.GetRightVector();
        const XMFLOAT3 up = camera.GetUpVector();
        const float panDistance = m_moveSpeed * m_panSensitivity;
        position.x += (-right.x * deltaX + up.x * deltaY) * panDistance;
        position.y += (-right.y * deltaX + up.y * deltaY) * panDistance;
        position.z += (-right.z * deltaX + up.z * deltaY) * panDistance;
        m_orbitPivot.x += (-right.x * deltaX + up.x * deltaY) * panDistance;
        m_orbitPivot.y += (-right.y * deltaX + up.y * deltaY) * panDistance;
        m_orbitPivot.z += (-right.z * deltaX + up.z * deltaY) * panDistance;
    }

    m_isLooking = lookActive;
    m_isPanning = panActive;
    m_isOrbiting = orbitActive;
    m_lastCursorX = cursorX;
    m_lastCursorY = cursorY;
    camera.SetWorldPosition(position);
}

void CameraController::FocusOn(Camera& camera, const XMFLOAT3& target, const float boundsRadius)
{
    FocusOnWorldPosition(
        camera,
        Core::Double3{target.x, target.y, target.z},
        boundsRadius);
}

void CameraController::FocusOnWorldPosition(
    Camera& camera,
    const Core::Double3& target,
    const float boundsRadius)
{
    m_orbitPivot = target;
    m_orbitDistance = std::max(2.0f, boundsRadius * 2.8f);
    const XMFLOAT3 forward = camera.GetForwardVector();
    camera.SetWorldPosition({
        target.x - forward.x * m_orbitDistance,
        target.y - forward.y * m_orbitDistance,
        target.z - forward.z * m_orbitDistance});
}

void CameraController::SynchronizeOrbitPivot(
    const Camera& camera,
    const Core::Double3& target)
{
    m_orbitPivot = target;
    const Core::Double3 offset =
        target - camera.GetWorldPosition();
    const double distance = std::sqrt(
        offset.x * offset.x
        + offset.y * offset.y
        + offset.z * offset.z);
    m_orbitDistance = std::max(
        0.25f,
        static_cast<float>(std::min(
            distance,
            static_cast<double>(
                std::numeric_limits<float>::max()))));
}

void CameraController::SetMoveSpeed(const float moveSpeed)
{
    m_moveSpeed = moveSpeed;
}

float CameraController::GetMoveSpeed() const
{
    return m_moveSpeed;
}

float CameraController::GetOrbitDistance() const
{
    return m_orbitDistance;
}

bool CameraController::IsLooking() const
{
    return m_isLooking;
}

bool CameraController::IsInteracting() const
{
    return m_isLooking || m_isPanning || m_isOrbiting;
}
} // namespace Prism::Scene
