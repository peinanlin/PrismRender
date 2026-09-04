#pragma once

#include <DirectXMath.h>

namespace Prism::Scene
{
struct DirectionalLight
{
    DirectX::XMFLOAT3 direction{0.25f, -1.0f, 0.15f};
    float intensity = 4.0f;
    DirectX::XMFLOAT3 color{1.0f, 1.0f, 1.0f};
    float padding = 0.0f;
};

struct PointLight
{
    DirectX::XMFLOAT3 position{0.0f, 2.0f, -2.0f};
    float range = 12.0f;
    DirectX::XMFLOAT3 color{1.0f, 0.8f, 0.6f};
    float intensity = 1.0f;
    bool castsShadow = true;
};

struct SpotLight
{
    DirectX::XMFLOAT3 position{0.0f, 6.0f, -2.0f};
    float range = 18.0f;
    DirectX::XMFLOAT3 direction{0.0f, -1.0f, 0.15f};
    float outerAngleRadians = DirectX::XMConvertToRadians(28.0f);
    DirectX::XMFLOAT3 color{1.0f, 0.88f, 0.68f};
    float intensity = 4.0f;
    float innerAngleRadians = DirectX::XMConvertToRadians(18.0f);
    bool castsShadow = true;
};
} // namespace Prism::Scene
