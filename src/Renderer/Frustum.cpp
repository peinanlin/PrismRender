#include "Renderer/Frustum.h"

using namespace DirectX;

namespace Prism::Renderer
{
namespace
{
XMFLOAT4 NormalizePlane(const XMFLOAT4& plane)
{
    const XMVECTOR vector = XMLoadFloat4(&plane);
    const XMVECTOR xyz = XMVectorSet(plane.x, plane.y, plane.z, 0.0f);
    const float length = XMVectorGetX(XMVector3Length(xyz));
    if (length <= 0.00001f)
    {
        return plane;
    }

    XMFLOAT4 normalized{};
    XMStoreFloat4(&normalized, vector / length);
    return normalized;
}
} // namespace

void Frustum::Build(const XMMATRIX& viewProjectionMatrix)
{
    XMFLOAT4X4 matrix{};
    XMStoreFloat4x4(&matrix, viewProjectionMatrix);

    m_planes[0] = NormalizePlane({matrix._14 + matrix._11, matrix._24 + matrix._21, matrix._34 + matrix._31, matrix._44 + matrix._41});
    m_planes[1] = NormalizePlane({matrix._14 - matrix._11, matrix._24 - matrix._21, matrix._34 - matrix._31, matrix._44 - matrix._41});
    m_planes[2] = NormalizePlane({matrix._14 + matrix._12, matrix._24 + matrix._22, matrix._34 + matrix._32, matrix._44 + matrix._42});
    m_planes[3] = NormalizePlane({matrix._14 - matrix._12, matrix._24 - matrix._22, matrix._34 - matrix._32, matrix._44 - matrix._42});
    m_planes[4] = NormalizePlane({matrix._13, matrix._23, matrix._33, matrix._43});
    m_planes[5] = NormalizePlane({matrix._14 - matrix._13, matrix._24 - matrix._23, matrix._34 - matrix._33, matrix._44 - matrix._43});
}

bool Frustum::IntersectsSphere(const XMFLOAT3& center, const float radius) const
{
    for (const XMFLOAT4& plane : m_planes)
    {
        const float distance = plane.x * center.x + plane.y * center.y + plane.z * center.z + plane.w;
        if (distance < -radius)
        {
            return false;
        }
    }

    return true;
}

const std::array<XMFLOAT4, 6>&
Frustum::GetPlanes() const
{
    return m_planes;
}
} // namespace Prism::Renderer
