#include "Renderer/Features/Ocean/OceanMapBuilder.h"

#include <algorithm>
#include <cmath>

namespace Prism::Renderer
{
OceanDerivedMapSample OceanMapBuilder::EvaluateAnalyticWave(
    const OceanAnalyticWave& wave, const float worldX,
    const float worldZ) noexcept
{
    OceanDerivedMapSample result{};
    const float waveNumber = std::sqrt(
        wave.waveVector.x * wave.waveVector.x
        + wave.waveVector.y * wave.waveVector.y);
    if (!(waveNumber > 1.0e-6f) || !std::isfinite(waveNumber)
        || !std::isfinite(wave.amplitude)
        || !std::isfinite(wave.phaseRadians))
    {
        return result;
    }
    const float directionX = wave.waveVector.x / waveNumber;
    const float directionZ = wave.waveVector.y / waveNumber;
    const float phase = wave.waveVector.x * worldX
        + wave.waveVector.y * worldZ + wave.phaseRadians;
    const float sine = std::sin(phase);
    const float cosine = std::cos(phase);
    const float height = wave.amplitude * sine;
    const float horizontalAmplitude = wave.lateralMultiplier
        * wave.amplitude;
    result.displacement = {horizontalAmplitude * directionX * cosine,
        height, horizontalAmplitude * directionZ * cosine};

    const float slopeX = wave.amplitude * wave.waveVector.x * cosine;
    const float slopeZ = wave.amplitude * wave.waveVector.y * cosine;
    const float horizontalDerivative = -horizontalAmplitude * sine;
    const float dDxDx = horizontalDerivative * directionX
        * wave.waveVector.x;
    const float dDxDz = horizontalDerivative * directionX
        * wave.waveVector.y;
    const float dDzDx = horizontalDerivative * directionZ
        * wave.waveVector.x;
    const float dDzDz = horizontalDerivative * directionZ
        * wave.waveVector.y;
    result.tangentX = {1.0f + dDxDx, slopeX, dDzDx};
    result.tangentZ = {dDxDz, slopeZ, 1.0f + dDzDz};
    const float normalX = result.tangentZ.y * result.tangentX.z
        - result.tangentZ.z * result.tangentX.y;
    const float normalY = result.tangentZ.z * result.tangentX.x
        - result.tangentZ.x * result.tangentX.z;
    const float normalZ = result.tangentZ.x * result.tangentX.y
        - result.tangentZ.y * result.tangentX.x;
    const float normalLength = std::sqrt(normalX * normalX
        + normalY * normalY + normalZ * normalZ);
    result.normal = normalLength > 1.0e-6f
        ? DirectX::XMFLOAT3{normalX / normalLength,
            normalY / normalLength, normalZ / normalLength}
        : DirectX::XMFLOAT3{0.0f, 1.0f, 0.0f};
    result.gradient = {slopeX, slopeZ};
    result.slopeMoments = {slopeX, slopeZ,
        slopeX * slopeX, slopeZ * slopeZ};
    result.jacobian = (1.0f + dDxDx) * (1.0f + dDzDz)
        - dDxDz * dDzDx;
    const float deformationX = 1.0f + dDxDx;
    const float deformationZ = 1.0f + dDzDz;
    const float symmetricOffDiagonal = 0.5f * (dDxDz + dDzDx);
    const float eigenDiscriminant = std::sqrt(std::max(
        (deformationX - deformationZ)
                * (deformationX - deformationZ)
            + 4.0f * symmetricOffDiagonal * symmetricOffDiagonal,
        0.0f));
    const float minimumPrincipalStretch = 0.5f
        * (deformationX + deformationZ - eigenDiscriminant);
    result.folding = std::max(0.0f, 1.0f - minimumPrincipalStretch)
        * OceanMapBuilder::FoldingResponseScale;
    return result;
}
} // namespace Prism::Renderer
