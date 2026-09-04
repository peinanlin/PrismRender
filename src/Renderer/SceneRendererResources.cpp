#include "Renderer/SceneRenderer.h"

#include "Renderer/DeferredTransientLayout.h"
#include "Asset/Texture.h"
#include "RHI/IFrameContext.h"
#include "RHI/IGraphicsDevice.h"
#include "RHI/IRenderBackend.h"

#include <string>
#include <string_view>

namespace Prism::Renderer
{
namespace
{
struct RenderExtent
{
    std::uint32_t width = 0;
    std::uint32_t height = 0;
};
}

void SceneRenderer::CreateShadowResources(
    RHI::IGraphicsDevice& device)
{
    RHI::TextureDescription textureDescription{};
    textureDescription.width = ShadowMapResolution;
    textureDescription.height = ShadowMapResolution;
    textureDescription.arrayLayers = ShadowCascadeCount;
    textureDescription.format = RHI::Format::D32Float;
    textureDescription.usage =
        RHI::TextureUsage::DepthStencil
        | RHI::TextureUsage::ShaderResource;
    m_shadowTexture = device.CreateTexture(textureDescription);

    for (std::uint32_t cascadeIndex = 0;
         cascadeIndex < ShadowCascadeCount;
         ++cascadeIndex)
    {
        RHI::TextureViewDescription depthViewDescription{};
        depthViewDescription.type = RHI::TextureViewType::DepthStencil;
        depthViewDescription.baseArrayLayer = cascadeIndex;
        depthViewDescription.arrayLayerCount = 1;
        m_shadowDepthViews[cascadeIndex] = device.CreateTextureView(
            m_shadowTexture,
            depthViewDescription);
    }

    RHI::TextureViewDescription sampledViewDescription{};
    sampledViewDescription.type = RHI::TextureViewType::Sampled;
    sampledViewDescription.arrayLayerCount = ShadowCascadeCount;
    m_shadowSampledView = device.CreateTextureView(
        m_shadowTexture,
        sampledViewDescription);
    m_shadowInitialized = false;
}

void SceneRenderer::CreateSizeDependentResources(
    RHI::IRenderBackend& backend)
{
    RHI::IGraphicsDevice& device = backend.GetGraphicsDevice();
    RHI::IFrameContext& frame = backend.GetFrameContext();
    const RenderExtent extent{
        frame.GetFrameWidth(),
        frame.GetFrameHeight()};
    m_finalOutputWidth = extent.width;
    m_finalOutputHeight = extent.height;
    m_transientTexturePool = device.CreateTransientTexturePool(
        BuildDeferredTransientTextureRequests(
            extent.width,
            extent.height));

    RHI::TextureViewDescription renderTargetDescription{};
    renderTargetDescription.type = RHI::TextureViewType::RenderTarget;
    RHI::TextureViewDescription sampledDescription{};
    sampledDescription.type = RHI::TextureViewType::Sampled;
    const auto createTarget =
        [&](std::shared_ptr<RHI::ITexture>& texture,
            std::shared_ptr<RHI::ITextureView>& renderTargetView,
            std::shared_ptr<RHI::ITextureView>& sampledView,
            const std::string_view resourceName)
        {
            texture = m_transientTexturePool->GetTexture(resourceName);
            renderTargetView = device.CreateTextureView(
                texture,
                renderTargetDescription);
            sampledView = device.CreateTextureView(
                texture,
                sampledDescription);
        };

    for (std::uint32_t index = 0; index < GBufferCount; ++index)
    {
        createTarget(
            m_gbufferTextures[index],
            m_gbufferRenderTargetViews[index],
            m_gbufferSampledViews[index],
            "GBuffer" + std::to_string(index));
    }
    createTarget(
        m_hdrTexture,
        m_hdrRenderTargetView,
        m_hdrSampledView,
        "HdrColor");
    createTarget(
        m_bloomTextureA,
        m_bloomRenderTargetViewA,
        m_bloomSampledViewA,
        "BloomA");
    createTarget(
        m_bloomTextureB,
        m_bloomRenderTargetViewB,
        m_bloomSampledViewB,
        "BloomB");

    RHI::TextureViewDescription storageDescription{};
    storageDescription.type = RHI::TextureViewType::Storage;
    m_bloomStorageViewA = device.CreateTextureView(
        m_bloomTextureA,
        storageDescription);
    m_bloomStorageViewB = device.CreateTextureView(
        m_bloomTextureB,
        storageDescription);
    std::shared_ptr<RHI::ITexture> depthTexture =
        frame.GetDepthStencilTexture();
    if (!m_renderToSwapChain)
    {
        RHI::TextureDescription depthDescription{};
        depthDescription.width = extent.width;
        depthDescription.height = extent.height;
        depthDescription.format = RHI::Format::D32Float;
        depthDescription.usage =
            RHI::TextureUsage::DepthStencil
            | RHI::TextureUsage::ShaderResource;
        m_depthTexture = device.CreateTexture(depthDescription);
        RHI::TextureViewDescription depthStencilDescription{};
        depthStencilDescription.type =
            RHI::TextureViewType::DepthStencil;
        m_depthStencilView = device.CreateTextureView(
            m_depthTexture,
            depthStencilDescription);
        depthTexture = m_depthTexture;
    }
    RHI::TextureViewDescription depthSampledDescription{};
    depthSampledDescription.type = RHI::TextureViewType::Sampled;
    m_depthSampledView = device.CreateTextureView(
        depthTexture,
        depthSampledDescription);

    if (!m_renderToSwapChain)
    {
        RHI::TextureDescription outputDescription{};
        outputDescription.width = extent.width;
        outputDescription.height = extent.height;
        outputDescription.format = frame.GetBackBufferRhiFormat();
        outputDescription.usage =
            RHI::TextureUsage::RenderTarget
            | RHI::TextureUsage::ShaderResource
            | RHI::TextureUsage::CopySource;
        m_finalOutputTexture = device.CreateTexture(outputDescription);

        RHI::TextureViewDescription outputRtv{};
        outputRtv.type = RHI::TextureViewType::RenderTarget;
        m_finalOutputRenderTargetView = device.CreateTextureView(
            m_finalOutputTexture,
            outputRtv);
        RHI::TextureViewDescription outputSrv{};
        outputSrv.type = RHI::TextureViewType::Sampled;
        m_finalOutputSampledView = device.CreateTextureView(
            m_finalOutputTexture,
            outputSrv);
    }

    RHI::TextureDescription hiZDescription{};
    hiZDescription.width = extent.width;
    hiZDescription.height = extent.height;
    hiZDescription.mipLevels = CalculateHiZMipCount(
        extent.width,
        extent.height);
    hiZDescription.format = RHI::Format::R32Float;
    hiZDescription.usage =
        RHI::TextureUsage::ShaderResource
        | RHI::TextureUsage::UnorderedAccess;
    m_hiZTexture = device.CreateTexture(hiZDescription);
    m_hiZSampledViews.clear();
    m_hiZStorageViews.clear();
    m_hiZSampledViews.reserve(hiZDescription.mipLevels);
    m_hiZStorageViews.reserve(hiZDescription.mipLevels);
    for (std::uint32_t mipIndex = 0;
         mipIndex < hiZDescription.mipLevels;
         ++mipIndex)
    {
        RHI::TextureViewDescription sampledView{};
        sampledView.type = RHI::TextureViewType::Sampled;
        sampledView.baseMipLevel = mipIndex;
        m_hiZSampledViews.push_back(
            device.CreateTextureView(m_hiZTexture, sampledView));
        RHI::TextureViewDescription storageView{};
        storageView.type = RHI::TextureViewType::Storage;
        storageView.baseMipLevel = mipIndex;
        m_hiZStorageViews.push_back(
            device.CreateTextureView(m_hiZTexture, storageView));
    }

    m_gbufferStates.fill(RHI::ResourceState::Undefined);
    m_hdrState = RHI::ResourceState::Undefined;
    m_bloomAState = RHI::ResourceState::Undefined;
    m_bloomBState = RHI::ResourceState::Undefined;
    m_finalOutputState = RHI::ResourceState::Undefined;
    m_hiZState = RHI::ResourceState::Undefined;
    m_depthState = RHI::ResourceState::Undefined;
    m_hiZValid = false;
    if (!m_featureRegistry.AreLifecycleFeaturesInitialized())
    {
        m_gpuDrivenVisibility.SetOcclusionTexture(m_hiZTexture);
    }
    // Initialization reaches this path before lifecycle registration. Runtime
    // resize is dispatched once by RecreateSwapChainResources instead.
    if (!m_featureRegistry.AreLifecycleFeaturesInitialized())
    {
        m_clusteredLighting.Resize(extent.width, extent.height);
    }
    if (!m_featureRegistry.AreLifecycleFeaturesInitialized())
    {
        m_planarReflections.Resize(extent.width, extent.height);
    }
    if (!m_featureRegistry.AreLifecycleFeaturesInitialized())
    {
        m_screenSpaceEffects.Resize(
            extent.width,
            extent.height,
            m_gbufferTextures[0],
            m_gbufferTextures[1],
            m_hdrTexture,
            m_hiZTexture,
            m_planarReflections.GetColorTextureShared());
    }
    m_fluidFeature.Resize(
        extent.width,
        extent.height,
        m_hdrTexture,
        depthTexture,
        m_environmentCubemap->GetRhiTexture());
    if (!m_featureRegistry.AreLifecycleFeaturesInitialized())
    {
        m_temporalAntiAliasing.Resize(
            extent.width,
            extent.height,
            m_screenSpaceEffects.GetCompositeTexture());
    }
}

void SceneRenderer::ReleaseSizeDependentResources()
{
    m_finalOutputSampledView.reset();
    m_finalOutputRenderTargetView.reset();
    m_finalOutputTexture.reset();
    m_finalOutputWidth = 0;
    m_finalOutputHeight = 0;
    m_hiZDescriptorSets.clear();
    m_hiZStorageViews.clear();
    m_hiZSampledViews.clear();
    m_hiZTexture.reset();
    m_depthSampledView.reset();
    m_depthStencilView.reset();
    m_depthTexture.reset();
    for (std::shared_ptr<RHI::IDescriptorSet>& descriptorSet
         : m_blurVerticalComputeDescriptorSets)
    {
        descriptorSet.reset();
    }
    for (std::shared_ptr<RHI::IDescriptorSet>& descriptorSet
         : m_blurHorizontalComputeDescriptorSets)
    {
        descriptorSet.reset();
    }
    for (std::shared_ptr<RHI::IDescriptorSet>& descriptorSet
         : m_brightExtractComputeDescriptorSets)
    {
        descriptorSet.reset();
    }
    for (std::shared_ptr<RHI::IDescriptorSet>& descriptorSet
         : m_shadowDebugDescriptorSets)
    {
        descriptorSet.reset();
    }
    for (std::shared_ptr<RHI::IDescriptorSet>& descriptorSet
         : m_tonemapDescriptorSets)
    {
        descriptorSet.reset();
    }
    for (std::shared_ptr<RHI::IDescriptorSet>& descriptorSet
         : m_deferredDescriptorSets)
    {
        descriptorSet.reset();
    }

    m_bloomSampledViewB.reset();
    m_bloomStorageViewB.reset();
    m_bloomRenderTargetViewB.reset();
    m_bloomTextureB.reset();
    m_bloomSampledViewA.reset();
    m_bloomStorageViewA.reset();
    m_bloomRenderTargetViewA.reset();
    m_bloomTextureA.reset();
    m_hdrSampledView.reset();
    m_hdrRenderTargetView.reset();
    m_hdrTexture.reset();
    for (std::uint32_t index = 0; index < GBufferCount; ++index)
    {
        m_gbufferSampledViews[index].reset();
        m_gbufferRenderTargetViews[index].reset();
        m_gbufferTextures[index].reset();
    }
    m_transientTexturePool.reset();
    m_gbufferStates.fill(RHI::ResourceState::Undefined);
    m_hdrState = RHI::ResourceState::Undefined;
    m_bloomAState = RHI::ResourceState::Undefined;
    m_bloomBState = RHI::ResourceState::Undefined;
    m_finalOutputState = RHI::ResourceState::Undefined;
    m_hiZState = RHI::ResourceState::Undefined;
    m_depthState = RHI::ResourceState::Undefined;
    m_hiZValid = false;
}
} // namespace Prism::Renderer
