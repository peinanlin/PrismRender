#pragma once

#include "Renderer/Features/Fluid/FluidSettings.h"
#include "Renderer/Features/Fluid/FluidStatistics.h"

#include "RHI/GraphicsResources.h"
#include "Renderer/RenderGraph.h"

#include <DirectXMath.h>

#include <cstdint>
#include <filesystem>
#include <memory>
#include <vector>

namespace Prism::Asset
{
class ShaderManager;
}

namespace Prism::RHI
{
class IBuffer;
class ICommandContext;
class IComputePipeline;
class IDescriptorSet;
class IDescriptorSetLayout;
class IGraphicsDevice;
}

namespace Prism::Renderer
{
class PipelineCache;

struct PbfFluidGraphResources
{
    BufferHandle constants;
    BufferHandle positions;
    BufferHandle predictedPositions;
    BufferHandle velocities;
    BufferHandle velocityScratch;
    BufferHandle lambdas;
    BufferHandle densities;
    BufferHandle deltaOrCurl;
    BufferHandle cellCounts;
    BufferHandle cellParticles;
    BufferHandle neighborCounts;
    BufferHandle neighborParticles;
    BufferHandle diagnostics;
    bool simulationScheduled = false;
};

class PbfFluidSimulation
{
public:
    static constexpr std::uint32_t ThreadGroupSize = 64;
    static constexpr std::uint32_t DiagnosticValueCount = 4;

    void Initialize(
        RHI::IGraphicsDevice& device,
        Asset::ShaderManager& shaderManager,
        PipelineCache& pipelineCache,
        const std::filesystem::path& shaderDirectory,
        RHI::ShaderBinaryFormat shaderFormat,
        std::uint32_t framesInFlight,
        const FluidSettings& initialSettings = {});
    void Update(
        std::uint32_t frameIndex,
        float deltaTimeSeconds,
        const FluidSettings& settings);
    [[nodiscard]] PbfFluidGraphResources RegisterRenderGraph(
        RenderGraph& graph,
        std::uint32_t frameIndex,
        bool simulate = true);
    static void AddPasses(
        RenderGraph& graph,
        PbfFluidGraphResources& resources,
        RenderGraph::ParameterExecuteCallback execute,
        RenderGraph::QueueClass queue =
            RenderGraph::QueueClass::Graphics);
    void Execute(
        RHI::ICommandContext& commandContext,
        std::uint32_t frameIndex);
    void EndFrame(
        bool simulationExecuted,
        RHI::ResourceState particlePositionFinalState =
            RHI::ResourceState::UnorderedAccess,
        RHI::ResourceState particleDensityFinalState =
            RHI::ResourceState::UnorderedAccess);
    void RequestReset();

    [[nodiscard]] RHI::IBuffer&
        GetParticlePositionBuffer() const;
    [[nodiscard]] std::shared_ptr<RHI::IBuffer>
        GetParticlePositionBufferShared() const;
    [[nodiscard]] std::shared_ptr<RHI::IBuffer>
        GetParticleDensityBufferShared() const;
    [[nodiscard]] RHI::ResourceState
        GetParticlePositionInitialState() const;
    [[nodiscard]] std::uint32_t GetParticleCount() const;
    [[nodiscard]] const FluidSettings& GetSettings() const;
    [[nodiscard]] const FluidStatistics& GetStatistics() const;
    [[nodiscard]] bool IsInitialized() const;

private:
    struct alignas(16) SimulationConstants
    {
        DirectX::XMFLOAT4 domainMinCellSize{};
        DirectX::XMFLOAT4 domainMaxParticleRadius{};
        DirectX::XMFLOAT4 spawnMinParticleMass{};
        DirectX::XMFLOAT4 spawnMaxRestDensity{};
        DirectX::XMFLOAT4 gravityDeltaTime{};
        DirectX::XMFLOAT4 solverParameters{};
        DirectX::XMFLOAT4 velocityParameters{};
        DirectX::XMFLOAT4 kernelParameters{};
        DirectX::XMFLOAT4 correctionParameters{};
        DirectX::XMFLOAT4 surfaceClassificationParameters{};
        DirectX::XMUINT4 gridDimensions{};
        DirectX::XMUINT4 simulationCounts{};
        DirectX::XMUINT4 spawnDimensions{};
        DirectX::XMUINT4 neighborParameters{};
    };
    static_assert(
        sizeof(SimulationConstants) == 224,
        "PBF simulation constants must match the HLSL cbuffer layout.");

    struct FrameResources
    {
        std::shared_ptr<RHI::IBuffer> constants;
        std::shared_ptr<RHI::IBuffer> diagnosticsReadback;
        std::shared_ptr<RHI::IDescriptorSet> integrateSet;
        std::shared_ptr<RHI::IDescriptorSet> gridSet;
        std::shared_ptr<RHI::IDescriptorSet> constraintSet;
        std::shared_ptr<RHI::IDescriptorSet> velocitySet;
        bool diagnosticsReadbackPending = false;
    };

    struct GridCapacity
    {
        std::uint32_t x = 1;
        std::uint32_t y = 1;
        std::uint32_t z = 1;
        std::uint32_t cellCount = 1;
        std::uint64_t indexCount = 1;
    };

    void CreatePipelines(
        Asset::ShaderManager& shaderManager,
        PipelineCache& pipelineCache,
        const std::filesystem::path& shaderDirectory,
        RHI::ShaderBinaryFormat shaderFormat);
    void RebuildSimulationResources();
    void RebuildFrameBindings();
    void ResolveDiagnostics(std::uint32_t frameIndex);
    [[nodiscard]] SimulationConstants BuildConstants(
        float substepDeltaTime) const;
    [[nodiscard]] static FluidSettings SanitizeSettings(
        const FluidSettings& settings);
    [[nodiscard]] static GridCapacity CalculateGridCapacity(
        const FluidSettings& settings);
    [[nodiscard]] static DirectX::XMUINT4
        CalculateSpawnDimensions(
            std::uint32_t particleCount,
            FluidSpawnLayout layout);
    [[nodiscard]] static bool RequiresResourceRebuild(
        const FluidSettings& previous,
        const FluidSettings& next);

    RHI::IGraphicsDevice* m_device = nullptr;
    std::uint32_t m_framesInFlight = 0;
    FluidSettings m_settings{};
    FluidStatistics m_statistics{};
    GridCapacity m_grid{};
    double m_fixedTimeAccumulator = 0.0;
    float m_substepDeltaTime = 0.0f;
    std::uint32_t m_pendingFixedStepCount = 0;
    std::uint32_t m_executeFixedStepCount = 0;
    bool m_shouldStep = false;
    bool m_resetPending = true;
    bool m_lastRegistrationScheduled = false;
    bool m_graphRegisteredThisFrame = false;

    std::shared_ptr<RHI::IDescriptorSetLayout> m_integrateLayout;
    std::shared_ptr<RHI::IDescriptorSetLayout> m_gridLayout;
    std::shared_ptr<RHI::IDescriptorSetLayout> m_constraintLayout;
    std::shared_ptr<RHI::IDescriptorSetLayout> m_velocityLayout;

    std::shared_ptr<RHI::IComputePipeline> m_initializePipeline;
    std::shared_ptr<RHI::IComputePipeline> m_predictPipeline;
    std::shared_ptr<RHI::IComputePipeline> m_clearGridPipeline;
    std::shared_ptr<RHI::IComputePipeline> m_buildGridPipeline;
    std::shared_ptr<RHI::IComputePipeline> m_sortGridPipeline;
    std::shared_ptr<RHI::IComputePipeline> m_clearDiagnosticsPipeline;
    std::shared_ptr<RHI::IComputePipeline> m_buildNeighborsPipeline;
    std::shared_ptr<RHI::IComputePipeline> m_reduceGridDiagnosticsPipeline;
    std::shared_ptr<RHI::IComputePipeline> m_lambdaPipeline;
    std::shared_ptr<RHI::IComputePipeline> m_deltaPipeline;
    std::shared_ptr<RHI::IComputePipeline> m_applyDeltaPipeline;
    std::shared_ptr<RHI::IComputePipeline> m_updateVelocityPipeline;
    std::shared_ptr<RHI::IComputePipeline> m_vorticityPipeline;
    std::shared_ptr<RHI::IComputePipeline> m_finalizePipeline;

    std::shared_ptr<RHI::IBuffer> m_positions;
    std::shared_ptr<RHI::IBuffer> m_predictedPositions;
    std::shared_ptr<RHI::IBuffer> m_velocities;
    std::shared_ptr<RHI::IBuffer> m_velocityScratch;
    std::shared_ptr<RHI::IBuffer> m_lambdas;
    std::shared_ptr<RHI::IBuffer> m_densities;
    std::shared_ptr<RHI::IBuffer> m_deltaOrCurl;
    std::shared_ptr<RHI::IBuffer> m_cellCounts;
    std::shared_ptr<RHI::IBuffer> m_cellParticles;
    std::shared_ptr<RHI::IBuffer> m_neighborCounts;
    std::shared_ptr<RHI::IBuffer> m_neighborParticles;
    std::shared_ptr<RHI::IBuffer> m_diagnostics;
    std::vector<FrameResources> m_frames;

    RHI::ResourceState m_positionState =
        RHI::ResourceState::UnorderedAccess;
    RHI::ResourceState m_densityState =
        RHI::ResourceState::UnorderedAccess;
    std::uint32_t m_diagnosticsCountdown = 0;
    bool m_collectDiagnosticsThisFrame = false;
};
} // namespace Prism::Renderer
