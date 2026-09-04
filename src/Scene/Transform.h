#pragma once

#include "Core/Math/Double3.h"

#include <DirectXMath.h>

namespace Prism::Scene
{
class Transform
{
public:
    void SetPosition(const DirectX::XMFLOAT3& position);
    void SetWorldPosition(const Core::Double3& position);
    void SetRotationEulerRadians(const DirectX::XMFLOAT3& rotation);
    void SetScale(const DirectX::XMFLOAT3& scale);
    bool SetWorldMatrix(
        const DirectX::XMFLOAT4X4& worldMatrix);

    const DirectX::XMFLOAT3& GetPosition() const;
    [[nodiscard]] const Core::Double3&
        GetWorldPosition() const;
    const DirectX::XMFLOAT3& GetRotationEulerRadians() const;
    const DirectX::XMFLOAT3& GetScale() const;

    DirectX::XMMATRIX GetWorldMatrix() const;
    DirectX::XMMATRIX GetRelativeWorldMatrix(
        const Core::Double3& renderOrigin) const;

private:
    DirectX::XMFLOAT3 m_position{0.0f, 0.0f, 0.0f};
    Core::Double3 m_worldPosition{};
    DirectX::XMFLOAT3 m_rotationEulerRadians{0.0f, 0.0f, 0.0f};
    DirectX::XMFLOAT3 m_scale{1.0f, 1.0f, 1.0f};
};
} // namespace Prism::Scene
