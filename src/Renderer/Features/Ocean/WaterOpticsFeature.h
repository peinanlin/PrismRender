#pragma once

#include "Renderer/Features/Ocean/WaterOpticsHistory.h"
#include "Renderer/Features/Ocean/WaterMediumState.h"
#include "Renderer/Features/Ocean/WaterOpticsSettings.h"
#include "Renderer/Features/Ocean/WaterVolumetricHistory.h"
#include "Renderer/Features/Ocean/WaterCoverageReadback.h"
#include "Renderer/Features/Ocean/WaterOpticsGraph.h"
#include "RHI/GraphicsResources.h"
#include "RHI/ShaderTypes.h"
#include "RHI/TransientResources.h"

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
class ICommandContext;
class IGraphicsDevice;
class IGraphicsPipeline;
class IComputePipeline;
class ISampler;
}

namespace Prism::Renderer
{
class PipelineCache;

struct WaterOpticsPassCallbacks
{
    RenderGraph::ParameterExecuteCallback depthCopy;
    RenderGraph::ParameterExecuteCallback visibility;
    RenderGraph::ParameterExecuteCallback refraction;
    RenderGraph::ParameterExecuteCallback caustics;
    RenderGraph::ParameterExecuteCallback volumetricAccumulate;
    RenderGraph::ParameterExecuteCallback volumetricTemporal;
    RenderGraph::ParameterExecuteCallback volumetricReconstruct;
    RenderGraph::ParameterExecuteCallback composite;
    RenderGraph::ParameterExecuteCallback publish;
};

struct WaterOpticsPassOptions
{
    bool refractionEnabled = true;
    bool rayMarchEnabled = false;
    bool causticsEnabled = false;
    bool volumetricsEnabled = false;
    bool compositeEnabled = false;
    std::uint32_t effectiveRayMarchSamples = 0u;
    std::uint32_t volumetricReadIndex = 0u;
};

// Static/shared inputs used only by the dedicated composite descriptor sets.
// RenderGraph dependencies are declared separately through
// WaterOpticsGraphInputs so this structure never owns graph scheduling.
struct WaterOpticsCompositeBindings
{
    const std::vector<std::shared_ptr<RHI::IBuffer>>* frameConstants =
        nullptr;
    std::shared_ptr<RHI::ITexture> environment;
    std::shared_ptr<RHI::ITextureView> shadowMap;
    std::shared_ptr<RHI::ITextureView> shadowMoments;
    std::shared_ptr<RHI::ITextureView> atmosphereSkyView;
    std::shared_ptr<RHI::ITextureView> slopeMoments;
    std::shared_ptr<RHI::ITextureView> spectralGradient;
    std::shared_ptr<RHI::ITextureView> localGradient;
    std::shared_ptr<RHI::ITexture> sceneMotion;
    DirectX::XMFLOAT2 localDomainCenter{};
    float localDomainSizeMeters = 1.0f;
    bool localWavesEnabled = false;
    std::uint32_t frameIndex = 0u;
    std::uint64_t surfaceHistoryVersion = 0u;
    std::uint64_t cameraCutVersion = 0u;
};

enum class WaterOpticsCaptureStage
{
    Depth,
    Mask,
    GBuffer0,
    GBuffer1,
    GBuffer2,
    Refraction,
    CausticsNear,
    CausticsMiddle,
    VolumetricCurrent,
    VolumetricHistory,
    VolumetricReconstruction,
    Composite
};

struct WaterOpticsFeatureStatistics
{
    std::uint64_t resourceGeneration = 0u;
    std::uint64_t historyVersion = 0u;
    std::uint32_t retiredResourceSets = 0u;
    float allocatedMegabytes = 0.0f;
    float waterPixelCoverage = 0.0f;
    bool waterCoverageAvailable = false;
    float refractionMilliseconds = 0.0f;
    float compositeMilliseconds = 0.0f;
    float causticsMilliseconds = 0.0f;
    float volumetricAccumulateMilliseconds = 0.0f;
    float volumetricTemporalMilliseconds = 0.0f;
    float volumetricReconstructMilliseconds = 0.0f;
    std::uint32_t refractionDispatchCount = 0u;
    std::uint32_t compositeDispatchCount = 0u;
    std::uint32_t causticsDispatchCount = 0u;
    std::uint32_t volumetricDispatchCount = 0u;
    std::uint32_t effectiveRayMarchSamples = 0u;
    std::uint32_t effectiveRefractionWidth = 0u;
    std::uint32_t effectiveRefractionHeight = 0u;
    std::uint32_t effectiveVolumetricWidth = 0u;
    std::uint32_t effectiveVolumetricHeight = 0u;
    float effectiveRefractionResolutionScale = 1.0f;
    WaterOpticsQuality effectiveQuality = WaterOpticsQuality::High;
    bool initialized = false;
    bool resourcesReady = false;
    bool historyValid = false;
    bool refractionEnabled = false;
    bool rayMarchEnabled = false;
    bool compositeEnabled = false;
    bool causticsEnabled = false;
    bool volumetricsEnabled = false;
    bool volumetricHistoryValid = false;
    bool cameraUnderwater = false;
    bool mediumUsesMeanSeaLevelFallback = true;
    std::uint64_t mediumVersion = 0u;
    bool gpuTimingAvailable = false;
};

// One 256-byte CBV shared by the ordinary indexed and adaptive tessellated
// water visibility variants. Field order mirrors WaterOpticsConstants in
// OceanSurface.hlsl.
struct alignas(256) WaterOpticsGpuConstants
{
    DirectX::XMFLOAT4 absorptionRoughness{};
    DirectX::XMFLOAT4 scatteringIor{};
    DirectX::XMFLOAT4 phaseThinBacklit{};
    DirectX::XMFLOAT4 distanceRanges{};
    DirectX::XMFLOAT4 refractionParameters0{};
    DirectX::XMFLOAT4 refractionParameters1{};
    DirectX::XMFLOAT4 causticsParameters{};
    DirectX::XMFLOAT4 volumetricParameters0{};
    DirectX::XMFLOAT4 volumetricParameters1{};
    std::array<std::uint32_t, 4> modes0{};
    std::array<std::uint32_t, 4> modes1{};
    std::array<std::uint32_t, 4> modes2{};
    DirectX::XMFLOAT4X4 inverseViewProjection{};
};

static_assert(sizeof(WaterOpticsGpuConstants) == 256u);

class WaterOpticsFeature
{
public:
    void Initialize(
        RHI::IGraphicsDevice& device,
        Asset::ShaderManager& shaderManager,
        PipelineCache& pipelineCache,
        const std::filesystem::path& shaderDirectory,
        RHI::ShaderBinaryFormat shaderFormat,
        std::uint32_t framesInFlight,
        std::shared_ptr<RHI::IDescriptorSetLayout> oceanLayout,
        bool indexedObjectDrawingSupported);
    void Shutdown();

    // Applies view-local enable/configuration events before graph building.
    // Resource replacement uses the frame-ring retirement path.
    void PrepareFrame(
        const WaterOpticsSettings& settings,
        std::uint64_t frameSerial,
        bool enabled);
    void EnsureResources(
        RHI::IGraphicsDevice& device,
        std::uint32_t width,
        std::uint32_t height,
        const std::shared_ptr<RHI::ITexture>& opaqueDepth,
        const std::shared_ptr<RHI::ITextureView>& opaqueDepthView,
        const std::shared_ptr<RHI::ITexture>& sceneColor,
        const std::shared_ptr<RHI::ITextureView>& sceneColorView,
        const WaterOpticsSettings& settings,
        std::uint64_t frameSerial,
        const WaterOpticsCompositeBindings* compositeBindings = nullptr);
    void ReleaseSizeDependentResources(
        std::uint64_t frameSerial,
        bool immediate = false);
    void EndFrame(std::uint64_t frameSerial, bool active);
    void ResetHistory();
    void NotifySceneChanged();
    void UpdateGpuTiming(float milliseconds, bool available) noexcept;
    void UpdateCompositeGpuTiming(
        float milliseconds, bool available) noexcept;
    void UpdateCausticsGpuTiming(
        float milliseconds, bool available) noexcept;
    void UpdateVolumetricGpuTiming(
        float accumulateMilliseconds,
        float temporalMilliseconds,
        float reconstructMilliseconds,
        bool available) noexcept;
    [[nodiscard]] WaterMediumResult UpdateCameraMedium(
        const WaterMediumSample& sample) noexcept;
    void SetInverseViewProjection(
        std::uint32_t frameIndex,
        const DirectX::XMFLOAT4X4& inverseViewProjection);

    void UpdateSurfaceDescriptorSet(
        RHI::IDescriptorSet& descriptorSet,
        std::uint32_t frameIndex) const;
    [[nodiscard]] WaterOpticsGraphContribution
        CreateRenderGraphContribution(
            WaterOpticsPassCallbacks passCallbacks) const;
    [[nodiscard]] static WaterOpticsGraphResult AddPasses(
        RenderGraph& graph,
        const WaterOpticsGraphInputs& inputs,
        TextureHandle refraction,
        TextureHandle composite,
        std::array<TextureHandle, 2> caustics,
        std::array<TextureHandle, 4> volumetrics,
        const WaterOpticsPassCallbacks& callbacks,
        const WaterOpticsPassOptions& options);
    void ExecuteDepthCopy(
        RHI::ICommandContext& commandContext,
        std::uint32_t width,
        std::uint32_t height) const;
    void ExecuteRefraction(
        RHI::ICommandContext& commandContext,
        std::uint32_t width,
        std::uint32_t height,
        std::uint32_t frameIndex) const;
    void ExecuteCaustics(
        RHI::ICommandContext& commandContext,
        std::uint32_t frameIndex) const;
    void ExecuteVolumetricAccumulate(
        RHI::ICommandContext& commandContext,
        std::uint32_t frameIndex) const;
    void ExecuteVolumetricTemporal(
        RHI::ICommandContext& commandContext,
        std::uint32_t frameIndex) const;
    void ExecuteVolumetricReconstruct(
        RHI::ICommandContext& commandContext,
        std::uint32_t frameIndex) const;
    void ExecuteComposite(
        RHI::ICommandContext& commandContext,
        std::uint32_t width,
        std::uint32_t height,
        std::uint32_t frameIndex) const;
    void ExecutePublish(
        RHI::ICommandContext& commandContext,
        std::uint32_t width,
        std::uint32_t height) const;

    [[nodiscard]] const RHI::IGraphicsPipeline& VisibilityPipeline() const;
    [[nodiscard]] const RHI::IGraphicsPipeline&
        VisibilityTessellationPipeline() const;
    [[nodiscard]] bool HasVisibilityTessellationPipeline() const noexcept;
    [[nodiscard]] const RHI::ITextureView& CompositeDepthView() const;
    [[nodiscard]] const std::array<
        std::shared_ptr<RHI::ITextureView>, 3>&
        GBufferRenderTargetViews() const noexcept;
    [[nodiscard]] const RHI::ITextureView&
        MotionRenderTargetView() const;
    [[nodiscard]] const std::shared_ptr<RHI::ITextureView>&
        GetCaptureView(WaterOpticsCaptureStage stage) const;
    [[nodiscard]] bool IsInitialized() const noexcept;
    [[nodiscard]] bool HasResources() const noexcept;
    [[nodiscard]] const WaterOpticsFeatureStatistics&
        GetStatistics() const noexcept;
    [[nodiscard]] const WaterOpticsHistoryState& GetHistoryState() const noexcept { return m_history; }
    [[nodiscard]] const WaterVolumetricHistoryState& GetVolumetricHistoryState() const noexcept { return m_volumetricHistory; }

private:
    struct RetiredResources
    {
        std::uint64_t retireAfterFrame = 0u;
        std::shared_ptr<RHI::ITransientTexturePool> pool;
        std::array<std::shared_ptr<RHI::ITexture>, 3> gbuffer;
        std::array<std::shared_ptr<RHI::ITextureView>, 3> gbufferViews;
        std::array<std::shared_ptr<RHI::ITextureView>, 3>
            gbufferSampledViews;
        std::shared_ptr<RHI::ITexture> motion;
        std::shared_ptr<RHI::ITextureView> motionView;
        std::shared_ptr<RHI::ITextureView> motionSampledView;
        std::shared_ptr<RHI::ITexture> compositeDepth;
        std::shared_ptr<RHI::ITextureView> compositeDepthView;
        std::shared_ptr<RHI::ITextureView> compositeDepthSampledView;
        std::shared_ptr<RHI::IDescriptorSet> depthCopySet;
        std::shared_ptr<RHI::ITexture> refraction;
        std::shared_ptr<RHI::ITextureView> refractionSampledView;
        std::shared_ptr<RHI::ITextureView> refractionStorageView;
        std::vector<std::shared_ptr<RHI::IDescriptorSet>> refractionSets;
        std::array<std::shared_ptr<RHI::ITexture>, 2> caustics;
        std::array<std::shared_ptr<RHI::ITextureView>, 2>
            causticSampledViews;
        std::array<std::shared_ptr<RHI::ITextureView>, 2>
            causticStorageViews;
        std::vector<std::shared_ptr<RHI::IDescriptorSet>> causticSets;
        std::array<std::shared_ptr<RHI::ITexture>, 4> volumetrics;
        std::array<std::shared_ptr<RHI::ITextureView>, 4>
            volumetricSampledViews;
        std::array<std::shared_ptr<RHI::ITextureView>, 4>
            volumetricStorageViews;
        std::vector<std::shared_ptr<RHI::IDescriptorSet>>
            volumetricAccumulateSets;
        std::array<std::vector<std::shared_ptr<RHI::IDescriptorSet>>, 2>
            volumetricTemporalSets;
        std::array<std::vector<std::shared_ptr<RHI::IDescriptorSet>>, 2>
            volumetricReconstructSets;
        std::shared_ptr<RHI::ITexture> composite;
        std::shared_ptr<RHI::ITextureView> compositeSampledView;
        std::shared_ptr<RHI::ITextureView> compositeStorageView;
        std::vector<std::shared_ptr<RHI::IDescriptorSet>> compositeSets;
        std::shared_ptr<RHI::IDescriptorSet> publishSet;
        std::shared_ptr<RHI::ITextureView> publishHdrStorageView;
        std::shared_ptr<RHI::ITextureView> publishMotionStorageView;
    };

    void RetireCurrentResources(std::uint64_t frameSerial);
    void CollectRetiredResources(std::uint64_t frameSerial);
    void UpdateConstants(
        const WaterOpticsSettings& settings,
        std::uint64_t frameSerial,
        const WaterOpticsCompositeBindings* compositeBindings);

    std::uint32_t m_framesInFlight = 0u;
    std::uint32_t m_width = 0u;
    std::uint32_t m_height = 0u;
    std::uint32_t m_refractionWidth = 0u;
    std::uint32_t m_refractionHeight = 0u;
    std::uint32_t m_volumetricWidth = 0u;
    std::uint32_t m_volumetricHeight = 0u;
    WaterOpticsQuality m_activeQuality = WaterOpticsQuality::High;
    float m_activeVolumetricResolutionScale = 0.0f;
    std::uint32_t m_activeCausticResolution = 0u;
    std::uint64_t m_sceneVersion = 1u;
    const RHI::ITexture* m_opaqueDepthSource = nullptr;
    const RHI::ITexture* m_sceneColorSource = nullptr;
    std::shared_ptr<RHI::IGraphicsPipeline> m_visibilityPipeline;
    std::shared_ptr<RHI::IGraphicsPipeline>
        m_visibilityTessellationPipeline;
    std::shared_ptr<RHI::IGraphicsPipeline> m_depthCopyPipeline;
    std::shared_ptr<RHI::IDescriptorSetLayout> m_depthCopyLayout;
    std::shared_ptr<RHI::IComputePipeline> m_refractionPipeline;
    std::shared_ptr<RHI::IDescriptorSetLayout> m_refractionLayout;
    std::shared_ptr<RHI::ISampler> m_refractionSampler;
    std::shared_ptr<RHI::IComputePipeline> m_compositePipeline;
    std::shared_ptr<RHI::IDescriptorSetLayout> m_compositeLayout;
    std::shared_ptr<RHI::ISampler> m_compositeShadowSampler;
    std::shared_ptr<RHI::IComputePipeline> m_causticPipeline;
    std::shared_ptr<RHI::IDescriptorSetLayout> m_causticLayout;
    std::shared_ptr<RHI::ISampler> m_causticWrapSampler;
    std::shared_ptr<RHI::IComputePipeline> m_volumetricAccumulatePipeline;
    std::shared_ptr<RHI::IComputePipeline> m_volumetricTemporalPipeline;
    std::shared_ptr<RHI::IComputePipeline> m_volumetricReconstructPipeline;
    std::shared_ptr<RHI::IDescriptorSetLayout> m_volumetricAccumulateLayout;
    std::shared_ptr<RHI::IDescriptorSetLayout> m_volumetricTemporalLayout;
    std::shared_ptr<RHI::IDescriptorSetLayout> m_volumetricReconstructLayout;
    std::shared_ptr<RHI::IComputePipeline> m_publishPipeline;
    std::shared_ptr<RHI::IDescriptorSetLayout> m_publishLayout;
    std::vector<std::shared_ptr<RHI::IBuffer>> m_constantBuffers;
    std::vector<DirectX::XMFLOAT4X4> m_inverseViewProjections;
    std::shared_ptr<RHI::ITransientTexturePool> m_pool;
    std::array<std::shared_ptr<RHI::ITexture>, 3> m_gbuffer;
    std::array<std::shared_ptr<RHI::ITextureView>, 3> m_gbufferViews;
    std::array<std::shared_ptr<RHI::ITextureView>, 3>
        m_gbufferSampledViews;
    std::shared_ptr<RHI::ITexture> m_motion;
    std::shared_ptr<RHI::ITextureView> m_motionView;
    std::shared_ptr<RHI::ITextureView> m_motionSampledView;
    std::shared_ptr<RHI::ITexture> m_compositeDepth;
    std::shared_ptr<RHI::ITextureView> m_compositeDepthView;
    std::shared_ptr<RHI::ITextureView> m_compositeDepthSampledView;
    std::shared_ptr<RHI::IDescriptorSet> m_depthCopySet;
    std::shared_ptr<RHI::ITexture> m_refraction;
    std::shared_ptr<RHI::ITextureView> m_refractionSampledView;
    std::shared_ptr<RHI::ITextureView> m_refractionStorageView;
    std::vector<std::shared_ptr<RHI::IDescriptorSet>> m_refractionSets;
    std::array<std::shared_ptr<RHI::ITexture>, 2> m_caustics;
    std::array<std::shared_ptr<RHI::ITextureView>, 2>
        m_causticSampledViews;
    std::array<std::shared_ptr<RHI::ITextureView>, 2>
        m_causticStorageViews;
    std::vector<std::shared_ptr<RHI::IDescriptorSet>> m_causticSets;
    std::array<std::shared_ptr<RHI::ITexture>, 4> m_volumetrics;
    std::array<std::shared_ptr<RHI::ITextureView>, 4>
        m_volumetricSampledViews;
    std::array<std::shared_ptr<RHI::ITextureView>, 4>
        m_volumetricStorageViews;
    std::vector<std::shared_ptr<RHI::IDescriptorSet>>
        m_volumetricAccumulateSets;
    std::array<std::vector<std::shared_ptr<RHI::IDescriptorSet>>, 2>
        m_volumetricTemporalSets;
    std::array<std::vector<std::shared_ptr<RHI::IDescriptorSet>>, 2>
        m_volumetricReconstructSets;
    std::uint32_t m_volumetricReadIndex = 0u;
    std::shared_ptr<RHI::ITexture> m_composite;
    std::shared_ptr<RHI::ITextureView> m_compositeSampledView;
    std::shared_ptr<RHI::ITextureView> m_compositeStorageView;
    std::vector<std::shared_ptr<RHI::IDescriptorSet>> m_compositeSets;
    std::shared_ptr<RHI::IDescriptorSet> m_publishSet;
    std::shared_ptr<RHI::ITextureView> m_publishHdrStorageView;
    std::shared_ptr<RHI::ITextureView> m_publishMotionStorageView;
    const RHI::ITexture* m_sceneMotionSource = nullptr;
    std::vector<RetiredResources> m_retired;
    std::array<RHI::ResourceState, 3> m_gbufferStates{};
    RHI::ResourceState m_motionState = RHI::ResourceState::Undefined;
    RHI::ResourceState m_compositeDepthState = RHI::ResourceState::Undefined;
    RHI::ResourceState m_refractionState = RHI::ResourceState::Undefined;
    std::array<RHI::ResourceState, 2> m_causticStates{};
    std::array<RHI::ResourceState, 4> m_volumetricStates{};
    RHI::ResourceState m_compositeState = RHI::ResourceState::Undefined;
    WaterOpticsPassOptions m_passOptions{};
    WaterOpticsHistoryState m_history;
    WaterVolumetricHistoryState m_volumetricHistory;
    WaterMediumState m_mediumState;
    WaterMediumResult m_mediumResult{};
    WaterOpticsSettings m_previousSettings{};
    bool m_hasPreviousSettings = false;
    WaterOpticsFeatureStatistics m_statistics{};
    WaterCoverageReadback m_coverage;
};
} // namespace Prism::Renderer
