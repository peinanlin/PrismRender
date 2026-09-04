#pragma once

#include "Renderer/RenderGraph.h"

#include <array>
#include <functional>

namespace Prism::Renderer
{
// Authoritative cross-feature inputs consumed by the water-optics subgraph.
// Feature-private scratch and history resources remain owned by
// WaterOpticsFeature and are intentionally absent here.
struct WaterOpticsGraphInputs
{
    BufferHandle frameConstants;
    BufferHandle clusterConstants;
    BufferHandle pointLights;
    BufferHandle clusterLightCounts;
    BufferHandle clusterLightIndices;
    TextureHandle sceneColor;
    TextureHandle sceneMotion;
    TextureHandle opaqueDepth;
    TextureHandle opaqueHiZ;
    TextureHandle shadowMap;
    TextureHandle shadowMoments;
    TextureHandle atmosphereSkyView;
    TextureHandle environment;
    std::array<TextureHandle, 3> waterGBuffer;
    TextureHandle waterMotion;
    TextureHandle compositeDepth;
    TextureHandle oceanDisplacement;
    TextureHandle oceanNormalFoam;
    TextureHandle oceanSlopeMoments;
    TextureHandle oceanFoamHistory;
    TextureHandle localWaveDisplacement;
    TextureHandle localWaveGradient;
    bool clusteredLightingEnabled = false;
    bool shadowsEnabled = false;
    bool varianceShadowsEnabled = false;
    bool atmosphereEnabled = false;
    bool localWaveEnabled = false;
};

// These are the only versions the shared pipeline consumes after water
// optics. Opaque depth is deliberately not returned or overwritten.
struct WaterOpticsGraphResult
{
    TextureHandle sceneColor;
    TextureHandle sceneMotion;
    TextureHandle compositeDepth;
    std::array<TextureHandle, 3> waterGBuffer;
    TextureHandle waterMotion;
};

using WaterOpticsGraphCallback = std::function<WaterOpticsGraphResult(
    RenderGraph&,
    const WaterOpticsGraphInputs&)>;

struct WaterOpticsFeatureSlot
{
};

// Native inputs and the private-subgraph entry point owned by WaterOptics.
// Specialized optical data stays out of the pipeline-wide aggregates.
struct WaterOpticsGraphContribution
{
    std::array<RHI::ITexture*, 3> waterGBuffer{};
    RHI::ITexture* waterMotion = nullptr;
    RHI::ITexture* compositeDepth = nullptr;
    std::array<RHI::ResourceState, 3> waterGBufferInitialStates{};
    RHI::ResourceState waterMotionInitialState =
        RHI::ResourceState::Undefined;
    RHI::ResourceState compositeDepthInitialState =
        RHI::ResourceState::Undefined;
    WaterOpticsGraphCallback build;
};
} // namespace Prism::Renderer
