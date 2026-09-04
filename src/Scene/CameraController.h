#pragma once

#include "Core/Math/Double3.h"

#include <DirectXMath.h>

namespace Prism::Platform
{
class Window;
}

namespace Prism::Scene
{
class Camera;

class CameraController
{
public:
    void Update(Camera& camera, Platform::Window& window, float deltaTime, bool inputEnabled);
    void FocusOn(Camera& camera, const DirectX::XMFLOAT3& target, float boundsRadius);
    void FocusOnWorldPosition(
        Camera& camera,
        const Core::Double3& target,
        float boundsRadius);
    void SynchronizeOrbitPivot(
        const Camera& camera,
        const Core::Double3& target);

    void SetMoveSpeed(float moveSpeed);
    float GetMoveSpeed() const;
    float GetOrbitDistance() const;
    bool IsLooking() const;
    bool IsInteracting() const;

private:
    bool m_isLooking = false;
    bool m_isPanning = false;
    bool m_isOrbiting = false;
    float m_moveSpeed = 8.0f;
    float m_lookSensitivity = 0.0025f;
    float m_panSensitivity = 0.012f;
    Core::Double3 m_orbitPivot{};
    float m_orbitDistance = 10.0f;
    double m_lastCursorX = 0.0;
    double m_lastCursorY = 0.0;
};
} // namespace Prism::Scene
