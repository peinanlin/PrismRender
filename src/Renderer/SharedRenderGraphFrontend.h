#pragma once

#include "Renderer/RenderGraph.h"
#include "Renderer/Features/Fluid/FluidGraph.h"
#include "Renderer/Features/Ocean/OceanSettings.h"
#include "Renderer/Features/Ocean/WaterOpticsGraph.h"

#include <array>
#include <cstdint>
#include <functional>

namespace Prism::RHI
{
class IBuffer;
class ITexture;
}

namespace Prism::Renderer
{
inline constexpr std::uint32_t SharedGBufferCount = 4;

using HiZExecuteCallback = std::function<void(
    RHI::ICommandContext&,
    const RenderGraphPassResources&,
    std::uint32_t)>;

struct SharedRenderGraphResources
{
    RHI::ITexture* environment = nullptr;
    RHI::IBuffer* frameConstants = nullptr;
    RHI::ITexture* shadowMap = nullptr;
    RHI::ITexture* depthBuffer = nullptr;
    RHI::ITexture* hiZ = nullptr;
    RHI::IBuffer* oceanQueryConstants = nullptr;
    RHI::IBuffer* oceanQueryPoints = nullptr;
    RHI::IBuffer* oceanQueryResults = nullptr;
    RHI::IBuffer* oceanQueryReadback = nullptr;
    std::uint32_t oceanQueryCount = 0u;
    std::array<RHI::ITexture*, SharedGBufferCount>
        gbuffer{};
    RHI::ITexture* hdrColor = nullptr;
    RHI::ITexture* bloomA = nullptr;
    RHI::ITexture* bloomB = nullptr;
    RHI::ITexture* outputColor = nullptr;

    RHI::ResourceState shadowInitialState =
        RHI::ResourceState::ShaderResource;
    RHI::ResourceState depthInitialState =
        RHI::ResourceState::ShaderResource;
    RHI::ResourceState hiZInitialState =
        RHI::ResourceState::ShaderResource;
    std::array<
        RHI::ResourceState,
        SharedGBufferCount> gbufferInitialStates{};
    RHI::ResourceState hdrInitialState =
        RHI::ResourceState::Undefined;
    RHI::ResourceState bloomAInitialState =
        RHI::ResourceState::Undefined;
    RHI::ResourceState bloomBInitialState =
        RHI::ResourceState::Undefined;
    RHI::ResourceState outputInitialState =
        RHI::ResourceState::ShaderResource;
};

struct SharedRenderGraphOptions
{
    std::uint64_t viewId = 0;
    bool shadowsEnabled = true;
    bool deferredRenderingEnabled = true;
    bool clusteredLightingEnabled = true;
    bool localLightShadowsEnabled = true;
    bool spotShadowsEnabled = true;
    bool pointShadowsEnabled = true;
    bool planarReflectionsEnabled = false;
    bool varianceShadowsEnabled = false;
    bool gtaoEnabled = true;
    bool screenSpaceReflectionsEnabled = true;
    bool temporalAntiAliasingEnabled = true;
    bool physicalAtmosphereEnabled = false;
    bool fftOceanEnabled = false;
    bool spectralOceanEnabled = false;
    bool oceanSimulationEnabled = true;
    bool oceanQueriesEnabled = false;
    bool localWaveEnabled = false;
    OceanImplementation oceanImplementation = OceanImplementation::LegacyFft;
    OceanOpticsModel oceanOpticsModel = OceanOpticsModel::Current;
    bool interactiveTerrainEnabled = false;
    // Shared terrain textures remain imported by every consuming view. Only
    // the selected LogicalFrameId producer records the mutation pass.
    bool interactiveTerrainUpdateEnabled = false;
    bool fluidEnabled = false;
    bool bloomEnabled = true;
    bool gpuDrivenEnabled = false;
    RHI::ResourceState outputReadyState =
        RHI::ResourceState::ShaderResource;
};

struct SharedRenderGraphCallbacks
{
    RenderGraph::ParameterExecuteCallback shadow;
    RenderGraph::ParameterExecuteCallback gbuffer;
    RenderGraph::ParameterExecuteCallback
        deferredLighting;
    RenderGraph::ParameterExecuteCallback forwardGeometry;
    RenderGraph::ParameterExecuteCallback
        transparentGeometry;
    HiZExecuteCallback hiZ;
    RenderGraph::ParameterExecuteCallback oceanQuery;
    RenderGraph::ParameterExecuteCallback bloomExtract;
    RenderGraph::ParameterExecuteCallback bloomHorizontal;
    RenderGraph::ParameterExecuteCallback bloomVertical;
    RenderGraph::ParameterExecuteCallback tonemap;
    RenderGraph::ParameterExecuteCallback outputReady;
};

struct SharedSceneGraphHandles
{
    TextureHandle environment;
    BufferHandle frameConstants;
    BufferHandle clusterConstants;
    BufferHandle pointLights;
    BufferHandle clusterLightCounts;
    BufferHandle clusterLightIndices;
    BufferHandle localLightConstants;
    BufferHandle localShadowRenderConstants;
    BufferHandle gpuObjectRecords;
    BufferHandle indirectArguments;
    BufferHandle indirectDrawCounts;
    TextureHandle shadowMap;
    TextureHandle spotShadowMap;
    TextureHandle pointShadowMap;
    TextureHandle planarReflection;
    TextureHandle planarReflectionDepth;
    TextureHandle shadowMoments;
    TextureHandle shadowMomentsScratch;
    TextureHandle depthBuffer;
    TextureHandle hiZ;
    std::array<TextureHandle, 3> waterGBuffer;
    TextureHandle waterMotion;
    TextureHandle waterCompositeDepth;
    TextureHandle ambientOcclusion;
    TextureHandle screenSpaceColor;
    TextureHandle motionVectors;
    TextureHandle temporalResolved;
    TextureHandle temporalHistoryRead;
    TextureHandle temporalHistoryWrite;
    TextureHandle atmosphereTransmittance;
    TextureHandle atmosphereSkyView;
    std::array<TextureHandle, 2> oceanSpectrumA;
    std::array<TextureHandle, 2> oceanSpectrumB;
    TextureHandle oceanInitialSpectrum;
    TextureHandle oceanDisplacement;
    TextureHandle oceanNormalFoam;
    TextureHandle oceanSlopeMoments;
    std::array<TextureHandle, 2> oceanFoamHistory;
    BufferHandle oceanQueryConstants;
    BufferHandle oceanQueryPoints;
    BufferHandle oceanQueryResults;
    BufferHandle oceanQueryReadback;
    std::array<TextureHandle, 2> localWaveHeight;
    std::array<TextureHandle, 2> localWaveVelocity;
    std::array<TextureHandle, 2> localWaveFoam;
    TextureHandle localWaveDisplacement;
    TextureHandle localWaveGradient;
    BufferHandle localWaveDisturbanceUpload;
    BufferHandle localWaveDisturbance;
    std::uint32_t localWaveReadIndex = 0u;
    std::uint32_t localWaveWriteIndex = 1u;
    std::uint32_t oceanFoamReadIndex = 0u;
    std::uint32_t oceanFoamWriteIndex = 1u;
    TextureHandle terrainRawHeight;
    TextureHandle terrainErodedHeight;
    std::array<TextureHandle, SharedGBufferCount>
        gbuffer;
    TextureHandle hdrColor;
    TextureHandle bloomA;
    TextureHandle bloomB;
    TextureHandle outputColor;
};

class SharedRenderGraphFrontend
{
public:
    [[nodiscard]] static SharedSceneGraphHandles Build(
        RenderGraph& graph,
        const SharedRenderGraphResources& resources,
        const SharedRenderGraphOptions& options,
        const SharedRenderGraphCallbacks& callbacks);
};
} // namespace Prism::Renderer
