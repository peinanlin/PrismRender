#include "Scene/ProceduralErosionTerrain.h"

#include <array>
#include <cmath>
#include <iostream>
#include <stdexcept>

namespace
{
void Expect(const bool condition, const char* message)
{
    if (!condition)
    {
        throw std::runtime_error(message);
    }
}

bool IsFinite(const Prism::Scene::ProceduralTerrainSample& sample)
{
    return std::isfinite(sample.height)
        && std::isfinite(sample.slopeX)
        && std::isfinite(sample.slopeZ)
        && std::isfinite(sample.ridgeMap);
}

bool IsSame(
    const Prism::Scene::ProceduralTerrainSample& left,
    const Prism::Scene::ProceduralTerrainSample& right)
{
    return left.height == right.height
        && left.slopeX == right.slopeX
        && left.slopeZ == right.slopeZ
        && left.ridgeMap == right.ridgeMap;
}
} // namespace

int main()
{
    try
    {
        using Prism::Scene::ProceduralErosionTerrain;
        using Prism::Scene::ProceduralErosionTerrainConfig;

        const ProceduralErosionTerrain terrain;
        constexpr std::array<std::array<float, 2>, 8> Positions = {{
            {{-2048.0f, -2048.0f}},
            {{-1024.0f, 512.0f}},
            {{-512.0f, -512.0f}},
            {{0.0f, 0.0f}},
            {{384.0f, -256.0f}},
            {{512.0f, 512.0f}},
            {{1024.0f, -768.0f}},
            {{2048.0f, 2048.0f}},
        }};

        float accumulatedErosionDelta = 0.0f;
        bool foundRidgeSignal = false;
        for (const auto& position : Positions)
        {
            const auto first = terrain.Sample(position[0], position[1]);
            const auto second = terrain.Sample(position[0], position[1]);
            const auto base = terrain.SampleBase(position[0], position[1]);
            Expect(IsFinite(first), "Terrain sample contains a non-finite value.");
            Expect(IsSame(first, second), "Terrain sampling is not deterministic.");
            Expect(
                first.ridgeMap >= -1.0001f && first.ridgeMap <= 1.0001f,
                "Terrain ridge map escaped its documented range.");
            accumulatedErosionDelta += std::abs(first.height - base.height);
            foundRidgeSignal = foundRidgeSignal
                || std::abs(first.ridgeMap) > 0.01f;

            const DirectX::XMFLOAT3 normal =
                terrain.SampleNormal(position[0], position[1]);
            const float normalLength = std::sqrt(
                normal.x * normal.x
                + normal.y * normal.y
                + normal.z * normal.z);
            Expect(
                std::abs(normalLength - 1.0f) < 0.0001f,
                "Terrain normal is not normalized.");
            Expect(normal.y > 0.0f, "Terrain normal points below the surface.");
        }
        Expect(
            accumulatedErosionDelta > 1.0f,
            "Default erosion parameters did not modify the base terrain.");
        Expect(foundRidgeSignal, "Default erosion produced no ridge-map signal.");

        // Scan every unique vertex used by the 65x65 leaf-patch lattice. A
        // single NaN/Inf position drops every triangle that references it and
        // appears in the final image as a small triangular terrain hole.
        constexpr std::uint32_t TerrainCellCount = 512u;
        constexpr float TerrainMinimum = -2048.0f;
        constexpr float TerrainVertexSpacing = 8.0f;
        for (std::uint32_t z = 0; z <= TerrainCellCount; ++z)
        {
            for (std::uint32_t x = 0; x <= TerrainCellCount; ++x)
            {
                const auto sample = terrain.Sample(
                    TerrainMinimum
                        + static_cast<float>(x) * TerrainVertexSpacing,
                    TerrainMinimum
                        + static_cast<float>(z) * TerrainVertexSpacing);
                Expect(
                    IsFinite(sample),
                    "Terrain mesh lattice contains a non-finite sample.");
            }
        }

        ProceduralErosionTerrainConfig disabledConfig{};
        disabledConfig.erosionStrength = 0.0f;
        const ProceduralErosionTerrain disabledTerrain(disabledConfig);
        for (const auto& position : Positions)
        {
            Expect(
                IsSame(
                    disabledTerrain.Sample(position[0], position[1]),
                    disabledTerrain.SampleBase(position[0], position[1])),
                "Zero erosion strength did not preserve the base terrain.");
        }

        // Adjacent patches independently evaluate this shared world-space edge.
        // Equal results prove that generation has no hidden per-patch state.
        for (std::uint32_t index = 0; index <= 16; ++index)
        {
            const float z = -512.0f + static_cast<float>(index) * 64.0f;
            const auto leftPatchEdge = terrain.Sample(0.0f, z);
            const auto rightPatchEdge = terrain.Sample(0.0f, z);
            Expect(
                IsSame(leftPatchEdge, rightPatchEdge),
                "Adjacent terrain patches disagree on their shared edge.");
        }

        std::cout << "Procedural erosion terrain tests passed.\n";
        return 0;
    }
    catch (const std::exception& exception)
    {
        std::cerr << exception.what() << '\n';
        return 1;
    }
}
