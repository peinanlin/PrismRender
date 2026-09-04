#include "Scene/Camera.h"

#include <algorithm>
#include <cmath>

using namespace DirectX;

namespace Prism::Scene
{
void Camera::SetPerspective(const float fieldOfViewYRadians, const float aspectRatio, const float nearPlane, const float farPlane)
{
    m_fieldOfViewYRadians = fieldOfViewYRadians;
    m_aspectRatio = aspectRatio;
    m_nearPlane = nearPlane;
    m_farPlane = farPlane;
}

void Camera::SetLookAt(const XMFLOAT3& position, const XMFLOAT3& target, const XMFLOAT3& upDirection)
{
    SetWorldLookAt(
        Core::Double3{position.x, position.y, position.z},
        Core::Double3{target.x, target.y, target.z},
        upDirection);
}

void Camera::SetWorldLookAt(
    const Core::Double3& position,
    const Core::Double3& target,
    const XMFLOAT3& upDirection)
{
    SetWorldPosition(position);

    const Core::Double3 direction = target - position;
    const double length = std::sqrt(
        direction.x * direction.x
        + direction.y * direction.y
        + direction.z * direction.z);
    if (length <= 0.000000001)
    {
        return;
    }
    const XMFLOAT3 forward{
        static_cast<float>(direction.x / length),
        static_cast<float>(direction.y / length),
        static_cast<float>(direction.z / length)};

    m_pitchRadians = std::asin(std::clamp(forward.y, -1.0f, 1.0f));
    m_yawRadians = std::atan2(forward.x, forward.z);

    const XMVECTOR up = XMVector3Normalize(XMLoadFloat3(&upDirection));
    XMFLOAT3 normalizedUp{};
    XMStoreFloat3(&normalizedUp, up);
    if (normalizedUp.y < 0.0f)
    {
        m_pitchRadians = -m_pitchRadians;
        m_yawRadians += XM_PI;
    }
}

void Camera::SetPosition(const XMFLOAT3& position)
{
    m_position = position;
    m_worldPosition = {
        static_cast<double>(position.x),
        static_cast<double>(position.y),
        static_cast<double>(position.z)};
}

void Camera::SetWorldPosition(
    const Core::Double3& position)
{
    m_worldPosition = position;
    m_position = {
        static_cast<float>(position.x),
        static_cast<float>(position.y),
        static_cast<float>(position.z)};
}

void Camera::SetRotation(const float pitchRadians, const float yawRadians)
{
    constexpr float pitchLimit = XM_PIDIV2 - 0.01f;
    m_pitchRadians = std::clamp(pitchRadians, -pitchLimit, pitchLimit);
    m_yawRadians = yawRadians;
}

void Camera::SetAspectRatio(const float aspectRatio)
{
    m_aspectRatio = aspectRatio;
}

void Camera::SetProjectionJitterNdc(
    const XMFLOAT2& jitter)
{
    m_projectionJitterNdc = jitter;
}

XMMATRIX Camera::GetViewMatrix() const
{
    const XMVECTOR position = XMLoadFloat3(&m_position);
    const XMVECTOR forward = GetForwardVectorSimd();
    const XMVECTOR target = position + forward;
    static constexpr XMVECTORF32 worldUp = {0.0f, 1.0f, 0.0f, 0.0f};
    return XMMatrixLookAtLH(position, target, worldUp);
}

XMMATRIX Camera::GetProjectionMatrix() const
{
    XMFLOAT4X4 projection{};
    XMStoreFloat4x4(
        &projection,
        XMMatrixPerspectiveFovLH(
            m_fieldOfViewYRadians,
            m_aspectRatio,
            m_nearPlane,
            m_farPlane));
    projection._31 += m_projectionJitterNdc.x;
    projection._32 += m_projectionJitterNdc.y;
    return XMLoadFloat4x4(&projection);
}

XMMATRIX Camera::GetViewProjectionMatrix() const
{
    return GetViewMatrix() * GetProjectionMatrix();
}

XMMATRIX Camera::GetRelativeViewMatrix() const
{
    const XMVECTOR position = XMVectorZero();
    const XMVECTOR target = GetForwardVectorSimd();
    static constexpr XMVECTORF32 worldUp = {
        0.0f,
        1.0f,
        0.0f,
        0.0f};
    return XMMatrixLookAtLH(
        position,
        target,
        worldUp);
}

XMMATRIX Camera::GetRelativeViewProjectionMatrix() const
{
    return GetRelativeViewMatrix()
        * GetProjectionMatrix();
}

const XMFLOAT3& Camera::GetPosition() const
{
    return m_position;
}

const Core::Double3& Camera::GetWorldPosition() const
{
    return m_worldPosition;
}

float Camera::GetPitch() const
{
    return m_pitchRadians;
}

float Camera::GetYaw() const
{
    return m_yawRadians;
}

float Camera::GetFieldOfViewYRadians() const
{
    return m_fieldOfViewYRadians;
}

float Camera::GetAspectRatio() const
{
    return m_aspectRatio;
}

float Camera::GetNearPlane() const
{
    return m_nearPlane;
}

float Camera::GetFarPlane() const
{
    return m_farPlane;
}

XMFLOAT3 Camera::GetForwardVector() const
{
    XMFLOAT3 forward{};
    XMStoreFloat3(&forward, GetForwardVectorSimd());
    return forward;
}

XMFLOAT3 Camera::GetRightVector() const
{
    const XMVECTOR forward = GetForwardVectorSimd();
    static constexpr XMVECTORF32 worldUp = {0.0f, 1.0f, 0.0f, 0.0f};
    XMFLOAT3 right{};
    XMStoreFloat3(&right, XMVector3Normalize(XMVector3Cross(worldUp, forward)));
    return right;
}

XMFLOAT3 Camera::GetUpVector() const
{
    const XMVECTOR forward = GetForwardVectorSimd();
    static constexpr XMVECTORF32 worldUp = {0.0f, 1.0f, 0.0f, 0.0f};
    const XMVECTOR right = XMVector3Normalize(XMVector3Cross(worldUp, forward));
    XMFLOAT3 up{};
    XMStoreFloat3(&up, XMVector3Normalize(XMVector3Cross(forward, right)));
    return up;
}

XMVECTOR Camera::GetForwardVectorSimd() const
{
    const float cosPitch = std::cos(m_pitchRadians);
    const float sinPitch = std::sin(m_pitchRadians);
    const float sinYaw = std::sin(m_yawRadians);
    const float cosYaw = std::cos(m_yawRadians);
    return XMVector3Normalize(XMVectorSet(sinYaw * cosPitch, sinPitch, cosYaw * cosPitch, 0.0f));
}
} // namespace Prism::Scene
