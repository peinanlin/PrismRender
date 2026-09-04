#pragma once

#include <DirectXMath.h>

namespace Prism::Renderer
{
struct WaterOpticalParameters
{
    DirectX::XMFLOAT3 absorption{0.16f, 0.055f, 0.025f};
    DirectX::XMFLOAT3 scattering{0.018f, 0.065f, 0.09f};
    float indexOfRefraction = 1.333f;
    float phaseG = 0.8f;
    float thinLayerStrength = 0.35f;
    float backlitStrength = 0.2f;
};

struct WaterOpticalInput
{
    float viewCosine = 1.0f;
    float lightCosine = 1.0f;
    float thickness = 0.0f;
};

struct WaterOpticalResult
{
    DirectX::XMFLOAT3 transmittance{1.0f, 1.0f, 1.0f};
    DirectX::XMFLOAT3 singleScattering{};
    DirectX::XMFLOAT3 thinLayer{};
    DirectX::XMFLOAT3 backlit{};
    float fresnel = 0.0f;
    float phase = 0.0f;
};

struct WaterFoamInput
{
    float simulatedFoam = 0.0f;
    // Project-owned world-space detail remapped to [0, 1]. It may only
    // modulate foam that already exists in the coherent simulation mask.
    float worldDetail = 0.5f;
    float baseRoughness = 0.08f;
};

struct WaterFoamResult
{
    float effectiveFoam = 0.0f;
    float roughness = 0.08f;
    float refractionWeight = 1.0f;
    float diffuseWeight = 0.0f;
};

[[nodiscard]] WaterOpticalResult EvaluateWaterOpticalResponse(
    const WaterOpticalInput& input,
    const WaterOpticalParameters& parameters) noexcept;

[[nodiscard]] WaterFoamResult EvaluateWaterFoamResponse(
    const WaterFoamInput& input) noexcept;
} // namespace Prism::Renderer
