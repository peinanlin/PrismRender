#if defined(PRISM_RENDER_HAS_D3D12)
#include "RHI/D3D11/D3D11TypeConversions.h"
#include "RHI/D3D12/D3D12TypeConversions.h"
#endif
#include "RHI/GraphicsApi.h"
#include "RHI/GraphicsTypes.h"
#include "RHI/GraphicsResources.h"
#include "RHI/PipelineState.h"
#include "RHI/ShaderLayoutBuilder.h"
#include "RHI/RayTracing.h"
#include "RHI/Rendering.h"
#include "RHI/Vulkan/VulkanTypeConversions.h"
#include "Renderer/Pipeline/PipelineCache.h"

#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>

namespace
{
class MockTexture final : public Prism::RHI::ITexture
{
public:
    explicit MockTexture(const Prism::RHI::TextureDescription& value) : description(value) {}
    Prism::RHI::GraphicsApi GetGraphicsApi() const override { return Prism::RHI::GraphicsApi::Vulkan; }
    const Prism::RHI::TextureDescription& GetDescription() const override { return description; }

private:
    Prism::RHI::TextureDescription description{};
};

class MockTextureView final : public Prism::RHI::ITextureView
{
public:
    MockTextureView(const MockTexture& source, const Prism::RHI::TextureViewDescription& value)
        : texture(&source), description(value) {}
    Prism::RHI::GraphicsApi GetGraphicsApi() const override { return Prism::RHI::GraphicsApi::Vulkan; }
    const Prism::RHI::TextureViewDescription& GetDescription() const override { return description; }
    const Prism::RHI::ITexture* GetTexture() const override { return texture; }

private:
    const MockTexture* texture = nullptr;
    Prism::RHI::TextureViewDescription description{};
};

void Expect(const bool condition, const char* message)
{
    if (!condition)
    {
        throw std::runtime_error(message);
    }
}

void TestApiNames()
{
    using namespace Prism::RHI;
    Expect(ToString(GraphicsApi::Direct3D12) == "Direct3D 12", "D3D12 display name mismatch.");
    Expect(TryParseGraphicsApi("DX12") == GraphicsApi::Direct3D12, "DX12 alias parsing failed.");
    Expect(TryParseGraphicsApi("d3d11") == GraphicsApi::Direct3D11, "D3D11 alias parsing failed.");
    Expect(TryParseGraphicsApi("Vk") == GraphicsApi::Vulkan, "Vulkan alias parsing failed.");
    Expect(!TryParseGraphicsApi("metal").has_value(), "Unknown API names must not parse.");
}

void TestSharedValidation()
{
    using namespace Prism::RHI;
    TextureDescription valid{};
    valid.width = 1920;
    valid.height = 1080;
    valid.format = Format::Rgba16Float;
    valid.usage = TextureUsage::ShaderResource | TextureUsage::RenderTarget;
    Expect(ValidateTextureDescription(valid), "Valid HDR target was rejected.");

    TextureDescription invalidCube = valid;
    invalidCube.dimension = TextureDimension::TextureCube;
    invalidCube.arrayLayers = 5;
    Expect(!ValidateTextureDescription(invalidCube), "Invalid cube layer count was accepted.");

    TextureDescription invalidDepth = valid;
    invalidDepth.format = Format::D32Float;
    Expect(!ValidateTextureDescription(invalidDepth), "Depth format without depth usage was accepted.");

    TextureDescription validDepth{};
    validDepth.width = 2048;
    validDepth.height = 2048;
    validDepth.arrayLayers = 3;
    validDepth.format = Format::D32Float;
    validDepth.usage = TextureUsage::DepthStencil | TextureUsage::ShaderResource;
    Expect(ValidateTextureDescription(validDepth), "Valid shadow array was rejected.");
}

void TestPublicRhiDescriptions()
{
    using namespace Prism::RHI;

    BufferDescription vertexBuffer{};
    vertexBuffer.size = 1024;
    vertexBuffer.stride = 32;
    vertexBuffer.usage = BufferUsage::Vertex;
    vertexBuffer.memoryAccess = MemoryAccess::CpuToGpu;
    Expect(ValidateBufferDescription(vertexBuffer, nullptr), "Valid public RHI vertex buffer was rejected.");

    BufferDescription invalidBuffer = vertexBuffer;
    invalidBuffer.stride = 0;
    Expect(!ValidateBufferDescription(invalidBuffer, nullptr), "Stride-less vertex buffer was accepted.");

    BufferDescription uploadedStructuredBuffer{};
    uploadedStructuredBuffer.size = 1024;
    uploadedStructuredBuffer.stride = 32;
    uploadedStructuredBuffer.usage = BufferUsage::ShaderResource;
    uploadedStructuredBuffer.memoryAccess = MemoryAccess::CpuToGpu;
    Expect(
        ValidateBufferDescription(uploadedStructuredBuffer, nullptr),
        "A CPU-uploaded read-only structured buffer was rejected.");

    BufferDescription invalidUploadedStorageBuffer = uploadedStructuredBuffer;
    invalidUploadedStorageBuffer.usage = BufferUsage::Storage
        | BufferUsage::ShaderResource;
    std::string invalidUploadedStorageError;
    Expect(
        !ValidateBufferDescription(
            invalidUploadedStorageBuffer,
            nullptr,
            &invalidUploadedStorageError),
        "A CPU-visible storage/UAV buffer was accepted.");
    Expect(
        invalidUploadedStorageError.find("CPU-visible") != std::string::npos,
        "CPU-visible storage validation did not return an actionable error.");

    BufferDescription indirectBuffer{};
    indirectBuffer.size = 200;
    indirectBuffer.stride = 20;
    indirectBuffer.usage =
        BufferUsage::Storage
        | BufferUsage::Indirect
        | BufferUsage::CopyDestination;
    indirectBuffer.memoryAccess =
        MemoryAccess::GpuOnly;
    Expect(
        ValidateBufferDescription(
            indirectBuffer,
            nullptr),
        "A valid GPU-written indirect buffer was rejected.");

    BufferDescription accelerationStructure{};
    accelerationStructure.size = 4096;
    accelerationStructure.usage =
        BufferUsage::AccelerationStructureStorage;
    accelerationStructure.memoryAccess =
        MemoryAccess::GpuOnly;
    Expect(
        ValidateBufferDescription(
            accelerationStructure,
            nullptr),
        "An uninitialized acceleration-structure storage buffer was rejected.");

    DescriptorSetLayoutDescription layout{};
    layout.bindings = {
        {0, DescriptorType::ConstantBuffer, 1, ShaderStageFlags::AllGraphics},
        {1, DescriptorType::DynamicConstantBuffer, 1, ShaderStageFlags::Vertex},
        {16, DescriptorType::SampledTexture, 1, ShaderStageFlags::Pixel},
        {48, DescriptorType::Sampler, 1, ShaderStageFlags::Pixel},
    };
    Expect(ValidateDescriptorSetLayoutDescription(layout), "Valid sparse descriptor layout was rejected.");

    DescriptorSetLayoutDescription invalidDynamicArray{};
    invalidDynamicArray.bindings = {
        {1, DescriptorType::DynamicConstantBuffer, 2, ShaderStageFlags::Vertex},
    };
    Expect(!ValidateDescriptorSetLayoutDescription(invalidDynamicArray),
           "An unsupported dynamic constant-buffer array was accepted.");

    layout.bindings.push_back({16, DescriptorType::StorageTexture, 1, ShaderStageFlags::Compute});
    Expect(!ValidateDescriptorSetLayoutDescription(layout), "Duplicate descriptor binding was accepted.");

    ShaderReflection hullReflection{};
    hullReflection.resources.push_back({"patchConstants",
        ShaderResourceKind::ConstantBuffer,
        ShaderResourceShape::Buffer, 0u, 0u, 16u});
    ShaderReflection domainReflection = hullReflection;
    const std::array tessellationStages = {
        ShaderLayoutStage{&hullReflection, ShaderStage::Hull},
        ShaderLayoutStage{&domainReflection, ShaderStage::Domain}};
    const DescriptorSetLayoutDescription tessellationLayout =
        BuildDescriptorSetLayout(tessellationStages);
    Expect(tessellationLayout.bindings.size() == 1u
            && HasAnyFlag(tessellationLayout.bindings.front().stages,
                ShaderStageFlags::Hull)
            && HasAnyFlag(tessellationLayout.bindings.front().stages,
                ShaderStageFlags::Domain),
        "Hull/domain reflection did not merge descriptor visibility.");

    TextureDescription shadowTexture{};
    shadowTexture.width = 2048;
    shadowTexture.height = 2048;
    shadowTexture.arrayLayers = 3;
    shadowTexture.format = Format::D32Float;
    shadowTexture.usage = TextureUsage::DepthStencil | TextureUsage::ShaderResource;
    TextureViewDescription cascadeView{};
    cascadeView.type = TextureViewType::DepthStencil;
    cascadeView.baseArrayLayer = 2;
    Expect(ValidateTextureViewDescription(shadowTexture, cascadeView),
           "Valid shadow-cascade view was rejected.");
    TextureViewDescription sampledShadow{};
    sampledShadow.type = TextureViewType::Sampled;
    sampledShadow.arrayLayerCount = 3;
    Expect(ValidateTextureViewDescription(shadowTexture, sampledShadow),
           "Valid sampled shadow-array view was rejected.");
    cascadeView.baseArrayLayer = 3;
    Expect(!ValidateTextureViewDescription(shadowTexture, cascadeView),
           "Out-of-range shadow-cascade view was accepted.");

    MockTexture mockShadow(shadowTexture);
    TextureViewDescription depthViewDescription{};
    depthViewDescription.type = TextureViewType::DepthStencil;
    MockTextureView depthView(mockShadow, depthViewDescription);
    RenderingInfo shadowRendering{};
    shadowRendering.width = 2048;
    shadowRendering.height = 2048;
    RenderingAttachment depthAttachment{};
    depthAttachment.view = &depthView;
    shadowRendering.depthAttachment = depthAttachment;
    Expect(ValidateRenderingInfo(shadowRendering), "Valid depth-only rendering scope was rejected.");

    TextureDescription gbufferTexture{};
    gbufferTexture.width = 1920;
    gbufferTexture.height = 1080;
    gbufferTexture.format = Format::Rgba16Float;
    gbufferTexture.usage = TextureUsage::RenderTarget | TextureUsage::ShaderResource;
    MockTexture mockGBuffer(gbufferTexture);
    TextureViewDescription colorViewDescription{};
    colorViewDescription.type = TextureViewType::RenderTarget;
    MockTextureView colorView(mockGBuffer, colorViewDescription);
    RenderingAttachment colorAttachment{};
    colorAttachment.view = &colorView;
    RenderingInfo gbufferRendering{};
    gbufferRendering.width = 1920;
    gbufferRendering.height = 1080;
    gbufferRendering.colorAttachments = {colorAttachment, colorAttachment, colorAttachment, colorAttachment};
    gbufferRendering.depthAttachment = depthAttachment;
    Expect(ValidateRenderingInfo(gbufferRendering), "Valid four-target GBuffer rendering scope was rejected.");
}

void TestRayTracingDescriptions()
{
    using namespace Prism::RHI;

    AccelerationStructureBuildDescription bottomLevel{};
    bottomLevel.geometries = {
        {24,
         32,
         36,
         IndexFormat::UInt32,
         RayTracingGeometryFlags::Opaque}};
    bottomLevel.flags =
        AccelerationStructureBuildFlags::PreferFastTrace
        | AccelerationStructureBuildFlags::AllowCompaction;
    Expect(
        ValidateAccelerationStructureBuildDescription(
            bottomLevel),
        "A valid BLAS description was rejected.");

    AccelerationStructureBuildDescription topLevel{};
    topLevel.type =
        AccelerationStructureType::TopLevel;
    topLevel.instanceCount = 128;
    topLevel.flags =
        AccelerationStructureBuildFlags::PreferFastBuild
        | AccelerationStructureBuildFlags::AllowUpdate;
    Expect(
        ValidateAccelerationStructureBuildDescription(
            topLevel),
        "A valid TLAS description was rejected.");

    bottomLevel.geometries.front().indexCount = 35;
    Expect(
        !ValidateAccelerationStructureBuildDescription(
            bottomLevel),
        "A non-triangle-list BLAS was accepted.");

    topLevel.geometries = {
        {3,
         12,
         0,
         IndexFormat::UInt32,
         RayTracingGeometryFlags::Opaque}};
    Expect(
        !ValidateAccelerationStructureBuildDescription(
            topLevel),
        "TLAS triangle geometry was accepted.");

    AccelerationStructureBuildDescription
        contradictory{};
    contradictory.geometries = {
        {3,
         12,
         0,
         IndexFormat::UInt32,
         RayTracingGeometryFlags::Opaque}};
    contradictory.flags =
        AccelerationStructureBuildFlags::PreferFastTrace
        | AccelerationStructureBuildFlags::PreferFastBuild;
    Expect(
        !ValidateAccelerationStructureBuildDescription(
            contradictory),
        "Contradictory AS build preferences were accepted.");
    Expect(
        std::string(ToString(RayTracingTier::Tier1_1))
            == "1.1",
        "Ray-tracing tier serialization mismatch.");
}

void TestPipelineDescriptions()
{
    using namespace Prism::RHI;
    auto makeShader = [](const ShaderStage stage)
    {
        ShaderBinary shader{};
        shader.stage = stage;
        shader.format = ShaderBinaryFormat::SpirV;
        shader.entryPoint = stage == ShaderStage::Compute ? "CSMain" : "Main";
        shader.emittedEntryPoint = "main";
        shader.bytecode = {1, 2, 3, 4};
        return shader;
    };

    GraphicsPipelineDescription fullscreen{};
    fullscreen.vertexShader = makeShader(ShaderStage::Vertex);
    fullscreen.pixelShader = makeShader(ShaderStage::Pixel);
    Expect(ValidateGraphicsPipelineDescription(fullscreen), "Valid fullscreen pipeline was rejected.");

    GraphicsPipelineDescription tessellated = fullscreen;
    tessellated.topology = PrimitiveTopology::PatchList;
    tessellated.patchControlPointCount = 3u;
    tessellated.hullShader = makeShader(ShaderStage::Hull);
    tessellated.domainShader = makeShader(ShaderStage::Domain);
    Expect(ValidateGraphicsPipelineDescription(tessellated),
        "Valid tessellation pipeline was rejected.");
    tessellated.domainShader = {};
    Expect(!ValidateGraphicsPipelineDescription(tessellated),
        "Partial tessellation stages were accepted.");
    tessellated.domainShader = makeShader(ShaderStage::Domain);
    tessellated.topology = PrimitiveTopology::TriangleList;
    Expect(!ValidateGraphicsPipelineDescription(tessellated),
        "Tessellation stages were accepted with non-patch topology.");
    tessellated.topology = PrimitiveTopology::PatchList;
    tessellated.patchControlPointCount = 0u;
    Expect(!ValidateGraphicsPipelineDescription(tessellated),
        "Tessellation pipeline accepted an invalid control-point count.");

    GraphicsPipelineDescription mesh = fullscreen;
    mesh.vertexBindings = {{0, 48, VertexInputRate::PerVertex}};
    mesh.vertexAttributes = {
        {0, 0, VertexElementFormat::Float3, 0},
        {1, 0, VertexElementFormat::Float4, 12},
    };
    Expect(ValidateGraphicsPipelineDescription(mesh), "Valid vertex layout was rejected.");

    GraphicsPipelineDescription gbuffer = mesh;
    gbuffer.colorFormats.assign(4, Format::Rgba16Float);
    gbuffer.blendAttachments.assign(4, BlendAttachmentDescription{});
    Expect(ValidateGraphicsPipelineDescription(gbuffer), "Valid four-target GBuffer pipeline was rejected.");

    mesh.vertexAttributes.push_back({2, 0, VertexElementFormat::Float4, 40});
    Expect(!ValidateGraphicsPipelineDescription(mesh), "Out-of-stride vertex attribute was accepted.");

    GraphicsPipelineDescription shadow = fullscreen;
    shadow.colorFormats.clear();
    shadow.blendAttachments.clear();
    shadow.pixelShader = {};
    shadow.depthFormat = Format::D32Float;
    shadow.depthStencil.depthTestEnabled = true;
    shadow.depthStencil.depthWriteEnabled = true;
    Expect(ValidateGraphicsPipelineDescription(shadow), "Valid depth-only shadow pipeline was rejected.");

    ComputePipelineDescription compute{};
    compute.computeShader = makeShader(ShaderStage::Compute);
    Expect(ValidateComputePipelineDescription(compute), "Valid compute pipeline was rejected.");
    compute.computeShader.stage = ShaderStage::Pixel;
    Expect(!ValidateComputePipelineDescription(compute), "Pixel shader was accepted as a compute pipeline.");
}

#if defined(PRISM_RENDER_HAS_D3D12)
void TestD3D12Conversions()
{
    using namespace Prism::RHI;
    TextureDescription texture{};
    texture.width = 1280;
    texture.height = 720;
    texture.mipLevels = 4;
    texture.format = Format::Rgba16Float;
    texture.usage = TextureUsage::ShaderResource | TextureUsage::RenderTarget;
    const D3D12_RESOURCE_DESC nativeTexture = D3D12::ToNativeTextureDescription(texture);
    Expect(nativeTexture.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D, "D3D12 texture dimension mismatch.");
    Expect(nativeTexture.Format == DXGI_FORMAT_R16G16B16A16_FLOAT, "D3D12 HDR format mismatch.");
    Expect(nativeTexture.MipLevels == 4, "D3D12 mip count mismatch.");
    Expect((nativeTexture.Flags & D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET) != 0, "D3D12 RTV flag missing.");

    const D3D12_RESOURCE_STATES states = D3D12::ToNativeResourceStates(
        ResourceState::ShaderResource | ResourceState::CopySource);
    Expect((states & D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE) != 0, "D3D12 pixel SRV state missing.");
    Expect((states & D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE) != 0, "D3D12 compute SRV state missing.");
    Expect((states & D3D12_RESOURCE_STATE_COPY_SOURCE) != 0, "D3D12 copy-source state missing.");
    const D3D12_RESOURCE_STATES indirectState =
        D3D12::ToNativeResourceStates(
            ResourceState::IndirectArgument);
    Expect(
        (indirectState
         & D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT)
            != 0,
        "D3D12 indirect-argument state missing.");

    SamplerDescription sampler{};
    sampler.filter = Filter::ComparisonLinear;
    sampler.addressU = AddressMode::ClampToBorder;
    sampler.comparison = CompareOperation::LessEqual;
    const D3D12_SAMPLER_DESC nativeSampler = D3D12::ToNativeSamplerDescription(sampler);
    Expect(nativeSampler.Filter == D3D12_FILTER_COMPARISON_MIN_MAG_LINEAR_MIP_POINT, "D3D12 comparison filter mismatch.");
    Expect(nativeSampler.AddressU == D3D12_TEXTURE_ADDRESS_MODE_BORDER, "D3D12 address mode mismatch.");
    Expect(nativeSampler.ComparisonFunc == D3D12_COMPARISON_FUNC_LESS_EQUAL, "D3D12 comparison mismatch.");

    RasterizerDescription rasterizer{};
    rasterizer.fillMode = FillMode::Wireframe;
    rasterizer.cullMode = CullMode::Front;
    rasterizer.frontFace = FrontFace::CounterClockwise;
    const D3D12_RASTERIZER_DESC nativeRasterizer = D3D12::ToNativeRasterizerDescription(rasterizer);
    Expect(nativeRasterizer.FillMode == D3D12_FILL_MODE_WIREFRAME, "D3D12 fill mode mismatch.");
    Expect(nativeRasterizer.CullMode == D3D12_CULL_MODE_FRONT, "D3D12 cull mode mismatch.");
    Expect(nativeRasterizer.FrontCounterClockwise == TRUE, "D3D12 front-face conversion mismatch.");
}

void TestD3D12SampledViews()
{
    using namespace Prism::RHI;
    TextureDescription texture{};
    texture.width = texture.height = 32u;
    texture.format = Format::Rgba16Float;
    texture.usage = TextureUsage::ShaderResource;
    texture.mipLevels = 5u;
    TextureViewDescription view{};
    view.type = TextureViewType::Sampled;
    view.baseMipLevel = 1u;
    view.mipLevelCount = 3u;
    auto native = D3D12::ToNativeSampledTextureViewDescription(texture, view);
    Expect(native.ViewDimension == D3D12_SRV_DIMENSION_TEXTURE2D
        && native.Texture2D.MostDetailedMip == 1u && native.Texture2D.MipLevels == 3u
        && native.Format == DXGI_FORMAT_R16G16B16A16_FLOAT
        && native.Shader4ComponentMapping == D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING,
        "D3D12 sampled 2D view lost format, mip range, or component mapping.");
    texture.arrayLayers = 4u;
    view.baseArrayLayer = 1u;
    view.arrayLayerCount = 2u;
    native = D3D12::ToNativeSampledTextureViewDescription(texture, view);
    Expect(native.ViewDimension == D3D12_SRV_DIMENSION_TEXTURE2DARRAY
        && native.Texture2DArray.FirstArraySlice == 1u && native.Texture2DArray.ArraySize == 2u
        && native.Texture2DArray.MostDetailedMip == 1u && native.Texture2DArray.MipLevels == 3u,
        "D3D12 sampled array view lost its subresource range.");
    texture.dimension = TextureDimension::TextureCube;
    texture.arrayLayers = 18u;
    view.baseArrayLayer = 6u;
    view.arrayLayerCount = 12u;
    native = D3D12::ToNativeSampledTextureViewDescription(texture, view);
    Expect(native.ViewDimension == D3D12_SRV_DIMENSION_TEXTURECUBEARRAY
        && native.TextureCubeArray.First2DArrayFace == 6u && native.TextureCubeArray.NumCubes == 2u
        && native.TextureCubeArray.MipLevels == 3u,
        "D3D12 sampled cube-array view lost faces or mips.");
    texture.arrayLayers = 6u;
    view.baseArrayLayer = 0u;
    view.arrayLayerCount = 6u;
    native = D3D12::ToNativeSampledTextureViewDescription(texture, view);
    Expect(native.ViewDimension == D3D12_SRV_DIMENSION_TEXTURECUBE
        && native.TextureCube.MostDetailedMip == 1u && native.TextureCube.MipLevels == 3u,
        "D3D12 sampled cube view lost its mip range.");
    texture.dimension = TextureDimension::Texture2D;
    texture.arrayLayers = view.arrayLayerCount = 1u;
    texture.format = Format::D32Float;
    native = D3D12::ToNativeSampledTextureViewDescription(texture, view);
    Expect(native.Format == DXGI_FORMAT_R32_FLOAT, "D3D12 sampled depth format changed.");
    view.mipLevelCount = 5u;
    bool rejected = false;
    try { (void)D3D12::ToNativeSampledTextureViewDescription(texture, view); }
    catch (const std::invalid_argument&) { rejected = true; }
    Expect(rejected, "D3D12 sampled view accepted an out-of-range mip.");
}

void TestD3D11Conversions()
{
    using namespace Prism::RHI;
    TextureDescription texture{};
    texture.width = 512;
    texture.height = 512;
    texture.dimension = TextureDimension::TextureCube;
    texture.arrayLayers = 6;
    texture.format = Format::Rgba8UnormSrgb;
    texture.usage = TextureUsage::ShaderResource | TextureUsage::RenderTarget;
    const D3D11_TEXTURE2D_DESC nativeTexture = D3D11::ToNativeTextureDescription(texture);
    Expect(nativeTexture.Format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, "D3D11 sRGB format mismatch.");
    Expect((nativeTexture.BindFlags & D3D11_BIND_SHADER_RESOURCE) != 0, "D3D11 SRV bind flag missing.");
    Expect((nativeTexture.BindFlags & D3D11_BIND_RENDER_TARGET) != 0, "D3D11 RTV bind flag missing.");
    Expect((nativeTexture.MiscFlags & D3D11_RESOURCE_MISC_TEXTURECUBE) != 0, "D3D11 cube flag missing.");

    const UINT bufferBindings = D3D11::ToNativeBindFlags(
        ResourceState::VertexBuffer | ResourceState::ConstantBuffer);
    Expect((bufferBindings & D3D11_BIND_VERTEX_BUFFER) != 0, "D3D11 vertex binding missing.");
    Expect((bufferBindings & D3D11_BIND_CONSTANT_BUFFER) != 0, "D3D11 constant binding missing.");

    SamplerDescription sampler{};
    sampler.filter = Filter::Anisotropic;
    sampler.maxAnisotropy = 32;
    const D3D11_SAMPLER_DESC nativeSampler = D3D11::ToNativeSamplerDescription(sampler);
    Expect(nativeSampler.Filter == D3D11_FILTER_ANISOTROPIC, "D3D11 anisotropic filter mismatch.");
    Expect(nativeSampler.MaxAnisotropy == 16, "D3D11 anisotropy clamp mismatch.");

    BlendAttachmentDescription blend{};
    blend.blendEnabled = true;
    blend.sourceColor = BlendFactor::SourceAlpha;
    blend.destinationColor = BlendFactor::InverseSourceAlpha;
    const D3D11_BLEND_DESC nativeBlend = D3D11::ToNativeBlendDescription(blend);
    Expect(nativeBlend.RenderTarget[0].BlendEnable == TRUE, "D3D11 blend enable mismatch.");
    Expect(nativeBlend.RenderTarget[0].SrcBlend == D3D11_BLEND_SRC_ALPHA, "D3D11 source blend mismatch.");
    Expect(nativeBlend.RenderTarget[0].DestBlend == D3D11_BLEND_INV_SRC_ALPHA, "D3D11 destination blend mismatch.");
}
#endif

void TestVulkanConversions()
{
    using namespace Prism::RHI;

    Expect(Vulkan::ToNativeFormat(Format::Rgba16Float) == VK_FORMAT_R16G16B16A16_SFLOAT,
           "Vulkan HDR format mismatch.");
    const VkImageUsageFlags usage = Vulkan::ToNativeImageUsage(
        TextureUsage::ShaderResource | TextureUsage::RenderTarget | TextureUsage::CopySource);
    Expect((usage & VK_IMAGE_USAGE_SAMPLED_BIT) != 0, "Vulkan sampled-image usage missing.");
    Expect((usage & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT) != 0, "Vulkan color-attachment usage missing.");
    Expect((usage & VK_IMAGE_USAGE_TRANSFER_SRC_BIT) != 0, "Vulkan transfer-source usage missing.");

    const auto shaderStages = Vulkan::ToNativeResourceState(ResourceState::ShaderResource).pipelineStages;
    Expect(Vulkan::QueueCompatiblePipelineStages(shaderStages, CommandQueueType::Compute)
        == VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        "Dedicated Vulkan compute barriers retained unsupported graphics shader stages.");
    Expect(Vulkan::QueueCompatiblePipelineStages(shaderStages, CommandQueueType::Graphics) == shaderStages,
        "Graphics barriers lost their shader stages.");
    const Vulkan::ResourceStateMapping renderTarget = Vulkan::ToNativeResourceState(ResourceState::RenderTarget);
    Expect(renderTarget.imageLayout == VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
           "Vulkan render-target layout mismatch.");
    Expect((renderTarget.pipelineStages & VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT) != 0,
           "Vulkan render-target pipeline stage missing.");
    Expect((renderTarget.accessMask & VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT) != 0,
           "Vulkan render-target write access missing.");

    const Vulkan::ResourceStateMapping indirect =
        Vulkan::ToNativeResourceState(
            ResourceState::IndirectArgument);
    Expect(
        (indirect.pipelineStages
         & VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT)
            != 0
            && (indirect.accessMask
                & VK_ACCESS_INDIRECT_COMMAND_READ_BIT)
                != 0,
        "Vulkan indirect-draw synchronization mapping is incomplete.");

    const Vulkan::ResourceStateMapping depthRead = Vulkan::ToNativeResourceState(
        ResourceState::ShaderResource,
        true);
    Expect(depthRead.imageLayout == VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL,
           "Vulkan sampled-depth layout mismatch.");
    Expect(Vulkan::ToNativeAddressMode(AddressMode::ClampToBorder) == VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER,
           "Vulkan sampler address mode mismatch.");
    Expect(Vulkan::ToNativeCompareOperation(CompareOperation::LessEqual) == VK_COMPARE_OP_LESS_OR_EQUAL,
           "Vulkan comparison operation mismatch.");
    Expect(Vulkan::ToNativeCullMode(CullMode::Back) == VK_CULL_MODE_BACK_BIT,
           "Vulkan cull mode mismatch.");
    Expect(Vulkan::ToNativeShaderStage(ShaderStage::Pixel) == VK_SHADER_STAGE_FRAGMENT_BIT,
           "Vulkan shader stage mismatch.");
    Expect(
        Vulkan::ToNativeShaderStage(
            ShaderStage::RayGeneration)
            == VK_SHADER_STAGE_RAYGEN_BIT_KHR,
        "Vulkan ray-generation stage mismatch.");
    const Vulkan::ResourceStateMapping accelerationStructure =
        Vulkan::ToNativeResourceState(
            ResourceState::AccelerationStructure);
    Expect(
        (accelerationStructure.pipelineStages
         & VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR)
            != 0
            && (accelerationStructure.accessMask
                & VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR)
                != 0,
        "Vulkan acceleration-structure synchronization mapping is incomplete.");
}

void TestPipelineCacheTessellationIdentity()
{
    Prism::RHI::GraphicsPipelineDescription description{};
    description.topology = Prism::RHI::PrimitiveTopology::PatchList;
    description.patchControlPointCount = 3u;
    description.vertexShader.stage = Prism::RHI::ShaderStage::Vertex;
    description.vertexShader.bytecode = {1u, 2u, 3u};
    description.hullShader.stage = Prism::RHI::ShaderStage::Hull;
    description.hullShader.bytecode = {4u, 5u};
    description.domainShader.stage = Prism::RHI::ShaderStage::Domain;
    description.domainShader.bytecode = {6u, 7u};
    description.pixelShader.stage = Prism::RHI::ShaderStage::Pixel;
    description.pixelShader.bytecode = {8u};
    const std::string tessellatedIdentity =
        Prism::Renderer::PipelineCache::SerializeGraphicsIdentity(description);
    description.hullShader.bytecode[0] ^= 0xffu;
    const std::string changedIdentity =
        Prism::Renderer::PipelineCache::SerializeGraphicsIdentity(description);
    Expect(!tessellatedIdentity.empty()
            && tessellatedIdentity != changedIdentity,
        "Pipeline cache identity must include Hull/Domain bytecode.");
    description.hullShader.bytecode = {4u, 5u};
    description.topology = Prism::RHI::PrimitiveTopology::TriangleList;
    const std::string fallbackIdentity =
        Prism::Renderer::PipelineCache::SerializeGraphicsIdentity(description);
    Expect(fallbackIdentity != tessellatedIdentity,
        "Patch topology must participate in pipeline cache identity.");
}
} // namespace

int main()
{
    try
    {
        TestApiNames();
        TestSharedValidation();
        TestPublicRhiDescriptions();
        TestRayTracingDescriptions();
        TestPipelineDescriptions();
#if defined(PRISM_RENDER_HAS_D3D12)
        TestD3D12Conversions();
        TestD3D12SampledViews();
        TestD3D11Conversions();
#endif
        TestVulkanConversions();
        TestPipelineCacheTessellationIdentity();
#if defined(PRISM_RENDER_HAS_D3D12)
        std::cout << "PrismRender RHI translation tests passed for D3D12, D3D11, and Vulkan.\n";
#else
        std::cout << "PrismRender RHI translation tests passed for the common RHI and Vulkan.\n";
#endif
        return EXIT_SUCCESS;
    }
    catch (const std::exception& exception)
    {
        std::cerr << "RHI translation test failed: " << exception.what() << '\n';
        return EXIT_FAILURE;
    }
}
