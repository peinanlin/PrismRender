#pragma once

#include "Core/Math/Double3.h"

#include <DirectXMath.h>

namespace Prism::Scene
{
class Camera
{
public:
    void SetPerspective(float fieldOfViewYRadians, float aspectRatio, float nearPlane, float farPlane);
    void SetLookAt(const DirectX::XMFLOAT3& position, const DirectX::XMFLOAT3& target, const DirectX::XMFLOAT3& upDirection);
    void SetPosition(const DirectX::XMFLOAT3& position);
    void SetWorldPosition(const Core::Double3& position);
    void SetWorldLookAt(
        const Core::Double3& position,
        const Core::Double3& target,
        const DirectX::XMFLOAT3& upDirection);
    void SetRotation(float pitchRadians, float yawRadians);
    void SetAspectRatio(float aspectRatio);
    void SetProjectionJitterNdc(
        const DirectX::XMFLOAT2& jitter);

    DirectX::XMMATRIX GetViewMatrix() const;
    DirectX::XMMATRIX GetProjectionMatrix() const;
    DirectX::XMMATRIX GetViewProjectionMatrix() const;
    DirectX::XMMATRIX GetRelativeViewMatrix() const;
    DirectX::XMMATRIX
        GetRelativeViewProjectionMatrix() const;

    const DirectX::XMFLOAT3& GetPosition() const;
    [[nodiscard]] const Core::Double3&
        GetWorldPosition() const;
    float GetPitch() const;
    float GetYaw() const;
    float GetFieldOfViewYRadians() const;
    float GetAspectRatio() const;
    float GetNearPlane() const;
    float GetFarPlane() const;
    DirectX::XMFLOAT3 GetForwardVector() const;
    DirectX::XMFLOAT3 GetRightVector() const;
    DirectX::XMFLOAT3 GetUpVector() const;

private:
    DirectX::XMVECTOR GetForwardVectorSimd() const;

    DirectX::XMFLOAT3 m_position{0.0f, 0.0f, -5.0f};
    Core::Double3 m_worldPosition{0.0, 0.0, -5.0};
    float m_fieldOfViewYRadians = DirectX::XM_PIDIV4;
    float m_aspectRatio = 16.0f / 9.0f;
    float m_nearPlane = 0.1f;
    float m_farPlane = 100.0f;
    float m_pitchRadians = 0.0f;
    float m_yawRadians = 0.0f;
    DirectX::XMFLOAT2 m_projectionJitterNdc{};
};
} // namespace Prism::Scene
