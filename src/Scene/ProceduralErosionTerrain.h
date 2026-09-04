/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#pragma once

#include <DirectXMath.h>

#include <cstdint>

namespace Prism::Scene
{
struct ProceduralErosionTerrainConfig
{
    float worldSize = 4096.0f;

    // Horizontal scale is expressed in world metres. Strength is relative to
    // that scale, matching the analytical erosion filter formulation.
    float erosionScale = 320.0f;
    float erosionStrength = 0.11f;
    float gullyWeight = 0.48f;
    float detail = 1.5f;
    float cellScale = 0.7f;
    float normalization = 0.5f;
    std::uint32_t erosionOctaves = 4;
    float lacunarity = 2.0f;
    float gain = 0.5f;

    float ridgeRounding = 0.14f;
    float creaseRounding = 0.06f;
    float inputRoundingMultiplier = 0.10f;
    float octaveRoundingMultiplier = 2.0f;
    float inputOnset = 1.25f;
    float octaveOnset = 1.25f;
    float ridgeMapInputOnset = 2.8f;
    float ridgeMapOctaveOnset = 1.5f;
    float assumedSlope = 0.6f;
    float assumedSlopeBlend = 1.0f;

    float valleyAltitude = 4.0f;
    float peakAltitude = 170.0f;
    float erosionFadeStartAltitude = -2.0f;
    float erosionFadeEndAltitude = 34.0f;
    float carveBias = 0.08f;
};

struct ProceduralTerrainSample
{
    float height = 0.0f;
    float slopeX = 0.0f;
    float slopeZ = 0.0f;
    // -1 marks a gully crease, +1 marks a ridge, and 0 is neutral.
    float ridgeMap = 0.0f;
};

class ProceduralErosionTerrain
{
public:
    explicit ProceduralErosionTerrain(
        ProceduralErosionTerrainConfig config = {});

    ProceduralTerrainSample Sample(float worldX, float worldZ) const;
    ProceduralTerrainSample SampleBase(float worldX, float worldZ) const;
    DirectX::XMFLOAT3 SampleNormal(float worldX, float worldZ) const;

    const ProceduralErosionTerrainConfig& GetConfig() const;

private:
    ProceduralErosionTerrainConfig m_config;
};
} // namespace Prism::Scene
