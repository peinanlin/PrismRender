#include "Renderer/Features/Fluid/PbfFluidSimulation.h"

#include "Asset/ShaderManager.h"
#include "Core/Assert.h"
#include "Renderer/Pipeline/PipelineCache.h"
#include "RHI/ICommandContext.h"
#include "RHI/IGraphicsDevice.h"
#include "RHI/ShaderLayoutBuilder.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <string>
#include <string_view>
#include <utility>

namespace Prism::Renderer
{
namespace
{
constexpr std::uint32_t MaxParticleCount = 256u * 1024u;
constexpr std::uint32_t MaxGridCellCount = 2u * 1024u * 1024u;
constexpr std::uint32_t MaxFixedStepsPerFrame = 1u;
constexpr std::uint32_t DiagnosticsSampleInterval = 30u;
constexpr double FixedSimulationDeltaTime = 1.0 / 60.0;
constexpr float Pi = 3.14159265358979323846f;

float SanitizeFloat(
    const float value,
    const float fallback,
    const float minimum,
    const float maximum)
{
    return std::clamp(
        std::isfinite(value) ? value : fallback,
        minimum,
        maximum);
}

DirectX::XMFLOAT3 SanitizeVector(
    const DirectX::XMFLOAT3& value,
    const DirectX::XMFLOAT3& fallback)
{
    return {
        std::isfinite(value.x) ? value.x : fallback.x,
        std::isfinite(value.y) ? value.y : fallback.y,
        std::isfinite(value.z) ? value.z : fallback.z};
}

bool Equal(const DirectX::XMFLOAT3& lhs, const DirectX::XMFLOAT3& rhs)
{
    return lhs.x == rhs.x
        && lhs.y == rhs.y
        && lhs.z == rhs.z;
}

std::uint32_t DivideRoundUp(
    const std::uint32_t value,
    const std::uint32_t divisor)
{
    return (value + divisor - 1u) / divisor;
}
} // namespace

void PbfFluidSimulation::Initialize(
    RHI::IGraphicsDevice& device,
    Asset::ShaderManager& shaderManager,
    PipelineCache& pipelineCache,
    const std::filesystem::path& shaderDirectory,
    const RHI::ShaderBinaryFormat shaderFormat,
    const std::uint32_t framesInFlight,
    const FluidSettings& initialSettings)
{
    Core::Check(
        m_device == nullptr,
        "PBF fluid simulation cannot be initialized twice.");
    Core::Check(
        framesInFlight > 0,
        "PBF fluid simulation requires at least one frame in flight.");
    Core::Check(
        !shaderDirectory.empty(),
        "PBF fluid simulation requires a shader directory.");

    m_device = &device;
    m_framesInFlight = framesInFlight;
    m_settings = SanitizeSettings(initialSettings);
    m_frames.resize(framesInFlight);
    CreatePipelines(
        shaderManager,
        pipelineCache,
        shaderDirectory,
        shaderFormat);
    RebuildSimulationResources();
}

void PbfFluidSimulation::CreatePipelines(
    Asset::ShaderManager& shaderManager,
    PipelineCache& pipelineCache,
    const std::filesystem::path& shaderDirectory,
    const RHI::ShaderBinaryFormat shaderFormat)
{
    const RHI::ShaderBinary& initializeShader =
        shaderManager.LoadShader(
            shaderDirectory / "PbfIntegrate.hlsl",
            "InitializeParticlesCS",
            RHI::ShaderStage::Compute,
            shaderFormat);
    const RHI::ShaderBinary& predictShader =
        shaderManager.LoadShader(
            shaderDirectory / "PbfIntegrate.hlsl",
            "PredictPositionsCS",
            RHI::ShaderStage::Compute,
            shaderFormat);
    const std::array integrateStages{
        RHI::ShaderLayoutStage{
            &initializeShader.reflection,
            RHI::ShaderStage::Compute},
        RHI::ShaderLayoutStage{
            &predictShader.reflection,
            RHI::ShaderStage::Compute}};
    m_integrateLayout = m_device->CreateDescriptorSetLayout(
        RHI::BuildDescriptorSetLayout(integrateStages, {}));

    const RHI::ShaderBinary& clearGridShader =
        shaderManager.LoadShader(
            shaderDirectory / "PbfGrid.hlsl",
            "ClearGridCS",
            RHI::ShaderStage::Compute,
            shaderFormat);
    const RHI::ShaderBinary& buildGridShader =
        shaderManager.LoadShader(
            shaderDirectory / "PbfGrid.hlsl",
            "BuildGridCS",
            RHI::ShaderStage::Compute,
            shaderFormat);
    const RHI::ShaderBinary& clearDiagnosticsShader =
        shaderManager.LoadShader(
            shaderDirectory / "PbfGrid.hlsl",
            "ClearDiagnosticsCS",
            RHI::ShaderStage::Compute,
            shaderFormat);
    const RHI::ShaderBinary& sortGridShader =
        shaderManager.LoadShader(shaderDirectory / "PbfGrid.hlsl",
            "SortGridCS", RHI::ShaderStage::Compute, shaderFormat);
    const RHI::ShaderBinary& buildNeighborsShader =
        shaderManager.LoadShader(
            shaderDirectory / "PbfGrid.hlsl",
            "BuildNeighborsCS",
            RHI::ShaderStage::Compute,
            shaderFormat);
    const RHI::ShaderBinary& reduceGridDiagnosticsShader =
        shaderManager.LoadShader(
            shaderDirectory / "PbfGrid.hlsl",
            "ReduceGridDiagnosticsCS",
            RHI::ShaderStage::Compute,
            shaderFormat);
    const std::array gridStages{
        RHI::ShaderLayoutStage{&sortGridShader.reflection, RHI::ShaderStage::Compute},
        RHI::ShaderLayoutStage{
            &clearGridShader.reflection,
            RHI::ShaderStage::Compute},
        RHI::ShaderLayoutStage{
            &buildGridShader.reflection,
            RHI::ShaderStage::Compute},
        RHI::ShaderLayoutStage{
            &clearDiagnosticsShader.reflection,
            RHI::ShaderStage::Compute},
        RHI::ShaderLayoutStage{
            &buildNeighborsShader.reflection,
            RHI::ShaderStage::Compute},
        RHI::ShaderLayoutStage{
            &reduceGridDiagnosticsShader.reflection,
            RHI::ShaderStage::Compute}};
    m_gridLayout = m_device->CreateDescriptorSetLayout(
        RHI::BuildDescriptorSetLayout(gridStages, {}));

    const RHI::ShaderBinary& lambdaShader =
        shaderManager.LoadShader(
            shaderDirectory / "PbfConstraints.hlsl",
            "ComputeLambdaCS",
            RHI::ShaderStage::Compute,
            shaderFormat);
    const RHI::ShaderBinary& deltaShader =
        shaderManager.LoadShader(
            shaderDirectory / "PbfConstraints.hlsl",
            "ComputeDeltaCS",
            RHI::ShaderStage::Compute,
            shaderFormat);
    const RHI::ShaderBinary& applyDeltaShader =
        shaderManager.LoadShader(
            shaderDirectory / "PbfConstraints.hlsl",
            "ApplyDeltaCS",
            RHI::ShaderStage::Compute,
            shaderFormat);
    const std::array constraintStages{
        RHI::ShaderLayoutStage{
            &lambdaShader.reflection,
            RHI::ShaderStage::Compute},
        RHI::ShaderLayoutStage{
            &deltaShader.reflection,
            RHI::ShaderStage::Compute},
        RHI::ShaderLayoutStage{
            &applyDeltaShader.reflection,
            RHI::ShaderStage::Compute}};
    m_constraintLayout = m_device->CreateDescriptorSetLayout(
        RHI::BuildDescriptorSetLayout(constraintStages, {}));

    const RHI::ShaderBinary& updateVelocityShader =
        shaderManager.LoadShader(
            shaderDirectory / "PbfVelocity.hlsl",
            "UpdateVelocityCS",
            RHI::ShaderStage::Compute,
            shaderFormat);
    const RHI::ShaderBinary& vorticityShader =
        shaderManager.LoadShader(
            shaderDirectory / "PbfVelocity.hlsl",
            "ComputeVorticityCS",
            RHI::ShaderStage::Compute,
            shaderFormat);
    const RHI::ShaderBinary& finalizeShader =
        shaderManager.LoadShader(
            shaderDirectory / "PbfVelocity.hlsl",
            "FinalizeParticlesCS",
            RHI::ShaderStage::Compute,
            shaderFormat);
    const std::array velocityStages{
        RHI::ShaderLayoutStage{
            &updateVelocityShader.reflection,
            RHI::ShaderStage::Compute},
        RHI::ShaderLayoutStage{
            &vorticityShader.reflection,
            RHI::ShaderStage::Compute},
        RHI::ShaderLayoutStage{
            &finalizeShader.reflection,
            RHI::ShaderStage::Compute}};
    m_velocityLayout = m_device->CreateDescriptorSetLayout(
        RHI::BuildDescriptorSetLayout(velocityStages, {}));

    const auto createPipeline = [this, &pipelineCache](
                                    const RHI::ShaderBinary& shader,
                                    const std::shared_ptr<
                                        RHI::IDescriptorSetLayout>& layout,
                                    const std::string_view key)
    {
        RHI::ComputePipelineDescription description{};
        description.computeShader = shader;
        description.descriptorSetLayout = layout;
        return pipelineCache.GetOrCreateCompute(
            *m_device,
            key,
            description);
    };
    m_initializePipeline = createPipeline(
        initializeShader,
        m_integrateLayout,
        "Feature.PbfFluid.Initialize");
    m_predictPipeline = createPipeline(
        predictShader,
        m_integrateLayout,
        "Feature.PbfFluid.Predict");
    m_clearGridPipeline = createPipeline(
        clearGridShader,
        m_gridLayout,
        "Feature.PbfFluid.ClearGrid");
    m_buildGridPipeline = createPipeline(
        buildGridShader,
        m_gridLayout,
        "Feature.PbfFluid.BuildGrid");
    m_sortGridPipeline = createPipeline(
        sortGridShader, m_gridLayout, "Feature.PbfFluid.SortGrid");
    m_clearDiagnosticsPipeline = createPipeline(
        clearDiagnosticsShader,
        m_gridLayout,
        "Feature.PbfFluid.ClearDiagnostics");
    m_buildNeighborsPipeline = createPipeline(
        buildNeighborsShader,
        m_gridLayout,
        "Feature.PbfFluid.BuildNeighbors");
    m_reduceGridDiagnosticsPipeline = createPipeline(
        reduceGridDiagnosticsShader,
        m_gridLayout,
        "Feature.PbfFluid.ReduceGridDiagnostics");
    m_lambdaPipeline = createPipeline(
        lambdaShader,
        m_constraintLayout,
        "Feature.PbfFluid.Lambda");
    m_deltaPipeline = createPipeline(
        deltaShader,
        m_constraintLayout,
        "Feature.PbfFluid.Delta");
    m_applyDeltaPipeline = createPipeline(
        applyDeltaShader,
        m_constraintLayout,
        "Feature.PbfFluid.ApplyDelta");
    m_updateVelocityPipeline = createPipeline(
        updateVelocityShader,
        m_velocityLayout,
        "Feature.PbfFluid.UpdateVelocity");
    m_vorticityPipeline = createPipeline(
        vorticityShader,
        m_velocityLayout,
        "Feature.PbfFluid.Vorticity");
    m_finalizePipeline = createPipeline(
        finalizeShader,
        m_velocityLayout,
        "Feature.PbfFluid.Finalize");
}

void PbfFluidSimulation::Update(
    const std::uint32_t frameIndex,
    const float deltaTimeSeconds,
    const FluidSettings& settings)
{
    Core::Check(
        IsInitialized() && frameIndex < m_frames.size(),
        "PBF fluid update uses an invalid frame.");
    ResolveDiagnostics(frameIndex);
    m_statistics.resourceRebuiltThisFrame = false;

    FluidSettings sanitized = SanitizeSettings(settings);
    const bool resetEdge = sanitized.resetRequested
        && !m_settings.resetRequested;
    const bool rebuild = RequiresResourceRebuild(
        m_settings,
        sanitized);
    m_settings = sanitized;
    if (rebuild)
    {
        RebuildSimulationResources();
        m_statistics.resourceRebuiltThisFrame = true;
    }
    if (resetEdge)
    {
        RequestReset();
    }

    const float safeFrameDelta = std::isfinite(deltaTimeSeconds)
        ? std::max(deltaTimeSeconds, 0.0f)
        : 0.0f;
    const double simulationDelta = static_cast<double>(
        std::min(
            safeFrameDelta,
            m_settings.maxFrameDeltaTime)
        * m_settings.timeScale);
    if (m_settings.enabled && !m_settings.paused)
    {
        const double maximumAccumulatedTime =
            FixedSimulationDeltaTime
            * static_cast<double>(MaxFixedStepsPerFrame);
        m_fixedTimeAccumulator = std::min(
            m_fixedTimeAccumulator + simulationDelta,
            maximumAccumulatedTime);
        m_pendingFixedStepCount = std::min(
            static_cast<std::uint32_t>(
                m_fixedTimeAccumulator
                / FixedSimulationDeltaTime),
            MaxFixedStepsPerFrame);
    }
    else
    {
        // Pausing or disabling must not accumulate a large catch-up burst.
        m_fixedTimeAccumulator = 0.0;
        m_pendingFixedStepCount = 0;
    }
    m_shouldStep = m_pendingFixedStepCount > 0u;
    m_substepDeltaTime = m_shouldStep
        ? static_cast<float>(FixedSimulationDeltaTime)
            / static_cast<float>(m_settings.substepCount)
        : 0.0f;
    m_collectDiagnosticsThisFrame = m_settings.enabled
        && (m_resetPending || m_shouldStep)
        && m_diagnosticsCountdown == 0u;

    const SimulationConstants constants =
        BuildConstants(m_substepDeltaTime);
    m_frames[frameIndex].constants->Update(
        &constants,
        sizeof(constants));
    m_statistics.particleCount = m_settings.particleCount;
    m_statistics.gridCellCount = m_grid.cellCount;
    m_statistics.maxParticlesPerCell =
        m_settings.maxParticlesPerCell;
    m_statistics.maxNeighborsPerParticle =
        m_settings.maxNeighborsPerParticle;
    m_statistics.solverIterations =
        m_settings.solverIterations;
    m_statistics.substepCount =
        m_settings.substepCount;
    m_statistics.particleThreadGroups =
        DivideRoundUp(
            m_settings.particleCount,
            ThreadGroupSize);
    m_statistics.cellThreadGroups =
        DivideRoundUp(
            std::max(
                m_grid.cellCount,
                DiagnosticValueCount),
            ThreadGroupSize);
}

PbfFluidGraphResources
PbfFluidSimulation::RegisterRenderGraph(
    RenderGraph& graph,
    const std::uint32_t frameIndex,
    const bool simulate)
{
    Core::Check(
        IsInitialized() && frameIndex < m_frames.size(),
        "PBF fluid graph registration uses an invalid frame.");
    PbfFluidGraphResources resources{};
    resources.constants = graph.ImportBuffer(
        "PbfFluid.Constants",
        *m_frames[frameIndex].constants,
        RHI::ResourceState::ConstantBuffer);
    resources.positions = graph.ImportBuffer(
        "PbfFluid.Positions",
        *m_positions,
        m_positionState);
    resources.predictedPositions = graph.ImportBuffer(
        "PbfFluid.PredictedPositions",
        *m_predictedPositions,
        RHI::ResourceState::UnorderedAccess);
    resources.velocities = graph.ImportBuffer(
        "PbfFluid.Velocities",
        *m_velocities,
        RHI::ResourceState::UnorderedAccess);
    resources.velocityScratch = graph.ImportBuffer(
        "PbfFluid.VelocityScratch",
        *m_velocityScratch,
        RHI::ResourceState::UnorderedAccess);
    resources.lambdas = graph.ImportBuffer(
        "PbfFluid.Lambdas",
        *m_lambdas,
        RHI::ResourceState::UnorderedAccess);
    resources.densities = graph.ImportBuffer(
        "PbfFluid.Densities",
        *m_densities,
        m_densityState);
    resources.deltaOrCurl = graph.ImportBuffer(
        "PbfFluid.DeltaOrCurl",
        *m_deltaOrCurl,
        RHI::ResourceState::UnorderedAccess);
    resources.cellCounts = graph.ImportBuffer(
        "PbfFluid.CellCounts",
        *m_cellCounts,
        RHI::ResourceState::UnorderedAccess);
    resources.cellParticles = graph.ImportBuffer(
        "PbfFluid.CellParticles",
        *m_cellParticles,
        RHI::ResourceState::UnorderedAccess);
    resources.neighborCounts = graph.ImportBuffer(
        "PbfFluid.NeighborCounts",
        *m_neighborCounts,
        RHI::ResourceState::UnorderedAccess);
    resources.neighborParticles = graph.ImportBuffer(
        "PbfFluid.NeighborParticles",
        *m_neighborParticles,
        RHI::ResourceState::UnorderedAccess);
    resources.diagnostics = graph.ImportBuffer(
        "PbfFluid.Diagnostics",
        *m_diagnostics,
        RHI::ResourceState::UnorderedAccess);

    // Reset remains available while paused so the renderer never consumes an
    // uninitialized position buffer. `simulate` only gates time stepping.
    m_executeFixedStepCount =
        simulate ? m_pendingFixedStepCount : 0u;
    resources.simulationScheduled = m_settings.enabled
        && (m_resetPending || m_executeFixedStepCount > 0u);
    m_graphRegisteredThisFrame = true;
    m_lastRegistrationScheduled =
        resources.simulationScheduled;
    m_statistics.simulationScheduled =
        resources.simulationScheduled;
    return resources;
}

void PbfFluidSimulation::AddPasses(
    RenderGraph& graph,
    PbfFluidGraphResources& resources,
    RenderGraph::ParameterExecuteCallback execute,
    const RenderGraph::QueueClass queue)
{
    Core::Check(
        static_cast<bool>(execute),
        "PBF fluid simulation requires an execute callback.");
    auto parameters = graph.CreatePassParameters();
    parameters.ReadBuffer(
        resources.constants,
        RHI::ResourceState::ConstantBuffer);
    resources.positions = parameters.WriteBuffer(
        resources.positions,
        RHI::ResourceState::UnorderedAccess);
    resources.predictedPositions = parameters.WriteBuffer(
        resources.predictedPositions,
        RHI::ResourceState::UnorderedAccess);
    resources.velocities = parameters.WriteBuffer(
        resources.velocities,
        RHI::ResourceState::UnorderedAccess);
    resources.velocityScratch = parameters.WriteBuffer(
        resources.velocityScratch,
        RHI::ResourceState::UnorderedAccess);
    resources.lambdas = parameters.WriteBuffer(
        resources.lambdas,
        RHI::ResourceState::UnorderedAccess);
    resources.densities = parameters.WriteBuffer(
        resources.densities,
        RHI::ResourceState::UnorderedAccess);
    resources.deltaOrCurl = parameters.WriteBuffer(
        resources.deltaOrCurl,
        RHI::ResourceState::UnorderedAccess);
    resources.cellCounts = parameters.WriteBuffer(
        resources.cellCounts,
        RHI::ResourceState::UnorderedAccess);
    resources.cellParticles = parameters.WriteBuffer(
        resources.cellParticles,
        RHI::ResourceState::UnorderedAccess);
    resources.neighborCounts = parameters.WriteBuffer(
        resources.neighborCounts,
        RHI::ResourceState::UnorderedAccess);
    resources.neighborParticles = parameters.WriteBuffer(
        resources.neighborParticles,
        RHI::ResourceState::UnorderedAccess);
    resources.diagnostics = parameters.WriteBuffer(
        resources.diagnostics,
        RHI::ResourceState::UnorderedAccess);
    graph.AddParameterPass(
        "PbfFluid.Simulate",
        std::move(parameters),
        std::move(execute),
        RenderGraph::PassOptions{
            queue,
            true,
            false,
            false});
}

void PbfFluidSimulation::Execute(
    RHI::ICommandContext& commandContext,
    const std::uint32_t frameIndex)
{
    Core::Check(
        IsInitialized()
            && frameIndex < m_frames.size()
            && (m_executeFixedStepCount > 0u || m_resetPending),
        "PBF fluid execution uses an invalid or idle frame.");
    FrameResources& frame = m_frames[frameIndex];
    const std::uint32_t particleGroups =
        DivideRoundUp(
            m_settings.particleCount,
            ThreadGroupSize);
    const std::uint32_t cellGroups =
        DivideRoundUp(
            std::max(
                m_grid.cellCount,
                DiagnosticValueCount),
            ThreadGroupSize);
    std::uint32_t dispatchCount = 0;

    const auto dispatch = [
                              &commandContext,
                              &dispatchCount](
                              const std::shared_ptr<
                                  RHI::IComputePipeline>& pipeline,
                              const std::shared_ptr<
                                  RHI::IDescriptorSet>& descriptorSet,
                              const std::uint32_t groups)
    {
        commandContext.BindComputePipeline(*pipeline);
        commandContext.BindDescriptorSet(*descriptorSet);
        commandContext.Dispatch(groups, 1, 1);
        ++dispatchCount;
    };
    const auto uavBarrier = [&commandContext](
                                const std::shared_ptr<RHI::IBuffer>& buffer)
    {
        commandContext.BufferBarrier({
            buffer.get(),
            RHI::ResourceState::UnorderedAccess,
            RHI::ResourceState::UnorderedAccess});
    };

    if (m_collectDiagnosticsThisFrame)
    {
        dispatch(
            m_clearDiagnosticsPipeline,
            frame.gridSet,
            1u);
        uavBarrier(m_diagnostics);
    }

    if (m_resetPending)
    {
        dispatch(
            m_initializePipeline,
            frame.integrateSet,
            particleGroups);
        uavBarrier(m_positions);
        uavBarrier(m_velocities);
        if (m_executeFixedStepCount == 0u)
        {
            uavBarrier(m_predictedPositions);
        }
    }

    if (m_executeFixedStepCount > 0u)
    {
        for (std::uint32_t fixedStep = 0;
             fixedStep < m_executeFixedStepCount;
             ++fixedStep)
        {
            for (std::uint32_t substep = 0;
                 substep < m_settings.substepCount;
                 ++substep)
            {
                dispatch(
                    m_predictPipeline,
                    frame.integrateSet,
                    particleGroups);
                dispatch(
                    m_clearGridPipeline,
                    frame.gridSet,
                    cellGroups);
                uavBarrier(m_predictedPositions);
                uavBarrier(m_cellCounts);
                dispatch(
                    m_buildGridPipeline,
                    frame.gridSet,
                    particleGroups);
                uavBarrier(m_cellCounts);
                uavBarrier(m_cellParticles);
                dispatch(m_sortGridPipeline, frame.gridSet, cellGroups);
                uavBarrier(m_cellParticles);
                const bool finalSubstep =
                    fixedStep + 1u == m_executeFixedStepCount
                    && substep + 1u == m_settings.substepCount;
                if (m_collectDiagnosticsThisFrame && finalSubstep)
                {
                    dispatch(
                        m_reduceGridDiagnosticsPipeline,
                        frame.gridSet,
                        cellGroups);
                }
                dispatch(
                    m_buildNeighborsPipeline,
                    frame.gridSet,
                    particleGroups);
                uavBarrier(m_neighborCounts);
                uavBarrier(m_neighborParticles);
                for (std::uint32_t iteration = 0;
                     iteration < m_settings.solverIterations;
                     ++iteration)
                {
                    dispatch(
                        m_lambdaPipeline,
                        frame.constraintSet,
                        particleGroups);
                    uavBarrier(m_lambdas);
                    if (iteration + 1u
                        == m_settings.solverIterations)
                    {
                        uavBarrier(m_densities);
                    }
                    dispatch(
                        m_deltaPipeline,
                        frame.constraintSet,
                        particleGroups);
                    uavBarrier(m_deltaOrCurl);
                    dispatch(
                        m_applyDeltaPipeline,
                        frame.constraintSet,
                        particleGroups);
                    uavBarrier(m_predictedPositions);
                }
                dispatch(
                    m_updateVelocityPipeline,
                    frame.velocitySet,
                    particleGroups);
                uavBarrier(m_velocityScratch);
                if (m_settings.vorticity > 1.0e-6f)
                {
                    dispatch(
                        m_vorticityPipeline,
                        frame.velocitySet,
                        particleGroups);
                    uavBarrier(m_deltaOrCurl);
                }
                dispatch(
                    m_finalizePipeline,
                    frame.velocitySet,
                    particleGroups);
                uavBarrier(m_positions);
                uavBarrier(m_velocities);
            }
        }
        m_fixedTimeAccumulator = std::max(
            m_fixedTimeAccumulator
                - FixedSimulationDeltaTime
                    * static_cast<double>(
                        m_executeFixedStepCount),
            0.0);
        m_pendingFixedStepCount = 0;
        m_executeFixedStepCount = 0;
        m_shouldStep = false;
    }
    else
    {
        // A reset-only frame still initializes diagnostics and a valid grid.
        dispatch(
            m_clearGridPipeline,
            frame.gridSet,
            cellGroups);
        uavBarrier(m_cellCounts);
        dispatch(
            m_buildGridPipeline,
            frame.gridSet,
            particleGroups);
        uavBarrier(m_cellCounts);
        uavBarrier(m_cellParticles);
        if (m_collectDiagnosticsThisFrame)
        {
            dispatch(
                m_reduceGridDiagnosticsPipeline,
                frame.gridSet,
                cellGroups);
        }
    }

    if (m_collectDiagnosticsThisFrame)
    {
        commandContext.BufferBarrier({
            m_diagnostics.get(),
            RHI::ResourceState::UnorderedAccess,
            RHI::ResourceState::CopySource});
        commandContext.CopyBuffer(
            *m_diagnostics,
            *frame.diagnosticsReadback,
            DiagnosticValueCount * sizeof(std::uint32_t));
        commandContext.BufferBarrier({
            m_diagnostics.get(),
            RHI::ResourceState::CopySource,
            RHI::ResourceState::UnorderedAccess});
        frame.diagnosticsReadbackPending = true;
        m_diagnosticsCountdown = DiagnosticsSampleInterval - 1u;
    }
    else if (m_diagnosticsCountdown > 0u)
    {
        --m_diagnosticsCountdown;
    }
    m_collectDiagnosticsThisFrame = false;
    m_resetPending = false;
    m_statistics.dispatchCount = dispatchCount;
}

void PbfFluidSimulation::EndFrame(
    const bool simulationExecuted,
    const RHI::ResourceState particlePositionFinalState,
    const RHI::ResourceState particleDensityFinalState)
{
    Core::Check(
        !simulationExecuted || m_lastRegistrationScheduled,
        "PBF fluid end-frame reported an execution that was not scheduled.");
    Core::Check(
        particlePositionFinalState
                == RHI::ResourceState::UnorderedAccess
            || particlePositionFinalState
                == RHI::ResourceState::ShaderResource,
        "PBF particle positions must end in UAV or shader-resource state.");
    Core::Check(
        particleDensityFinalState
                == RHI::ResourceState::UnorderedAccess
            || particleDensityFinalState
                == RHI::ResourceState::ShaderResource,
        "PBF particle densities must end in UAV or shader-resource state.");
    if (m_graphRegisteredThisFrame)
    {
        m_positionState = particlePositionFinalState;
        m_densityState = particleDensityFinalState;
    }
    m_lastRegistrationScheduled = false;
    m_graphRegisteredThisFrame = false;
    m_executeFixedStepCount = 0;
}

void PbfFluidSimulation::RequestReset()
{
    Core::Check(
        IsInitialized(),
        "PBF fluid reset requires an initialized simulation.");
    m_fixedTimeAccumulator = 0.0;
    m_pendingFixedStepCount = 0;
    m_executeFixedStepCount = 0;
    m_shouldStep = false;
    m_resetPending = true;
    m_diagnosticsCountdown = 0u;
    m_collectDiagnosticsThisFrame = false;
}

RHI::IBuffer&
PbfFluidSimulation::GetParticlePositionBuffer() const
{
    Core::Check(
        m_positions != nullptr,
        "PBF particle positions are not initialized.");
    return *m_positions;
}

std::shared_ptr<RHI::IBuffer>
PbfFluidSimulation::GetParticlePositionBufferShared() const
{
    return m_positions;
}

std::shared_ptr<RHI::IBuffer>
PbfFluidSimulation::GetParticleDensityBufferShared() const
{
    return m_densities;
}

RHI::ResourceState
PbfFluidSimulation::GetParticlePositionInitialState() const
{
    return m_positionState;
}

std::uint32_t PbfFluidSimulation::GetParticleCount() const
{
    return m_settings.particleCount;
}

const FluidSettings& PbfFluidSimulation::GetSettings() const
{
    return m_settings;
}

const FluidStatistics& PbfFluidSimulation::GetStatistics() const
{
    return m_statistics;
}

bool PbfFluidSimulation::IsInitialized() const
{
    return m_device != nullptr
        && m_initializePipeline != nullptr
        && m_positions != nullptr
        && !m_frames.empty();
}

void PbfFluidSimulation::RebuildSimulationResources()
{
    Core::Check(
        m_device != nullptr,
        "PBF resource rebuild requires a graphics device.");
    m_grid = CalculateGridCapacity(m_settings);

    const auto createStorageBuffer = [this](
                                         const std::string_view name,
                                         const std::uint64_t elementCount,
                                         const std::uint32_t stride,
                                         const RHI::BufferUsage additionalUsage)
    {
        Core::Check(
            elementCount > 0
                && elementCount
                    <= std::numeric_limits<std::size_t>::max()
                        / stride,
            "PBF buffer capacity exceeds the host address space.");
        RHI::BufferDescription description{};
        description.size = static_cast<std::size_t>(
            elementCount * stride);
        description.stride = stride;
        description.usage =
            RHI::BufferUsage::Storage
            | RHI::BufferUsage::CopyDestination
            | additionalUsage;
        description.memoryAccess =
            RHI::MemoryAccess::GpuOnly;
        std::shared_ptr<RHI::IBuffer> buffer =
            m_device->CreateBuffer(description);
        buffer->SetDebugName(name);
        return buffer;
    };

    const std::uint64_t particleCount =
        m_settings.particleCount;
    m_positions = createStorageBuffer(
        "PbfFluid.Positions",
        particleCount,
        sizeof(DirectX::XMFLOAT4),
        RHI::BufferUsage::ShaderResource);
    m_predictedPositions = createStorageBuffer(
        "PbfFluid.PredictedPositions",
        particleCount,
        sizeof(DirectX::XMFLOAT4),
        RHI::BufferUsage::ShaderResource);
    m_velocities = createStorageBuffer(
        "PbfFluid.Velocities",
        particleCount,
        sizeof(DirectX::XMFLOAT4),
        RHI::BufferUsage::None);
    m_velocityScratch = createStorageBuffer(
        "PbfFluid.VelocityScratch",
        particleCount,
        sizeof(DirectX::XMFLOAT4),
        RHI::BufferUsage::None);
    m_lambdas = createStorageBuffer(
        "PbfFluid.Lambdas",
        particleCount,
        sizeof(float),
        RHI::BufferUsage::None);
    m_densities = createStorageBuffer(
        "PbfFluid.Densities",
        particleCount,
        sizeof(float),
        RHI::BufferUsage::ShaderResource);
    m_deltaOrCurl = createStorageBuffer(
        "PbfFluid.DeltaOrCurl",
        particleCount,
        sizeof(DirectX::XMFLOAT4),
        RHI::BufferUsage::None);
    m_cellCounts = createStorageBuffer(
        "PbfFluid.CellCounts",
        m_grid.cellCount,
        sizeof(std::uint32_t),
        RHI::BufferUsage::None);
    m_cellParticles = createStorageBuffer(
        "PbfFluid.CellParticles",
        m_grid.indexCount,
        sizeof(std::uint32_t),
        RHI::BufferUsage::None);
    m_neighborCounts = createStorageBuffer(
        "PbfFluid.NeighborCounts",
        particleCount,
        sizeof(std::uint32_t),
        RHI::BufferUsage::None);
    m_neighborParticles = createStorageBuffer(
        "PbfFluid.NeighborParticles",
        particleCount
            * static_cast<std::uint64_t>(
                m_settings.maxNeighborsPerParticle),
        sizeof(std::uint32_t),
        RHI::BufferUsage::None);
    m_diagnostics = createStorageBuffer(
        "PbfFluid.Diagnostics",
        DiagnosticValueCount,
        sizeof(std::uint32_t),
        RHI::BufferUsage::CopySource);

    m_positionState = RHI::ResourceState::UnorderedAccess;
    m_densityState = RHI::ResourceState::UnorderedAccess;
    m_fixedTimeAccumulator = 0.0;
    m_pendingFixedStepCount = 0;
    m_executeFixedStepCount = 0;
    m_shouldStep = false;
    m_resetPending = true;
    m_diagnosticsCountdown = 0u;
    m_collectDiagnosticsThisFrame = false;
    m_statistics.diagnosticsValid = false;
    m_statistics.gridOverflowCount = 0;
    m_statistics.maximumCellOccupancy = 0;
    m_statistics.invalidParticleCount = 0;
    m_statistics.neighborOverflowCount = 0;
    RebuildFrameBindings();

    m_statistics.allocatedBufferBytes =
        m_positions->GetDescription().size
        + m_predictedPositions->GetDescription().size
        + m_velocities->GetDescription().size
        + m_velocityScratch->GetDescription().size
        + m_lambdas->GetDescription().size
        + m_densities->GetDescription().size
        + m_deltaOrCurl->GetDescription().size
        + m_cellCounts->GetDescription().size
        + m_cellParticles->GetDescription().size
        + m_neighborCounts->GetDescription().size
        + m_neighborParticles->GetDescription().size
        + m_diagnostics->GetDescription().size;
    for (const FrameResources& frame : m_frames)
    {
        m_statistics.allocatedBufferBytes +=
            frame.constants->GetDescription().size
            + frame.diagnosticsReadback->GetDescription().size;
    }
}

void PbfFluidSimulation::RebuildFrameBindings()
{
    const SimulationConstants initialConstants =
        BuildConstants(0.0f);
    for (std::uint32_t frameIndex = 0;
         frameIndex < m_frames.size();
         ++frameIndex)
    {
        FrameResources& frame = m_frames[frameIndex];
        frame = {};

        RHI::BufferDescription constants{};
        constants.size = sizeof(SimulationConstants);
        constants.stride = sizeof(SimulationConstants);
        constants.usage = RHI::BufferUsage::Constant;
        constants.memoryAccess = RHI::MemoryAccess::CpuToGpu;
        frame.constants = m_device->CreateBuffer(
            constants,
            &initialConstants);
        frame.constants->SetDebugName(
            "PbfFluid.Constants."
            + std::to_string(frameIndex));

        RHI::BufferDescription readback{};
        readback.size =
            DiagnosticValueCount
            * sizeof(std::uint32_t);
        readback.stride = sizeof(std::uint32_t);
        readback.usage =
            RHI::BufferUsage::CopyDestination;
        readback.memoryAccess =
            RHI::MemoryAccess::GpuToCpu;
        frame.diagnosticsReadback =
            m_device->CreateBuffer(readback);
        frame.diagnosticsReadback->SetDebugName(
            "PbfFluid.DiagnosticsReadback."
            + std::to_string(frameIndex));

        frame.integrateSet =
            m_device->CreateDescriptorSet(
                m_integrateLayout);
        frame.integrateSet->WriteBuffer(0, frame.constants);
        frame.integrateSet->WriteBuffer(32, m_positions);
        frame.integrateSet->WriteBuffer(33, m_predictedPositions);
        frame.integrateSet->WriteBuffer(34, m_velocities);
        frame.integrateSet->WriteBuffer(35, m_velocityScratch);
        frame.integrateSet->WriteBuffer(36, m_lambdas);
        frame.integrateSet->WriteBuffer(37, m_deltaOrCurl);
        frame.integrateSet->WriteBuffer(38, m_densities);

        frame.gridSet =
            m_device->CreateDescriptorSet(m_gridLayout);
        frame.gridSet->WriteBuffer(0, frame.constants);
        frame.gridSet->WriteBuffer(32, m_predictedPositions);
        frame.gridSet->WriteBuffer(33, m_cellCounts);
        frame.gridSet->WriteBuffer(34, m_cellParticles);
        frame.gridSet->WriteBuffer(35, m_neighborCounts);
        frame.gridSet->WriteBuffer(36, m_neighborParticles);
        frame.gridSet->WriteBuffer(37, m_diagnostics);

        frame.constraintSet =
            m_device->CreateDescriptorSet(
                m_constraintLayout);
        frame.constraintSet->WriteBuffer(0, frame.constants);
        frame.constraintSet->WriteBuffer(32, m_predictedPositions);
        frame.constraintSet->WriteBuffer(33, m_lambdas);
        frame.constraintSet->WriteBuffer(34, m_deltaOrCurl);
        frame.constraintSet->WriteBuffer(35, m_neighborCounts);
        frame.constraintSet->WriteBuffer(36, m_neighborParticles);
        frame.constraintSet->WriteBuffer(37, m_densities);

        frame.velocitySet =
            m_device->CreateDescriptorSet(
                m_velocityLayout);
        frame.velocitySet->WriteBuffer(0, frame.constants);
        frame.velocitySet->WriteBuffer(32, m_positions);
        frame.velocitySet->WriteBuffer(33, m_predictedPositions);
        frame.velocitySet->WriteBuffer(34, m_velocities);
        frame.velocitySet->WriteBuffer(35, m_velocityScratch);
        frame.velocitySet->WriteBuffer(36, m_deltaOrCurl);
        frame.velocitySet->WriteBuffer(37, m_neighborCounts);
        frame.velocitySet->WriteBuffer(38, m_neighborParticles);
        frame.velocitySet->WriteBuffer(39, m_diagnostics);
        frame.velocitySet->WriteBuffer(40, m_densities);
    }
}

void PbfFluidSimulation::ResolveDiagnostics(
    const std::uint32_t frameIndex)
{
    FrameResources& frame = m_frames[frameIndex];
    if (!frame.diagnosticsReadbackPending)
    {
        return;
    }
    std::array<std::uint32_t, DiagnosticValueCount> values{};
    frame.diagnosticsReadback->Read(
        values.data(),
        sizeof(values));
    m_statistics.gridOverflowCount = values[0];
    m_statistics.maximumCellOccupancy = values[1];
    m_statistics.invalidParticleCount = values[2];
    m_statistics.neighborOverflowCount = values[3];
    m_statistics.diagnosticsValid = true;
    frame.diagnosticsReadbackPending = false;
}

PbfFluidSimulation::SimulationConstants
PbfFluidSimulation::BuildConstants(
    const float substepDeltaTime) const
{
    static_assert(
        sizeof(SimulationConstants) % 16u == 0u,
        "PBF constants must preserve cbuffer alignment.");
    SimulationConstants constants{};
    constants.domainMinCellSize = {
        m_settings.domainMin.x,
        m_settings.domainMin.y,
        m_settings.domainMin.z,
        m_settings.smoothingRadius};
    constants.domainMaxParticleRadius = {
        m_settings.domainMax.x,
        m_settings.domainMax.y,
        m_settings.domainMax.z,
        m_settings.particleRadius};
    constants.spawnMinParticleMass = {
        m_settings.spawnMin.x,
        m_settings.spawnMin.y,
        m_settings.spawnMin.z,
        m_settings.particleMass};
    constants.spawnMaxRestDensity = {
        m_settings.spawnMax.x,
        m_settings.spawnMax.y,
        m_settings.spawnMax.z,
        m_settings.restDensity};
    constants.gravityDeltaTime = {
        m_settings.gravity.x
            + m_settings.externalAcceleration.x,
        m_settings.gravity.y
            + m_settings.externalAcceleration.y,
        m_settings.gravity.z
            + m_settings.externalAcceleration.z,
        substepDeltaTime};
    constants.solverParameters = {
        m_settings.lambdaEpsilon,
        m_settings.artificialPressureK,
        m_settings.artificialPressureQ,
        m_settings.artificialPressureN};
    constants.velocityParameters = {
        m_settings.viscosity,
        m_settings.vorticity,
        m_settings.boundaryRestitution,
        m_settings.maxVelocity};

    const float smoothingRadius =
        m_settings.smoothingRadius;
    const float radiusSquared =
        smoothingRadius * smoothingRadius;
    const float radiusSixth =
        radiusSquared * radiusSquared * radiusSquared;
    const float radiusNinth =
        radiusSixth
        * radiusSquared
        * smoothingRadius;
    const float poly6Coefficient =
        315.0f / (64.0f * Pi * radiusNinth);
    const float spikyCoefficient =
        -45.0f / (Pi * radiusSixth);
    const float referenceDistance =
        m_settings.artificialPressureQ
        * smoothingRadius;
    const float referenceDifference =
        radiusSquared
        - referenceDistance * referenceDistance;
    const float referenceKernel =
        poly6Coefficient
        * referenceDifference
        * referenceDifference
        * referenceDifference;
    constants.kernelParameters = {
        poly6Coefficient,
        spikyCoefficient,
        referenceKernel,
        1.0f / m_settings.restDensity};
    constants.correctionParameters = {
        m_settings.maxPositionCorrection,
        m_settings.boundaryCornerDamping,
        m_settings.boundaryCornerUpwardVelocityLimit,
        0.0f};
    constants.surfaceClassificationParameters = {
        static_cast<float>(
            m_settings.minimumSplashNeighborCount),
        m_settings.splashVelocityThreshold,
        m_settings.splashDensityRatioThreshold,
        0.0f};
    constants.gridDimensions = {
        m_grid.x,
        m_grid.y,
        m_grid.z,
        m_settings.maxParticlesPerCell};
    constants.simulationCounts = {
        m_settings.particleCount,
        m_grid.cellCount,
        m_settings.solverIterations,
        m_settings.substepCount};
    constants.spawnDimensions =
        CalculateSpawnDimensions(
            m_settings.particleCount,
            m_settings.spawnLayout);
    constants.neighborParameters = {
        m_settings.maxNeighborsPerParticle,
        m_collectDiagnosticsThisFrame ? 1u : 0u,
        0u,
        0u};
    return constants;
}

FluidSettings PbfFluidSimulation::SanitizeSettings(
    const FluidSettings& settings)
{
    FluidSettings result = settings;
    const FluidSettings defaults{};
    result.particleCount = std::clamp(
        result.particleCount,
        1u,
        MaxParticleCount);
    result.solverIterations = std::clamp(
        result.solverIterations,
        1u,
        8u);
    result.substepCount = std::clamp(
        result.substepCount,
        1u,
        8u);
    result.maxParticlesPerCell = std::clamp(
        result.maxParticlesPerCell,
        8u,
        128u);
    result.maxNeighborsPerParticle = std::clamp(
        result.maxNeighborsPerParticle,
        32u,
        256u);
    result.timeScale = SanitizeFloat(
        result.timeScale,
        defaults.timeScale,
        0.0f,
        4.0f);
    result.maxFrameDeltaTime = SanitizeFloat(
        result.maxFrameDeltaTime,
        defaults.maxFrameDeltaTime,
        1.0f / 240.0f,
        0.1f);
    result.particleRadius = SanitizeFloat(
        result.particleRadius,
        defaults.particleRadius,
        0.001f,
        1.0f);
    result.smoothingRadius = SanitizeFloat(
        result.smoothingRadius,
        defaults.smoothingRadius,
        std::max(2.0f * result.particleRadius, 0.01f),
        2.0f);
    result.restDensity = SanitizeFloat(
        result.restDensity,
        defaults.restDensity,
        1.0f,
        100000.0f);
    result.particleMass = SanitizeFloat(
        result.particleMass,
        defaults.particleMass,
        1.0e-5f,
        1000.0f);
    result.lambdaEpsilon = SanitizeFloat(
        result.lambdaEpsilon,
        defaults.lambdaEpsilon,
        1.0e-6f,
        100000.0f);
    result.maxPositionCorrection = SanitizeFloat(
        result.maxPositionCorrection,
        defaults.maxPositionCorrection,
        1.0e-5f,
        result.smoothingRadius);
    result.artificialPressureK = SanitizeFloat(
        result.artificialPressureK,
        defaults.artificialPressureK,
        0.0f,
        1.0f);
    result.artificialPressureQ = SanitizeFloat(
        result.artificialPressureQ,
        defaults.artificialPressureQ,
        0.01f,
        0.99f);
    result.artificialPressureN = SanitizeFloat(
        result.artificialPressureN,
        defaults.artificialPressureN,
        1.0f,
        8.0f);
    result.viscosity = SanitizeFloat(
        result.viscosity,
        defaults.viscosity,
        0.0f,
        1.0f);
    result.vorticity = SanitizeFloat(
        result.vorticity,
        defaults.vorticity,
        0.0f,
        10.0f);
    result.boundaryRestitution = SanitizeFloat(
        result.boundaryRestitution,
        defaults.boundaryRestitution,
        0.0f,
        1.0f);
    result.boundaryCornerDamping = SanitizeFloat(
        result.boundaryCornerDamping,
        defaults.boundaryCornerDamping,
        0.0f,
        1.0f);
    result.boundaryCornerUpwardVelocityLimit = SanitizeFloat(
        result.boundaryCornerUpwardVelocityLimit,
        defaults.boundaryCornerUpwardVelocityLimit,
        0.0f,
        10.0f);
    result.maxVelocity = SanitizeFloat(
        result.maxVelocity,
        defaults.maxVelocity,
        0.1f,
        100.0f);
    result.minimumSplashNeighborCount = std::clamp(
        result.minimumSplashNeighborCount,
        1u,
        result.maxNeighborsPerParticle);
    result.splashVelocityThreshold = SanitizeFloat(
        result.splashVelocityThreshold,
        defaults.splashVelocityThreshold,
        0.0f,
        result.maxVelocity);
    result.splashDensityRatioThreshold = SanitizeFloat(
        result.splashDensityRatioThreshold,
        defaults.splashDensityRatioThreshold,
        0.0f,
        2.0f);
    result.gravity = SanitizeVector(
        result.gravity,
        defaults.gravity);
    result.externalAcceleration = SanitizeVector(
        result.externalAcceleration,
        defaults.externalAcceleration);
    result.domainMin = SanitizeVector(
        result.domainMin,
        defaults.domainMin);
    result.domainMax = SanitizeVector(
        result.domainMax,
        defaults.domainMax);
    const float minimumExtent = std::max(
        result.smoothingRadius,
        2.0f * result.particleRadius);
    result.domainMax.x = std::max(
        result.domainMax.x,
        result.domainMin.x + minimumExtent);
    result.domainMax.y = std::max(
        result.domainMax.y,
        result.domainMin.y + minimumExtent);
    result.domainMax.z = std::max(
        result.domainMax.z,
        result.domainMin.z + minimumExtent);

    result.spawnMin = SanitizeVector(
        result.spawnMin,
        defaults.spawnMin);
    result.spawnMax = SanitizeVector(
        result.spawnMax,
        defaults.spawnMax);
    const DirectX::XMFLOAT3 lower{
        result.domainMin.x + result.particleRadius,
        result.domainMin.y + result.particleRadius,
        result.domainMin.z + result.particleRadius};
    const DirectX::XMFLOAT3 upper{
        result.domainMax.x - result.particleRadius,
        result.domainMax.y - result.particleRadius,
        result.domainMax.z - result.particleRadius};
    result.spawnMin = {
        std::clamp(result.spawnMin.x, lower.x, upper.x),
        std::clamp(result.spawnMin.y, lower.y, upper.y),
        std::clamp(result.spawnMin.z, lower.z, upper.z)};
    result.spawnMax = {
        std::clamp(result.spawnMax.x, result.spawnMin.x, upper.x),
        std::clamp(result.spawnMax.y, result.spawnMin.y, upper.y),
        std::clamp(result.spawnMax.z, result.spawnMin.z, upper.z)};

    if (static_cast<std::uint32_t>(result.renderMode)
        > static_cast<std::uint32_t>(FluidRenderMode::Foam))
    {
        result.renderMode = FluidRenderMode::Realistic;
    }
    if (static_cast<std::uint32_t>(result.demoPipeline)
        > static_cast<std::uint32_t>(
            FluidDemoPipeline::ScreenSpaceToonFoam))
    {
        result.demoPipeline = defaults.demoPipeline;
    }
    if (static_cast<std::uint32_t>(result.spawnLayout)
        > static_cast<std::uint32_t>(
            FluidSpawnLayout::DoubleDam))
    {
        result.spawnLayout = defaults.spawnLayout;
    }
    result.renderParticleRadiusScale = SanitizeFloat(
        result.renderParticleRadiusScale,
        defaults.renderParticleRadiusScale,
        0.75f,
        3.0f);
    result.surfaceCoverageThreshold = SanitizeFloat(
        result.surfaceCoverageThreshold,
        defaults.surfaceCoverageThreshold,
        0.0f,
        2.0f);
    result.bilateralIterations = std::min(
        result.bilateralIterations,
        8u);
    result.bilateralRadius = std::clamp(
        result.bilateralRadius,
        1u,
        15u);
    result.bilateralSpatialSigma = SanitizeFloat(
        result.bilateralSpatialSigma,
        defaults.bilateralSpatialSigma,
        0.1f,
        20.0f);
    result.bilateralDepthSigma = SanitizeFloat(
        result.bilateralDepthSigma,
        defaults.bilateralDepthSigma,
        0.0001f,
        10.0f);
    result.waterColor = SanitizeVector(
        result.waterColor,
        defaults.waterColor);
    result.absorption = SanitizeVector(
        result.absorption,
        defaults.absorption);
    result.scattering = SanitizeVector(
        result.scattering,
        defaults.scattering);
    result.ior = SanitizeFloat(
        result.ior,
        defaults.ior,
        1.0f,
        3.0f);
    result.refractionScale = SanitizeFloat(
        result.refractionScale,
        defaults.refractionScale,
        0.0f,
        1.0f);
    result.reflectionStrength = SanitizeFloat(
        result.reflectionStrength,
        defaults.reflectionStrength,
        0.0f,
        1.0f);
    result.thicknessScale = SanitizeFloat(
        result.thicknessScale,
        defaults.thicknessScale,
        0.0f,
        100.0f);
    result.toonBands = std::clamp(
        result.toonBands,
        2u,
        8u);
    result.toonEdgeWidth = SanitizeFloat(
        result.toonEdgeWidth,
        defaults.toonEdgeWidth,
        0.0f,
        8.0f);
    result.foamDensityThreshold = SanitizeFloat(
        result.foamDensityThreshold,
        defaults.foamDensityThreshold,
        0.0f,
        2.0f);
    result.foamErosionIterations = std::min(
        result.foamErosionIterations,
        8u);
    result.causticsIntensity = SanitizeFloat(
        result.causticsIntensity,
        defaults.causticsIntensity,
        0.0f,
        20.0f);
    result.causticsRefractionScalePixels = SanitizeFloat(
        result.causticsRefractionScalePixels,
        defaults.causticsRefractionScalePixels,
        0.0f,
        256.0f);
    result.causticsDepthAttenuation = SanitizeFloat(
        result.causticsDepthAttenuation,
        defaults.causticsDepthAttenuation,
        0.0f,
        10.0f);
    result.causticsFocusStrength = SanitizeFloat(
        result.causticsFocusStrength,
        defaults.causticsFocusStrength,
        0.0f,
        32.0f);
    result.causticsFocusPower = SanitizeFloat(
        result.causticsFocusPower,
        defaults.causticsFocusPower,
        0.1f,
        16.0f);
    result.causticsBlurRadius = std::min(
        result.causticsBlurRadius,
        16u);
    result.causticsBlurSigma = SanitizeFloat(
        result.causticsBlurSigma,
        defaults.causticsBlurSigma,
        0.1f,
        20.0f);
    return result;
}

PbfFluidSimulation::GridCapacity
PbfFluidSimulation::CalculateGridCapacity(
    const FluidSettings& settings)
{
    const auto dimension = [cellSize = settings.smoothingRadius](
                               const float minimum,
                               const float maximum)
    {
        return std::max(
            static_cast<std::uint32_t>(
                std::ceil((maximum - minimum) / cellSize)),
            1u);
    };
    GridCapacity result{};
    result.x = dimension(
        settings.domainMin.x,
        settings.domainMax.x);
    result.y = dimension(
        settings.domainMin.y,
        settings.domainMax.y);
    result.z = dimension(
        settings.domainMin.z,
        settings.domainMax.z);
    const std::uint64_t cellCount =
        static_cast<std::uint64_t>(result.x)
        * result.y * result.z;
    Core::Check(
        cellCount > 0
            && cellCount <= MaxGridCellCount,
        "PBF grid exceeds the supported cell capacity.");
    result.cellCount =
        static_cast<std::uint32_t>(cellCount);
    result.indexCount = cellCount
        * settings.maxParticlesPerCell;
    Core::Check(
        result.indexCount
            <= std::numeric_limits<std::uint32_t>::max(),
        "PBF fixed-bucket grid exceeds structured-buffer indexing limits.");
    return result;
}

DirectX::XMUINT4
PbfFluidSimulation::CalculateSpawnDimensions(
    const std::uint32_t particleCount,
    const FluidSpawnLayout layout)
{
    if (layout == FluidSpawnLayout::DoubleDam)
    {
        const std::uint32_t particlesPerDam =
            (particleCount + 1u) / 2u;
        const std::uint32_t horizontalSide = std::max(
            static_cast<std::uint32_t>(std::ceil(std::cbrt(
                static_cast<double>(particlesPerDam) / 4.0))),
            1u);
        const std::uint64_t horizontalPlane =
            static_cast<std::uint64_t>(horizontalSide)
            * horizontalSide;
        const std::uint32_t verticalCount =
            static_cast<std::uint32_t>(
                (particlesPerDam + horizontalPlane - 1u)
                / horizontalPlane);
        return {
            horizontalSide,
            std::max(verticalCount, 1u),
            horizontalSide,
            1u};
    }
    const std::uint32_t side = std::max(
        static_cast<std::uint32_t>(
            std::ceil(std::cbrt(
                static_cast<double>(particleCount)))),
        1u);
    const std::uint64_t plane =
        static_cast<std::uint64_t>(side) * side;
    const std::uint32_t depth =
        static_cast<std::uint32_t>(
            (particleCount + plane - 1u) / plane);
    return {side, side, std::max(depth, 1u), 0u};
}

bool PbfFluidSimulation::RequiresResourceRebuild(
    const FluidSettings& previous,
    const FluidSettings& next)
{
    return previous.particleCount != next.particleCount
        || previous.maxParticlesPerCell
            != next.maxParticlesPerCell
        || previous.maxNeighborsPerParticle
            != next.maxNeighborsPerParticle
        || previous.smoothingRadius
            != next.smoothingRadius
        || !Equal(previous.domainMin, next.domainMin)
        || !Equal(previous.domainMax, next.domainMax);
}
} // namespace Prism::Renderer
