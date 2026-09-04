#include "Renderer/SceneRenderer.h"

#include "Asset/Mesh.h"
#include "Renderer/DeferredTransientLayout.h"
#include "RHI/IFrameContext.h"
#include "RHI/IGraphicsDevice.h"
#include "RHI/IRenderBackend.h"

#include <cstddef>
#include <filesystem>
#include <memory>
#include <string_view>

namespace Prism::Renderer
{
void SceneRenderer::CreatePipeline(
    RHI::IRenderBackend& backend)
{
    RHI::IGraphicsDevice& device =
        backend.GetGraphicsDevice();
    const RHI::IFrameContext& frame =
        backend.GetFrameContext();
    RHI::GraphicsPipelineDescription gbufferDescription{};
    gbufferDescription.vertexShader = GetShaderManager().LoadShader(
        m_shaderPath,
        m_indexedObjectDrawingSupported
            ? "VSIndexedMain"
            : "VSMain",
        RHI::ShaderStage::Vertex,
        m_shaderFormat);
    gbufferDescription.pixelShader = GetShaderManager().LoadShader(
        m_shaderPath, "GBufferPS", RHI::ShaderStage::Pixel, m_shaderFormat);
    gbufferDescription.descriptorSetLayout = m_descriptorSetLayout;
    gbufferDescription.vertexBindings = {{0, sizeof(Asset::MeshVertex), RHI::VertexInputRate::PerVertex}};
    gbufferDescription.vertexAttributes = {
        {0, 0, RHI::VertexElementFormat::Float3, static_cast<std::uint32_t>(offsetof(Asset::MeshVertex, position))},
        {1, 0, RHI::VertexElementFormat::Float4, static_cast<std::uint32_t>(offsetof(Asset::MeshVertex, color))},
        {2, 0, RHI::VertexElementFormat::Float3, static_cast<std::uint32_t>(offsetof(Asset::MeshVertex, normal))},
        {3, 0, RHI::VertexElementFormat::Float2, static_cast<std::uint32_t>(offsetof(Asset::MeshVertex, texCoord))},
        {4, 0, RHI::VertexElementFormat::Float4, static_cast<std::uint32_t>(offsetof(Asset::MeshVertex, tangent))},
    };
    gbufferDescription.rasterizer.cullMode = RHI::CullMode::Back;
    gbufferDescription.rasterizer.frontFace = RHI::FrontFace::CounterClockwise;
    gbufferDescription.depthStencil.depthTestEnabled = true;
    gbufferDescription.depthStencil.depthWriteEnabled = true;
    gbufferDescription.depthStencil.depthComparison = RHI::CompareOperation::Less;
    const auto& gbufferFormats = GetDeferredGBufferFormats();
    gbufferDescription.colorFormats.assign(
        gbufferFormats.begin(),
        gbufferFormats.end());
    gbufferDescription.colorFormats.push_back(
        RHI::Format::Rg16Float);
    gbufferDescription.blendAttachments.assign(
        GBufferCount + 1u,
        RHI::BlendAttachmentDescription{});
    gbufferDescription.depthFormat = RHI::Format::D32Float;
    m_gbufferPipeline =
        GetPipelineCache().GetOrCreateGraphics(
            device,
            "Scene.GBuffer",
            gbufferDescription);
    RHI::GraphicsPipelineDescription
        gbufferDoubleSidedDescription =
            gbufferDescription;
    gbufferDoubleSidedDescription.rasterizer.cullMode =
        RHI::CullMode::None;
    m_gbufferDoubleSidedPipeline =
        GetPipelineCache().GetOrCreateGraphics(
            device,
            "Scene.GBuffer.DoubleSided",
            gbufferDoubleSidedDescription);

    const std::filesystem::path oceanShaderPath =
        m_shaderPath.parent_path() / "Ocean" / "OceanSurface.slang";
    RHI::GraphicsPipelineDescription oceanGBufferDescription =
        gbufferDescription;
    oceanGBufferDescription.descriptorSetLayout =
        m_oceanDescriptorSetLayout;
    oceanGBufferDescription.vertexShader = GetShaderManager().LoadShader(
        oceanShaderPath,
        "OceanVSMain",
        RHI::ShaderStage::Vertex,
        m_shaderFormat);
    oceanGBufferDescription.pixelShader = GetShaderManager().LoadShader(
        oceanShaderPath,
        "OceanGBufferPS",
        RHI::ShaderStage::Pixel,
        m_shaderFormat);
    m_oceanGBufferPipeline = GetPipelineCache().GetOrCreateGraphics(
        device,
        "Scene.Ocean.GBuffer",
        oceanGBufferDescription);
    RHI::GraphicsPipelineDescription oceanGBufferDoubleSidedDescription =
        oceanGBufferDescription;
    oceanGBufferDoubleSidedDescription.rasterizer.cullMode =
        RHI::CullMode::None;
    m_oceanGBufferDoubleSidedPipeline =
        GetPipelineCache().GetOrCreateGraphics(
            device,
            "Scene.Ocean.GBuffer.DoubleSided",
            oceanGBufferDoubleSidedDescription);
    RHI::GraphicsPipelineDescription oceanGBufferWireframeDescription =
        oceanGBufferDescription;
    oceanGBufferWireframeDescription.rasterizer.fillMode =
        RHI::FillMode::Wireframe;
    oceanGBufferWireframeDescription.rasterizer.cullMode =
        RHI::CullMode::None;
    m_oceanGBufferWireframePipeline =
        GetPipelineCache().GetOrCreateGraphics(
            device,
            "Scene.Ocean.GBuffer.Wireframe",
            oceanGBufferWireframeDescription);
    RHI::GraphicsPipelineDescription oceanGBufferTessellationDescription =
        oceanGBufferDescription;
    // Adaptive patches always consume OceanPatchInstance data, independently
    // of whether the backend supports the ordinary indexed-object path.
    // D3D12 deliberately keeps mesh object indexing disabled, but that must
    // not select the non-instanced ocean vertex entry for quadtree draws.
    oceanGBufferTessellationDescription.vertexShader =
        GetShaderManager().LoadShader(
            oceanShaderPath,
            "OceanVSIndexedMain",
            RHI::ShaderStage::Vertex,
            m_shaderFormat);
    oceanGBufferTessellationDescription.hullShader = GetShaderManager().LoadShader(
        oceanShaderPath, "OceanHullMain", RHI::ShaderStage::Hull,
        m_shaderFormat);
    oceanGBufferTessellationDescription.domainShader = GetShaderManager().LoadShader(
        oceanShaderPath, "OceanDomainMain", RHI::ShaderStage::Domain,
        m_shaderFormat);
    oceanGBufferTessellationDescription.topology =
        RHI::PrimitiveTopology::PatchList;
    oceanGBufferTessellationDescription.patchControlPointCount = 3u;
    m_oceanGBufferTessellationPipeline =
        GetPipelineCache().GetOrCreateGraphics(
            device,
            "Scene.Ocean.GBuffer.Tessellation",
            oceanGBufferTessellationDescription);
    RHI::GraphicsPipelineDescription oceanGBufferTessellationDoubleSidedDescription =
        oceanGBufferTessellationDescription;
    oceanGBufferTessellationDoubleSidedDescription.rasterizer.cullMode =
        RHI::CullMode::None;
    m_oceanGBufferTessellationDoubleSidedPipeline =
        GetPipelineCache().GetOrCreateGraphics(
            device,
            "Scene.Ocean.GBuffer.Tessellation.DoubleSided",
            oceanGBufferTessellationDoubleSidedDescription);
    RHI::GraphicsPipelineDescription oceanGBufferTessellationWireframeDescription =
        oceanGBufferTessellationDescription;
    oceanGBufferTessellationWireframeDescription.rasterizer.fillMode =
        RHI::FillMode::Wireframe;
    oceanGBufferTessellationWireframeDescription.rasterizer.cullMode =
        RHI::CullMode::None;
    m_oceanGBufferTessellationWireframePipeline =
        GetPipelineCache().GetOrCreateGraphics(
            device,
            "Scene.Ocean.GBuffer.Tessellation.Wireframe",
            oceanGBufferTessellationWireframeDescription);

    RHI::GraphicsPipelineDescription gbufferWireframeDescription =
        gbufferDescription;
    gbufferWireframeDescription.rasterizer.fillMode =
        RHI::FillMode::Wireframe;
    gbufferWireframeDescription.rasterizer.cullMode =
        RHI::CullMode::None;
    m_gbufferWireframePipeline =
        GetPipelineCache().GetOrCreateGraphics(
            device,
            "Scene.GBuffer.Wireframe",
            gbufferWireframeDescription);
    RHI::GraphicsPipelineDescription gbufferLineDescription =
        gbufferDescription;
    gbufferLineDescription.rasterizer.cullMode =
        RHI::CullMode::None;
    gbufferLineDescription.topology =
        RHI::PrimitiveTopology::LineList;
    m_gbufferDebugLinePipeline =
        GetPipelineCache().GetOrCreateGraphics(
            device,
            "Scene.GBuffer.EditorDebugLine",
            gbufferLineDescription);

    RHI::GraphicsPipelineDescription forwardDescription =
        gbufferDescription;
    forwardDescription.pixelShader = GetShaderManager().LoadShader(
        m_shaderPath,
        "PSMain",
        RHI::ShaderStage::Pixel,
        m_shaderFormat);
    forwardDescription.colorFormats = {
        RHI::Format::Rgba16Float};
    forwardDescription.blendAttachments = {
        RHI::BlendAttachmentDescription{}};
    m_forwardPipeline =
        GetPipelineCache().GetOrCreateGraphics(
            device,
            "Scene.Forward",
            forwardDescription);
    RHI::GraphicsPipelineDescription
        forwardDoubleSidedDescription =
            forwardDescription;
    forwardDoubleSidedDescription.rasterizer.cullMode =
        RHI::CullMode::None;
    m_forwardDoubleSidedPipeline =
        GetPipelineCache().GetOrCreateGraphics(
            device,
            "Scene.Forward.DoubleSided",
            forwardDoubleSidedDescription);

    RHI::GraphicsPipelineDescription oceanForwardDescription =
        oceanGBufferDescription;
    oceanForwardDescription.pixelShader = GetShaderManager().LoadShader(
        oceanShaderPath,
        "OceanPSMain",
        RHI::ShaderStage::Pixel,
        m_shaderFormat);
    oceanForwardDescription.colorFormats = {RHI::Format::Rgba16Float};
    oceanForwardDescription.blendAttachments = {
        RHI::BlendAttachmentDescription{}};
    m_oceanForwardPipeline = GetPipelineCache().GetOrCreateGraphics(
        device,
        "Scene.Ocean.Forward",
        oceanForwardDescription);
    RHI::GraphicsPipelineDescription oceanForwardDoubleSidedDescription =
        oceanForwardDescription;
    oceanForwardDoubleSidedDescription.rasterizer.cullMode =
        RHI::CullMode::None;
    m_oceanForwardDoubleSidedPipeline =
        GetPipelineCache().GetOrCreateGraphics(
            device,
            "Scene.Ocean.Forward.DoubleSided",
            oceanForwardDoubleSidedDescription);
    RHI::GraphicsPipelineDescription oceanForwardWireframeDescription =
        oceanForwardDescription;
    oceanForwardWireframeDescription.rasterizer.fillMode =
        RHI::FillMode::Wireframe;
    oceanForwardWireframeDescription.rasterizer.cullMode =
        RHI::CullMode::None;
    m_oceanForwardWireframePipeline =
        GetPipelineCache().GetOrCreateGraphics(
            device,
            "Scene.Ocean.Forward.Wireframe",
            oceanForwardWireframeDescription);
    RHI::GraphicsPipelineDescription oceanForwardTessellationDescription =
        oceanForwardDescription;
    oceanForwardTessellationDescription.vertexShader =
        oceanGBufferTessellationDescription.vertexShader;
    oceanForwardTessellationDescription.hullShader =
        oceanGBufferTessellationDescription.hullShader;
    oceanForwardTessellationDescription.domainShader =
        oceanGBufferTessellationDescription.domainShader;
    oceanForwardTessellationDescription.topology =
        RHI::PrimitiveTopology::PatchList;
    oceanForwardTessellationDescription.patchControlPointCount = 3u;
    m_oceanForwardTessellationPipeline =
        GetPipelineCache().GetOrCreateGraphics(
            device,
            "Scene.Ocean.Forward.Tessellation",
            oceanForwardTessellationDescription);
    RHI::GraphicsPipelineDescription oceanForwardTessellationDoubleSidedDescription =
        oceanForwardTessellationDescription;
    oceanForwardTessellationDoubleSidedDescription.rasterizer.cullMode =
        RHI::CullMode::None;
    m_oceanForwardTessellationDoubleSidedPipeline =
        GetPipelineCache().GetOrCreateGraphics(
            device,
            "Scene.Ocean.Forward.Tessellation.DoubleSided",
            oceanForwardTessellationDoubleSidedDescription);
    RHI::GraphicsPipelineDescription oceanForwardTessellationWireframeDescription =
        oceanForwardTessellationDescription;
    oceanForwardTessellationWireframeDescription.rasterizer.fillMode =
        RHI::FillMode::Wireframe;
    oceanForwardTessellationWireframeDescription.rasterizer.cullMode =
        RHI::CullMode::None;
    m_oceanForwardTessellationWireframePipeline =
        GetPipelineCache().GetOrCreateGraphics(
            device,
            "Scene.Ocean.Forward.Tessellation.Wireframe",
            oceanForwardTessellationWireframeDescription);
    RHI::GraphicsPipelineDescription forwardWireframeDescription =
        forwardDescription;
    forwardWireframeDescription.rasterizer.fillMode =
        RHI::FillMode::Wireframe;
    forwardWireframeDescription.rasterizer.cullMode =
        RHI::CullMode::None;
    m_forwardWireframePipeline =
        GetPipelineCache().GetOrCreateGraphics(
            device,
            "Scene.Forward.Wireframe",
            forwardWireframeDescription);

    RHI::GraphicsPipelineDescription transparentDescription =
        forwardDescription;
    transparentDescription.depthStencil.depthWriteEnabled =
        false;
    transparentDescription.blendAttachments = {
        RHI::BlendAttachmentDescription{
            true,
            RHI::BlendFactor::SourceAlpha,
            RHI::BlendFactor::InverseSourceAlpha,
            RHI::BlendOperation::Add,
            RHI::BlendFactor::One,
            RHI::BlendFactor::InverseSourceAlpha,
            RHI::BlendOperation::Add,
            0x0f}};
    m_transparentPipeline =
        GetPipelineCache().GetOrCreateGraphics(
            device,
            "Scene.Transparent",
            transparentDescription);
    RHI::GraphicsPipelineDescription
        transparentDoubleSidedDescription =
            transparentDescription;
    transparentDoubleSidedDescription.rasterizer.cullMode =
        RHI::CullMode::None;
    m_transparentDoubleSidedPipeline =
        GetPipelineCache().GetOrCreateGraphics(
            device,
            "Scene.Transparent.DoubleSided",
            transparentDoubleSidedDescription);
    RHI::GraphicsPipelineDescription transparentWireframeDescription =
        transparentDescription;
    transparentWireframeDescription.rasterizer.fillMode =
        RHI::FillMode::Wireframe;
    transparentWireframeDescription.rasterizer.cullMode =
        RHI::CullMode::None;
    m_transparentWireframePipeline =
        GetPipelineCache().GetOrCreateGraphics(
            device,
            "Scene.Transparent.Wireframe",
            transparentWireframeDescription);
    RHI::GraphicsPipelineDescription forwardLineDescription =
        forwardDescription;
    forwardLineDescription.topology =
        RHI::PrimitiveTopology::LineList;
    m_forwardDebugLinePipeline =
        GetPipelineCache().GetOrCreateGraphics(
            device,
            "Scene.Forward.EditorDebugLine",
            forwardLineDescription);

    const std::filesystem::path shadowShaderPath =
        m_shaderPath.parent_path() / "Shadow.hlsl";
    RHI::GraphicsPipelineDescription shadowDescription{};
    shadowDescription.vertexShader = GetShaderManager().LoadShader(
        shadowShaderPath, "VSMain", RHI::ShaderStage::Vertex, m_shaderFormat);
    shadowDescription.pixelShader = GetShaderManager().LoadShader(
        shadowShaderPath, "PSMain", RHI::ShaderStage::Pixel, m_shaderFormat);
    shadowDescription.descriptorSetLayout = m_shadowDescriptorSetLayout;
    shadowDescription.vertexBindings = {{0, sizeof(Asset::MeshVertex), RHI::VertexInputRate::PerVertex}};
    shadowDescription.vertexAttributes = {
        {0, 0, RHI::VertexElementFormat::Float3,
         static_cast<std::uint32_t>(offsetof(Asset::MeshVertex, position))},
        {3, 0, RHI::VertexElementFormat::Float2,
         static_cast<std::uint32_t>(offsetof(Asset::MeshVertex, texCoord))},
    };
    shadowDescription.rasterizer.cullMode = RHI::CullMode::Back;
    shadowDescription.rasterizer.frontFace = RHI::FrontFace::CounterClockwise;
    shadowDescription.rasterizer.depthBias = 2;
    shadowDescription.rasterizer.slopeScaledDepthBias = 2.0f;
    shadowDescription.depthStencil.depthTestEnabled = true;
    shadowDescription.depthStencil.depthWriteEnabled = true;
    shadowDescription.depthStencil.depthComparison = RHI::CompareOperation::Less;
    shadowDescription.colorFormats.clear();
    shadowDescription.blendAttachments.clear();
    shadowDescription.depthFormat = RHI::Format::D32Float;
    m_shadowPipeline =
        GetPipelineCache().GetOrCreateGraphics(
            device,
            "Scene.DirectionalShadow",
            shadowDescription);
    RHI::GraphicsPipelineDescription
        shadowDoubleSidedDescription =
            shadowDescription;
    shadowDoubleSidedDescription.rasterizer.cullMode =
        RHI::CullMode::None;
    m_shadowDoubleSidedPipeline =
        GetPipelineCache().GetOrCreateGraphics(
            device,
            "Scene.DirectionalShadow.DoubleSided",
            shadowDoubleSidedDescription);

    const std::filesystem::path skyShaderPath =
        m_shaderPath.parent_path() / "PostProcess.hlsl";
    RHI::GraphicsPipelineDescription skyDescription{};
    skyDescription.vertexShader = GetShaderManager().LoadShader(
        skyShaderPath, "FullscreenVS", RHI::ShaderStage::Vertex, m_shaderFormat);
    skyDescription.pixelShader = GetShaderManager().LoadShader(
        skyShaderPath, "SkyboxPS", RHI::ShaderStage::Pixel, m_shaderFormat);
    skyDescription.descriptorSetLayout = m_postProcessDescriptorSetLayout;
    skyDescription.rasterizer.cullMode = RHI::CullMode::None;
    skyDescription.depthStencil.depthTestEnabled = false;
    skyDescription.depthStencil.depthWriteEnabled = false;
    skyDescription.colorFormats = {RHI::Format::Rgba16Float};
    skyDescription.depthFormat = RHI::Format::Unknown;
    m_skyPipeline =
        GetPipelineCache().GetOrCreateGraphics(
            device,
            "Scene.Sky",
            skyDescription);

    const auto createFullscreenPipeline =
        [&](const std::string_view cacheKey,
            const std::filesystem::path& path,
            const char* pixelEntry,
            const std::shared_ptr<RHI::IDescriptorSetLayout>& layout,
            const RHI::Format format)
        {
            RHI::GraphicsPipelineDescription description{};
            description.vertexShader = GetShaderManager().LoadShader(
                path, "FullscreenVS", RHI::ShaderStage::Vertex, m_shaderFormat);
            description.pixelShader = GetShaderManager().LoadShader(
                path, pixelEntry, RHI::ShaderStage::Pixel, m_shaderFormat);
            description.descriptorSetLayout = layout;
            description.rasterizer.cullMode = RHI::CullMode::None;
            description.depthStencil.depthTestEnabled = false;
            description.depthStencil.depthWriteEnabled = false;
            description.depthFormat = RHI::Format::Unknown;
            description.colorFormats = {format};
            return GetPipelineCache().GetOrCreateGraphics(
                device,
                cacheKey,
                description);
        };

    const std::filesystem::path deferredPath =
        m_shaderPath.parent_path() / "Deferred.hlsl";
    m_deferredPipeline = createFullscreenPipeline(
        "Scene.DeferredLighting",
        deferredPath,
        "DeferredLightingPS",
        m_deferredDescriptorSetLayout,
        RHI::Format::Rgba16Float);

    const std::filesystem::path postProcessPath =
        m_shaderPath.parent_path() / "PostProcess.hlsl";
    const auto createComputePipeline =
        [&](const std::string_view cacheKey,
            const char* entry)
        {
            RHI::ComputePipelineDescription description{};
            description.computeShader =
                GetShaderManager().LoadShader(
                    postProcessPath,
                    entry,
                    RHI::ShaderStage::Compute,
                    m_shaderFormat);
            description.descriptorSetLayout =
                m_bloomComputeDescriptorSetLayout;
            return GetPipelineCache().GetOrCreateCompute(
                device,
                cacheKey,
                description);
        };
    m_brightExtractComputePipeline =
        createComputePipeline(
            "Scene.Bloom.BrightExtract",
            "BrightExtractCS");
    m_blurHorizontalComputePipeline =
        createComputePipeline(
            "Scene.Bloom.Horizontal",
            "BlurHorizontalCS");
    m_blurVerticalComputePipeline =
        createComputePipeline(
            "Scene.Bloom.Vertical",
            "BlurVerticalCS");

    const std::filesystem::path hiZPath =
        m_shaderPath.parent_path() / "HiZ.hlsl";
    const auto createHiZPipeline =
        [&](const std::string_view cacheKey,
            const char* entry)
        {
            RHI::ComputePipelineDescription description{};
            description.computeShader =
                GetShaderManager().LoadShader(
                    hiZPath,
                    entry,
                    RHI::ShaderStage::Compute,
                    m_shaderFormat);
            description.descriptorSetLayout =
                m_hiZComputeDescriptorSetLayout;
            return GetPipelineCache().GetOrCreateCompute(
                device,
                cacheKey,
                description);
        };
    m_hiZCopyPipeline =
        createHiZPipeline(
            "Scene.HiZ.Copy",
            "CopyDepthCS");
    m_hiZDownsamplePipeline =
        createHiZPipeline(
            "Scene.HiZ.Downsample",
            "DownsampleDepthCS");
    m_tonemapPipeline = createFullscreenPipeline(
        "Scene.Tonemap",
        postProcessPath,
        "TonemapPS",
        m_postProcessDescriptorSetLayout,
        frame.GetBackBufferRhiFormat());
    const std::filesystem::path debugPath =
        m_shaderPath.parent_path() / "DebugView.hlsl";
    m_shadowDebugPipeline = createFullscreenPipeline(
        "Scene.ShadowDebug",
        debugPath,
        "ShadowDebugPS",
        m_shadowDebugDescriptorSetLayout,
        frame.GetBackBufferRhiFormat());
}
} // namespace Prism::Renderer
