#pragma once

#include "RHI/GraphicsResources.h"
#include "RHI/ShaderTypes.h"
#include "Renderer/Features/Ocean/OceanSpectrumGenerator.h"
#include "Renderer/Features/Ocean/OceanSettings.h"
#include "Renderer/Features/Ocean/OceanStatistics.h"
#include "Renderer/RenderGraph.h"

#include <array>
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
class IGraphicsDevice;
class ICommandContext;
}

namespace Prism::Renderer
{
class PipelineCache;

struct OceanSpectrumPrecisionPolicy
{
    RHI::Format workingFormat = RHI::Format::Rgba32Float;
    std::uint32_t bitsPerChannel = 32u;
    bool allowHalfPrecisionOptimization = false;
};

struct OceanSpectrumWorkingResource
{
    std::shared_ptr<RHI::ITexture> texture;
    std::shared_ptr<RHI::ITextureView> sampledArray;
    std::shared_ptr<RHI::ITextureView> storageArray;
    std::array<std::shared_ptr<RHI::ITextureView>, 4> sampledSlices;
    std::array<std::shared_ptr<RHI::ITextureView>, 4> storageSlices;
};

struct OceanPublishedMapResource
{
    std::shared_ptr<RHI::ITexture> texture;
    std::shared_ptr<RHI::ITextureView> sampledArray;
    std::array<std::shared_ptr<RHI::ITextureView>, 4> sampledSlices;
    std::vector<std::shared_ptr<RHI::ITextureView>> sampledMips;
    std::vector<std::shared_ptr<RHI::ITextureView>> storageMips;
};

struct OceanPublishedMapMetadata
{
    OceanSimulationQuality quality = OceanSimulationQuality::Normal;
    std::uint32_t resolution = 0u;
    std::uint32_t cascadeCount = 0u;
    std::uint32_t mipLevelCount = 0u;
    std::uint64_t resourceGeneration = 0u;
    RHI::Format displacementFormat = RHI::Format::Unknown;
    RHI::Format gradientFormat = RHI::Format::Unknown;
    RHI::Format momentFormat = RHI::Format::Unknown;
    RHI::Format foamFormat = RHI::Format::Unknown;
    bool yUpXzHorizontal = true;
};

struct SpectralOceanGraphHandles
{
    TextureHandle initialSpectrum;
    std::array<TextureHandle, 2> spectrumA;
    std::array<TextureHandle, 2> spectrumB;
    TextureHandle displacement;
    TextureHandle gradientFoam;
    TextureHandle slopeMoments;
    std::array<TextureHandle, 2> foamHistory;
    std::uint32_t foamReadIndex = 0u;
    std::uint32_t foamWriteIndex = 1u;
};

struct SpectralOceanGraphCallbacks
{
    bool asyncCompute = false;
    RenderGraph::ParameterExecuteCallback initialSpectrum;
    RenderGraph::ParameterExecuteCallback evolution;
    RenderGraph::ParameterExecuteCallback horizontalFft;
    RenderGraph::ParameterExecuteCallback verticalFft;
    RenderGraph::ParameterExecuteCallback outputMaps;
    RenderGraph::ParameterExecuteCallback foam;
    RenderGraph::ParameterExecuteCallback mips;
};

struct SpectralOceanFeatureSlot
{
};

struct SpectralOceanGraphContribution
{
    RHI::ITexture* initialSpectrum = nullptr;
    std::array<RHI::ITexture*, 2> spectrumA{};
    std::array<RHI::ITexture*, 2> spectrumB{};
    RHI::ITexture* displacement = nullptr;
    RHI::ITexture* gradientFoam = nullptr;
    RHI::ITexture* slopeMoments = nullptr;
    std::array<RHI::ITexture*, 2> foamHistory{};
    RHI::ResourceState initialSpectrumState =
        RHI::ResourceState::Undefined;
    std::array<RHI::ResourceState, 2> spectrumAStates{};
    std::array<RHI::ResourceState, 2> spectrumBStates{};
    RHI::ResourceState displacementState =
        RHI::ResourceState::Undefined;
    RHI::ResourceState gradientFoamState =
        RHI::ResourceState::Undefined;
    RHI::ResourceState slopeMomentsState =
        RHI::ResourceState::Undefined;
    std::array<RHI::ResourceState, 2> foamHistoryStates{};
    std::uint32_t foamReadIndex = 0u;
    std::uint32_t foamWriteIndex = 1u;
    bool rebuildInitialSpectrum = false;
    SpectralOceanGraphCallbacks callbacks;
};

class SpectralOceanSimulation
{
public:
    static constexpr std::uint32_t CascadeCount = 4u;
    static constexpr std::uint32_t WorkingPingCount = 2u;
    static constexpr std::uint32_t AllCascadesMask =
        (1u << CascadeCount) - 1u;

    SpectralOceanSimulation() = default;
    ~SpectralOceanSimulation();
    SpectralOceanSimulation(const SpectralOceanSimulation&) = delete;
    SpectralOceanSimulation& operator=(const SpectralOceanSimulation&) = delete;
    SpectralOceanSimulation(SpectralOceanSimulation&&) noexcept;
    SpectralOceanSimulation& operator=(SpectralOceanSimulation&&) noexcept;

    void InitializeWorkingResources(RHI::IGraphicsDevice& device,
        OceanSimulationQuality quality);
    void InitializeGpu(RHI::IGraphicsDevice& device,
        Asset::ShaderManager& shaderManager,
        PipelineCache& pipelineCache,
        const std::filesystem::path& shaderDirectory,
        RHI::ShaderBinaryFormat shaderFormat,
        OceanSimulationQuality quality,
        std::uint32_t framesInFlight);
    void Reset() noexcept;

    [[nodiscard]] bool ConfigureSpectrum(const OceanSettings& settings,
        std::uint32_t randomSeed = 0x4f1bbcdcu) noexcept;
    [[nodiscard]] bool IsInitialSpectrumRebuildPending() const noexcept;
    void MarkInitialSpectrumCommitted() noexcept;
    [[nodiscard]] std::uint64_t PendingSpectrumVersion() const noexcept
    {
        return m_pendingSpectrumVersion;
    }
    [[nodiscard]] std::uint64_t ActiveSpectrumVersion() const noexcept
    {
        return m_activeSpectrumVersion;
    }
    [[nodiscard]] bool Update(std::uint32_t frameIndex,
        double absoluteTimeSeconds,
        const OceanSettings& settings,
        std::uint32_t randomSeed = 0x4f1bbcdcu);
    void SetActiveCascadeMask(std::uint32_t mask) noexcept;
    void SetAsyncComputeEnabled(bool enabled) noexcept
    {
        m_asyncComputeEnabled = enabled;
    }
    [[nodiscard]] std::uint32_t ActiveCascadeMask() const noexcept;

    [[nodiscard]] SpectralOceanGraphCallbacks CreateGraphCallbacks(
        std::uint32_t frameIndex);
    [[nodiscard]] SpectralOceanGraphContribution
        CreateRenderGraphContribution(std::uint32_t frameIndex);
    void EndFrame(bool enabled) noexcept;
    static void AddPasses(RenderGraph& graph,
        SpectralOceanGraphHandles& handles,
        bool rebuildInitialSpectrum,
        const SpectralOceanGraphCallbacks& callbacks);

    void ExecuteInitialSpectrum(RHI::ICommandContext& commandContext,
        std::uint32_t frameIndex);
    void ExecuteEvolution(RHI::ICommandContext& commandContext,
        std::uint32_t frameIndex) const;
    void ExecuteHorizontalFft(RHI::ICommandContext& commandContext) const;
    void ExecuteVerticalFft(RHI::ICommandContext& commandContext) const;
    void ExecuteOutputMaps(RHI::ICommandContext& commandContext) const;
    void ExecuteFoam(RHI::ICommandContext& commandContext,
        std::uint32_t frameIndex) const;
    void ExecuteMips(RHI::ICommandContext& commandContext) const;
    void ResetFoamHistory() noexcept;

    [[nodiscard]] bool IsInitialized() const noexcept;
    [[nodiscard]] bool IsGpuReady() const noexcept;
    [[nodiscard]] std::uint32_t Resolution() const noexcept;
    [[nodiscard]] std::uint32_t MipLevelCount() const noexcept;
    [[nodiscard]] OceanSimulationQuality ActiveQuality() const noexcept;
    [[nodiscard]] std::uint64_t ResourceGeneration() const noexcept;
    [[nodiscard]] std::uint64_t HistoryVersion() const noexcept;
    [[nodiscard]] bool IsFoamHistoryResetPending() const noexcept;
    [[nodiscard]] std::size_t RetiredResourceSetCount() const noexcept;
    [[nodiscard]] static constexpr OceanSpectrumPrecisionPolicy PrecisionPolicy() noexcept
    {
        return {};
    }
    [[nodiscard]] const OceanSpectrumWorkingResource& SpectrumA(
        std::uint32_t pingIndex) const;
    [[nodiscard]] const OceanSpectrumWorkingResource& SpectrumB(
        std::uint32_t pingIndex) const;
    [[nodiscard]] const OceanSpectrumWorkingResource& InitialSpectrum() const;
    [[nodiscard]] const OceanPublishedMapResource& DisplacementMap() const;
    [[nodiscard]] const OceanPublishedMapResource& GradientMap() const;
    [[nodiscard]] const OceanPublishedMapResource& SlopeMomentMap() const;
    [[nodiscard]] const OceanPublishedMapResource& FoamMap() const;
    [[nodiscard]] const OceanPublishedMapResource& FoamHistoryMap(
        std::uint32_t index) const;
    [[nodiscard]] std::uint32_t FoamReadIndex() const noexcept
    {
        return m_foamReadIndex;
    }
    [[nodiscard]] std::uint32_t FoamWriteIndex() const noexcept
    {
        return m_foamWriteIndex;
    }
    [[nodiscard]] const OceanStatistics& GetStatistics() const noexcept;
    void UpdateGpuTimings(float spectrumMilliseconds,
        float horizontalFftMilliseconds,
        float verticalFftMilliseconds,
        float mapMilliseconds,
        float foamMilliseconds,
        float mipMilliseconds,
        bool available) noexcept;
    [[nodiscard]] OceanPublishedMapMetadata PublishedMetadata() const noexcept;
    [[nodiscard]] const OceanSpectrumGenerator& SpectrumGenerator() const noexcept
    {
        return m_spectrumGenerator;
    }

private:
    struct GpuState;
    struct RetiredState
    {
        std::shared_ptr<GpuState> resources;
        std::uint64_t releaseAfterFrame = 0u;
    };

    [[nodiscard]] static std::shared_ptr<GpuState> CreateResourceOnlyState(
        RHI::IGraphicsDevice& device,
        OceanSimulationQuality quality,
        std::uint64_t generation);
    [[nodiscard]] std::shared_ptr<GpuState> CreateGpuState(
        OceanSimulationQuality quality,
        std::uint64_t generation) const;
    void RetireExpiredResourceSets();
    void UpdateGpuConstants(std::uint32_t frameIndex,
        double absoluteTimeSeconds,
        const OceanSettings& settings);
    void InvalidateFoamHistory() noexcept;
    void RefreshResourceStatistics(bool initialSpectrumScheduled) noexcept;

    RHI::IGraphicsDevice* m_device = nullptr;
    Asset::ShaderManager* m_shaderManager = nullptr;
    PipelineCache* m_pipelineCache = nullptr;
    std::filesystem::path m_shaderDirectory;
    RHI::ShaderBinaryFormat m_shaderFormat = RHI::ShaderBinaryFormat::Dxil;
    std::uint32_t m_framesInFlight = 0u;
    std::uint32_t m_activeCascadeMask = AllCascadesMask;
    bool m_asyncComputeEnabled = false;
    std::uint64_t m_frameSerial = 0u;
    std::uint64_t m_resourceGeneration = 0u;
    std::uint64_t m_historyVersion = 0u;
    // Spectrum changes use a three-stage frame commit. Recording H0 does not
    // publish it: EndFrame promotes only the exact version whose complete
    // graph chain was submitted, so rapid edits cannot clear a newer dirty
    // version or expose mixed cascade settings.
    std::uint64_t m_pendingSpectrumVersion = 0u;
    std::uint64_t m_recordedSpectrumVersion = 0u;
    std::uint64_t m_activeSpectrumVersion = 0u;
    std::uint32_t m_foamReadIndex = 0u;
    std::uint32_t m_foamWriteIndex = 1u;
    bool m_foamResetPending = true;
    bool m_hasPreviousFoamTime = false;
    double m_previousFoamTimeSeconds = 0.0;
    float m_foamDeltaSeconds = 0.0f;
    std::shared_ptr<GpuState> m_gpu;
    std::vector<RetiredState> m_retiredStates;
    OceanSpectrumGenerator m_spectrumGenerator;
    OceanStatistics m_statistics{};
};
} // namespace Prism::Renderer
