#pragma once

#include "RHI/GraphicsResources.h"
#include "Renderer/RenderGraph.h"

#include <DirectXMath.h>

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
enum class ShaderBinaryFormat;
class IBuffer;
class ICommandContext;
class IComputePipeline;
class IDescriptorSet;
class IDescriptorSetLayout;
class IGraphicsDevice;
class IGraphicsPipeline;
class ISampler;
class ITexture;
class ITextureView;
}

namespace Prism::Renderer
{
class PipelineCache;

enum class ScreenSpaceFluidShadingMode : std::uint32_t
{
    Realistic = 0,
    Toon = 1
};

enum class ScreenSpaceFluidDisplayMode : std::uint32_t
{
    Composite = 0,
    RawDepth,
    SmoothDepth,
    Thickness,
    Normal,
    Foam,
    Mask
};

inline constexpr std::uint32_t MaxScreenSpaceFluidFilterIterations = 8;

// Per-frame optical controls remain independent from FluidSettings so the
// simulation and renderer can evolve without coupling their resource layouts.
struct ScreenSpaceFluidFrameParameters
{
    DirectX::XMMATRIX view = DirectX::XMMatrixIdentity();
    DirectX::XMMATRIX projection = DirectX::XMMatrixIdentity();
    DirectX::XMFLOAT3 cameraPosition{};
    DirectX::XMFLOAT3 lightDirection{0.3f, -1.0f, 0.2f};

    float particleRadius = 0.045f;
    std::uint32_t bilateralIterations = 3;
    std::uint32_t bilateralRadius = 4;
    float bilateralSpatialSigma = 3.0f;
    float bilateralDepthSigma = 0.12f;
    std::uint32_t normalSmoothingRadius = 4;
    std::uint32_t silhouetteSmoothingRadius = 3;
    float minimumDensityRatio = 0.20f;
    float surfaceCoverageThreshold = 0.0f;
    float foamDensityRatio = 0.50f;

    DirectX::XMFLOAT3 absorption{0.32f, 0.12f, 0.055f};
    DirectX::XMFLOAT3 waterColor{0.035f, 0.32f, 0.42f};
    float scatteringStrength = 0.55f;
    float indexOfRefraction = 1.333f;
    float refractionScale = 0.018f;
    float reflectionStrength = 1.0f;
    float thicknessScale = 1.0f;

    float foamIntensity = 1.0f;
    float foamCurvatureThreshold = 0.16f;
    float foamThicknessThreshold = 0.11f;
    float foamNeighborhoodThreshold = 0.45f;
    float outlineWidth = 1.0f;
    float outlineDepthThreshold = 0.08f;
    float causticsIntensity = 1.0f;
    std::uint32_t toonDiffuseSteps = 4;
    std::uint32_t toonReflectionSteps = 3;

    ScreenSpaceFluidShadingMode shadingMode =
        ScreenSpaceFluidShadingMode::Realistic;
    ScreenSpaceFluidDisplayMode displayMode =
        ScreenSpaceFluidDisplayMode::Composite;
};

struct ScreenSpaceFluidTextures
{
    std::shared_ptr<RHI::ITexture> rawDepth;
    std::shared_ptr<RHI::ITexture> mask;
    std::shared_ptr<RHI::ITexture> depthStencil;
    std::shared_ptr<RHI::ITexture> thickness;
    std::shared_ptr<RHI::ITexture> smoothDepthA;
    std::shared_ptr<RHI::ITexture> smoothDepthB;
    std::shared_ptr<RHI::ITexture> normal;
    std::shared_ptr<RHI::ITexture> foamRaw;
    std::shared_ptr<RHI::ITexture> foam;
    std::shared_ptr<RHI::ITexture> composite;
};

// Input handles are supplied by the PBF and scene pipelines. The remaining
// handles point at persistent textures owned by ScreenSpaceFluidRenderer.
struct ScreenSpaceFluidGraphResources
{
    BufferHandle particlePositions;
    BufferHandle particleDensities;
    TextureHandle sceneColor;
    TextureHandle sceneDepth;
    TextureHandle environment;
    TextureHandle caustics;

    TextureHandle rawDepth;
    TextureHandle mask;
    TextureHandle depthStencil;
    TextureHandle thickness;
    TextureHandle smoothDepthA;
    TextureHandle smoothDepthB;
    TextureHandle normal;
    TextureHandle foamRaw;
    TextureHandle foam;
    TextureHandle composite;
};

struct ScreenSpaceFluidPassCallbacks
{
    RenderGraph::ParameterExecuteCallback particleDepth;
    RenderGraph::ParameterExecuteCallback particleThickness;
    std::array<
        RenderGraph::ParameterExecuteCallback,
        MaxScreenSpaceFluidFilterIterations>
        bilateralHorizontal;
    std::array<
        RenderGraph::ParameterExecuteCallback,
        MaxScreenSpaceFluidFilterIterations>
        bilateralVertical;
    RenderGraph::ParameterExecuteCallback reconstructNormalFoam;
    RenderGraph::ParameterExecuteCallback refineFoam;
    RenderGraph::ParameterExecuteCallback composite;
};

class ScreenSpaceFluidRenderer
{
public:
    static constexpr std::uint32_t ComputeGroupSize = 8;
    static constexpr std::uint32_t MaxBilateralIterations =
        MaxScreenSpaceFluidFilterIterations;
    static constexpr std::uint32_t MaxBilateralRadius = 15;

    void Initialize(
        RHI::IGraphicsDevice& device,
        Asset::ShaderManager& shaderManager,
        PipelineCache& pipelineCache,
        const std::filesystem::path& shaderDirectory,
        RHI::ShaderBinaryFormat shaderFormat,
        std::uint32_t framesInFlight);
    void Resize(std::uint32_t width, std::uint32_t height);
    void SetInputs(
        std::shared_ptr<RHI::IBuffer> particlePositions,
        std::shared_ptr<RHI::IBuffer> particleDensities,
        std::uint32_t particleCount,
        std::shared_ptr<RHI::ITexture> sceneColor,
        std::shared_ptr<RHI::ITexture> sceneDepth,
        std::shared_ptr<RHI::ITexture> environment,
        std::shared_ptr<RHI::ITexture> caustics = {});
    void SetCausticsTexture(
        std::shared_ptr<RHI::ITexture> caustics);
    void Update(
        std::uint32_t frameIndex,
        const ScreenSpaceFluidFrameParameters& parameters);

    [[nodiscard]] ScreenSpaceFluidGraphResources
        RegisterRenderGraph(
            RenderGraph& graph,
            BufferHandle particlePositions,
            BufferHandle particleDensities,
            TextureHandle sceneColor,
            TextureHandle sceneDepth,
            TextureHandle environment,
            TextureHandle caustics = {});
    [[nodiscard]] ScreenSpaceFluidPassCallbacks
        CreatePassCallbacks(std::uint32_t frameIndex) const;

    static void AddSurfacePasses(
        RenderGraph& graph,
        ScreenSpaceFluidGraphResources& resources,
        const ScreenSpaceFluidPassCallbacks& callbacks);
    // Minimal particle-preview path used by the PBF diagnostics demo. It
    // renders only the depth/mask impostors and composites the mask, without
    // touching thickness, filtering, normals, foam, or caustics.
    static void AddParticlePreviewPasses(
        RenderGraph& graph,
        ScreenSpaceFluidGraphResources& resources,
        const ScreenSpaceFluidPassCallbacks& callbacks);
    static void AddFilterPasses(
        RenderGraph& graph,
        ScreenSpaceFluidGraphResources& resources,
        std::uint32_t bilateralIterations,
        bool generateFoam,
        const ScreenSpaceFluidPassCallbacks& callbacks);
    static void AddCompositePass(
        RenderGraph& graph,
        ScreenSpaceFluidGraphResources& resources,
        const ScreenSpaceFluidPassCallbacks& callbacks);

    void ExecuteParticleDepth(
        RHI::ICommandContext& commandContext,
        std::uint32_t frameIndex) const;
    void ExecuteParticleThickness(
        RHI::ICommandContext& commandContext,
        std::uint32_t frameIndex) const;
    void ExecuteBilateralHorizontal(
        RHI::ICommandContext& commandContext,
        std::uint32_t frameIndex,
        std::uint32_t iteration) const;
    void ExecuteBilateralVertical(
        RHI::ICommandContext& commandContext,
        std::uint32_t frameIndex,
        std::uint32_t iteration) const;
    void ExecuteNormalFoam(
        RHI::ICommandContext& commandContext,
        std::uint32_t frameIndex) const;
    void ExecuteFoamRefine(
        RHI::ICommandContext& commandContext,
        std::uint32_t frameIndex) const;
    void ExecuteComposite(
        RHI::ICommandContext& commandContext,
        std::uint32_t frameIndex) const;
    void EndFrame(
        bool surfaceExecuted,
        bool filterExecuted,
        bool foamExecuted,
        bool compositeExecuted);
    void EndParticlePreviewFrame(bool executed);

    [[nodiscard]] const ScreenSpaceFluidTextures&
        GetTextures() const;
    [[nodiscard]] std::shared_ptr<RHI::ITexture>
        GetCompositeTexture() const;
    [[nodiscard]] std::shared_ptr<RHI::ITextureView>
        GetCompositeSampledView() const;
    [[nodiscard]] std::shared_ptr<RHI::ITextureView>
        GetRawDepthSampledView() const;
    [[nodiscard]] std::shared_ptr<RHI::ITextureView>
        GetNormalSampledView() const;
    [[nodiscard]] RHI::ResourceState
        GetCompositeInitialState() const;
    [[nodiscard]] std::uint32_t
        GetBilateralIterations() const;
    [[nodiscard]] bool IsReady() const;

private:
    struct alignas(16) Constants
    {
        DirectX::XMFLOAT4X4 view{};
        DirectX::XMFLOAT4X4 projection{};
        DirectX::XMFLOAT4X4 inverseProjection{};
        DirectX::XMFLOAT4X4 inverseView{};
        DirectX::XMFLOAT4 resolutionInverseResolution{};
        DirectX::XMFLOAT4 cameraPositionParticleRadius{};
        DirectX::XMFLOAT4 filterParameters{};
        DirectX::XMFLOAT4 densityParameters{};
        DirectX::XMFLOAT4 absorptionScattering{};
        DirectX::XMFLOAT4 waterColorIor{};
        DirectX::XMFLOAT4 opticalParameters{};
        DirectX::XMFLOAT4 foamParameters{};
        DirectX::XMFLOAT4 toonParameters{};
        DirectX::XMFLOAT4 lightDirectionOutline{};
        DirectX::XMUINT4 countsAndModes{};
    };
    static_assert(
        sizeof(Constants) == 432,
        "Screen-space fluid constants must match the HLSL cbuffer layout.");

    struct FrameResources
    {
        std::shared_ptr<RHI::IBuffer> constants;
        std::array<
            std::shared_ptr<RHI::IBuffer>,
            MaxBilateralIterations> filterConstants;
        std::shared_ptr<RHI::IDescriptorSet> surfaceSet;
        std::array<
            std::shared_ptr<RHI::IDescriptorSet>,
            MaxBilateralIterations> bilateralHorizontalSets;
        std::array<
            std::shared_ptr<RHI::IDescriptorSet>,
            MaxBilateralIterations> bilateralVerticalSets;
        std::shared_ptr<RHI::IDescriptorSet> normalFoamSet;
        std::shared_ptr<RHI::IDescriptorSet> foamRefineSet;
        std::shared_ptr<RHI::IDescriptorSet> compositeSet;
    };

    struct TextureViews
    {
        std::shared_ptr<RHI::ITextureView> rawDepthTarget;
        std::shared_ptr<RHI::ITextureView> rawDepthSampled;
        std::shared_ptr<RHI::ITextureView> maskTarget;
        std::shared_ptr<RHI::ITextureView> depthStencil;
        std::shared_ptr<RHI::ITextureView> thicknessTarget;
        std::shared_ptr<RHI::ITextureView> smoothDepthAStorage;
        std::shared_ptr<RHI::ITextureView> smoothDepthBStorage;
        std::shared_ptr<RHI::ITextureView> normalStorage;
        std::shared_ptr<RHI::ITextureView> normalSampled;
        std::shared_ptr<RHI::ITextureView> foamRawStorage;
        std::shared_ptr<RHI::ITextureView> foamStorage;
        std::shared_ptr<RHI::ITextureView> compositeStorage;
        std::shared_ptr<RHI::ITextureView> compositeSampled;
    };

    struct ResourceStates
    {
        RHI::ResourceState rawDepth = RHI::ResourceState::Undefined;
        RHI::ResourceState mask = RHI::ResourceState::Undefined;
        RHI::ResourceState depthStencil = RHI::ResourceState::Undefined;
        RHI::ResourceState thickness = RHI::ResourceState::Undefined;
        RHI::ResourceState smoothDepthA = RHI::ResourceState::Undefined;
        RHI::ResourceState smoothDepthB = RHI::ResourceState::Undefined;
        RHI::ResourceState normal = RHI::ResourceState::Undefined;
        RHI::ResourceState foamRaw = RHI::ResourceState::Undefined;
        RHI::ResourceState foam = RHI::ResourceState::Undefined;
        RHI::ResourceState composite = RHI::ResourceState::Undefined;
    };

    void RebuildDescriptorSets();
    void RebuildDescriptorSetsIfReady();
    void DispatchFullscreen(
        RHI::ICommandContext& commandContext,
        const RHI::IComputePipeline& pipeline,
        const RHI::IDescriptorSet& descriptorSet) const;

    RHI::IGraphicsDevice* m_device = nullptr;
    std::shared_ptr<RHI::IDescriptorSetLayout> m_surfaceLayout;
    std::shared_ptr<RHI::IDescriptorSetLayout> m_bilateralLayout;
    std::shared_ptr<RHI::IDescriptorSetLayout> m_normalFoamLayout;
    std::shared_ptr<RHI::IDescriptorSetLayout> m_foamRefineLayout;
    std::shared_ptr<RHI::IDescriptorSetLayout> m_compositeLayout;
    std::shared_ptr<RHI::IGraphicsPipeline> m_particleDepthPipeline;
    std::shared_ptr<RHI::IGraphicsPipeline> m_particleThicknessPipeline;
    std::shared_ptr<RHI::IComputePipeline> m_bilateralHorizontalPipeline;
    std::shared_ptr<RHI::IComputePipeline> m_bilateralVerticalPipeline;
    std::shared_ptr<RHI::IComputePipeline> m_normalFoamPipeline;
    std::shared_ptr<RHI::IComputePipeline> m_foamRefinePipeline;
    std::shared_ptr<RHI::IComputePipeline> m_compositePipeline;
    std::shared_ptr<RHI::ISampler> m_linearClampSampler;
    std::vector<FrameResources> m_frames;

    std::shared_ptr<RHI::IBuffer> m_particlePositions;
    std::shared_ptr<RHI::IBuffer> m_particleDensities;
    std::shared_ptr<RHI::ITexture> m_sceneColor;
    std::shared_ptr<RHI::ITexture> m_sceneDepth;
    std::shared_ptr<RHI::ITexture> m_environment;
    std::shared_ptr<RHI::ITexture> m_caustics;
    ScreenSpaceFluidTextures m_textures;
    TextureViews m_views;
    ResourceStates m_states;
    ScreenSpaceFluidFrameParameters m_parameters{};
    std::uint32_t m_particleCount = 0;
    std::uint32_t m_width = 0;
    std::uint32_t m_height = 0;
};
} // namespace Prism::Renderer
