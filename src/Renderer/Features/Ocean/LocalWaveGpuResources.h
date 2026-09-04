#pragma once

#include "RHI/GraphicsResources.h"
#include "Renderer/Features/Ocean/OceanSettings.h"
#include "Renderer/RenderGraph.h"

#include <array>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>
#include <vector>

namespace Prism::RHI
{
class IGraphicsDevice;
class ICommandContext;
class IComputePipeline;
class IDescriptorSet;
class IDescriptorSetLayout;
class IBuffer;
enum class ShaderBinaryFormat;
}

namespace Prism::Asset
{
class ShaderManager;
}

namespace Prism::Renderer
{
struct LocalWaveDisturbance;
class PipelineCache;
}

namespace Prism::Renderer
{
struct LocalWaveTextureResource
{
    std::shared_ptr<RHI::ITexture> texture;
    std::shared_ptr<RHI::ITextureView> sampled;
    std::shared_ptr<RHI::ITextureView> storage;
};

struct LocalWaveGraphHandles
{
    BufferHandle disturbanceUpload;
    BufferHandle disturbance;
    std::array<TextureHandle, 2> height{};
    std::array<TextureHandle, 2> velocity{};
    std::array<TextureHandle, 2> foam{};
    TextureHandle displacement;
    TextureHandle gradient;
    std::uint32_t readIndex = 0u;
    std::uint32_t writeIndex = 1u;
};

struct LocalWaveGraphCallbacks
{
    bool asyncCompute = false;
    RenderGraph::ParameterExecuteCallback copyDisturbances;
    RenderGraph::ParameterExecuteCallback simulate;
};

struct LocalWaveFeatureSlot
{
};

struct LocalWaveGraphContribution
{
    std::array<RHI::ITexture*, 2> height{};
    std::array<RHI::ITexture*, 2> velocity{};
    std::array<RHI::ITexture*, 2> foam{};
    RHI::ITexture* displacement = nullptr;
    RHI::ITexture* gradient = nullptr;
    RHI::IBuffer* disturbanceUpload = nullptr;
    RHI::IBuffer* disturbance = nullptr;
    std::array<RHI::ResourceState, 2> heightStates{};
    std::array<RHI::ResourceState, 2> velocityStates{};
    std::array<RHI::ResourceState, 2> foamStates{};
    RHI::ResourceState displacementState =
        RHI::ResourceState::Undefined;
    RHI::ResourceState gradientState =
        RHI::ResourceState::Undefined;
    RHI::ResourceState disturbanceUploadState =
        RHI::ResourceState::CopySource;
    RHI::ResourceState disturbanceState =
        RHI::ResourceState::ShaderResource;
    std::uint32_t readIndex = 0u;
    std::uint32_t writeIndex = 1u;
    LocalWaveGraphCallbacks callbacks;
};

// Owns the persistent local-wave GPU resources and their graph scheduling
// contract. CPU disturbance records are copied into the storage buffer before
// the compute update, while the resource layout remains backend-neutral.
class LocalWaveGpuResources
{
public:
    static constexpr std::uint32_t MinimumGridSize = 128u;
    static constexpr std::uint32_t MaximumGridSize = 2048u;

    bool Configure(RHI::IGraphicsDevice& device,
        const OceanLocalWaveSettings& settings);
    void InitializeGpu(RHI::IGraphicsDevice& device,
        Asset::ShaderManager& shaderManager,
        PipelineCache& pipelineCache,
        const std::filesystem::path& shaderDirectory,
        RHI::ShaderBinaryFormat shaderFormat,
        std::uint32_t framesInFlight);
    void Update(std::uint32_t frameIndex, float deltaSeconds,
        const OceanLocalWaveSettings& settings, bool paused,
        std::span<const LocalWaveDisturbance> disturbances = {});
    void SetAsyncComputeEnabled(bool enabled) noexcept
    {
        m_asyncComputeEnabled = enabled;
    }
    [[nodiscard]] LocalWaveGraphCallbacks CreateGraphCallbacks(
        std::uint32_t frameIndex);
    [[nodiscard]] LocalWaveGraphContribution
        CreateRenderGraphContribution(std::uint32_t frameIndex);
    void EndFrame(bool enabled) noexcept;
    static void AddPasses(RenderGraph& graph, LocalWaveGraphHandles& handles,
        const LocalWaveGraphCallbacks& callbacks);
    void Reset() noexcept;

    [[nodiscard]] bool IsInitialized() const noexcept
    {
        return m_gridSize != 0u;
    }
    [[nodiscard]] std::uint32_t GridSize() const noexcept
    {
        return m_gridSize;
    }
    [[nodiscard]] float DomainSizeMeters() const noexcept
    {
        return m_domainSizeMeters;
    }
    [[nodiscard]] float AllocatedMegabytes() const noexcept
    {
        return m_allocatedMegabytes;
    }
    [[nodiscard]] std::uint64_t ResourceGeneration() const noexcept
    {
        return m_resourceGeneration;
    }
    [[nodiscard]] bool IsGpuReady() const noexcept
    {
        return m_computePipeline != nullptr && !m_frames.empty();
    }
    [[nodiscard]] bool SimulationScheduled() const noexcept
    {
        return m_simulationScheduled;
    }
    [[nodiscard]] std::uint32_t DispatchCount() const noexcept
    {
        return m_simulationScheduled ? 1u : 0u;
    }
    [[nodiscard]] float LastDeltaSeconds() const noexcept
    {
        return m_deltaSeconds;
    }

    [[nodiscard]] const std::array<LocalWaveTextureResource, 2>&
        Height() const noexcept { return m_height; }
    [[nodiscard]] const std::array<LocalWaveTextureResource, 2>&
        Velocity() const noexcept { return m_velocity; }
    [[nodiscard]] const LocalWaveTextureResource& Displacement() const noexcept
    {
        return m_displacement;
    }
    [[nodiscard]] const LocalWaveTextureResource& Gradient() const noexcept
    {
        return m_gradient;
    }
    [[nodiscard]] const std::array<LocalWaveTextureResource, 2>&
        Foam() const noexcept { return m_foam; }

    [[nodiscard]] static std::uint32_t ClampGridSize(
        std::uint32_t requested) noexcept;
    [[nodiscard]] static float EstimateAllocatedMegabytes(
        std::uint32_t gridSize) noexcept;

private:
    struct FrameResources
    {
        std::shared_ptr<RHI::IBuffer> constants;
        std::array<std::shared_ptr<RHI::IDescriptorSet>, 2> sets;
    };
    void RebindDescriptorSets();
    void Execute(RHI::ICommandContext& commandContext,
        std::uint32_t frameIndex) const;
    void ExecuteCopy(RHI::ICommandContext& commandContext) const;
    [[nodiscard]] static LocalWaveTextureResource CreateTexture(
        RHI::IGraphicsDevice& device,
        std::uint32_t gridSize,
        RHI::Format format,
        const char* debugName);
    std::array<LocalWaveTextureResource, 2> m_height{};
    std::array<LocalWaveTextureResource, 2> m_velocity{};
    LocalWaveTextureResource m_displacement{};
    LocalWaveTextureResource m_gradient{};
    std::array<LocalWaveTextureResource, 2> m_foam{};
    std::uint32_t m_gridSize = 0u;
    bool m_asyncComputeEnabled = false;
    float m_domainSizeMeters = 0.0f;
    float m_allocatedMegabytes = 0.0f;
    std::uint64_t m_resourceGeneration = 0u;
    RHI::IGraphicsDevice* m_device = nullptr;
    std::shared_ptr<RHI::IComputePipeline> m_computePipeline;
    std::shared_ptr<RHI::IDescriptorSetLayout> m_layout;
    std::vector<FrameResources> m_frames;
    std::shared_ptr<RHI::IBuffer> m_disturbanceBuffer;
    std::shared_ptr<RHI::IBuffer> m_disturbanceUploadBuffer;
    std::uint32_t m_framesInFlight = 0u;
    std::uint32_t m_readIndex = 0u;
    std::uint32_t m_frameIndex = 0u;
    std::uint32_t m_resolutionConstants = 0u;
    float m_deltaSeconds = 0.0f;
    bool m_simulationScheduled = false;
    bool m_needsInitialization = true;
    // Continue long enough for an impulse to propagate and its foam to decay,
    // while allowing an untouched local solver to consume no GPU work.
    std::uint32_t m_activeFramesRemaining = 0u;
    std::uint32_t m_disturbanceCount = 0u;
    OceanLocalWaveSettings m_settings{};
};
} // namespace Prism::Renderer
