#include "Renderer/Features/Ocean/WaterOpticalModel.h"

#include <algorithm>
#include <array>
#include <cmath>

namespace Prism::Renderer
{
namespace
{
constexpr float Pi = 3.14159265358979323846f;

float FiniteOr(const float value, const float fallback) noexcept
{
    return std::isfinite(value) ? value : fallback;
}

float FresnelSchlick(const float cosine, const float ior) noexcept
{
    const float ratio = (ior - 1.0f) / (ior + 1.0f);
    const float f0 = ratio * ratio;
    const float oneMinusCosine = 1.0f - cosine;
    return std::clamp(f0 + (1.0f - f0)
        * oneMinusCosine * oneMinusCosine * oneMinusCosine
        * oneMinusCosine * oneMinusCosine, 0.0f, 1.0f);
}
} // namespace

WaterOpticalResult EvaluateWaterOpticalResponse(
    const WaterOpticalInput& input,
    const WaterOpticalParameters& parameters) noexcept
{
    WaterOpticalResult result{};
    const float viewCosine = std::clamp(
        FiniteOr(input.viewCosine, 1.0f), 0.0f, 1.0f);
    const float lightCosine = std::clamp(
        FiniteOr(input.lightCosine, 0.0f), -1.0f, 1.0f);
    const float thickness = std::clamp(
        FiniteOr(input.thickness, 0.0f), 0.0f, 10000.0f);
    const float ior = std::clamp(FiniteOr(
        parameters.indexOfRefraction, 1.333f), 1.0001f, 3.0f);
    const float phaseG = std::clamp(
        FiniteOr(parameters.phaseG, 0.0f), -0.95f, 0.95f);
    const float thinStrength = std::clamp(FiniteOr(
        parameters.thinLayerStrength, 0.0f), 0.0f, 8.0f);
    const float backlitStrength = std::clamp(FiniteOr(
        parameters.backlitStrength, 0.0f), 0.0f, 8.0f);
    result.fresnel = FresnelSchlick(viewCosine, ior);
    const float phaseDenominator = std::max(1.0f + phaseG * phaseG
        - 2.0f * phaseG * lightCosine, 1.0e-4f);
    result.phase = std::clamp((1.0f - phaseG * phaseG)
        / (4.0f * Pi * std::pow(phaseDenominator, 1.5f)),
        0.0f, 16.0f);

    const std::array<float, 3> absorption{
        parameters.absorption.x, parameters.absorption.y,
        parameters.absorption.z};
    const std::array<float, 3> scattering{
        parameters.scattering.x, parameters.scattering.y,
        parameters.scattering.z};
    float* transmittance = &result.transmittance.x;
    float* singleScattering = &result.singleScattering.x;
    float* thinLayer = &result.thinLayer.x;
    float* backlit = &result.backlit.x;
    for (std::size_t channel = 0u; channel < 3u; ++channel)
    {
        const float absorptionValue = std::clamp(
            FiniteOr(absorption[channel], 0.0f), 0.0f, 100.0f);
        const float scatteringValue = std::clamp(
            FiniteOr(scattering[channel], 0.0f), 0.0f, 100.0f);
        const float extinction = absorptionValue + scatteringValue;
        const float transmission = std::exp(-std::min(
            extinction * thickness, 80.0f));
        const float albedo = extinction > 1.0e-6f
            ? scatteringValue / extinction : 0.0f;
        transmittance[channel] = transmission;
        singleScattering[channel] = std::clamp(
            (1.0f - transmission) * albedo * result.phase,
            0.0f, 16.0f);
        thinLayer[channel] = std::clamp(
            transmission * (1.0f - result.fresnel)
                * (1.0f - viewCosine) * thinStrength,
            0.0f, 8.0f);
        backlit[channel] = std::clamp(
            (1.0f - transmission) * albedo
                * std::max(-lightCosine, 0.0f) * backlitStrength,
            0.0f, 8.0f);
    }
    return result;
}

WaterFoamResult EvaluateWaterFoamResponse(
    const WaterFoamInput& input) noexcept
{
    WaterFoamResult result{};
    const float simulatedFoam = std::clamp(
        FiniteOr(input.simulatedFoam, 0.0f), 0.0f, 1.0f);
    const float detail = std::clamp(
        FiniteOr(input.worldDetail, 0.5f), 0.0f, 1.0f);
    const float baseRoughness = std::clamp(
        FiniteOr(input.baseRoughness, 0.08f), 0.001f, 1.0f);

    // Calm water remains exactly clear. Detail can shape existing simulated
    // foam, but is never allowed to seed foam on its own.
    if (simulatedFoam <= 1.0e-5f)
    {
        result.roughness = baseRoughness;
        return result;
    }

    const float detailModulation = std::clamp(
        0.72f + detail * 0.56f, 0.72f, 1.28f);
    result.effectiveFoam = std::clamp(
        simulatedFoam * detailModulation, 0.0f, 1.0f);
    result.roughness = std::clamp(baseRoughness
        + result.effectiveFoam * 0.55f, 0.001f, 1.0f);
    result.refractionWeight = std::clamp(
        1.0f - result.effectiveFoam * 0.92f, 0.08f, 1.0f);
    result.diffuseWeight = result.effectiveFoam;
    return result;
}
} // namespace Prism::Renderer
