#include "Scene/Transform.h"

#include <cmath>

using namespace DirectX;

namespace Prism::Scene
{
void Transform::SetPosition(const XMFLOAT3& position)
{
    m_position = position;
    m_worldPosition = {
        static_cast<double>(position.x),
        static_cast<double>(position.y),
        static_cast<double>(position.z)};
}

void Transform::SetWorldPosition(
    const Core::Double3& position)
{
    m_worldPosition = position;
    m_position = {
        static_cast<float>(position.x),
        static_cast<float>(position.y),
        static_cast<float>(position.z)};
}

void Transform::SetRotationEulerRadians(const XMFLOAT3& rotation)
{
    m_rotationEulerRadians = rotation;
}

void Transform::SetScale(const XMFLOAT3& scale)
{
    m_scale = scale;
}

bool Transform::SetWorldMatrix(
    const XMFLOAT4X4& worldMatrix)
{
    XMVECTOR scale{};
    XMVECTOR rotation{};
    XMVECTOR translation{};
    if (!XMMatrixDecompose(
            &scale,
            &rotation,
            &translation,
            XMLoadFloat4x4(&worldMatrix)))
    {
        return false;
    }

    XMFLOAT3 scaleValue{};
    XMFLOAT3 translationValue{};
    XMFLOAT4 quaternion{};
    XMStoreFloat3(&scaleValue, scale);
    XMStoreFloat3(
        &translationValue,
        translation);
    XMStoreFloat4(&quaternion, rotation);

    const float sinrCosp =
        2.0f
        * (quaternion.w * quaternion.x
           + quaternion.y * quaternion.z);
    const float cosrCosp =
        1.0f
        - 2.0f
            * (quaternion.x * quaternion.x
               + quaternion.y * quaternion.y);
    const float pitch =
        std::atan2(sinrCosp, cosrCosp);
    const float sinp =
        2.0f
        * (quaternion.w * quaternion.y
           - quaternion.z * quaternion.x);
    const float yaw =
        std::abs(sinp) >= 1.0f
        ? std::copysign(XM_PIDIV2, sinp)
        : std::asin(sinp);
    const float sinyCosp =
        2.0f
        * (quaternion.w * quaternion.z
           + quaternion.x * quaternion.y);
    const float cosyCosp =
        1.0f
        - 2.0f
            * (quaternion.y * quaternion.y
               + quaternion.z * quaternion.z);
    const float roll =
        std::atan2(sinyCosp, cosyCosp);

    SetPosition(translationValue);
    SetScale(scaleValue);
    SetRotationEulerRadians(
        {pitch, yaw, roll});
    return true;
}

const XMFLOAT3& Transform::GetPosition() const
{
    return m_position;
}

const Core::Double3& Transform::GetWorldPosition() const
{
    return m_worldPosition;
}

const XMFLOAT3& Transform::GetRotationEulerRadians() const
{
    return m_rotationEulerRadians;
}

const XMFLOAT3& Transform::GetScale() const
{
    return m_scale;
}

XMMATRIX Transform::GetWorldMatrix() const
{
    return XMMatrixScaling(m_scale.x, m_scale.y, m_scale.z)
           * XMMatrixRotationRollPitchYaw(m_rotationEulerRadians.x, m_rotationEulerRadians.y, m_rotationEulerRadians.z)
           * XMMatrixTranslation(m_position.x, m_position.y, m_position.z);
}

XMMATRIX Transform::GetRelativeWorldMatrix(
    const Core::Double3& renderOrigin) const
{
    const Core::Double3 relative =
        m_worldPosition - renderOrigin;
    return XMMatrixScaling(
               m_scale.x,
               m_scale.y,
               m_scale.z)
        * XMMatrixRotationRollPitchYaw(
              m_rotationEulerRadians.x,
              m_rotationEulerRadians.y,
              m_rotationEulerRadians.z)
        * XMMatrixTranslation(
              static_cast<float>(relative.x),
              static_cast<float>(relative.y),
              static_cast<float>(relative.z));
}
} // namespace Prism::Scene
