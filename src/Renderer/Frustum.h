#pragma once

#include <array>

#include <DirectXMath.h>

namespace Prism::Renderer
{
class Frustum
{
public:
    void Build(const DirectX::XMMATRIX& viewProjectionMatrix);
    bool IntersectsSphere(const DirectX::XMFLOAT3& center, float radius) const;
    const std::array<DirectX::XMFLOAT4, 6>&
        GetPlanes() const;

private:
    std::array<DirectX::XMFLOAT4, 6> m_planes{};
};
} // namespace Prism::Renderer
