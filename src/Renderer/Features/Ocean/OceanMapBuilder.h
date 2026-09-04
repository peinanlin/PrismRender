#pragma once

#include <DirectXMath.h>

namespace Prism::Renderer
{
struct OceanAnalyticWave
{
    float amplitude = 0.0f;
    DirectX::XMFLOAT2 waveVector{};
    float phaseRadians = 0.0f;
    float lateralMultiplier = 1.0f;
};

struct OceanDerivedMapSample
{
    DirectX::XMFLOAT3 displacement{};
    DirectX::XMFLOAT3 tangentX{1.0f, 0.0f, 0.0f};
    DirectX::XMFLOAT3 tangentZ{0.0f, 0.0f, 1.0f};
    DirectX::XMFLOAT3 normal{0.0f, 1.0f, 0.0f};
    DirectX::XMFLOAT2 gradient{};
    DirectX::XMFLOAT4 slopeMoments{};
    float jacobian = 1.0f;
    float folding = 0.0f;
};

class OceanMapBuilder
{
public:
    // Maps the dimensionless minimum-principal-stretch deficit to the
    // WaveWorks-style foam threshold range used by the Lab. It does not alter
    // displacement or the published physical Jacobian.
    static constexpr float FoldingResponseScale = 2.25f;

    void Reset() noexcept {}

    // CPU analytic oracle for the GPU map kernel. Horizontal coordinates are
    // XZ and vertical displacement is Y, matching PrismRender world space.
    [[nodiscard]] static OceanDerivedMapSample EvaluateAnalyticWave(
        const OceanAnalyticWave& wave,
        float worldX,
        float worldZ) noexcept;
};
} // namespace Prism::Renderer
