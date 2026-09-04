#include "Renderer/Features/Ocean/WaterOpticsFeature.h"

#include "Asset/Mesh.h"
#include "Asset/ShaderManager.h"
#include "Core/Assert.h"
#include "Renderer/Features/Ocean/WaterOpticsTransientLayout.h"
#include "Renderer/Pipeline/PipelineCache.h"
#include "RHI/ICommandContext.h"
#include "RHI/IGraphicsDevice.h"
#include "RHI/ShaderLayoutBuilder.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <string>
#include <utility>

namespace Prism::Renderer
{
void WaterOpticsFeature::Initialize(
    RHI::IGraphicsDevice& device,
    Asset::ShaderManager& shaderManager,
    PipelineCache& pipelineCache,
    const std::filesystem::path& shaderDirectory,
    const RHI::ShaderBinaryFormat shaderFormat,
    const std::uint32_t framesInFlight,
    std::shared_ptr<RHI::IDescriptorSetLayout> oceanLayout,
    const bool indexedObjectDrawingSupported)
{
    Core::Check(framesInFlight > 0u && oceanLayout != nullptr,
        "Water optics initialization requires frames and an ocean layout.");
    Shutdown();
    m_framesInFlight = framesInFlight;

    const std::filesystem::path surfacePath =
        shaderDirectory / "Ocean" / "OceanSurface.slang";
    const auto load = [&](const std::filesystem::path& path,
                          const char* entry,
                          const RHI::ShaderStage stage)
        -> const RHI::ShaderBinary&
    {
        return shaderManager.LoadShader(
            path, entry, stage, shaderFormat);
    };

    RHI::GraphicsPipelineDescription visibility{};
    // The indexed entry point emits hull-shader control points and therefore
    // is only valid for the tessellation PSO. The ordinary triangle PSO must
    // emit the pixel shader's VSOutput directly.
    visibility.vertexShader = load(surfacePath,
        "OceanVSMain", RHI::ShaderStage::Vertex);
    const std::filesystem::path visibilityPath =
        shaderDirectory / "Ocean" / "WaterVisibility.slang";
    visibility.pixelShader = load(visibilityPath,
        "WaterVisibilityPS", RHI::ShaderStage::Pixel);
    visibility.descriptorSetLayout = std::move(oceanLayout);
    visibility.vertexBindings = {{
        0u, sizeof(Asset::MeshVertex), RHI::VertexInputRate::PerVertex}};
    visibility.vertexAttributes = {
        {0u, 0u, RHI::VertexElementFormat::Float3,
            static_cast<std::uint32_t>(offsetof(Asset::MeshVertex, position))},
        {1u, 0u, RHI::VertexElementFormat::Float4,
            static_cast<std::uint32_t>(offsetof(Asset::MeshVertex, color))},
        {2u, 0u, RHI::VertexElementFormat::Float3,
            static_cast<std::uint32_t>(offsetof(Asset::MeshVertex, normal))},
        {3u, 0u, RHI::VertexElementFormat::Float2,
            static_cast<std::uint32_t>(offsetof(Asset::MeshVertex, texCoord))},
        {4u, 0u, RHI::VertexElementFormat::Float4,
            static_cast<std::uint32_t>(offsetof(Asset::MeshVertex, tangent))}};
    visibility.rasterizer.cullMode = RHI::CullMode::None;
    visibility.rasterizer.frontFace = RHI::FrontFace::CounterClockwise;
    visibility.depthStencil.depthTestEnabled = true;
    visibility.depthStencil.depthWriteEnabled = true;
    visibility.depthStencil.depthComparison = RHI::CompareOperation::Less;
    visibility.colorFormats = {RHI::Format::Rgba16Float,
        RHI::Format::Rgba8Unorm, RHI::Format::Rgba8Unorm,
        RHI::Format::Rg16Float};
    visibility.blendAttachments.assign(
        visibility.colorFormats.size(), RHI::BlendAttachmentDescription{});
    visibility.depthFormat = RHI::Format::D32Float;
    m_visibilityPipeline = pipelineCache.GetOrCreateGraphics(
        device, "Feature.WaterOptics.Visibility", visibility);

    // Adaptive patches always consume OceanPatchInstance data through
    // OceanVSIndexedMain. This is independent of ordinary indexed-object
    // drawing: D3D12 disables that path because StartInstanceLocation is not
    // an object index, but the unified ocean draw intentionally starts its
    // patch instances at zero. Gating this PSO on ordinary object indexing
    // made the HPWater pass bind OceanVSMain to the adaptive unit grid, so no
    // real ocean surface reached the water GBuffer on D3D12.
    (void)indexedObjectDrawingSupported;
    RHI::GraphicsPipelineDescription tessellation = visibility;
    tessellation.vertexShader = load(surfacePath,
        "OceanVSIndexedMain", RHI::ShaderStage::Vertex);
    tessellation.hullShader = load(surfacePath,
        "OceanHullMain", RHI::ShaderStage::Hull);
    tessellation.domainShader = load(surfacePath,
        "OceanDomainMain", RHI::ShaderStage::Domain);
    tessellation.topology = RHI::PrimitiveTopology::PatchList;
    tessellation.patchControlPointCount = 3u;
    m_visibilityTessellationPipeline =
        pipelineCache.GetOrCreateGraphics(
            device,
            "Feature.WaterOptics.Visibility.Tessellation",
            tessellation);

    const std::filesystem::path depthCopyPath =
        shaderDirectory / "Ocean" / "WaterDepthCopy.slang";
    const RHI::ShaderBinary& depthVertex = load(depthCopyPath,
        "FullscreenVS", RHI::ShaderStage::Vertex);
    const RHI::ShaderBinary& depthPixel = load(depthCopyPath,
        "WaterDepthCopyPS", RHI::ShaderStage::Pixel);
    const std::array stages = {
        RHI::ShaderLayoutStage{
            &depthVertex.reflection, RHI::ShaderStage::Vertex},
        RHI::ShaderLayoutStage{
            &depthPixel.reflection, RHI::ShaderStage::Pixel}};
    m_depthCopyLayout = device.CreateDescriptorSetLayout(
        RHI::BuildDescriptorSetLayout(stages, {}));
    RHI::GraphicsPipelineDescription depthCopy{};
    depthCopy.vertexShader = depthVertex;
    depthCopy.pixelShader = depthPixel;
    depthCopy.descriptorSetLayout = m_depthCopyLayout;
    depthCopy.colorFormats.clear();
    depthCopy.blendAttachments.clear();
    depthCopy.rasterizer.cullMode = RHI::CullMode::None;
    depthCopy.depthStencil.depthTestEnabled = true;
    depthCopy.depthStencil.depthWriteEnabled = true;
    depthCopy.depthStencil.depthComparison = RHI::CompareOperation::Always;
    depthCopy.depthFormat = RHI::Format::D32Float;
    m_depthCopyPipeline = pipelineCache.GetOrCreateGraphics(
        device, "Feature.WaterOptics.DepthCopy", depthCopy);

    const std::filesystem::path opticsPath =
        shaderDirectory / "Ocean" / "WaterOptics.slang";
    const RHI::ShaderBinary& refractionShader = load(opticsPath,
        "WaterRefractionCS", RHI::ShaderStage::Compute);
    const std::array refractionStages{RHI::ShaderLayoutStage{
        &refractionShader.reflection, RHI::ShaderStage::Compute}};
    m_refractionLayout = device.CreateDescriptorSetLayout(
        RHI::BuildDescriptorSetLayout(refractionStages, {}));
    RHI::ComputePipelineDescription refractionPipeline{};
    refractionPipeline.computeShader = refractionShader;
    refractionPipeline.descriptorSetLayout = m_refractionLayout;
    m_refractionPipeline = pipelineCache.GetOrCreateCompute(
        device, "Feature.WaterOptics.Refraction", refractionPipeline);
    RHI::SamplerDescription refractionSampler{};
    refractionSampler.filter = RHI::Filter::Linear;
    refractionSampler.addressU = RHI::AddressMode::ClampToEdge;
    refractionSampler.addressV = RHI::AddressMode::ClampToEdge;
    refractionSampler.addressW = RHI::AddressMode::ClampToEdge;
    m_refractionSampler = device.CreateSampler(refractionSampler);

    const std::filesystem::path causticPath =
        shaderDirectory / "Ocean" / "WaterCaustics.slang";
    const RHI::ShaderBinary& causticShader = load(causticPath,
        "WaterCausticAccumulateCS", RHI::ShaderStage::Compute);
    const std::array causticStages{RHI::ShaderLayoutStage{
        &causticShader.reflection, RHI::ShaderStage::Compute}};
    m_causticLayout = device.CreateDescriptorSetLayout(
        RHI::BuildDescriptorSetLayout(causticStages, {}));
    RHI::ComputePipelineDescription causticPipeline{};
    causticPipeline.computeShader = causticShader;
    causticPipeline.descriptorSetLayout = m_causticLayout;
    m_causticPipeline = pipelineCache.GetOrCreateCompute(
        device, "Feature.WaterOptics.Caustics", causticPipeline);
    RHI::SamplerDescription causticWrapSampler{};
    causticWrapSampler.filter = RHI::Filter::Linear;
    causticWrapSampler.addressU = RHI::AddressMode::Repeat;
    causticWrapSampler.addressV = RHI::AddressMode::Repeat;
    causticWrapSampler.addressW = RHI::AddressMode::Repeat;
    m_causticWrapSampler = device.CreateSampler(causticWrapSampler);

    const std::filesystem::path volumetricPath =
        shaderDirectory / "Ocean" / "WaterVolumetrics.slang";
    const auto createVolumetricPipeline = [&device, &load, &pipelineCache,
                                            &volumetricPath](
        const char* entry,
        const char* name,
        std::shared_ptr<RHI::IDescriptorSetLayout>& layout)
    {
        const RHI::ShaderBinary& shader = load(
            volumetricPath, entry, RHI::ShaderStage::Compute);
        const std::array stages{RHI::ShaderLayoutStage{
            &shader.reflection, RHI::ShaderStage::Compute}};
        layout = device.CreateDescriptorSetLayout(
            RHI::BuildDescriptorSetLayout(stages, {}));
        RHI::ComputePipelineDescription description{};
        description.computeShader = shader;
        description.descriptorSetLayout = layout;
        return pipelineCache.GetOrCreateCompute(device, name, description);
    };
    m_volumetricAccumulatePipeline = createVolumetricPipeline(
        "WaterVolumetricAccumulateCS", "Feature.WaterOptics.Volume.Accumulate",
        m_volumetricAccumulateLayout);
    m_volumetricTemporalPipeline = createVolumetricPipeline(
        "WaterVolumetricTemporalCS", "Feature.WaterOptics.Volume.Temporal",
        m_volumetricTemporalLayout);
    m_volumetricReconstructPipeline = createVolumetricPipeline(
        "WaterVolumetricReconstructCS",
        "Feature.WaterOptics.Volume.Reconstruct",
        m_volumetricReconstructLayout);

    const RHI::ShaderBinary& compositeShader = load(opticsPath,
        "WaterCompositeCS", RHI::ShaderStage::Compute);
    const std::array compositeStages{RHI::ShaderLayoutStage{
        &compositeShader.reflection, RHI::ShaderStage::Compute}};
    m_compositeLayout = device.CreateDescriptorSetLayout(
        RHI::BuildDescriptorSetLayout(compositeStages, {}));
    RHI::ComputePipelineDescription compositePipeline{};
    compositePipeline.computeShader = compositeShader;
    compositePipeline.descriptorSetLayout = m_compositeLayout;
    m_compositePipeline = pipelineCache.GetOrCreateCompute(
        device, "Feature.WaterOptics.Composite", compositePipeline);
    const RHI::ShaderBinary& publishShader = load(opticsPath,
        "WaterCompositePublishCS", RHI::ShaderStage::Compute);
    const std::array publishStages{RHI::ShaderLayoutStage{
        &publishShader.reflection, RHI::ShaderStage::Compute}};
    m_publishLayout = device.CreateDescriptorSetLayout(
        RHI::BuildDescriptorSetLayout(publishStages, {}));
    RHI::ComputePipelineDescription publishPipeline{};
    publishPipeline.computeShader = publishShader;
    publishPipeline.descriptorSetLayout = m_publishLayout;
    m_publishPipeline = pipelineCache.GetOrCreateCompute(
        device, "Feature.WaterOptics.Publish", publishPipeline);
    RHI::SamplerDescription shadowSampler{};
    shadowSampler.filter = RHI::Filter::ComparisonLinear;
    shadowSampler.addressU = RHI::AddressMode::ClampToBorder;
    shadowSampler.addressV = RHI::AddressMode::ClampToBorder;
    shadowSampler.addressW = RHI::AddressMode::ClampToBorder;
    shadowSampler.comparison = RHI::CompareOperation::LessEqual;
    shadowSampler.maxLod = 0.0f;
    m_compositeShadowSampler = device.CreateSampler(shadowSampler);

    m_constantBuffers.resize(framesInFlight);
    m_inverseViewProjections.resize(framesInFlight);
    const DirectX::XMMATRIX identity = DirectX::XMMatrixIdentity();
    for (auto& buffer : m_constantBuffers)
    {
        RHI::BufferDescription description{};
        description.size = sizeof(WaterOpticsGpuConstants);
        description.usage = RHI::BufferUsage::Constant;
        description.memoryAccess = RHI::MemoryAccess::CpuToGpu;
        buffer = device.CreateBuffer(description);
    }
    for (auto& inverseViewProjection : m_inverseViewProjections)
    {
        DirectX::XMStoreFloat4x4(
            &inverseViewProjection,
            DirectX::XMMatrixTranspose(identity));
    }
    m_statistics.initialized = true;
    m_coverage.Initialize(device, shaderManager, pipelineCache, shaderDirectory,
        shaderFormat, framesInFlight);
}

void WaterOpticsFeature::Shutdown()
{
    ReleaseSizeDependentResources(0u, true);
    m_retired.clear();
    m_constantBuffers.clear();
    m_inverseViewProjections.clear();
    m_depthCopyPipeline.reset();
    m_refractionPipeline.reset();
    m_causticPipeline.reset();
    m_volumetricAccumulatePipeline.reset();
    m_volumetricTemporalPipeline.reset();
    m_volumetricReconstructPipeline.reset();
    m_compositePipeline.reset();
    m_publishPipeline.reset();
    m_visibilityTessellationPipeline.reset();
    m_visibilityPipeline.reset();
    m_depthCopyLayout.reset();
    m_refractionLayout.reset();
    m_causticLayout.reset();
    m_volumetricAccumulateLayout.reset();
    m_volumetricTemporalLayout.reset();
    m_volumetricReconstructLayout.reset();
    m_compositeLayout.reset();
    m_publishLayout.reset();
    m_refractionSampler.reset();
    m_causticWrapSampler.reset();
    m_compositeShadowSampler.reset();
    m_framesInFlight = 0u;
    m_history.Reset();
    m_volumetricHistory.Reset();
    m_mediumState.Reset();
    m_statistics = {};
    m_coverage = {};
    m_hasPreviousSettings = false;
}

void WaterOpticsFeature::PrepareFrame(
    const WaterOpticsSettings& settings,
    const std::uint64_t frameSerial,
    const bool enabled)
{
    CollectRetiredResources(frameSerial);
    const WaterOpticsDirtyScope dirtyScopes = m_hasPreviousSettings
        ? ClassifyWaterOpticsDirtyScopes(m_previousSettings, settings)
        : WaterOpticsDirtyScope::None;

    if (!enabled)
    {
        if (HasResources())
        {
            ReleaseSizeDependentResources(frameSerial);
            ResetHistory();
        }
    }
    else
    {
        if (HasDirtyScope(dirtyScopes, WaterOpticsDirtyScope::Resources)
            && HasResources())
        {
            RetireCurrentResources(frameSerial);
        }
        if (HasDirtyScope(dirtyScopes, WaterOpticsDirtyScope::History))
        {
            ResetHistory();
        }
    }

    m_previousSettings = settings;
    m_hasPreviousSettings = true;
}

void WaterOpticsFeature::EnsureResources(
    RHI::IGraphicsDevice& device,
    const std::uint32_t width,
    const std::uint32_t height,
    const std::shared_ptr<RHI::ITexture>& opaqueDepth,
    const std::shared_ptr<RHI::ITextureView>& opaqueDepthView,
    const std::shared_ptr<RHI::ITexture>& sceneColor,
    const std::shared_ptr<RHI::ITextureView>& sceneColorView,
    const WaterOpticsSettings& settings,
    const std::uint64_t frameSerial,
    const WaterOpticsCompositeBindings* compositeBindings)
{
    Core::Check(IsInitialized() && width > 0u && height > 0u
            && opaqueDepth != nullptr && opaqueDepthView != nullptr
            && sceneColor != nullptr && sceneColorView != nullptr,
        "Water optics resource creation received invalid inputs.");
    CollectRetiredResources(frameSerial);
    const WaterOpticsQualityPolicy qualityPolicy =
        GetWaterOpticsQualityPolicy(settings.quality);
    const float effectiveVolumetricScale = std::clamp(
        settings.volumetrics.resolutionScale
            * qualityPolicy.volumetricResolutionMultiplier,
        0.25f, 1.0f);
    const bool recreate = m_pool == nullptr
        || width != m_width || height != m_height
        || settings.quality != m_activeQuality
        || std::abs(effectiveVolumetricScale
            - m_activeVolumetricResolutionScale) > 1.0e-6f
        || settings.caustics.resolution != m_activeCausticResolution;
    if (recreate)
    {
        RetireCurrentResources(frameSerial);
        WaterOpticsTransientLayoutConfig config{};
        config.width = width;
        config.height = height;
        config.refractionResolutionScale =
            qualityPolicy.refractionResolutionScale;
        config.volumetricResolutionScale = effectiveVolumetricScale;
        config.causticResolution = settings.caustics.resolution;
        m_pool = device.CreateTransientTexturePool(
            BuildWaterOpticsTransientTextureRequests(config));
        m_width = width;
        m_height = height;
        m_activeQuality = settings.quality;
        m_activeVolumetricResolutionScale = effectiveVolumetricScale;
        m_activeCausticResolution = settings.caustics.resolution;

        RHI::TextureViewDescription renderTarget{};
        renderTarget.type = RHI::TextureViewType::RenderTarget;
        RHI::TextureViewDescription sampled{};
        sampled.type = RHI::TextureViewType::Sampled;
        for (std::uint32_t index = 0u; index < m_gbuffer.size(); ++index)
        {
            m_gbuffer[index] = m_pool->GetTexture(
                "WaterGBuffer" + std::to_string(index));
            m_gbufferViews[index] = device.CreateTextureView(
                m_gbuffer[index], renderTarget);
            m_gbufferSampledViews[index] = device.CreateTextureView(
                m_gbuffer[index], sampled);
        }
        m_motion = m_pool->GetTexture("WaterMotion");
        m_motionView = device.CreateTextureView(m_motion, renderTarget);
        m_motionSampledView = device.CreateTextureView(m_motion, sampled);
        m_compositeDepth = m_pool->GetTexture("WaterCompositeDepth");
        RHI::TextureViewDescription depth{};
        depth.type = RHI::TextureViewType::DepthStencil;
        m_compositeDepthView = device.CreateTextureView(
            m_compositeDepth, depth);
        m_compositeDepthSampledView = device.CreateTextureView(
            m_compositeDepth, sampled);
        m_refraction = m_pool->GetTexture("WaterRefraction");
        m_refractionWidth = m_refraction->GetDescription().width;
        m_refractionHeight = m_refraction->GetDescription().height;
        m_refractionSampledView = device.CreateTextureView(
            m_refraction, sampled);
        RHI::TextureViewDescription storage{};
        storage.type = RHI::TextureViewType::Storage;
        m_refractionStorageView = device.CreateTextureView(
            m_refraction, storage);
        for (std::uint32_t index = 0u; index < m_caustics.size(); ++index)
        {
            m_caustics[index] = m_pool->GetTexture(index == 0u
                ? "WaterCausticsNear" : "WaterCausticsMiddle");
            m_causticSampledViews[index] = device.CreateTextureView(
                m_caustics[index], sampled);
            m_causticStorageViews[index] = device.CreateTextureView(
                m_caustics[index], storage);
        }
        constexpr std::array<const char*, 4> VolumetricNames = {
            "WaterVolumetricCurrent",
            "WaterVolumetricHistoryA",
            "WaterVolumetricHistoryB",
            "WaterVolumetricReconstruction"};
        for (std::uint32_t index = 0u; index < m_volumetrics.size(); ++index)
        {
            m_volumetrics[index] = m_pool->GetTexture(
                VolumetricNames[index]);
            m_volumetricSampledViews[index] = device.CreateTextureView(
                m_volumetrics[index], sampled);
            m_volumetricStorageViews[index] = device.CreateTextureView(
                m_volumetrics[index], storage);
        }
        m_volumetricWidth = m_volumetrics[0]->GetDescription().width;
        m_volumetricHeight = m_volumetrics[0]->GetDescription().height;
        m_volumetricReadIndex = 0u;
        m_composite = m_pool->GetTexture("WaterCompositeScratch");
        m_compositeSampledView = device.CreateTextureView(
            m_composite, sampled);
        m_compositeStorageView = device.CreateTextureView(
            m_composite, storage);
        m_gbufferStates.fill(RHI::ResourceState::Undefined);
        m_motionState = RHI::ResourceState::Undefined;
        m_compositeDepthState = RHI::ResourceState::Undefined;
        m_refractionState = RHI::ResourceState::Undefined;
        m_causticStates.fill(RHI::ResourceState::Undefined);
        m_volumetricStates.fill(RHI::ResourceState::Undefined);
        m_compositeState = RHI::ResourceState::Undefined;
        m_publishSet.reset();
        m_publishHdrStorageView.reset();
        m_publishMotionStorageView.reset();
        m_sceneMotionSource = nullptr;
        m_causticSets.clear();
        m_volumetricAccumulateSets.clear();
        for (auto& sets : m_volumetricTemporalSets)
            sets.clear();
        for (auto& sets : m_volumetricReconstructSets)
            sets.clear();
        m_volumetricHistory.Reset();
        ++m_statistics.resourceGeneration;
        m_statistics.allocatedMegabytes = static_cast<float>(
            m_pool->GetStatistics().physicalBytes)
            / (1024.0f * 1024.0f);
        m_statistics.effectiveRefractionWidth = m_refractionWidth;
        m_statistics.effectiveRefractionHeight = m_refractionHeight;
        m_statistics.effectiveRefractionResolutionScale =
            qualityPolicy.refractionResolutionScale;
        m_statistics.effectiveVolumetricWidth = m_volumetricWidth;
        m_statistics.effectiveVolumetricHeight = m_volumetricHeight;
        m_statistics.effectiveQuality = settings.quality;
    }

    const bool opaqueDepthChanged =
        m_opaqueDepthSource != opaqueDepth.get();
    if (m_depthCopySet == nullptr || opaqueDepthChanged)
    {
        m_depthCopySet = device.CreateDescriptorSet(m_depthCopyLayout);
        m_depthCopySet->WriteTextureView(16u, opaqueDepthView);
        m_opaqueDepthSource = opaqueDepth.get();
        m_refractionSets.clear();
        m_causticSets.clear();
        m_volumetricAccumulateSets.clear();
        for (auto& sets : m_volumetricTemporalSets)
            sets.clear();
        for (auto& sets : m_volumetricReconstructSets)
            sets.clear();
        m_compositeSets.clear();
        m_publishSet.reset();
    }
    if (m_refractionSets.empty()
        || m_sceneColorSource != sceneColor.get())
    {
        m_refractionSets.clear();
        m_causticSets.clear();
        m_volumetricAccumulateSets.clear();
        for (auto& sets : m_volumetricTemporalSets)
            sets.clear();
        for (auto& sets : m_volumetricReconstructSets)
            sets.clear();
        m_compositeSets.clear();
        m_publishSet.reset();
        m_refractionSets.reserve(m_constantBuffers.size());
        for (const auto& constantBuffer : m_constantBuffers)
        {
            auto set = device.CreateDescriptorSet(m_refractionLayout);
            set->WriteBuffer(5u, constantBuffer);
            set->WriteTextureView(21u, sceneColorView);
            set->WriteTextureView(22u, opaqueDepthView);
            set->WriteTextureView(23u, m_compositeDepthSampledView);
            set->WriteTextureView(24u, m_gbufferSampledViews[0]);
            set->WriteTextureView(25u, m_gbufferSampledViews[2]);
            set->WriteTextureView(34u, m_refractionStorageView);
            set->WriteSampler(50u, m_refractionSampler);
            m_refractionSets.push_back(std::move(set));
        }
        m_sceneColorSource = sceneColor.get();
        m_opaqueDepthSource = opaqueDepth.get();
    }
    m_passOptions.compositeEnabled = compositeBindings != nullptr;
    if (compositeBindings != nullptr)
    {
        Core::Check(compositeBindings->frameConstants != nullptr
                && compositeBindings->frameConstants->size()
                    == m_constantBuffers.size()
                && compositeBindings->environment != nullptr
                && compositeBindings->shadowMap != nullptr
                && compositeBindings->shadowMoments != nullptr
                && compositeBindings->atmosphereSkyView != nullptr
                && compositeBindings->slopeMoments != nullptr
                && compositeBindings->spectralGradient != nullptr
                && compositeBindings->localGradient != nullptr
                && compositeBindings->sceneMotion != nullptr,
            "Water composite bindings are incomplete.");
        m_refractionSets.at(compositeBindings->frameIndex)->WriteBuffer(
            0u, (*compositeBindings->frameConstants)[compositeBindings->frameIndex]);
        if (m_causticSets.empty())
        {
            m_causticSets.reserve(m_constantBuffers.size());
            for (std::size_t frameIndex = 0u;
                 frameIndex < m_constantBuffers.size(); ++frameIndex)
            {
                auto set = device.CreateDescriptorSet(m_causticLayout);
                set->WriteBuffer(0u,
                    (*compositeBindings->frameConstants)[frameIndex]);
                set->WriteBuffer(5u, m_constantBuffers[frameIndex]);
                set->WriteTextureView(
                    16u, compositeBindings->spectralGradient);
                set->WriteTextureView(
                    17u, compositeBindings->localGradient);
                set->WriteTextureView(32u, m_causticStorageViews[0]);
                set->WriteTextureView(33u, m_causticStorageViews[1]);
                set->WriteSampler(48u, m_causticWrapSampler);
                set->WriteSampler(49u, m_refractionSampler);
                m_causticSets.push_back(std::move(set));
            }
        }
        if (m_volumetricAccumulateSets.empty())
        {
            RHI::TextureViewDescription sampledMotion{};
            sampledMotion.type = RHI::TextureViewType::Sampled;
            const auto sceneMotionView = device.CreateTextureView(
                compositeBindings->sceneMotion, sampledMotion);
            m_volumetricAccumulateSets.reserve(m_constantBuffers.size());
            for (auto& sets : m_volumetricTemporalSets)
                sets.reserve(m_constantBuffers.size());
            for (auto& sets : m_volumetricReconstructSets)
                sets.reserve(m_constantBuffers.size());
            for (std::size_t frameIndex = 0u;
                 frameIndex < m_constantBuffers.size(); ++frameIndex)
            {
                auto accumulate = device.CreateDescriptorSet(
                    m_volumetricAccumulateLayout);
                accumulate->WriteBuffer(0u,
                    (*compositeBindings->frameConstants)[frameIndex]);
                accumulate->WriteBuffer(5u, m_constantBuffers[frameIndex]);
                accumulate->WriteTextureView(16u, opaqueDepthView);
                accumulate->WriteTextureView(
                    17u, m_compositeDepthSampledView);
                accumulate->WriteTextureView(18u, m_gbufferSampledViews[2]);
                accumulate->WriteTextureView(19u, m_causticSampledViews[0]);
                accumulate->WriteTextureView(20u, m_causticSampledViews[1]);
                accumulate->WriteTextureView(
                    21u, compositeBindings->shadowMap);
                accumulate->WriteTextureView(
                    32u, m_volumetricStorageViews[0]);
                accumulate->WriteSampler(48u, m_refractionSampler);
                accumulate->WriteSampler(49u, m_compositeShadowSampler);
                m_volumetricAccumulateSets.push_back(std::move(accumulate));

                for (std::uint32_t readIndex = 0u; readIndex < 2u;
                     ++readIndex)
                {
                    const std::uint32_t writeIndex = 1u - readIndex;
                    auto temporal = device.CreateDescriptorSet(
                        m_volumetricTemporalLayout);
                    temporal->WriteBuffer(5u, m_constantBuffers[frameIndex]);
                    temporal->WriteTextureView(
                        23u, m_volumetricSampledViews[0]);
                    temporal->WriteTextureView(
                        24u, m_volumetricSampledViews[1u + readIndex]);
                    temporal->WriteTextureView(25u, sceneMotionView);
                    temporal->WriteTextureView(18u, m_gbufferSampledViews[2]);
                    temporal->WriteTextureView(28u, m_motionSampledView);
                    temporal->WriteTextureView(
                        34u, m_volumetricStorageViews[1u + writeIndex]);
                    temporal->WriteSampler(48u, m_refractionSampler);
                    m_volumetricTemporalSets[readIndex].push_back(
                        std::move(temporal));

                    auto reconstruct = device.CreateDescriptorSet(
                        m_volumetricReconstructLayout);
                    reconstruct->WriteBuffer(0u,
                        (*compositeBindings->frameConstants)[frameIndex]);
                    reconstruct->WriteBuffer(
                        5u, m_constantBuffers[frameIndex]);
                    reconstruct->WriteTextureView(
                        26u, m_volumetricSampledViews[1u + writeIndex]);
                    reconstruct->WriteTextureView(
                        27u, m_compositeDepthSampledView);
                    reconstruct->WriteTextureView(
                        35u, m_volumetricStorageViews[3]);
                    m_volumetricReconstructSets[readIndex].push_back(
                        std::move(reconstruct));
                }
            }
        }
        if (m_compositeSets.empty())
        {
            m_compositeSets.reserve(m_constantBuffers.size());
            for (std::size_t frameIndex = 0u;
                 frameIndex < m_constantBuffers.size(); ++frameIndex)
            {
                Core::Check(
                    (*compositeBindings->frameConstants)[frameIndex]
                        != nullptr,
                    "Water composite frame constants are unavailable.");
                auto set = device.CreateDescriptorSet(m_compositeLayout);
                set->WriteBuffer(0u,
                    (*compositeBindings->frameConstants)[frameIndex]);
                set->WriteBuffer(5u, m_constantBuffers[frameIndex]);
                set->WriteTextureView(21u, sceneColorView);
                set->WriteTextureView(22u, opaqueDepthView);
                set->WriteTextureView(
                    23u, m_compositeDepthSampledView);
                set->WriteTextureView(24u, m_gbufferSampledViews[0]);
                set->WriteTextureView(25u, m_gbufferSampledViews[2]);
                set->WriteTextureView(26u, m_refractionSampledView);
                set->WriteTextureView(27u, m_gbufferSampledViews[1]);
                set->WriteTextureView(
                    28u, compositeBindings->slopeMoments);
                set->WriteTextureView(29u, compositeBindings->shadowMap);
                set->WriteTextureView(
                    30u, compositeBindings->shadowMoments);
                set->WriteTextureView(
                    31u, compositeBindings->atmosphereSkyView);
                set->WriteTextureView(39u, m_causticSampledViews[0]);
                set->WriteTextureView(40u, m_causticSampledViews[1]);
                set->WriteTextureView(
                    41u, m_volumetricSampledViews[3]);
                set->WriteTexture(19u, compositeBindings->environment);
                set->WriteTextureView(36u, m_compositeStorageView);
                set->WriteSampler(50u, m_refractionSampler);
                set->WriteSampler(52u, m_compositeShadowSampler);
                m_compositeSets.push_back(std::move(set));
            }
        }
        const bool motionChanged = m_sceneMotionSource
            != compositeBindings->sceneMotion.get();
        if (m_publishSet == nullptr || motionChanged)
        {
            RHI::TextureViewDescription storage{};
            storage.type = RHI::TextureViewType::Storage;
            m_publishHdrStorageView = device.CreateTextureView(
                sceneColor, storage);
            m_publishMotionStorageView = device.CreateTextureView(
                compositeBindings->sceneMotion, storage);
            m_publishSet = device.CreateDescriptorSet(m_publishLayout);
            m_publishSet->WriteTextureView(
                25u, m_gbufferSampledViews[2]);
            m_publishSet->WriteTextureView(
                21u, m_compositeSampledView);
            m_publishSet->WriteTextureView(
                24u, m_motionSampledView);
            m_publishSet->WriteTextureView(
                37u, m_publishHdrStorageView);
            m_publishSet->WriteTextureView(
                38u, m_publishMotionStorageView);
            m_sceneMotionSource = compositeBindings->sceneMotion.get();
        }
    }
    else
    {
        m_causticSets.clear();
        m_volumetricAccumulateSets.clear();
        for (auto& sets : m_volumetricTemporalSets)
            sets.clear();
        for (auto& sets : m_volumetricReconstructSets)
            sets.clear();
        m_compositeSets.clear();
        m_publishSet.reset();
        m_publishHdrStorageView.reset();
        m_publishMotionStorageView.reset();
        m_sceneMotionSource = nullptr;
    }
    if (compositeBindings != nullptr && !m_causticSets.empty())
    {
        auto& set = m_causticSets.at(compositeBindings->frameIndex);
        set->WriteTextureView(16u, compositeBindings->spectralGradient);
        set->WriteTextureView(17u, compositeBindings->localGradient);
    }
    if (m_hasPreviousSettings && HasDirtyScope(
            ClassifyWaterOpticsDirtyScopes(m_previousSettings, settings),
            WaterOpticsDirtyScope::History))
        m_history.Reset();
    m_previousSettings = settings;
    m_hasPreviousSettings = true;
    WaterOpticsHistoryKey key{};
    key.width = width;
    key.height = height;
    key.quality = settings.quality;
    key.sceneVersion = m_sceneVersion;
    key.surfaceHistoryVersion = compositeBindings != nullptr
        ? compositeBindings->surfaceHistoryVersion : 0u;
    key.cameraCutVersion = compositeBindings != nullptr
        ? compositeBindings->cameraCutVersion : 0u;
    key.explicitResetSerial = settings.historyResetSerial;
    [[maybe_unused]] const bool historyInvalidated = m_history.Update(key);
    WaterVolumetricHistoryKey volumetricKey{};
    volumetricKey.width = m_volumetricWidth;
    volumetricKey.height = m_volumetricHeight;
    volumetricKey.quality = settings.quality;
    volumetricKey.opticalHistoryVersion = m_history.GetVersion();
    volumetricKey.surfaceHistoryVersion = key.surfaceHistoryVersion;
    volumetricKey.mediumVersion = m_mediumResult.version;
    volumetricKey.sceneVersion = m_sceneVersion;
    (void)m_volumetricHistory.Update(volumetricKey);
    UpdateConstants(settings, frameSerial, compositeBindings);
    if (compositeBindings != nullptr)
    {
        m_coverage.Prepare(device, compositeBindings->frameIndex, width, height, m_gbufferSampledViews[2]);
        m_statistics.waterPixelCoverage = m_coverage.Coverage();
        m_statistics.waterCoverageAvailable = m_coverage.Available();
    }
    m_statistics.historyVersion = m_history.GetVersion();
    m_statistics.historyValid = m_history.IsValid();
    m_statistics.resourcesReady = true;
}

void WaterOpticsFeature::RetireCurrentResources(
    const std::uint64_t frameSerial)
{
    if (m_pool == nullptr)
        return;
    // SceneRenderer advances frameSerial only after BeginFrame has reclaimed
    // the current frame slot. Waiting one complete frame-ring therefore
    // represents completed slot fences, rather than elapsed CPU frames.
    m_coverage.Retire(frameSerial);
    RetiredResources retired{};
    retired.retireAfterFrame = frameSerial + m_framesInFlight;
    retired.pool = std::move(m_pool);
    retired.gbuffer = std::move(m_gbuffer);
    retired.gbufferViews = std::move(m_gbufferViews);
    retired.gbufferSampledViews = std::move(m_gbufferSampledViews);
    retired.motion = std::move(m_motion);
    retired.motionView = std::move(m_motionView);
    retired.motionSampledView = std::move(m_motionSampledView);
    retired.compositeDepth = std::move(m_compositeDepth);
    retired.compositeDepthView = std::move(m_compositeDepthView);
    retired.compositeDepthSampledView =
        std::move(m_compositeDepthSampledView);
    retired.depthCopySet = std::move(m_depthCopySet);
    retired.refraction = std::move(m_refraction);
    retired.refractionSampledView = std::move(m_refractionSampledView);
    retired.refractionStorageView = std::move(m_refractionStorageView);
    retired.refractionSets = std::move(m_refractionSets);
    retired.caustics = std::move(m_caustics);
    retired.causticSampledViews = std::move(m_causticSampledViews);
    retired.causticStorageViews = std::move(m_causticStorageViews);
    retired.causticSets = std::move(m_causticSets);
    retired.volumetrics = std::move(m_volumetrics);
    retired.volumetricSampledViews = std::move(m_volumetricSampledViews);
    retired.volumetricStorageViews = std::move(m_volumetricStorageViews);
    retired.volumetricAccumulateSets =
        std::move(m_volumetricAccumulateSets);
    retired.volumetricTemporalSets = std::move(m_volumetricTemporalSets);
    retired.volumetricReconstructSets =
        std::move(m_volumetricReconstructSets);
    retired.composite = std::move(m_composite);
    retired.compositeSampledView = std::move(m_compositeSampledView);
    retired.compositeStorageView = std::move(m_compositeStorageView);
    retired.compositeSets = std::move(m_compositeSets);
    retired.publishSet = std::move(m_publishSet);
    retired.publishHdrStorageView =
        std::move(m_publishHdrStorageView);
    retired.publishMotionStorageView =
        std::move(m_publishMotionStorageView);
    m_retired.push_back(std::move(retired));
    m_opaqueDepthSource = nullptr;
    m_sceneColorSource = nullptr;
    m_sceneMotionSource = nullptr;
    m_width = 0u;
    m_height = 0u;
    m_refractionWidth = 0u;
    m_refractionHeight = 0u;
    m_volumetricWidth = 0u;
    m_volumetricHeight = 0u;
    m_volumetricHistory.Reset();
    m_activeVolumetricResolutionScale = 0.0f;
    m_activeCausticResolution = 0u;
    m_statistics.resourcesReady = false;
    m_statistics.retiredResourceSets =
        static_cast<std::uint32_t>(m_retired.size());
}

void WaterOpticsFeature::CollectRetiredResources(
    const std::uint64_t frameSerial)
{
    m_coverage.Collect(frameSerial);
    std::erase_if(m_retired,
        [frameSerial](const RetiredResources& resources)
        {
            return resources.retireAfterFrame <= frameSerial;
        });
    m_statistics.retiredResourceSets =
        static_cast<std::uint32_t>(m_retired.size());
}

void WaterOpticsFeature::ReleaseSizeDependentResources(
    const std::uint64_t frameSerial,
    const bool immediate)
{
    if (immediate)
    {
        m_coverage.Retire(frameSerial, true);
        m_pool.reset();
        m_gbuffer = {};
        m_gbufferViews = {};
        m_gbufferSampledViews = {};
        m_motion.reset();
        m_motionView.reset();
        m_motionSampledView.reset();
        m_compositeDepth.reset();
        m_compositeDepthView.reset();
        m_compositeDepthSampledView.reset();
        m_depthCopySet.reset();
        m_refraction.reset();
        m_refractionSampledView.reset();
        m_refractionStorageView.reset();
        m_refractionSets.clear();
        m_caustics = {};
        m_causticSampledViews = {};
        m_causticStorageViews = {};
        m_causticSets.clear();
        m_volumetrics = {};
        m_volumetricSampledViews = {};
        m_volumetricStorageViews = {};
        m_volumetricAccumulateSets.clear();
        for (auto& sets : m_volumetricTemporalSets)
            sets.clear();
        for (auto& sets : m_volumetricReconstructSets)
            sets.clear();
        m_composite.reset();
        m_compositeSampledView.reset();
        m_compositeStorageView.reset();
        m_compositeSets.clear();
        m_publishSet.reset();
        m_publishHdrStorageView.reset();
        m_publishMotionStorageView.reset();
        m_retired.clear();
        m_opaqueDepthSource = nullptr;
        m_sceneColorSource = nullptr;
        m_sceneMotionSource = nullptr;
        m_width = 0u;
        m_height = 0u;
        m_refractionWidth = 0u;
        m_refractionHeight = 0u;
        m_volumetricWidth = 0u;
        m_volumetricHeight = 0u;
        m_volumetricHistory.Reset();
        m_activeVolumetricResolutionScale = 0.0f;
        m_activeCausticResolution = 0u;
        m_statistics.resourcesReady = false;
        m_statistics.retiredResourceSets = 0u;
        m_passOptions.compositeEnabled = false;
        return;
    }
    RetireCurrentResources(frameSerial);
}

void WaterOpticsFeature::UpdateConstants(
    const WaterOpticsSettings& settings,
    const std::uint64_t frameSerial,
    const WaterOpticsCompositeBindings* compositeBindings)
{
    const WaterOpticsQualityPolicy qualityPolicy =
        GetWaterOpticsQualityPolicy(settings.quality);
    m_passOptions.refractionEnabled = settings.refraction.enabled;
    m_passOptions.rayMarchEnabled = settings.refraction.enabled
        && settings.refraction.highPrecision
        && qualityPolicy.maximumRayMarchSamples > 0u;
    m_passOptions.effectiveRayMarchSamples = m_passOptions.rayMarchEnabled
        ? std::clamp(settings.refraction.rayMarchSampleCount,
            1u, qualityPolicy.maximumRayMarchSamples)
        : 0u;
    m_passOptions.causticsEnabled = settings.caustics.enabled
        && settings.caustics.mode != WaterCausticsMode::Disabled
        && m_passOptions.compositeEnabled;
    m_passOptions.volumetricsEnabled = settings.volumetrics.enabled
        && m_passOptions.compositeEnabled
        && m_mediumResult.medium == WaterCameraMedium::Underwater;
    m_passOptions.volumetricReadIndex = m_volumetricReadIndex;
    m_statistics.refractionEnabled = m_passOptions.refractionEnabled;
    m_statistics.rayMarchEnabled = m_passOptions.rayMarchEnabled;
    m_statistics.effectiveRayMarchSamples =
        m_passOptions.effectiveRayMarchSamples;
    m_statistics.refractionDispatchCount =
        m_passOptions.refractionEnabled ? 1u : 0u;
    m_statistics.compositeEnabled = m_passOptions.compositeEnabled;
    m_statistics.compositeDispatchCount =
        m_passOptions.compositeEnabled ? 2u : 0u; // Composite + Publish.
    m_statistics.causticsEnabled = m_passOptions.causticsEnabled;
    m_statistics.causticsDispatchCount =
        m_passOptions.causticsEnabled ? 1u : 0u;
    m_statistics.volumetricsEnabled = m_passOptions.volumetricsEnabled;
    m_statistics.volumetricDispatchCount =
        m_passOptions.volumetricsEnabled ? 3u : 0u;
    m_statistics.volumetricHistoryValid = m_volumetricHistory.IsValid();
    m_statistics.cameraUnderwater =
        m_mediumResult.medium == WaterCameraMedium::Underwater;
    m_statistics.mediumUsesMeanSeaLevelFallback =
        m_mediumResult.usedMeanSeaLevelFallback;
    m_statistics.mediumVersion = m_mediumResult.version;
    WaterOpticsGpuConstants constants{};
    constants.absorptionRoughness = {settings.material.absorption.x,
        settings.material.absorption.y, settings.material.absorption.z,
        settings.material.roughness};
    constants.scatteringIor = {settings.material.scattering.x,
        settings.material.scattering.y, settings.material.scattering.z,
        settings.material.indexOfRefraction};
    constants.phaseThinBacklit = {settings.material.phaseG,
        settings.material.thinLayerStrength,
        settings.material.backlitStrength,
        compositeBindings != nullptr && compositeBindings->localWavesEnabled
            ? 1.0f : 0.0f};
    constants.distanceRanges = {settings.distance.nearEndMeters,
        settings.distance.middleEndMeters, settings.distance.farEndMeters,
        settings.distance.transitionFraction};
    constants.refractionParameters0 = {
        settings.refraction.distortionStrength,
        settings.refraction.maximumUvOffset,
        settings.refraction.thicknessOffsetMeters,
        settings.refraction.maximumThicknessMeters};
    const DirectX::XMFLOAT2 localDomainCenter = compositeBindings != nullptr
        ? compositeBindings->localDomainCenter : DirectX::XMFLOAT2{};
    const float localDomainSize = compositeBindings != nullptr
        ? std::max(compositeBindings->localDomainSizeMeters, 1.0f) : 1.0f;
    constants.refractionParameters1 = {
        settings.refraction.rayStepScale,
        localDomainCenter.x, localDomainCenter.y, localDomainSize};
    constants.causticsParameters = {
        settings.caustics.nearCoverageMeters,
        settings.caustics.middleCoverageMeters,
        settings.caustics.intensity,
        settings.caustics.dispersion};
    constants.volumetricParameters0 = {
        settings.volumetrics.resolutionScale,
        settings.volumetrics.maximumDistanceMeters,
        settings.volumetrics.historyWeight,
        settings.volumetrics.depthRejectionMeters};
    constants.volumetricParameters1 = {
        settings.volumetrics.surfaceHysteresisMeters,
        settings.caustics.edgeFadeFraction,
        m_mediumResult.medium == WaterCameraMedium::Underwater ? 1.0f : 0.0f,
        m_volumetricHistory.IsValid() ? 1.0f : 0.0f};
    constants.modes0 = {static_cast<std::uint32_t>(settings.quality),
        static_cast<std::uint32_t>(settings.debugView),
        m_passOptions.rayMarchEnabled
            ? m_passOptions.effectiveRayMarchSamples : 1u,
        m_passOptions.rayMarchEnabled ? 1u : 0u};
    constants.modes1 = {
        settings.refraction.enabled ? 1u : 0u,
        settings.caustics.enabled ? 1u : 0u,
        static_cast<std::uint32_t>(GetEffectiveWaterCausticsMode(settings)),
        settings.caustics.resolution};
    constants.modes2 = {
        m_passOptions.volumetricsEnabled ? 1u : 0u,
        settings.volumetrics.sampleCount,
        settings.volumetrics.atrousIterations,
        static_cast<std::uint32_t>(frameSerial)};
    // Only the slot whose fence has completed may be written. Other slots
    // can still be referenced by previously submitted GPU command lists.
    const std::size_t frameIndex = compositeBindings != nullptr
        ? compositeBindings->frameIndex : frameSerial % m_constantBuffers.size();
    Core::Check(frameIndex < m_constantBuffers.size(),
        "Invalid water optics frame slot.");
    constants.inverseViewProjection = m_inverseViewProjections[frameIndex];
    m_constantBuffers[frameIndex]->Update(&constants, sizeof(constants));
}

void WaterOpticsFeature::SetInverseViewProjection(
    const std::uint32_t frameIndex,
    const DirectX::XMFLOAT4X4& inverseViewProjection)
{
    Core::Check(frameIndex < m_inverseViewProjections.size(),
        "Water optics inverse view-projection uses an invalid frame.");
    m_inverseViewProjections[frameIndex] = inverseViewProjection;
}

void WaterOpticsFeature::EndFrame(
    const std::uint64_t frameSerial,
    const bool active)
{
    CollectRetiredResources(frameSerial);
    if (active && HasResources())
    {
        m_gbufferStates.fill(m_passOptions.compositeEnabled
            ? RHI::ResourceState::ShaderResource
            : RHI::ResourceState::RenderTarget);
        m_motionState = m_passOptions.compositeEnabled
            ? RHI::ResourceState::ShaderResource
            : RHI::ResourceState::RenderTarget;
        m_compositeDepthState = RHI::ResourceState::DepthWrite;
        if (m_passOptions.refractionEnabled)
        {
            m_refractionState = m_passOptions.compositeEnabled
                ? RHI::ResourceState::ShaderResource
                : RHI::ResourceState::UnorderedAccess;
        }
        if (m_passOptions.causticsEnabled || m_passOptions.compositeEnabled)
            m_causticStates.fill(RHI::ResourceState::ShaderResource);
        if (m_passOptions.compositeEnabled)
        {
            m_refractionState = RHI::ResourceState::ShaderResource;
            m_volumetricStates[3] = RHI::ResourceState::ShaderResource;
        }
        if (m_passOptions.volumetricsEnabled)
        {
            m_volumetricStates.fill(RHI::ResourceState::ShaderResource);
            m_volumetricReadIndex = 1u - m_volumetricReadIndex;
            m_volumetricHistory.Commit();
            m_statistics.volumetricHistoryValid = true;
        }
        if (m_passOptions.compositeEnabled)
            m_compositeState = RHI::ResourceState::ShaderResource;
    }
}

void WaterOpticsFeature::UpdateGpuTiming(
    const float milliseconds,
    const bool available) noexcept
{
    m_statistics.refractionMilliseconds =
        available ? std::max(milliseconds, 0.0f) : 0.0f;
    m_statistics.gpuTimingAvailable = available;
}

void WaterOpticsFeature::UpdateCompositeGpuTiming(
    const float milliseconds,
    const bool available) noexcept
{
    m_statistics.compositeMilliseconds =
        available ? std::max(milliseconds, 0.0f) : 0.0f;
    m_statistics.gpuTimingAvailable =
        m_statistics.gpuTimingAvailable || available;
}

void WaterOpticsFeature::UpdateCausticsGpuTiming(
    const float milliseconds,
    const bool available) noexcept
{
    m_statistics.causticsMilliseconds =
        available ? std::max(milliseconds, 0.0f) : 0.0f;
    m_statistics.gpuTimingAvailable =
        m_statistics.gpuTimingAvailable || available;
}

void WaterOpticsFeature::UpdateVolumetricGpuTiming(
    const float accumulateMilliseconds,
    const float temporalMilliseconds,
    const float reconstructMilliseconds,
    const bool available) noexcept
{
    m_statistics.volumetricAccumulateMilliseconds = available
        ? std::max(accumulateMilliseconds, 0.0f) : 0.0f;
    m_statistics.volumetricTemporalMilliseconds = available
        ? std::max(temporalMilliseconds, 0.0f) : 0.0f;
    m_statistics.volumetricReconstructMilliseconds = available
        ? std::max(reconstructMilliseconds, 0.0f) : 0.0f;
    m_statistics.gpuTimingAvailable =
        m_statistics.gpuTimingAvailable || available;
}

WaterMediumResult WaterOpticsFeature::UpdateCameraMedium(
    const WaterMediumSample& sample) noexcept
{
    m_mediumResult = m_mediumState.Update(sample);
    if (m_mediumResult.transitioned)
        m_volumetricHistory.Reset();
    return m_mediumResult;
}

void WaterOpticsFeature::ResetHistory()
{
    m_history.Reset();
    m_volumetricHistory.Reset();
    m_mediumState.Reset(false);
    m_statistics.historyVersion = m_history.GetVersion();
    m_statistics.historyValid = false;
}

void WaterOpticsFeature::NotifySceneChanged()
{
    ++m_sceneVersion;
    m_history.Reset();
    m_volumetricHistory.Reset();
    m_mediumState.Reset(true);
    m_statistics.historyVersion = m_history.GetVersion();
    m_statistics.historyValid = false;
}

void WaterOpticsFeature::UpdateSurfaceDescriptorSet(
    RHI::IDescriptorSet& descriptorSet,
    const std::uint32_t frameIndex) const
{
    Core::Check(frameIndex < m_constantBuffers.size()
            && m_constantBuffers[frameIndex] != nullptr,
        "Water optics descriptor update received an invalid frame.");
    descriptorSet.WriteBuffer(5u, m_constantBuffers[frameIndex]);
}

WaterOpticsGraphContribution
WaterOpticsFeature::CreateRenderGraphContribution(
    WaterOpticsPassCallbacks passCallbacks) const
{
    Core::Check(HasResources(),
        "Water optics graph publication requires active resources.");
    Core::Check(static_cast<bool>(passCallbacks.depthCopy)
            && static_cast<bool>(passCallbacks.visibility)
            && (!m_passOptions.refractionEnabled
                || static_cast<bool>(passCallbacks.refraction))
            && (!m_passOptions.causticsEnabled
                || static_cast<bool>(passCallbacks.caustics))
            && (!m_passOptions.volumetricsEnabled
                || (static_cast<bool>(passCallbacks.volumetricAccumulate)
                    && static_cast<bool>(passCallbacks.volumetricTemporal)
                    && static_cast<bool>(passCallbacks.volumetricReconstruct)))
            && (!m_passOptions.compositeEnabled
                || (static_cast<bool>(passCallbacks.composite)
                    && static_cast<bool>(passCallbacks.publish))),
        "Water optics registration requires its active execution callbacks.");
    WaterOpticsGraphContribution contribution{};
    for (std::uint32_t index = 0u; index < m_gbuffer.size(); ++index)
        contribution.waterGBuffer[index] = m_gbuffer[index].get();
    contribution.waterMotion = m_motion.get();
    contribution.compositeDepth = m_compositeDepth.get();
    contribution.waterGBufferInitialStates = m_gbufferStates;
    contribution.waterMotionInitialState = m_motionState;
    contribution.compositeDepthInitialState = m_compositeDepthState;
    contribution.build =
        [this, passCallbacks = std::move(passCallbacks)](
            RenderGraph& graph,
            const WaterOpticsGraphInputs& inputs)
        {
            Core::Check(m_refraction != nullptr && m_composite != nullptr,
                "Water optics private resources are unavailable.");
            const TextureHandle refraction = graph.ImportTexture(
                "WaterRefraction", *m_refraction, m_refractionState);
            const TextureHandle composite = graph.ImportTexture(
                "WaterCompositeScratch", *m_composite, m_compositeState);
            std::array<TextureHandle, 2> caustics{};
            caustics[0] = graph.ImportTexture(
                "WaterCausticsNear", *m_caustics[0], m_causticStates[0]);
            caustics[1] = graph.ImportTexture(
                "WaterCausticsMiddle", *m_caustics[1], m_causticStates[1]);
            std::array<TextureHandle, 4> volumetrics{};
            constexpr std::array<const char*, 4> VolumetricNames = {
                "WaterVolumetricCurrent", "WaterVolumetricHistoryA",
                "WaterVolumetricHistoryB",
                "WaterVolumetricReconstruction"};
            for (std::uint32_t index = 0u; index < volumetrics.size(); ++index)
            {
                volumetrics[index] = graph.ImportTexture(
                    VolumetricNames[index], *m_volumetrics[index],
                    m_volumetricStates[index]);
            }
            auto result = AddPasses(graph, inputs, refraction, composite, caustics,
                volumetrics, passCallbacks, m_passOptions);
            if (m_passOptions.compositeEnabled)
                m_coverage.AddPasses(graph, result.waterGBuffer[2]);
            return result;
        };
    return contribution;
}

WaterOpticsGraphResult WaterOpticsFeature::AddPasses(
    RenderGraph& graph,
    const WaterOpticsGraphInputs& inputs,
    TextureHandle refraction,
    TextureHandle composite,
    std::array<TextureHandle, 2> caustics,
    std::array<TextureHandle, 4> volumetrics,
    const WaterOpticsPassCallbacks& callbacks,
    const WaterOpticsPassOptions& options)
{
    Core::Check(static_cast<bool>(callbacks.depthCopy)
            && static_cast<bool>(callbacks.visibility)
            && inputs.sceneColor.IsValid()
            && inputs.sceneMotion.IsValid()
            && inputs.opaqueDepth.IsValid()
            && inputs.opaqueHiZ.IsValid()
            && inputs.compositeDepth.IsValid()
            && inputs.oceanDisplacement.IsValid()
            && inputs.oceanNormalFoam.IsValid()
            && inputs.oceanSlopeMoments.IsValid()
            && inputs.oceanFoamHistory.IsValid()
            && inputs.waterMotion.IsValid()
            && (!options.refractionEnabled || refraction.IsValid())
            && (!options.causticsEnabled
                || std::ranges::all_of(caustics,
                    [](const TextureHandle handle)
                    {
                        return handle.IsValid();
                    }))
            && (!options.volumetricsEnabled
                || std::ranges::all_of(volumetrics,
                    [](const TextureHandle handle)
                    {
                        return handle.IsValid();
                    }))
            && (!options.compositeEnabled || composite.IsValid())
            && std::ranges::all_of(inputs.waterGBuffer,
                [](const TextureHandle handle)
                {
                    return handle.IsValid();
                }),
        "Water optics subgraph received incomplete inputs.");

    WaterOpticsGraphResult result{};
    result.sceneColor = inputs.sceneColor;
    result.sceneMotion = inputs.sceneMotion;
    result.compositeDepth = inputs.compositeDepth;
    result.waterGBuffer = inputs.waterGBuffer;
    result.waterMotion = inputs.waterMotion;

    auto depthCopyParameters = graph.CreatePassParameters();
    // HDR and Hi-Z are authoritative ordering inputs for the complete optics
    // subgraph even though the initial depth-copy shader only samples depth.
    depthCopyParameters.ReadTexture(
        inputs.sceneColor, RHI::ResourceState::RenderTarget);
    depthCopyParameters.ReadTexture(
        inputs.opaqueDepth, RHI::ResourceState::ShaderResource);
    depthCopyParameters.ReadTexture(
        inputs.opaqueHiZ, RHI::ResourceState::ShaderResource);
    result.compositeDepth = depthCopyParameters.WriteTexture(
        result.compositeDepth, RHI::ResourceState::DepthWrite);
    graph.AddParameterPass(
        "WaterOptics.DepthCopy",
        std::move(depthCopyParameters),
        callbacks.depthCopy);

    auto visibilityParameters = graph.CreatePassParameters();
    visibilityParameters.ReadBuffer(
        inputs.frameConstants, RHI::ResourceState::ConstantBuffer);
    visibilityParameters.ReadTexture(
        inputs.oceanDisplacement, RHI::ResourceState::ShaderResource);
    visibilityParameters.ReadTexture(
        inputs.oceanNormalFoam, RHI::ResourceState::ShaderResource);
    visibilityParameters.ReadTexture(
        inputs.oceanSlopeMoments, RHI::ResourceState::ShaderResource);
    visibilityParameters.ReadTexture(
        inputs.oceanFoamHistory, RHI::ResourceState::ShaderResource);
    if (inputs.localWaveEnabled)
    {
        Core::Check(inputs.localWaveDisplacement.IsValid()
                && inputs.localWaveGradient.IsValid(),
            "Water optics local-wave inputs are incomplete.");
        visibilityParameters.ReadTexture(inputs.localWaveDisplacement,
            RHI::ResourceState::ShaderResource);
        visibilityParameters.ReadTexture(inputs.localWaveGradient,
            RHI::ResourceState::ShaderResource);
    }
    if (inputs.clusteredLightingEnabled)
    {
        visibilityParameters.ReadBuffer(
            inputs.clusterConstants, RHI::ResourceState::ConstantBuffer);
        visibilityParameters.ReadBuffer(
            inputs.pointLights, RHI::ResourceState::ShaderResource);
        visibilityParameters.ReadBuffer(inputs.clusterLightCounts,
            RHI::ResourceState::ShaderResource);
        visibilityParameters.ReadBuffer(inputs.clusterLightIndices,
            RHI::ResourceState::ShaderResource);
    }
    if (inputs.shadowsEnabled)
    {
        visibilityParameters.ReadTexture(
            inputs.shadowMap, RHI::ResourceState::ShaderResource);
    }
    if (inputs.varianceShadowsEnabled)
    {
        visibilityParameters.ReadTexture(
            inputs.shadowMoments, RHI::ResourceState::ShaderResource);
    }
    if (inputs.atmosphereEnabled)
    {
        visibilityParameters.ReadTexture(
            inputs.atmosphereSkyView, RHI::ResourceState::ShaderResource);
    }
    visibilityParameters.ReadTexture(
        result.compositeDepth, RHI::ResourceState::DepthWrite);
    for (TextureHandle& waterGBuffer : result.waterGBuffer)
    {
        waterGBuffer = visibilityParameters.WriteTexture(
            waterGBuffer, RHI::ResourceState::RenderTarget);
    }
    result.waterMotion = visibilityParameters.WriteTexture(
        result.waterMotion, RHI::ResourceState::RenderTarget);
    result.compositeDepth = visibilityParameters.WriteTexture(
        result.compositeDepth, RHI::ResourceState::DepthWrite);
    graph.AddParameterPass(
        "WaterOptics.Visibility",
        std::move(visibilityParameters),
        callbacks.visibility);
    if (options.causticsEnabled)
    {
        Core::Check(static_cast<bool>(callbacks.caustics),
            "Water caustics require an execution callback.");
        auto causticParameters = graph.CreatePassParameters();
        causticParameters.ReadBuffer(
            inputs.frameConstants, RHI::ResourceState::ConstantBuffer);
        causticParameters.ReadTexture(
            inputs.oceanNormalFoam, RHI::ResourceState::ShaderResource);
        if (inputs.localWaveEnabled)
        {
            causticParameters.ReadTexture(
                inputs.localWaveGradient, RHI::ResourceState::ShaderResource);
        }
        for (TextureHandle& caustic : caustics)
        {
            caustic = causticParameters.WriteTexture(
                caustic, RHI::ResourceState::UnorderedAccess);
        }
        RenderGraph::PassOptions causticOptions{};
        causticOptions.queue = RenderGraph::QueueClass::Graphics;
        causticOptions.sideEffect = true;
        causticOptions.allowCulling = false;
        graph.AddParameterPass("WaterOptics.Caustics.Accumulate",
            std::move(causticParameters), callbacks.caustics,
            causticOptions);
    }
    if (options.refractionEnabled)
    {
        Core::Check(static_cast<bool>(callbacks.refraction),
            "Water optics refraction requires an execution callback.");
        auto refractionParameters = graph.CreatePassParameters();
        refractionParameters.ReadBuffer(inputs.frameConstants, RHI::ResourceState::ConstantBuffer);
        refractionParameters.ReadTexture(
            inputs.sceneColor, RHI::ResourceState::ShaderResource);
        refractionParameters.ReadTexture(
            inputs.opaqueDepth, RHI::ResourceState::ShaderResource);
        refractionParameters.ReadTexture(
            result.compositeDepth, RHI::ResourceState::ShaderResource);
        refractionParameters.ReadTexture(
            result.waterGBuffer[0], RHI::ResourceState::ShaderResource);
        refractionParameters.ReadTexture(
            result.waterGBuffer[2], RHI::ResourceState::ShaderResource);
        refraction = refractionParameters.WriteTexture(
            refraction, RHI::ResourceState::UnorderedAccess);
        RenderGraph::PassOptions passOptions{};
        // The composite currently executes on the graphics queue as well, so
        // keeping refraction here preserves deterministic capture ordering.
        passOptions.queue = RenderGraph::QueueClass::Graphics;
        passOptions.sideEffect = true;
        passOptions.allowCulling = false;
        graph.AddParameterPass(
            options.rayMarchEnabled
                ? "WaterOptics.Refraction.RayMarch"
                : "WaterOptics.Refraction.Approximate",
            std::move(refractionParameters),
            callbacks.refraction,
            passOptions);
    }
    if (options.volumetricsEnabled)
    {
        Core::Check(static_cast<bool>(callbacks.volumetricAccumulate)
                && static_cast<bool>(callbacks.volumetricTemporal)
                && static_cast<bool>(callbacks.volumetricReconstruct),
            "Water volumetrics require all execution callbacks.");
        const std::uint32_t readIndex =
            std::min(options.volumetricReadIndex, 1u);
        const std::uint32_t writeIndex = 1u - readIndex;

        auto accumulateParameters = graph.CreatePassParameters();
        accumulateParameters.ReadBuffer(
            inputs.frameConstants, RHI::ResourceState::ConstantBuffer);
        accumulateParameters.ReadTexture(
            inputs.opaqueDepth, RHI::ResourceState::ShaderResource);
        accumulateParameters.ReadTexture(
            result.compositeDepth, RHI::ResourceState::ShaderResource);
        accumulateParameters.ReadTexture(
            result.waterGBuffer[2], RHI::ResourceState::ShaderResource);
        if (caustics[0].IsValid() && caustics[1].IsValid())
        {
            for (const TextureHandle caustic : caustics)
            {
                accumulateParameters.ReadTexture(
                    caustic, RHI::ResourceState::ShaderResource);
            }
        }
        if (inputs.shadowsEnabled)
        {
            accumulateParameters.ReadTexture(
                inputs.shadowMap, RHI::ResourceState::ShaderResource);
        }
        volumetrics[0] = accumulateParameters.WriteTexture(
            volumetrics[0], RHI::ResourceState::UnorderedAccess);
        graph.AddParameterPass("WaterOptics.Volume.Accumulate",
            std::move(accumulateParameters), callbacks.volumetricAccumulate,
            RenderGraph::PassOptions{
                RenderGraph::QueueClass::Graphics, true, false, true,
                RenderGraph::ParallelRecordingContract::AuditedIndependent()});

        auto temporalParameters = graph.CreatePassParameters();
        temporalParameters.ReadTexture(
            volumetrics[0], RHI::ResourceState::ShaderResource);
        temporalParameters.ReadTexture(
            volumetrics[1u + readIndex], RHI::ResourceState::ShaderResource);
        temporalParameters.ReadTexture(
            inputs.sceneMotion, RHI::ResourceState::ShaderResource);
        temporalParameters.ReadTexture(
            result.waterGBuffer[2], RHI::ResourceState::ShaderResource);
        temporalParameters.ReadTexture(
            result.waterMotion, RHI::ResourceState::ShaderResource);
        volumetrics[1u + writeIndex] = temporalParameters.WriteTexture(
            volumetrics[1u + writeIndex], RHI::ResourceState::UnorderedAccess);
        graph.AddParameterPass("WaterOptics.Volume.Temporal",
            std::move(temporalParameters), callbacks.volumetricTemporal,
            RenderGraph::PassOptions{
                RenderGraph::QueueClass::Graphics, true, false, true,
                RenderGraph::ParallelRecordingContract::AuditedIndependent()});

        auto reconstructParameters = graph.CreatePassParameters();
        reconstructParameters.ReadBuffer(
            inputs.frameConstants, RHI::ResourceState::ConstantBuffer);
        reconstructParameters.ReadTexture(
            volumetrics[1u + writeIndex], RHI::ResourceState::ShaderResource);
        reconstructParameters.ReadTexture(
            result.compositeDepth, RHI::ResourceState::ShaderResource);
        volumetrics[3] = reconstructParameters.WriteTexture(
            volumetrics[3], RHI::ResourceState::UnorderedAccess);
        graph.AddParameterPass("WaterOptics.Volume.Reconstruct",
            std::move(reconstructParameters),
            callbacks.volumetricReconstruct,
            RenderGraph::PassOptions{
                RenderGraph::QueueClass::Graphics, true, false, true,
                RenderGraph::ParallelRecordingContract::AuditedIndependent()});
    }
    if (options.compositeEnabled)
    {
        Core::Check(static_cast<bool>(callbacks.composite),
            "Water optics composite requires an execution callback.");
        auto compositeParameters = graph.CreatePassParameters();
        if (inputs.environment.IsValid())
            compositeParameters.ReadTexture(inputs.environment, RHI::ResourceState::ShaderResource);
        compositeParameters.ReadBuffer(
            inputs.frameConstants, RHI::ResourceState::ConstantBuffer);
        compositeParameters.ReadTexture(
            inputs.sceneColor, RHI::ResourceState::ShaderResource);
        compositeParameters.ReadTexture(
            inputs.opaqueDepth, RHI::ResourceState::ShaderResource);
        if (refraction.IsValid())
        {
            compositeParameters.ReadTexture(
                refraction, RHI::ResourceState::ShaderResource);
        }
        compositeParameters.ReadTexture(
            result.compositeDepth, RHI::ResourceState::ShaderResource);
        for (const TextureHandle waterGBuffer : result.waterGBuffer)
        {
            compositeParameters.ReadTexture(
                waterGBuffer, RHI::ResourceState::ShaderResource);
        }
        compositeParameters.ReadTexture(
            inputs.oceanSlopeMoments, RHI::ResourceState::ShaderResource);
        if (caustics[0].IsValid() && caustics[1].IsValid())
        {
            for (const TextureHandle caustic : caustics)
            {
                compositeParameters.ReadTexture(
                    caustic, RHI::ResourceState::ShaderResource);
            }
        }
        if (volumetrics[3].IsValid())
        {
            compositeParameters.ReadTexture(
                volumetrics[3], RHI::ResourceState::ShaderResource);
        }
        if (inputs.shadowsEnabled)
        {
            compositeParameters.ReadTexture(
                inputs.shadowMap, RHI::ResourceState::ShaderResource);
        }
        if (inputs.varianceShadowsEnabled)
        {
            compositeParameters.ReadTexture(
                inputs.shadowMoments, RHI::ResourceState::ShaderResource);
        }
        if (inputs.atmosphereEnabled)
        {
            compositeParameters.ReadTexture(
                inputs.atmosphereSkyView,
                RHI::ResourceState::ShaderResource);
        }
        composite = compositeParameters.WriteTexture(
            composite, RHI::ResourceState::UnorderedAccess);
        RenderGraph::PassOptions passOptions{};
        passOptions.queue = RenderGraph::QueueClass::Graphics;
        passOptions.sideEffect = true;
        passOptions.allowCulling = false;
        graph.AddParameterPass(
            "WaterOptics.Composite",
            std::move(compositeParameters),
            callbacks.composite,
            passOptions);

        Core::Check(static_cast<bool>(callbacks.publish),
            "Water optics composite publication requires an execution callback.");
        auto publishParameters = graph.CreatePassParameters();
        publishParameters.ReadTexture(
            composite, RHI::ResourceState::ShaderResource);
        publishParameters.ReadTexture(
            result.waterMotion, RHI::ResourceState::ShaderResource);
        publishParameters.ReadTexture(
            result.waterGBuffer[2], RHI::ResourceState::ShaderResource);
        result.sceneColor = publishParameters.WriteTexture(
            result.sceneColor, RHI::ResourceState::UnorderedAccess);
        result.sceneMotion = publishParameters.WriteTexture(
            result.sceneMotion, RHI::ResourceState::UnorderedAccess);
        graph.AddParameterPass(
            "WaterOptics.Publish",
            std::move(publishParameters),
            callbacks.publish,
            RenderGraph::PassOptions{
                RenderGraph::QueueClass::Graphics,
                true,
                false,
                true,
                RenderGraph::ParallelRecordingContract::AuditedIndependent()});
    }
    return result;
}

void WaterOpticsFeature::ExecuteDepthCopy(
    RHI::ICommandContext& commandContext,
    const std::uint32_t width,
    const std::uint32_t height) const
{
    Core::Check(m_depthCopyPipeline != nullptr
            && m_depthCopySet != nullptr
            && m_compositeDepthView != nullptr,
        "Water depth copy resources are unavailable.");
    RHI::RenderingInfo info{};
    info.width = width;
    info.height = height;
    RHI::RenderingAttachment depth{};
    depth.view = m_compositeDepthView.get();
    depth.loadOperation = RHI::LoadOperation::Clear;
    depth.storeOperation = RHI::StoreOperation::Store;
    depth.clearDepthStencil.depth = 1.0f;
    depth.stateBefore = RHI::ResourceState::DepthWrite;
    depth.stateAfter = RHI::ResourceState::DepthWrite;
    info.depthAttachment = depth;
    commandContext.BeginRendering(info);
    commandContext.BindGraphicsPipeline(*m_depthCopyPipeline);
    commandContext.BindDescriptorSet(*m_depthCopySet);
    commandContext.Draw(3u);
    commandContext.EndRendering();
}

void WaterOpticsFeature::ExecuteRefraction(
    RHI::ICommandContext& commandContext,
    const std::uint32_t /*width*/,
    const std::uint32_t /*height*/,
    const std::uint32_t frameIndex) const
{
    Core::Check(m_refractionPipeline != nullptr
            && frameIndex < m_refractionSets.size()
            && m_refractionSets[frameIndex] != nullptr,
        "Water refraction resources are unavailable.");
    commandContext.BindComputePipeline(*m_refractionPipeline);
    commandContext.BindDescriptorSet(*m_refractionSets[frameIndex]);
    commandContext.Dispatch(
        (m_refractionWidth + 7u) / 8u,
        (m_refractionHeight + 7u) / 8u,
        1u);
}

void WaterOpticsFeature::ExecuteComposite(
    RHI::ICommandContext& commandContext,
    const std::uint32_t width,
    const std::uint32_t height,
    const std::uint32_t frameIndex) const
{
    Core::Check(m_compositePipeline != nullptr
            && frameIndex < m_compositeSets.size()
            && m_compositeSets[frameIndex] != nullptr,
        "Water composite resources are unavailable.");
    commandContext.BindComputePipeline(*m_compositePipeline);
    commandContext.BindDescriptorSet(*m_compositeSets[frameIndex]);
    commandContext.Dispatch(
        (width + 7u) / 8u,
        (height + 7u) / 8u,
        1u);
}

void WaterOpticsFeature::ExecuteCaustics(
    RHI::ICommandContext& commandContext,
    const std::uint32_t frameIndex) const
{
    Core::Check(m_causticPipeline != nullptr
            && frameIndex < m_causticSets.size()
            && m_causticSets[frameIndex] != nullptr,
        "Water caustic resources are unavailable.");
    commandContext.BindComputePipeline(*m_causticPipeline);
    commandContext.BindDescriptorSet(*m_causticSets[frameIndex]);
    commandContext.Dispatch(
        (m_activeCausticResolution + 7u) / 8u,
        (m_activeCausticResolution + 7u) / 8u,
        1u);
}

void WaterOpticsFeature::ExecuteVolumetricAccumulate(
    RHI::ICommandContext& commandContext,
    const std::uint32_t frameIndex) const
{
    Core::Check(m_volumetricAccumulatePipeline != nullptr
            && frameIndex < m_volumetricAccumulateSets.size(),
        "Water volumetric accumulation resources are unavailable.");
    commandContext.BindComputePipeline(*m_volumetricAccumulatePipeline);
    commandContext.BindDescriptorSet(
        *m_volumetricAccumulateSets[frameIndex]);
    commandContext.Dispatch((m_volumetricWidth + 7u) / 8u,
        (m_volumetricHeight + 7u) / 8u, 1u);
}

void WaterOpticsFeature::ExecuteVolumetricTemporal(
    RHI::ICommandContext& commandContext,
    const std::uint32_t frameIndex) const
{
    const std::uint32_t readIndex = std::min(m_volumetricReadIndex, 1u);
    Core::Check(m_volumetricTemporalPipeline != nullptr
            && frameIndex < m_volumetricTemporalSets[readIndex].size(),
        "Water volumetric temporal resources are unavailable.");
    commandContext.BindComputePipeline(*m_volumetricTemporalPipeline);
    commandContext.BindDescriptorSet(
        *m_volumetricTemporalSets[readIndex][frameIndex]);
    commandContext.Dispatch((m_volumetricWidth + 7u) / 8u,
        (m_volumetricHeight + 7u) / 8u, 1u);
}

void WaterOpticsFeature::ExecuteVolumetricReconstruct(
    RHI::ICommandContext& commandContext,
    const std::uint32_t frameIndex) const
{
    const std::uint32_t readIndex = std::min(m_volumetricReadIndex, 1u);
    Core::Check(m_volumetricReconstructPipeline != nullptr
            && frameIndex < m_volumetricReconstructSets[readIndex].size(),
        "Water volumetric reconstruction resources are unavailable.");
    commandContext.BindComputePipeline(*m_volumetricReconstructPipeline);
    commandContext.BindDescriptorSet(
        *m_volumetricReconstructSets[readIndex][frameIndex]);
    commandContext.Dispatch((m_width + 7u) / 8u,
        (m_height + 7u) / 8u, 1u);
}

void WaterOpticsFeature::ExecutePublish(
    RHI::ICommandContext& commandContext,
    const std::uint32_t width,
    const std::uint32_t height) const
{
    Core::Check(m_publishPipeline != nullptr
            && m_publishSet != nullptr,
        "Water composite publication resources are unavailable.");
    commandContext.BindComputePipeline(*m_publishPipeline);
    commandContext.BindDescriptorSet(*m_publishSet);
    commandContext.Dispatch(
        (width + 7u) / 8u,
        (height + 7u) / 8u,
        1u);
}

const RHI::IGraphicsPipeline&
WaterOpticsFeature::VisibilityPipeline() const
{
    Core::Check(m_visibilityPipeline != nullptr,
        "Water visibility pipeline is unavailable.");
    return *m_visibilityPipeline;
}

const RHI::IGraphicsPipeline&
WaterOpticsFeature::VisibilityTessellationPipeline() const
{
    Core::Check(m_visibilityTessellationPipeline != nullptr,
        "Water tessellation visibility pipeline is unavailable.");
    return *m_visibilityTessellationPipeline;
}

bool WaterOpticsFeature::HasVisibilityTessellationPipeline() const noexcept
{
    return m_visibilityTessellationPipeline != nullptr;
}

const RHI::ITextureView& WaterOpticsFeature::CompositeDepthView() const
{
    Core::Check(m_compositeDepthView != nullptr,
        "Water composite depth view is unavailable.");
    return *m_compositeDepthView;
}

const std::array<std::shared_ptr<RHI::ITextureView>, 3>&
WaterOpticsFeature::GBufferRenderTargetViews() const noexcept
{
    return m_gbufferViews;
}

const RHI::ITextureView&
WaterOpticsFeature::MotionRenderTargetView() const
{
    Core::Check(m_motionView != nullptr,
        "Water motion target is unavailable.");
    return *m_motionView;
}

const std::shared_ptr<RHI::ITextureView>&
WaterOpticsFeature::GetCaptureView(
    const WaterOpticsCaptureStage stage) const
{
    Core::Check(HasResources(),
        "Water optics capture requires active resources.");
    switch (stage)
    {
    case WaterOpticsCaptureStage::Depth:
        return m_compositeDepthSampledView;
    case WaterOpticsCaptureStage::Mask:
    case WaterOpticsCaptureStage::GBuffer2:
        return m_gbufferSampledViews[2];
    case WaterOpticsCaptureStage::GBuffer0:
        return m_gbufferSampledViews[0];
    case WaterOpticsCaptureStage::GBuffer1:
        return m_gbufferSampledViews[1];
    case WaterOpticsCaptureStage::Refraction:
        return m_refractionSampledView;
    case WaterOpticsCaptureStage::CausticsNear:
        return m_causticSampledViews[0];
    case WaterOpticsCaptureStage::CausticsMiddle:
        return m_causticSampledViews[1];
    case WaterOpticsCaptureStage::VolumetricCurrent:
        return m_volumetricSampledViews[0];
    case WaterOpticsCaptureStage::VolumetricHistory:
        return m_volumetricSampledViews[1u + m_volumetricReadIndex];
    case WaterOpticsCaptureStage::VolumetricReconstruction:
        return m_volumetricSampledViews[3];
    case WaterOpticsCaptureStage::Composite:
        return m_compositeSampledView;
    }
    Core::Check(false, "Unknown water optics capture stage.");
    return m_gbufferSampledViews[0];
}

bool WaterOpticsFeature::IsInitialized() const noexcept
{
    return m_statistics.initialized;
}

bool WaterOpticsFeature::HasResources() const noexcept
{
    return m_pool != nullptr && m_compositeDepthView != nullptr
        && m_refractionSampledView != nullptr
        && m_compositeSampledView != nullptr;
}

const WaterOpticsFeatureStatistics&
WaterOpticsFeature::GetStatistics() const noexcept
{
    return m_statistics;
}
} // namespace Prism::Renderer
