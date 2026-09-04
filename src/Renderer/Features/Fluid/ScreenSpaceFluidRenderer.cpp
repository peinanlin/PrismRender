#include "Renderer/Features/Fluid/ScreenSpaceFluidRenderer.h"

#include "Asset/ShaderManager.h"
#include "Core/Assert.h"
#include "Renderer/Pipeline/PipelineCache.h"
#include "RHI/ICommandContext.h"
#include "RHI/IGraphicsDevice.h"
#include "RHI/ShaderLayoutBuilder.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <string>
#include <utility>

namespace Prism::Renderer
{
namespace
{
std::shared_ptr<RHI::IBuffer> CreateConstantsBuffer(
    RHI::IGraphicsDevice& device,
    const std::size_t size)
{
    RHI::BufferDescription description{};
    description.size = size;
    description.stride = static_cast<std::uint32_t>(size);
    description.usage = RHI::BufferUsage::Constant;
    description.memoryAccess = RHI::MemoryAccess::CpuToGpu;
    return device.CreateBuffer(description);
}

std::shared_ptr<RHI::ITexture> CreateFluidTexture(
    RHI::IGraphicsDevice& device,
    const std::uint32_t width,
    const std::uint32_t height,
    const RHI::Format format,
    const RHI::TextureUsage usage,
    const std::string& debugName)
{
    RHI::TextureDescription description{};
    description.width = width;
    description.height = height;
    description.format = format;
    description.usage = usage;
    std::shared_ptr<RHI::ITexture> texture =
        device.CreateTexture(description);
    texture->SetDebugName(debugName);
    return texture;
}

std::shared_ptr<RHI::ITextureView> CreateView(
    RHI::IGraphicsDevice& device,
    const std::shared_ptr<RHI::ITexture>& texture,
    const RHI::TextureViewType type)
{
    RHI::TextureViewDescription description{};
    description.type = type;
    return device.CreateTextureView(texture, description);
}

void AddParticleDepthPass(
    RenderGraph& graph,
    ScreenSpaceFluidGraphResources& resources,
    const RenderGraph::ParameterExecuteCallback& execute)
{
    auto parameters = graph.CreatePassParameters();
    parameters.ReadBuffer(
        resources.particlePositions,
        RHI::ResourceState::ShaderResource);
    if (resources.particleDensities.IsValid())
    {
        parameters.ReadBuffer(
            resources.particleDensities,
            RHI::ResourceState::ShaderResource);
    }
    parameters.ReadTexture(
        resources.sceneDepth,
        RHI::ResourceState::ShaderResource);
    resources.rawDepth = parameters.WriteTexture(
        resources.rawDepth,
        RHI::ResourceState::RenderTarget);
    resources.mask = parameters.WriteTexture(
        resources.mask,
        RHI::ResourceState::RenderTarget);
    resources.depthStencil = parameters.WriteTexture(
        resources.depthStencil,
        RHI::ResourceState::DepthWrite);
    graph.AddParameterPass(
        "Fluid.SurfaceDepth",
        std::move(parameters),
        execute,
        RenderGraph::PassOptions{
            RenderGraph::QueueClass::Graphics,
            true,
            false,
            true});
}
} // namespace

void ScreenSpaceFluidRenderer::Initialize(
    RHI::IGraphicsDevice& device,
    Asset::ShaderManager& shaderManager,
    PipelineCache& pipelineCache,
    const std::filesystem::path& shaderDirectory,
    const RHI::ShaderBinaryFormat shaderFormat,
    const std::uint32_t framesInFlight)
{
    Core::Check(
        framesInFlight > 0,
        "Screen-space fluid rendering requires at least one frame in flight.");
    m_device = &device;

    const std::filesystem::path surfacePath =
        shaderDirectory / "FluidSurface.hlsl";
    const std::filesystem::path filterPath =
        shaderDirectory / "FluidFilter.hlsl";
    const std::filesystem::path toonFoamPath =
        shaderDirectory / "FluidToonFoam.hlsl";
    const std::filesystem::path compositePath =
        shaderDirectory / "FluidComposite.hlsl";

    const RHI::ShaderBinary& surfaceVertex =
        shaderManager.LoadShader(
            surfacePath,
            "ParticleBillboardVS",
            RHI::ShaderStage::Vertex,
            shaderFormat);
    const RHI::ShaderBinary& depthPixel =
        shaderManager.LoadShader(
            surfacePath,
            "ParticleDepthPS",
            RHI::ShaderStage::Pixel,
            shaderFormat);
    const RHI::ShaderBinary& thicknessPixel =
        shaderManager.LoadShader(
            surfacePath,
            "ParticleThicknessPS",
            RHI::ShaderStage::Pixel,
            shaderFormat);
    const RHI::ShaderBinary& bilateralHorizontal =
        shaderManager.LoadShader(
            filterPath,
            "BilateralHorizontalCS",
            RHI::ShaderStage::Compute,
            shaderFormat);
    const RHI::ShaderBinary& bilateralVertical =
        shaderManager.LoadShader(
            filterPath,
            "BilateralVerticalCS",
            RHI::ShaderStage::Compute,
            shaderFormat);
    const RHI::ShaderBinary& normalFoam =
        shaderManager.LoadShader(
            toonFoamPath,
            "ReconstructNormalFoamCS",
            RHI::ShaderStage::Compute,
            shaderFormat);
    const RHI::ShaderBinary& foamRefine =
        shaderManager.LoadShader(
            toonFoamPath,
            "RefineFoamCS",
            RHI::ShaderStage::Compute,
            shaderFormat);
    const RHI::ShaderBinary& composite =
        shaderManager.LoadShader(
            compositePath,
            "FluidCompositeCS",
            RHI::ShaderStage::Compute,
            shaderFormat);

    const std::array surfaceStages{
        RHI::ShaderLayoutStage{
            &surfaceVertex.reflection,
            RHI::ShaderStage::Vertex},
        RHI::ShaderLayoutStage{
            &depthPixel.reflection,
            RHI::ShaderStage::Pixel},
        RHI::ShaderLayoutStage{
            &thicknessPixel.reflection,
            RHI::ShaderStage::Pixel}};
    const std::array bilateralStages{
        RHI::ShaderLayoutStage{
            &bilateralHorizontal.reflection,
            RHI::ShaderStage::Compute},
        RHI::ShaderLayoutStage{
            &bilateralVertical.reflection,
            RHI::ShaderStage::Compute}};
    const std::array normalStages{
        RHI::ShaderLayoutStage{
            &normalFoam.reflection,
            RHI::ShaderStage::Compute}};
    const std::array foamStages{
        RHI::ShaderLayoutStage{
            &foamRefine.reflection,
            RHI::ShaderStage::Compute}};
    const std::array compositeStages{
        RHI::ShaderLayoutStage{
            &composite.reflection,
            RHI::ShaderStage::Compute}};
    m_surfaceLayout = device.CreateDescriptorSetLayout(
        RHI::BuildDescriptorSetLayout(surfaceStages, {}));
    m_bilateralLayout = device.CreateDescriptorSetLayout(
        RHI::BuildDescriptorSetLayout(bilateralStages, {}));
    m_normalFoamLayout = device.CreateDescriptorSetLayout(
        RHI::BuildDescriptorSetLayout(normalStages, {}));
    m_foamRefineLayout = device.CreateDescriptorSetLayout(
        RHI::BuildDescriptorSetLayout(foamStages, {}));
    m_compositeLayout = device.CreateDescriptorSetLayout(
        RHI::BuildDescriptorSetLayout(compositeStages, {}));

    RHI::GraphicsPipelineDescription depthPipeline{};
    depthPipeline.vertexShader = surfaceVertex;
    depthPipeline.pixelShader = depthPixel;
    depthPipeline.descriptorSetLayout = m_surfaceLayout;
    depthPipeline.rasterizer.cullMode = RHI::CullMode::None;
    depthPipeline.rasterizer.frontFace =
        RHI::FrontFace::CounterClockwise;
    depthPipeline.depthStencil.depthTestEnabled = true;
    depthPipeline.depthStencil.depthWriteEnabled = true;
    depthPipeline.depthStencil.depthComparison =
        RHI::CompareOperation::Less;
    depthPipeline.colorFormats = {
        RHI::Format::R32Float,
        RHI::Format::R8Unorm};
    depthPipeline.blendAttachments = {
        RHI::BlendAttachmentDescription{},
        RHI::BlendAttachmentDescription{}};
    depthPipeline.depthFormat = RHI::Format::D32Float;
    m_particleDepthPipeline = pipelineCache.GetOrCreateGraphics(
        device,
        "Feature.Fluid.ParticleDepth",
        depthPipeline);

    RHI::GraphicsPipelineDescription thicknessPipeline{};
    thicknessPipeline.vertexShader = surfaceVertex;
    thicknessPipeline.pixelShader = thicknessPixel;
    thicknessPipeline.descriptorSetLayout = m_surfaceLayout;
    thicknessPipeline.rasterizer.cullMode = RHI::CullMode::None;
    thicknessPipeline.rasterizer.frontFace =
        RHI::FrontFace::CounterClockwise;
    thicknessPipeline.depthStencil.depthTestEnabled = false;
    thicknessPipeline.depthStencil.depthWriteEnabled = false;
    thicknessPipeline.colorFormats = {RHI::Format::R16Float};
    thicknessPipeline.blendAttachments = {
        RHI::BlendAttachmentDescription{
            true,
            RHI::BlendFactor::One,
            RHI::BlendFactor::One,
            RHI::BlendOperation::Add,
            RHI::BlendFactor::One,
            RHI::BlendFactor::One,
            RHI::BlendOperation::Add,
            0x0f}};
    thicknessPipeline.depthFormat = RHI::Format::Unknown;
    m_particleThicknessPipeline =
        pipelineCache.GetOrCreateGraphics(
            device,
            "Feature.Fluid.ParticleThickness",
            thicknessPipeline);

    RHI::ComputePipelineDescription compute{};
    compute.computeShader = bilateralHorizontal;
    compute.descriptorSetLayout = m_bilateralLayout;
    m_bilateralHorizontalPipeline =
        pipelineCache.GetOrCreateCompute(
            device,
            "Feature.Fluid.BilateralHorizontal",
            compute);
    compute.computeShader = bilateralVertical;
    m_bilateralVerticalPipeline =
        pipelineCache.GetOrCreateCompute(
            device,
            "Feature.Fluid.BilateralVertical",
            compute);
    compute.computeShader = normalFoam;
    compute.descriptorSetLayout = m_normalFoamLayout;
    m_normalFoamPipeline = pipelineCache.GetOrCreateCompute(
        device,
        "Feature.Fluid.NormalFoam",
        compute);
    compute.computeShader = foamRefine;
    compute.descriptorSetLayout = m_foamRefineLayout;
    m_foamRefinePipeline = pipelineCache.GetOrCreateCompute(
        device,
        "Feature.Fluid.FoamRefine",
        compute);
    compute.computeShader = composite;
    compute.descriptorSetLayout = m_compositeLayout;
    m_compositePipeline = pipelineCache.GetOrCreateCompute(
        device,
        "Feature.Fluid.Composite",
        compute);

    RHI::SamplerDescription sampler{};
    sampler.filter = RHI::Filter::Linear;
    sampler.addressU = RHI::AddressMode::ClampToEdge;
    sampler.addressV = RHI::AddressMode::ClampToEdge;
    sampler.addressW = RHI::AddressMode::ClampToEdge;
    m_linearClampSampler = device.CreateSampler(sampler);

    m_frames.resize(framesInFlight);
    for (FrameResources& frame : m_frames)
    {
        frame.constants = CreateConstantsBuffer(
            device, sizeof(Constants));
        for (std::shared_ptr<RHI::IBuffer>& filterConstants :
             frame.filterConstants)
        {
            filterConstants = CreateConstantsBuffer(
                device, sizeof(Constants));
        }
    }
}

void ScreenSpaceFluidRenderer::Resize(
    const std::uint32_t width,
    const std::uint32_t height)
{
    Core::Check(
        m_device != nullptr && width > 0 && height > 0,
        "Screen-space fluid resize requires a device and a non-zero extent.");
    m_width = width;
    m_height = height;

    const RHI::TextureUsage sampledTarget =
        RHI::TextureUsage::ShaderResource
        | RHI::TextureUsage::RenderTarget;
    const RHI::TextureUsage sampledStorage =
        RHI::TextureUsage::ShaderResource
        | RHI::TextureUsage::UnorderedAccess;
    m_textures.rawDepth = CreateFluidTexture(
        *m_device,
        width,
        height,
        RHI::Format::R32Float,
        sampledTarget,
        "Fluid.RawDepth");
    m_textures.mask = CreateFluidTexture(
        *m_device,
        width,
        height,
        RHI::Format::R8Unorm,
        sampledTarget,
        "Fluid.Mask");
    m_textures.depthStencil = CreateFluidTexture(
        *m_device,
        width,
        height,
        RHI::Format::D32Float,
        RHI::TextureUsage::DepthStencil,
        "Fluid.DepthStencil");
    m_textures.thickness = CreateFluidTexture(
        *m_device,
        width,
        height,
        RHI::Format::R16Float,
        sampledTarget,
        "Fluid.Thickness");
    m_textures.smoothDepthA = CreateFluidTexture(
        *m_device,
        width,
        height,
        RHI::Format::R32Float,
        sampledStorage,
        "Fluid.SmoothDepthA");
    m_textures.smoothDepthB = CreateFluidTexture(
        *m_device,
        width,
        height,
        RHI::Format::R32Float,
        sampledStorage,
        "Fluid.SmoothDepthB");
    m_textures.normal = CreateFluidTexture(
        *m_device,
        width,
        height,
        RHI::Format::Rgba16Float,
        sampledStorage,
        "Fluid.Normal");
    m_textures.foamRaw = CreateFluidTexture(
        *m_device,
        width,
        height,
        RHI::Format::R8Unorm,
        sampledStorage,
        "Fluid.FoamRaw");
    m_textures.foam = CreateFluidTexture(
        *m_device,
        width,
        height,
        RHI::Format::R8Unorm,
        sampledStorage,
        "Fluid.Foam");
    m_textures.composite = CreateFluidTexture(
        *m_device,
        width,
        height,
        RHI::Format::Rgba16Float,
        sampledStorage,
        "Fluid.Composite");

    m_views.rawDepthTarget = CreateView(
        *m_device,
        m_textures.rawDepth,
        RHI::TextureViewType::RenderTarget);
    m_views.rawDepthSampled = CreateView(
        *m_device,
        m_textures.rawDepth,
        RHI::TextureViewType::Sampled);
    m_views.maskTarget = CreateView(
        *m_device,
        m_textures.mask,
        RHI::TextureViewType::RenderTarget);
    m_views.depthStencil = CreateView(
        *m_device,
        m_textures.depthStencil,
        RHI::TextureViewType::DepthStencil);
    m_views.thicknessTarget = CreateView(
        *m_device,
        m_textures.thickness,
        RHI::TextureViewType::RenderTarget);
    m_views.smoothDepthAStorage = CreateView(
        *m_device,
        m_textures.smoothDepthA,
        RHI::TextureViewType::Storage);
    m_views.smoothDepthBStorage = CreateView(
        *m_device,
        m_textures.smoothDepthB,
        RHI::TextureViewType::Storage);
    m_views.normalStorage = CreateView(
        *m_device,
        m_textures.normal,
        RHI::TextureViewType::Storage);
    m_views.normalSampled = CreateView(
        *m_device,
        m_textures.normal,
        RHI::TextureViewType::Sampled);
    m_views.foamRawStorage = CreateView(
        *m_device,
        m_textures.foamRaw,
        RHI::TextureViewType::Storage);
    m_views.foamStorage = CreateView(
        *m_device,
        m_textures.foam,
        RHI::TextureViewType::Storage);
    m_views.compositeStorage = CreateView(
        *m_device,
        m_textures.composite,
        RHI::TextureViewType::Storage);
    m_views.compositeSampled = CreateView(
        *m_device,
        m_textures.composite,
        RHI::TextureViewType::Sampled);

    m_states = {};
    RebuildDescriptorSetsIfReady();
}

void ScreenSpaceFluidRenderer::SetInputs(
    std::shared_ptr<RHI::IBuffer> particlePositions,
    std::shared_ptr<RHI::IBuffer> particleDensities,
    const std::uint32_t particleCount,
    std::shared_ptr<RHI::ITexture> sceneColor,
    std::shared_ptr<RHI::ITexture> sceneDepth,
    std::shared_ptr<RHI::ITexture> environment,
    std::shared_ptr<RHI::ITexture> caustics)
{
    Core::Check(
        particlePositions != nullptr
            && particleCount > 0
            && sceneColor != nullptr
            && sceneDepth != nullptr
            && environment != nullptr,
        "Screen-space fluid inputs require particles, scene color/depth, and an environment cubemap.");
    Core::Check(
        environment->GetDescription().dimension
            == RHI::TextureDimension::TextureCube,
        "Screen-space fluid reflections require a cubemap texture.");
    const bool bindingsChanged =
        m_particlePositions != particlePositions
        || m_particleDensities != particleDensities
        || m_sceneColor != sceneColor
        || m_sceneDepth != sceneDepth
        || m_environment != environment
        || m_caustics != caustics;
    m_particlePositions = std::move(particlePositions);
    m_particleDensities = std::move(particleDensities);
    m_particleCount = particleCount;
    m_sceneColor = std::move(sceneColor);
    m_sceneDepth = std::move(sceneDepth);
    m_environment = std::move(environment);
    m_caustics = std::move(caustics);
    if (bindingsChanged)
    {
        RebuildDescriptorSetsIfReady();
    }
}

void ScreenSpaceFluidRenderer::SetCausticsTexture(
    std::shared_ptr<RHI::ITexture> caustics)
{
    if (m_caustics == caustics)
    {
        return;
    }
    m_caustics = std::move(caustics);
    RebuildDescriptorSetsIfReady();
}

void ScreenSpaceFluidRenderer::Update(
    const std::uint32_t frameIndex,
    const ScreenSpaceFluidFrameParameters& parameters)
{
    Core::Check(
        frameIndex < m_frames.size() && IsReady(),
        "Screen-space fluid update uses invalid resources.");
    m_parameters = parameters;
    m_parameters.particleRadius =
        std::max(parameters.particleRadius, 0.0001f);
    m_parameters.bilateralIterations = std::clamp(
        parameters.bilateralIterations,
        1u,
        MaxBilateralIterations);
    m_parameters.bilateralRadius = std::clamp(
        parameters.bilateralRadius,
        1u,
        MaxBilateralRadius);
    m_parameters.bilateralSpatialSigma =
        std::max(parameters.bilateralSpatialSigma, 0.01f);
    m_parameters.bilateralDepthSigma =
        std::max(parameters.bilateralDepthSigma, 0.0001f);
    m_parameters.normalSmoothingRadius = std::clamp(
        parameters.normalSmoothingRadius,
        1u,
        8u);
    m_parameters.silhouetteSmoothingRadius = std::clamp(
        parameters.silhouetteSmoothingRadius,
        0u,
        8u);
    m_parameters.minimumDensityRatio = std::clamp(
        parameters.minimumDensityRatio, 0.0f, 1.0f);
    m_parameters.surfaceCoverageThreshold = std::clamp(
        parameters.surfaceCoverageThreshold, 0.0f, 2.0f);
    m_parameters.foamDensityRatio = std::clamp(
        parameters.foamDensityRatio,
        m_parameters.minimumDensityRatio,
        2.0f);
    m_parameters.indexOfRefraction =
        std::max(parameters.indexOfRefraction, 1.0001f);
    m_parameters.thicknessScale =
        std::max(parameters.thicknessScale, 0.0f);
    m_parameters.toonDiffuseSteps =
        std::max(parameters.toonDiffuseSteps, 1u);
    m_parameters.toonReflectionSteps =
        std::max(parameters.toonReflectionSteps, 1u);

    Constants constants{};
    DirectX::XMStoreFloat4x4(
        &constants.view,
        DirectX::XMMatrixTranspose(parameters.view));
    DirectX::XMStoreFloat4x4(
        &constants.projection,
        DirectX::XMMatrixTranspose(parameters.projection));
    DirectX::XMVECTOR determinant{};
    const DirectX::XMMATRIX inverseProjection =
        DirectX::XMMatrixInverse(
            &determinant,
            parameters.projection);
    const DirectX::XMMATRIX inverseView =
        DirectX::XMMatrixInverse(
            &determinant,
            parameters.view);
    DirectX::XMStoreFloat4x4(
        &constants.inverseProjection,
        DirectX::XMMatrixTranspose(inverseProjection));
    DirectX::XMStoreFloat4x4(
        &constants.inverseView,
        DirectX::XMMatrixTranspose(inverseView));
    constants.resolutionInverseResolution = {
        static_cast<float>(m_width),
        static_cast<float>(m_height),
        1.0f / static_cast<float>(m_width),
        1.0f / static_cast<float>(m_height)};
    constants.cameraPositionParticleRadius = {
        parameters.cameraPosition.x,
        parameters.cameraPosition.y,
        parameters.cameraPosition.z,
        m_parameters.particleRadius};
    constants.filterParameters = {
        static_cast<float>(m_parameters.bilateralRadius),
        m_parameters.bilateralSpatialSigma,
        m_parameters.bilateralDepthSigma,
        static_cast<float>(m_parameters.normalSmoothingRadius)};
    constants.densityParameters = {
        m_parameters.minimumDensityRatio,
        m_parameters.foamDensityRatio,
        m_particleDensities != nullptr ? 1.0f : 0.0f,
        m_parameters.surfaceCoverageThreshold};
    constants.absorptionScattering = {
        std::max(parameters.absorption.x, 0.0f),
        std::max(parameters.absorption.y, 0.0f),
        std::max(parameters.absorption.z, 0.0f),
        std::max(parameters.scatteringStrength, 0.0f)};
    constants.waterColorIor = {
        std::max(parameters.waterColor.x, 0.0f),
        std::max(parameters.waterColor.y, 0.0f),
        std::max(parameters.waterColor.z, 0.0f),
        m_parameters.indexOfRefraction};
    constants.opticalParameters = {
        std::max(parameters.refractionScale, 0.0f),
        std::max(parameters.reflectionStrength, 0.0f),
        m_parameters.thicknessScale,
        std::max(parameters.foamIntensity, 0.0f)};
    constants.foamParameters = {
        std::max(parameters.foamCurvatureThreshold, 0.0001f),
        std::max(parameters.foamThicknessThreshold, 0.0001f),
        std::clamp(parameters.foamNeighborhoodThreshold, 0.0f, 1.0f),
        std::max(parameters.causticsIntensity, 0.0f)};
    constants.toonParameters = {
        static_cast<float>(m_parameters.toonDiffuseSteps),
        static_cast<float>(m_parameters.toonReflectionSteps),
        std::max(parameters.outlineDepthThreshold, 0.0001f),
        std::max(parameters.outlineWidth, 0.0f)};
    constants.lightDirectionOutline = {
        parameters.lightDirection.x,
        parameters.lightDirection.y,
        parameters.lightDirection.z,
        static_cast<float>(
            m_parameters.silhouetteSmoothingRadius)};
    constants.countsAndModes = {
        m_particleCount,
        static_cast<std::uint32_t>(parameters.shadingMode),
        static_cast<std::uint32_t>(parameters.displayMode),
        m_caustics != nullptr
                && parameters.causticsIntensity > 0.0f
            ? 1u
            : 0u};
    m_frames[frameIndex].constants->Update(
        &constants, sizeof(constants));
    const std::uint32_t initialRadius =
        m_parameters.bilateralRadius;
    for (std::uint32_t iteration = 0;
         iteration < MaxBilateralIterations;
         ++iteration)
    {
        Constants filterConstants = constants;
        const std::uint32_t radius = std::max(
            1u,
            initialRadius > iteration * 2u
                ? initialRadius - iteration * 2u
                : 1u);
        const float radiusScale = static_cast<float>(radius)
            / static_cast<float>(initialRadius);
        filterConstants.filterParameters = {
            static_cast<float>(radius),
            std::max(
                1.0f,
                m_parameters.bilateralSpatialSigma
                    * radiusScale),
            m_parameters.bilateralDepthSigma,
            static_cast<float>(iteration)};
        m_frames[frameIndex].filterConstants[iteration]->Update(
            &filterConstants,
            sizeof(filterConstants));
    }
}

ScreenSpaceFluidGraphResources
ScreenSpaceFluidRenderer::RegisterRenderGraph(
    RenderGraph& graph,
    const BufferHandle particlePositions,
    const BufferHandle particleDensities,
    const TextureHandle sceneColor,
    const TextureHandle sceneDepth,
    const TextureHandle environment,
    const TextureHandle caustics)
{
    Core::Check(
        IsReady()
            && particlePositions.IsValid()
            && sceneColor.IsValid()
            && sceneDepth.IsValid()
            && environment.IsValid(),
        "Screen-space fluid graph registration requires complete input handles.");
    ScreenSpaceFluidGraphResources resources{};
    resources.particlePositions = particlePositions;
    resources.particleDensities = particleDensities;
    resources.sceneColor = sceneColor;
    resources.sceneDepth = sceneDepth;
    resources.environment = environment;
    resources.caustics = caustics;
    resources.rawDepth = graph.DeclareTexture(
        "Fluid.RawDepth",
        *m_textures.rawDepth,
        m_states.rawDepth);
    resources.mask = graph.DeclareTexture(
        "Fluid.Mask",
        *m_textures.mask,
        m_states.mask);
    resources.depthStencil = graph.DeclareTexture(
        "Fluid.DepthStencil",
        *m_textures.depthStencil,
        m_states.depthStencil);
    // These textures are statically present in FluidCompositeCS's unified
    // descriptor set. Some demo branches deliberately prune their producer
    // passes, so import the persistent allocations as valid fallbacks instead
    // of declaring them as resources that must be produced in this graph.
    resources.thickness = graph.ImportTexture(
        "Fluid.Thickness",
        *m_textures.thickness,
        m_states.thickness);
    resources.smoothDepthA = graph.DeclareTexture(
        "Fluid.SmoothDepthA",
        *m_textures.smoothDepthA,
        m_states.smoothDepthA);
    resources.smoothDepthB = graph.ImportTexture(
        "Fluid.SmoothDepthB",
        *m_textures.smoothDepthB,
        m_states.smoothDepthB);
    resources.normal = graph.ImportTexture(
        "Fluid.Normal",
        *m_textures.normal,
        m_states.normal);
    resources.foamRaw = graph.DeclareTexture(
        "Fluid.FoamRaw",
        *m_textures.foamRaw,
        m_states.foamRaw);
    resources.foam = graph.ImportTexture(
        "Fluid.Foam",
        *m_textures.foam,
        m_states.foam);
    resources.composite = graph.DeclareTexture(
        "Fluid.Composite",
        *m_textures.composite,
        m_states.composite);
    return resources;
}

ScreenSpaceFluidPassCallbacks
ScreenSpaceFluidRenderer::CreatePassCallbacks(
    const std::uint32_t frameIndex) const
{
    Core::Check(
        frameIndex < m_frames.size() && IsReady(),
        "Screen-space fluid callbacks use an invalid frame.");
    ScreenSpaceFluidPassCallbacks callbacks{};
    callbacks.particleDepth =
        [this, frameIndex](
            RHI::ICommandContext& commandContext,
            const RenderGraphPassResources&)
        {
            ExecuteParticleDepth(commandContext, frameIndex);
        };
    callbacks.particleThickness =
        [this, frameIndex](
            RHI::ICommandContext& commandContext,
            const RenderGraphPassResources&)
        {
            ExecuteParticleThickness(commandContext, frameIndex);
        };
    for (std::uint32_t iteration = 0;
         iteration < MaxBilateralIterations;
         ++iteration)
    {
        callbacks.bilateralHorizontal[iteration] =
            [this, frameIndex, iteration](
                RHI::ICommandContext& commandContext,
                const RenderGraphPassResources&)
            {
                ExecuteBilateralHorizontal(
                    commandContext, frameIndex, iteration);
            };
        callbacks.bilateralVertical[iteration] =
            [this, frameIndex, iteration](
                RHI::ICommandContext& commandContext,
                const RenderGraphPassResources&)
            {
                ExecuteBilateralVertical(
                    commandContext, frameIndex, iteration);
            };
    }
    callbacks.reconstructNormalFoam =
        [this, frameIndex](
            RHI::ICommandContext& commandContext,
            const RenderGraphPassResources&)
        {
            ExecuteNormalFoam(commandContext, frameIndex);
        };
    callbacks.refineFoam =
        [this, frameIndex](
            RHI::ICommandContext& commandContext,
            const RenderGraphPassResources&)
        {
            ExecuteFoamRefine(commandContext, frameIndex);
        };
    callbacks.composite =
        [this, frameIndex](
            RHI::ICommandContext& commandContext,
            const RenderGraphPassResources&)
        {
            ExecuteComposite(commandContext, frameIndex);
        };
    return callbacks;
}

void ScreenSpaceFluidRenderer::AddSurfacePasses(
    RenderGraph& graph,
    ScreenSpaceFluidGraphResources& resources,
    const ScreenSpaceFluidPassCallbacks& callbacks)
{
    Core::Check(
        static_cast<bool>(callbacks.particleDepth)
            && static_cast<bool>(callbacks.particleThickness),
        "Screen-space fluid surface passes require execute callbacks.");
    AddParticleDepthPass(
        graph,
        resources,
        callbacks.particleDepth);

    auto thicknessParameters = graph.CreatePassParameters();
    thicknessParameters.ReadBuffer(
        resources.particlePositions,
        RHI::ResourceState::ShaderResource);
    if (resources.particleDensities.IsValid())
    {
        thicknessParameters.ReadBuffer(
            resources.particleDensities,
            RHI::ResourceState::ShaderResource);
    }
    thicknessParameters.ReadTexture(
        resources.sceneDepth,
        RHI::ResourceState::ShaderResource);
    resources.thickness = thicknessParameters.WriteTexture(
        resources.thickness,
        RHI::ResourceState::RenderTarget);
    graph.AddParameterPass(
        "Fluid.Thickness",
        std::move(thicknessParameters),
        callbacks.particleThickness,
        RenderGraph::PassOptions{
            RenderGraph::QueueClass::Graphics,
            true,
            false,
            true});
}

void ScreenSpaceFluidRenderer::AddParticlePreviewPasses(
    RenderGraph& graph,
    ScreenSpaceFluidGraphResources& resources,
    const ScreenSpaceFluidPassCallbacks& callbacks)
{
    Core::Check(
        static_cast<bool>(callbacks.particleDepth)
            && static_cast<bool>(callbacks.composite),
        "Particle preview requires depth and composite callbacks.");
    AddParticleDepthPass(
        graph,
        resources,
        callbacks.particleDepth);
    AddCompositePass(
        graph,
        resources,
        callbacks);
}

void ScreenSpaceFluidRenderer::AddFilterPasses(
    RenderGraph& graph,
    ScreenSpaceFluidGraphResources& resources,
    const std::uint32_t bilateralIterations,
    const bool generateFoam,
    const ScreenSpaceFluidPassCallbacks& callbacks)
{
    Core::Check(
        static_cast<bool>(callbacks.reconstructNormalFoam)
            && (!generateFoam
                || static_cast<bool>(callbacks.refineFoam)),
        "Screen-space fluid filtering requires execute callbacks.");
    const std::uint32_t iterationCount = std::clamp(
        bilateralIterations, 1u, MaxBilateralIterations);
    for (std::uint32_t iteration = 0;
         iteration < iterationCount;
         ++iteration)
    {
        Core::Check(
            static_cast<bool>(
                callbacks.bilateralHorizontal[iteration])
                && static_cast<bool>(
                    callbacks.bilateralVertical[iteration]),
            "Screen-space fluid filter iteration is missing callbacks.");
        auto horizontal = graph.CreatePassParameters();
        horizontal.ReadTexture(
            iteration == 0
                ? resources.rawDepth
                : resources.smoothDepthB,
            RHI::ResourceState::ShaderResource);
        resources.smoothDepthA = horizontal.WriteTexture(
            resources.smoothDepthA,
            RHI::ResourceState::UnorderedAccess);
        graph.AddParameterPass(
            "Fluid.BilateralHorizontal."
                + std::to_string(iteration),
            std::move(horizontal),
            callbacks.bilateralHorizontal[iteration],
            RenderGraph::PassOptions{
                RenderGraph::QueueClass::Compute,
                true,
                false,
                true});

        auto vertical = graph.CreatePassParameters();
        vertical.ReadTexture(
            resources.smoothDepthA,
            RHI::ResourceState::ShaderResource);
        resources.smoothDepthB = vertical.WriteTexture(
            resources.smoothDepthB,
            RHI::ResourceState::UnorderedAccess);
        graph.AddParameterPass(
            "Fluid.BilateralVertical."
                + std::to_string(iteration),
            std::move(vertical),
            callbacks.bilateralVertical[iteration],
            RenderGraph::PassOptions{
                RenderGraph::QueueClass::Compute,
                true,
                false,
                true});
    }

    auto normalFoam = graph.CreatePassParameters();
    normalFoam.ReadTexture(
        resources.smoothDepthB,
        RHI::ResourceState::ShaderResource);
    normalFoam.ReadTexture(
        resources.mask,
        RHI::ResourceState::ShaderResource);
    resources.normal = normalFoam.WriteTexture(
        resources.normal,
        RHI::ResourceState::UnorderedAccess);
    resources.foamRaw = normalFoam.WriteTexture(
        resources.foamRaw,
        RHI::ResourceState::UnorderedAccess);
    graph.AddParameterPass(
        "Fluid.ReconstructNormalFoam",
        std::move(normalFoam),
        callbacks.reconstructNormalFoam,
        RenderGraph::PassOptions{
            RenderGraph::QueueClass::Compute,
            true,
            false,
            true});

    if (generateFoam)
    {
        auto foamRefine = graph.CreatePassParameters();
        foamRefine.ReadTexture(
            resources.foamRaw,
            RHI::ResourceState::ShaderResource);
        foamRefine.ReadTexture(
            resources.mask,
            RHI::ResourceState::ShaderResource);
        resources.foam = foamRefine.WriteTexture(
            resources.foam,
            RHI::ResourceState::UnorderedAccess);
        graph.AddParameterPass(
            "Fluid.RefineFoam",
            std::move(foamRefine),
            callbacks.refineFoam,
            RenderGraph::PassOptions{
                RenderGraph::QueueClass::Compute,
                true,
                false,
                true});
    }
}

void ScreenSpaceFluidRenderer::AddCompositePass(
    RenderGraph& graph,
    ScreenSpaceFluidGraphResources& resources,
    const ScreenSpaceFluidPassCallbacks& callbacks)
{
    Core::Check(
        static_cast<bool>(callbacks.composite),
        "Screen-space fluid composition requires an execute callback.");
    auto parameters = graph.CreatePassParameters();
    // FluidCompositeCS reflects one descriptor layout for particle preview,
    // realistic, caustics, and toon modes. Declare every statically bound SRV
    // here even when a uniform-selected branch will not sample its contents.
    // This keeps Vulkan image layouts and D3D12 resource states valid without
    // adding any reconstruction/filter pass to the particle preview.
    parameters.ReadTexture(
        resources.sceneColor,
        RHI::ResourceState::ShaderResource);
    parameters.ReadTexture(
        resources.sceneDepth,
        RHI::ResourceState::ShaderResource);
    parameters.ReadTexture(
        resources.rawDepth,
        RHI::ResourceState::ShaderResource);
    parameters.ReadTexture(
        resources.smoothDepthB,
        RHI::ResourceState::ShaderResource);
    parameters.ReadTexture(
        resources.thickness,
        RHI::ResourceState::ShaderResource);
    parameters.ReadTexture(
        resources.normal,
        RHI::ResourceState::ShaderResource);
    parameters.ReadTexture(
        resources.foam,
        RHI::ResourceState::ShaderResource);
    parameters.ReadTexture(
        resources.mask,
        RHI::ResourceState::ShaderResource);
    parameters.ReadTexture(
        resources.environment,
        RHI::ResourceState::ShaderResource);
    Core::Check(
        resources.caustics.IsValid(),
        "Fluid composition requires a state-tracked caustics SRV or fallback.");
    parameters.ReadTexture(
        resources.caustics,
        RHI::ResourceState::ShaderResource);
    resources.composite = parameters.WriteTexture(
        resources.composite,
        RHI::ResourceState::UnorderedAccess);
    graph.AddParameterPass(
        "Fluid.Composite",
        std::move(parameters),
        callbacks.composite,
        RenderGraph::PassOptions{
            RenderGraph::QueueClass::Compute,
            true,
            false,
            true});
}

void ScreenSpaceFluidRenderer::EndParticlePreviewFrame(
    const bool executed)
{
    if (!executed)
    {
        return;
    }
    m_states.rawDepth = RHI::ResourceState::ShaderResource;
    m_states.mask = RHI::ResourceState::ShaderResource;
    m_states.depthStencil = RHI::ResourceState::DepthWrite;
    m_states.thickness = RHI::ResourceState::ShaderResource;
    m_states.smoothDepthB = RHI::ResourceState::ShaderResource;
    m_states.normal = RHI::ResourceState::ShaderResource;
    m_states.foam = RHI::ResourceState::ShaderResource;
    m_states.composite = RHI::ResourceState::ShaderResource;
}

void ScreenSpaceFluidRenderer::ExecuteParticleDepth(
    RHI::ICommandContext& commandContext,
    const std::uint32_t frameIndex) const
{
    Core::Check(
        frameIndex < m_frames.size() && IsReady(),
        "Fluid depth rendering uses invalid resources.");
    RHI::RenderingInfo rendering{};
    rendering.width = m_width;
    rendering.height = m_height;

    RHI::RenderingAttachment rawDepth{};
    rawDepth.view = m_views.rawDepthTarget.get();
    rawDepth.loadOperation = RHI::LoadOperation::Clear;
    rawDepth.storeOperation = RHI::StoreOperation::Store;
    rawDepth.clearColor = {0.0f, 0.0f, 0.0f, 0.0f};
    rawDepth.stateBefore = RHI::ResourceState::RenderTarget;
    rawDepth.stateAfter = RHI::ResourceState::RenderTarget;
    rendering.colorAttachments.push_back(rawDepth);

    RHI::RenderingAttachment mask{};
    mask.view = m_views.maskTarget.get();
    mask.loadOperation = RHI::LoadOperation::Clear;
    mask.storeOperation = RHI::StoreOperation::Store;
    mask.clearColor = {0.0f, 0.0f, 0.0f, 0.0f};
    mask.stateBefore = RHI::ResourceState::RenderTarget;
    mask.stateAfter = RHI::ResourceState::RenderTarget;
    rendering.colorAttachments.push_back(mask);

    RHI::RenderingAttachment depth{};
    depth.view = m_views.depthStencil.get();
    depth.loadOperation = RHI::LoadOperation::Clear;
    depth.storeOperation = RHI::StoreOperation::Store;
    depth.clearDepthStencil.depth = 1.0f;
    depth.stateBefore = RHI::ResourceState::DepthWrite;
    depth.stateAfter = RHI::ResourceState::DepthWrite;
    rendering.depthAttachment = depth;

    commandContext.BeginRendering(rendering);
    commandContext.BindGraphicsPipeline(*m_particleDepthPipeline);
    commandContext.BindDescriptorSet(
        *m_frames[frameIndex].surfaceSet);
    commandContext.Draw(6u, m_particleCount);
    commandContext.EndRendering();
}

void ScreenSpaceFluidRenderer::ExecuteParticleThickness(
    RHI::ICommandContext& commandContext,
    const std::uint32_t frameIndex) const
{
    Core::Check(
        frameIndex < m_frames.size() && IsReady(),
        "Fluid thickness rendering uses invalid resources.");
    RHI::RenderingInfo rendering{};
    rendering.width = m_width;
    rendering.height = m_height;
    RHI::RenderingAttachment thickness{};
    thickness.view = m_views.thicknessTarget.get();
    thickness.loadOperation = RHI::LoadOperation::Clear;
    thickness.storeOperation = RHI::StoreOperation::Store;
    thickness.clearColor = {0.0f, 0.0f, 0.0f, 0.0f};
    thickness.stateBefore = RHI::ResourceState::RenderTarget;
    thickness.stateAfter = RHI::ResourceState::RenderTarget;
    rendering.colorAttachments.push_back(thickness);
    commandContext.BeginRendering(rendering);
    commandContext.BindGraphicsPipeline(*m_particleThicknessPipeline);
    commandContext.BindDescriptorSet(
        *m_frames[frameIndex].surfaceSet);
    commandContext.Draw(6u, m_particleCount);
    commandContext.EndRendering();
}

void ScreenSpaceFluidRenderer::ExecuteBilateralHorizontal(
    RHI::ICommandContext& commandContext,
    const std::uint32_t frameIndex,
    const std::uint32_t iteration) const
{
    Core::Check(
        frameIndex < m_frames.size()
            && iteration < MaxBilateralIterations
            && IsReady(),
        "Fluid horizontal filtering uses invalid resources.");
    DispatchFullscreen(
        commandContext,
        *m_bilateralHorizontalPipeline,
        *m_frames[frameIndex]
             .bilateralHorizontalSets[iteration]);
}

void ScreenSpaceFluidRenderer::ExecuteBilateralVertical(
    RHI::ICommandContext& commandContext,
    const std::uint32_t frameIndex,
    const std::uint32_t iteration) const
{
    Core::Check(
        frameIndex < m_frames.size()
            && iteration < MaxBilateralIterations
            && IsReady(),
        "Fluid vertical filtering uses invalid resources.");
    DispatchFullscreen(
        commandContext,
        *m_bilateralVerticalPipeline,
        *m_frames[frameIndex]
             .bilateralVerticalSets[iteration]);
}

void ScreenSpaceFluidRenderer::ExecuteNormalFoam(
    RHI::ICommandContext& commandContext,
    const std::uint32_t frameIndex) const
{
    Core::Check(
        frameIndex < m_frames.size() && IsReady(),
        "Fluid normal reconstruction uses invalid resources.");
    DispatchFullscreen(
        commandContext,
        *m_normalFoamPipeline,
        *m_frames[frameIndex].normalFoamSet);
}

void ScreenSpaceFluidRenderer::ExecuteFoamRefine(
    RHI::ICommandContext& commandContext,
    const std::uint32_t frameIndex) const
{
    Core::Check(
        frameIndex < m_frames.size() && IsReady(),
        "Fluid foam refinement uses invalid resources.");
    DispatchFullscreen(
        commandContext,
        *m_foamRefinePipeline,
        *m_frames[frameIndex].foamRefineSet);
}

void ScreenSpaceFluidRenderer::ExecuteComposite(
    RHI::ICommandContext& commandContext,
    const std::uint32_t frameIndex) const
{
    Core::Check(
        frameIndex < m_frames.size() && IsReady(),
        "Fluid composition uses invalid resources.");
    DispatchFullscreen(
        commandContext,
        *m_compositePipeline,
        *m_frames[frameIndex].compositeSet);
}

void ScreenSpaceFluidRenderer::EndFrame(
    const bool surfaceExecuted,
    const bool filterExecuted,
    const bool foamExecuted,
    const bool compositeExecuted)
{
    if (surfaceExecuted)
    {
        m_states.rawDepth = RHI::ResourceState::RenderTarget;
        m_states.mask = RHI::ResourceState::RenderTarget;
        m_states.depthStencil = RHI::ResourceState::DepthWrite;
        m_states.thickness = RHI::ResourceState::RenderTarget;
    }
    if (filterExecuted)
    {
        m_states.rawDepth = RHI::ResourceState::ShaderResource;
        m_states.mask = RHI::ResourceState::ShaderResource;
        m_states.thickness = RHI::ResourceState::ShaderResource;
        m_states.smoothDepthA = RHI::ResourceState::ShaderResource;
        m_states.smoothDepthB = RHI::ResourceState::ShaderResource;
        m_states.normal = RHI::ResourceState::ShaderResource;
        m_states.foamRaw = foamExecuted
            ? RHI::ResourceState::ShaderResource
            : RHI::ResourceState::UnorderedAccess;
    }
    if (foamExecuted)
    {
        m_states.foam = RHI::ResourceState::ShaderResource;
    }
    if (compositeExecuted)
    {
        // The unified composite descriptor statically exposes foam even when
        // realistic/caustics modes select a branch that does not sample it.
        m_states.foam = RHI::ResourceState::ShaderResource;
        m_states.composite = RHI::ResourceState::ShaderResource;
    }
}

const ScreenSpaceFluidTextures&
ScreenSpaceFluidRenderer::GetTextures() const
{
    return m_textures;
}

std::shared_ptr<RHI::ITexture>
ScreenSpaceFluidRenderer::GetCompositeTexture() const
{
    return m_textures.composite;
}

std::shared_ptr<RHI::ITextureView>
ScreenSpaceFluidRenderer::GetCompositeSampledView() const
{
    return m_views.compositeSampled;
}

std::shared_ptr<RHI::ITextureView>
ScreenSpaceFluidRenderer::GetRawDepthSampledView() const
{
    return m_views.rawDepthSampled;
}

std::shared_ptr<RHI::ITextureView>
ScreenSpaceFluidRenderer::GetNormalSampledView() const
{
    return m_views.normalSampled;
}

RHI::ResourceState
ScreenSpaceFluidRenderer::GetCompositeInitialState() const
{
    return m_states.composite;
}

std::uint32_t
ScreenSpaceFluidRenderer::GetBilateralIterations() const
{
    return m_parameters.bilateralIterations;
}

bool ScreenSpaceFluidRenderer::IsReady() const
{
    return m_device != nullptr
        && m_particleDepthPipeline != nullptr
        && m_particleThicknessPipeline != nullptr
        && m_bilateralHorizontalPipeline != nullptr
        && m_bilateralVerticalPipeline != nullptr
        && m_normalFoamPipeline != nullptr
        && m_foamRefinePipeline != nullptr
        && m_compositePipeline != nullptr
        && m_width > 0
        && m_height > 0
        && m_particlePositions != nullptr
        && m_particleCount > 0
        && m_sceneColor != nullptr
        && m_sceneDepth != nullptr
        && m_environment != nullptr
        && m_textures.composite != nullptr
        && !m_frames.empty()
        && m_frames.front().surfaceSet != nullptr;
}

void ScreenSpaceFluidRenderer::RebuildDescriptorSets()
{
    Core::Check(
        m_device != nullptr
            && m_particlePositions != nullptr
            && m_sceneColor != nullptr
            && m_sceneDepth != nullptr
            && m_environment != nullptr
            && m_textures.rawDepth != nullptr
            && m_textures.mask != nullptr
            && m_textures.thickness != nullptr
            && m_textures.smoothDepthA != nullptr
            && m_textures.smoothDepthB != nullptr
            && m_textures.normal != nullptr
            && m_textures.foamRaw != nullptr
            && m_textures.foam != nullptr
            && m_views.smoothDepthAStorage != nullptr
            && m_views.smoothDepthBStorage != nullptr
            && m_views.normalStorage != nullptr
            && m_views.foamRawStorage != nullptr
            && m_views.foamStorage != nullptr
            && m_views.compositeStorage != nullptr,
        "Fluid descriptors require all input and output resources.");
    const std::shared_ptr<RHI::ITexture>& causticsTexture =
        m_caustics != nullptr ? m_caustics : m_textures.foam;
    for (FrameResources& frame : m_frames)
    {
        frame.surfaceSet = m_device->CreateDescriptorSet(
            m_surfaceLayout);
        frame.surfaceSet->WriteBuffer(0, frame.constants);
        frame.surfaceSet->WriteBuffer(16, m_particlePositions);
        frame.surfaceSet->WriteTexture(17, m_sceneDepth);
        frame.surfaceSet->WriteBuffer(
            18,
            m_particleDensities != nullptr
                ? m_particleDensities
                : m_particlePositions);

        for (std::uint32_t iteration = 0;
             iteration < MaxBilateralIterations;
             ++iteration)
        {
            frame.bilateralHorizontalSets[iteration] =
                m_device->CreateDescriptorSet(m_bilateralLayout);
            frame.bilateralHorizontalSets[iteration]->WriteBuffer(
                0, frame.filterConstants[iteration]);
            frame.bilateralHorizontalSets[iteration]->WriteTexture(
                16,
                iteration == 0
                    ? m_textures.rawDepth
                    : m_textures.smoothDepthB);
            frame.bilateralHorizontalSets[iteration]
                ->WriteTextureView(
                    32, m_views.smoothDepthAStorage);

            frame.bilateralVerticalSets[iteration] =
                m_device->CreateDescriptorSet(m_bilateralLayout);
            frame.bilateralVerticalSets[iteration]->WriteBuffer(
                0, frame.filterConstants[iteration]);
            frame.bilateralVerticalSets[iteration]->WriteTexture(
                16, m_textures.smoothDepthA);
            frame.bilateralVerticalSets[iteration]
                ->WriteTextureView(
                    32, m_views.smoothDepthBStorage);
        }

        frame.normalFoamSet = m_device->CreateDescriptorSet(
            m_normalFoamLayout);
        frame.normalFoamSet->WriteBuffer(0, frame.constants);
        frame.normalFoamSet->WriteTexture(
            16, m_textures.smoothDepthB);
        frame.normalFoamSet->WriteTexture(17, m_textures.mask);
        frame.normalFoamSet->WriteTextureView(
            32, m_views.normalStorage);
        frame.normalFoamSet->WriteTextureView(
            33, m_views.foamRawStorage);

        frame.foamRefineSet = m_device->CreateDescriptorSet(
            m_foamRefineLayout);
        frame.foamRefineSet->WriteBuffer(0, frame.constants);
        frame.foamRefineSet->WriteTexture(
            16, m_textures.foamRaw);
        frame.foamRefineSet->WriteTexture(17, m_textures.mask);
        frame.foamRefineSet->WriteTextureView(
            34, m_views.foamStorage);

        frame.compositeSet = m_device->CreateDescriptorSet(
            m_compositeLayout);
        frame.compositeSet->WriteBuffer(0, frame.constants);
        frame.compositeSet->WriteTexture(16, m_sceneColor);
        frame.compositeSet->WriteTexture(17, m_sceneDepth);
        frame.compositeSet->WriteTexture(
            18, m_textures.rawDepth);
        frame.compositeSet->WriteTexture(
            19, m_textures.smoothDepthB);
        frame.compositeSet->WriteTexture(
            20, m_textures.thickness);
        frame.compositeSet->WriteTexture(
            21, m_textures.normal);
        frame.compositeSet->WriteTexture(22, m_textures.foam);
        frame.compositeSet->WriteTexture(23, m_textures.mask);
        frame.compositeSet->WriteTexture(24, m_environment);
        frame.compositeSet->WriteTexture(25, causticsTexture);
        frame.compositeSet->WriteTextureView(
            32, m_views.compositeStorage);
        frame.compositeSet->WriteSampler(
            48, m_linearClampSampler);
    }
}

void ScreenSpaceFluidRenderer::RebuildDescriptorSetsIfReady()
{
    if (m_device == nullptr
        || m_particlePositions == nullptr
        || m_sceneColor == nullptr
        || m_sceneDepth == nullptr
        || m_environment == nullptr
        || m_textures.composite == nullptr)
    {
        return;
    }
    RebuildDescriptorSets();
}

void ScreenSpaceFluidRenderer::DispatchFullscreen(
    RHI::ICommandContext& commandContext,
    const RHI::IComputePipeline& pipeline,
    const RHI::IDescriptorSet& descriptorSet) const
{
    commandContext.BindComputePipeline(pipeline);
    commandContext.BindDescriptorSet(descriptorSet);
    commandContext.Dispatch(
        (m_width + ComputeGroupSize - 1u) / ComputeGroupSize,
        (m_height + ComputeGroupSize - 1u) / ComputeGroupSize,
        1u);
}
} // namespace Prism::Renderer
