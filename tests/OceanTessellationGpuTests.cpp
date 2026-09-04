#include "Asset/ShaderManager.h"
#include "Asset/Mesh.h"
#include "Platform/Window.h"
#include "RHI/ICommandContext.h"
#include "RHI/IFrameContext.h"
#include "RHI/IGraphicsDevice.h"
#include "RHI/IRenderBackend.h"
#include "RHI/RenderBackendFactory.h"
#include "RHI/ShaderLayoutBuilder.h"
#include "Renderer/Features/Ocean/WaterOpticsFeature.h"
#include "Renderer/Features/Ocean/WaterOpticalModel.h"
#include "Renderer/Pipeline/PipelineCache.h"

#include <cstdlib>
#include <array>
#include <cstddef>
#include <cmath>
#include <filesystem>
#include <iostream>
#include <memory>
#include <limits>
#include <stdexcept>
#include <string>

namespace
{
struct Float4
{
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
    float w = 0.0f;
};

struct RefractionFixtureInput
{
    Float4 uvNormal;
    Float4 depthsParameters;
    Float4 boundsParameters;
    Float4 fallbackColor;
    Float4 refractedColor;
};

struct RefractionFixtureOutput
{
    Float4 uvThicknessFallback;
    Float4 resolvedColor;
};

struct RayMarchFixtureInput
{
    Float4 uvDirection;
    Float4 depthParameters;
    Float4 marchParameters;
    Float4 depthSamples0;
    Float4 depthSamples1;
    Float4 fallbackColor;
    Float4 refractedColor;
};

struct WaterOpticalFixtureInput
{
    Float4 absorptionIor;
    Float4 scatteringPhase;
    Float4 responseParameters;
    Float4 strengths;
};

struct WaterOpticalFixtureOutput
{
    Float4 transmittanceFresnel;
    Float4 scatteringPhase;
    Float4 thinLayer;
    Float4 backlit;
};

struct CausticFixtureInput
{
    Float4 normalDepth;
    Float4 lightCoverage;
    Float4 extinctionIntensity;
};

struct VolumetricFixtureInput
{
    Float4 extinctionDistance;
    Float4 scatteringPhase;
};

static_assert(sizeof(RefractionFixtureInput) == 80u);
static_assert(sizeof(RefractionFixtureOutput) == 32u);
static_assert(sizeof(RayMarchFixtureInput) == 112u);
static_assert(sizeof(WaterOpticalFixtureInput) == 64u);
static_assert(sizeof(WaterOpticalFixtureOutput) == 64u);
static_assert(sizeof(CausticFixtureInput) == 48u);

Prism::RHI::GraphicsApi ParseApi(const int argc, char** argv)
{
    if (argc != 2)
        throw std::runtime_error("Usage: PrismOceanTessellationGpuTests <d3d12|vulkan>");
    const std::string api(argv[1]);
    if (api == "d3d12") return Prism::RHI::GraphicsApi::Direct3D12;
    if (api == "vulkan") return Prism::RHI::GraphicsApi::Vulkan;
    throw std::runtime_error("Unknown graphics API: " + api);
}
}

int main(int argc, char** argv)
{
    const char* phase = "argument parsing";
    try
    {
        const Prism::RHI::GraphicsApi api = ParseApi(argc, argv);
        phase = "backend initialization";
        Prism::Platform::Window window("Ocean Tessellation GPU Tests", 64u, 64u);
        std::unique_ptr<Prism::RHI::IRenderBackend> backend =
            Prism::RHI::CreateRenderBackend(api);
        backend->Initialize(window);
        Prism::RHI::IGraphicsDevice& device = backend->GetGraphicsDevice();
        Prism::Asset::ShaderManager shaderManager;
        Prism::Renderer::PipelineCache pipelineCache;
        const std::filesystem::path shaderDirectory =
            std::filesystem::path(PRISM_RENDER_SHADER_DIR) / "Ocean";
        const auto load = [&](const char* file, const char* entry,
                              const Prism::RHI::ShaderStage stage)
            -> const Prism::RHI::ShaderBinary&
        {
            return shaderManager.LoadShader(shaderDirectory / file, entry,
                stage, device.GetPreferredShaderBinaryFormat());
        };
        const Prism::RHI::ShaderBinary& vertex = load(
            "OceanTessellationVertex.slang", "OceanTessellationVS",
            Prism::RHI::ShaderStage::Vertex);
        const Prism::RHI::ShaderBinary& hull = load(
            "OceanHull.slang", "OceanHullMain", Prism::RHI::ShaderStage::Hull);
        const Prism::RHI::ShaderBinary& domain = load(
            "OceanDomain.slang", "OceanDomainMain", Prism::RHI::ShaderStage::Domain);
        const Prism::RHI::ShaderBinary& pixel = load(
            "OceanTessellationPixel.slang", "OceanTessellationPS",
            Prism::RHI::ShaderStage::Pixel);

        Prism::RHI::GraphicsPipelineDescription description{};
        description.vertexShader = vertex;
        description.hullShader = hull;
        description.domainShader = domain;
        description.pixelShader = pixel;
        description.descriptorSetLayout = device.CreateDescriptorSetLayout({});
        description.topology = Prism::RHI::PrimitiveTopology::PatchList;
        description.patchControlPointCount = 3u;
        description.colorFormats = {backend->GetFrameContext().GetBackBufferRhiFormat()};
        description.blendAttachments = {Prism::RHI::BlendAttachmentDescription{}};
        description.depthFormat = Prism::RHI::Format::Unknown;
        description.depthStencil.depthTestEnabled = false;
        description.depthStencil.depthWriteEnabled = false;
        const std::shared_ptr<Prism::RHI::IGraphicsPipeline> pipeline =
            pipelineCache.GetOrCreateGraphics(device,
                "Test.OceanTessellation.Triangle", description);
        Prism::RHI::IFrameContext& frame = backend->GetFrameContext();

        const std::filesystem::path surfacePath =
            std::filesystem::path(PRISM_RENDER_SHADER_DIR) / "Ocean"
            / "OceanSurface.slang";
        const Prism::RHI::ShaderBinary& surfaceVertex = shaderManager.LoadShader(
            surfacePath, "OceanVSIndexedMain", Prism::RHI::ShaderStage::Vertex,
            device.GetPreferredShaderBinaryFormat());
        const Prism::RHI::ShaderBinary& surfaceHull = shaderManager.LoadShader(
            surfacePath, "OceanHullMain", Prism::RHI::ShaderStage::Hull,
            device.GetPreferredShaderBinaryFormat());
        const Prism::RHI::ShaderBinary& surfaceDomain = shaderManager.LoadShader(
            surfacePath, "OceanDomainMain", Prism::RHI::ShaderStage::Domain,
            device.GetPreferredShaderBinaryFormat());
        const Prism::RHI::ShaderBinary& surfacePixel = shaderManager.LoadShader(
            surfacePath, "OceanPSMain", Prism::RHI::ShaderStage::Pixel,
            device.GetPreferredShaderBinaryFormat());
        const std::array surfaceStages{
            Prism::RHI::ShaderLayoutStage{&surfaceVertex.reflection,
                Prism::RHI::ShaderStage::Vertex},
            Prism::RHI::ShaderLayoutStage{&surfaceHull.reflection,
                Prism::RHI::ShaderStage::Hull},
            Prism::RHI::ShaderLayoutStage{&surfaceDomain.reflection,
                Prism::RHI::ShaderStage::Domain},
            Prism::RHI::ShaderLayoutStage{&surfacePixel.reflection,
                Prism::RHI::ShaderStage::Pixel}};
        const std::array<std::uint32_t, 1> surfaceDynamicBindings{1u};
        Prism::RHI::GraphicsPipelineDescription surfaceDescription{};
        surfaceDescription.vertexShader = surfaceVertex;
        surfaceDescription.hullShader = surfaceHull;
        surfaceDescription.domainShader = surfaceDomain;
        surfaceDescription.pixelShader = surfacePixel;
        surfaceDescription.descriptorSetLayout = device.CreateDescriptorSetLayout(
            Prism::RHI::BuildDescriptorSetLayout(
                surfaceStages, surfaceDynamicBindings));
        surfaceDescription.vertexBindings = {{0u, sizeof(Prism::Asset::MeshVertex),
            Prism::RHI::VertexInputRate::PerVertex}};
        surfaceDescription.vertexAttributes = {
            {0u, 0u, Prism::RHI::VertexElementFormat::Float3,
                static_cast<std::uint32_t>(offsetof(Prism::Asset::MeshVertex, position))},
            {1u, 0u, Prism::RHI::VertexElementFormat::Float4,
                static_cast<std::uint32_t>(offsetof(Prism::Asset::MeshVertex, color))},
            {2u, 0u, Prism::RHI::VertexElementFormat::Float3,
                static_cast<std::uint32_t>(offsetof(Prism::Asset::MeshVertex, normal))},
            {3u, 0u, Prism::RHI::VertexElementFormat::Float2,
                static_cast<std::uint32_t>(offsetof(Prism::Asset::MeshVertex, texCoord))},
            {4u, 0u, Prism::RHI::VertexElementFormat::Float4,
                static_cast<std::uint32_t>(offsetof(Prism::Asset::MeshVertex, tangent))}};
        surfaceDescription.topology = Prism::RHI::PrimitiveTopology::PatchList;
        surfaceDescription.patchControlPointCount = 3u;
        surfaceDescription.colorFormats = {frame.GetBackBufferRhiFormat()};
        surfaceDescription.blendAttachments = {Prism::RHI::BlendAttachmentDescription{}};
        surfaceDescription.depthFormat = Prism::RHI::Format::Unknown;
        surfaceDescription.depthStencil.depthTestEnabled = false;
        surfaceDescription.depthStencil.depthWriteEnabled = false;
        const auto surfacePipeline = pipelineCache.GetOrCreateGraphics(device,
            "Test.OceanSurface.Tessellation", surfaceDescription);
        if (surfacePipeline == nullptr)
            throw std::runtime_error("Ocean surface tessellation PSO was not created.");

        const Prism::RHI::ShaderBinary& waterVisibilityPixel =
            shaderManager.LoadShader(
                std::filesystem::path(PRISM_RENDER_SHADER_DIR) / "Ocean"
                    / "WaterVisibility.slang",
                "WaterVisibilityPS",
                Prism::RHI::ShaderStage::Pixel,
                device.GetPreferredShaderBinaryFormat());
        const std::array waterVisibilityStages{
            Prism::RHI::ShaderLayoutStage{&surfaceVertex.reflection,
                Prism::RHI::ShaderStage::Vertex},
            Prism::RHI::ShaderLayoutStage{&surfaceHull.reflection,
                Prism::RHI::ShaderStage::Hull},
            Prism::RHI::ShaderLayoutStage{&surfaceDomain.reflection,
                Prism::RHI::ShaderStage::Domain},
            Prism::RHI::ShaderLayoutStage{&waterVisibilityPixel.reflection,
                Prism::RHI::ShaderStage::Pixel}};
        Prism::RHI::GraphicsPipelineDescription waterVisibilityDescription =
            surfaceDescription;
        waterVisibilityDescription.pixelShader = waterVisibilityPixel;
        waterVisibilityDescription.descriptorSetLayout =
            device.CreateDescriptorSetLayout(
                Prism::RHI::BuildDescriptorSetLayout(
                    waterVisibilityStages,
                    surfaceDynamicBindings));
        waterVisibilityDescription.colorFormats = {
            Prism::RHI::Format::Rgba16Float,
            Prism::RHI::Format::Rgba8Unorm,
            Prism::RHI::Format::Rgba8Unorm,
            Prism::RHI::Format::Rg16Float};
        waterVisibilityDescription.blendAttachments.assign(
            waterVisibilityDescription.colorFormats.size(),
            Prism::RHI::BlendAttachmentDescription{});
        waterVisibilityDescription.depthFormat = Prism::RHI::Format::D32Float;
        waterVisibilityDescription.depthStencil.depthTestEnabled = true;
        waterVisibilityDescription.depthStencil.depthWriteEnabled = true;
        waterVisibilityDescription.depthStencil.depthComparison =
            Prism::RHI::CompareOperation::Less;
        const auto waterVisibilityPipeline =
            pipelineCache.GetOrCreateGraphics(
                device,
                "Test.WaterVisibility.Tessellation",
                waterVisibilityDescription);
        if (waterVisibilityPipeline == nullptr)
            throw std::runtime_error(
                "Water visibility four-target/depth PSO was not created.");

        Prism::RHI::TextureViewDescription sampledDepthDescription{};
        sampledDepthDescription.type = Prism::RHI::TextureViewType::Sampled;
        const auto opaqueDepthView = device.CreateTextureView(
            frame.GetDepthStencilTexture(), sampledDepthDescription);
        Prism::Renderer::WaterOpticsFeature waterOptics;
        phase = "water optics lifecycle fixture";
        waterOptics.Initialize(
            device,
            shaderManager,
            pipelineCache,
            std::filesystem::path(PRISM_RENDER_SHADER_DIR),
            device.GetPreferredShaderBinaryFormat(),
            frame.GetFramesInFlight(),
            waterVisibilityDescription.descriptorSetLayout,
            true);
        if (!waterOptics.IsInitialized())
            throw std::runtime_error("Water optics feature did not initialize.");

        Prism::Renderer::WaterOpticsSettings waterSettings =
            Prism::Renderer::WaterOpticsSettings::HpWaterReference();
        waterSettings.caustics.resolution = 64u;
        waterOptics.EnsureResources(device, 64u, 64u,
            frame.GetDepthStencilTexture(), opaqueDepthView,
            frame.GetDepthStencilTexture(), opaqueDepthView,
            waterSettings, 1u);
        const auto initialStatistics = waterOptics.GetStatistics();
        if (!initialStatistics.resourcesReady
            || !initialStatistics.historyValid
            || initialStatistics.resourceGeneration != 1u
            || initialStatistics.allocatedMegabytes <= 0.0f
            || initialStatistics.effectiveRefractionWidth != 48u
            || initialStatistics.effectiveRefractionHeight != 48u
            || std::abs(initialStatistics.effectiveRefractionResolutionScale
                    - 0.75f) > 1.0e-6f)
        {
            throw std::runtime_error(
                "Water optics initial resource statistics are invalid.");
        }
        const std::array captureStages{
            Prism::Renderer::WaterOpticsCaptureStage::Depth,
            Prism::Renderer::WaterOpticsCaptureStage::Mask,
            Prism::Renderer::WaterOpticsCaptureStage::GBuffer0,
            Prism::Renderer::WaterOpticsCaptureStage::GBuffer1,
            Prism::Renderer::WaterOpticsCaptureStage::GBuffer2,
            Prism::Renderer::WaterOpticsCaptureStage::Refraction,
            Prism::Renderer::WaterOpticsCaptureStage::CausticsNear,
            Prism::Renderer::WaterOpticsCaptureStage::CausticsMiddle,
            Prism::Renderer::WaterOpticsCaptureStage::VolumetricCurrent,
            Prism::Renderer::WaterOpticsCaptureStage::VolumetricHistory,
            Prism::Renderer::WaterOpticsCaptureStage::VolumetricReconstruction,
            Prism::Renderer::WaterOpticsCaptureStage::Composite};
        for (const auto stage : captureStages)
        {
            if (waterOptics.GetCaptureView(stage) == nullptr)
                throw std::runtime_error(
                    "Water optics capture hook returned no view.");
        }
        const auto captureStatistics = waterOptics.GetStatistics();
        if (captureStatistics.resourceGeneration
                != initialStatistics.resourceGeneration
            || captureStatistics.historyVersion
                != initialStatistics.historyVersion
            || captureStatistics.historyValid
                != initialStatistics.historyValid)
        {
            throw std::runtime_error(
                "Water optics capture mutated resources or history.");
        }

        waterOptics.ResetHistory();
        if (waterOptics.GetStatistics().historyValid)
            throw std::runtime_error("Water optics reset retained history.");
        const std::uint64_t resetVersion =
            waterOptics.GetStatistics().historyVersion;
        waterOptics.NotifySceneChanged();
        if (waterOptics.GetStatistics().historyValid
            || waterOptics.GetStatistics().historyVersion <= resetVersion)
        {
            throw std::runtime_error(
                "Water optics scene switch did not invalidate history.");
        }

        waterSettings.refraction.highPrecision = true;
        waterSettings.refraction.rayMarchSampleCount = 32u;
        waterOptics.EnsureResources(device, 96u, 64u,
            frame.GetDepthStencilTexture(), opaqueDepthView,
            frame.GetDepthStencilTexture(), opaqueDepthView,
            waterSettings, 2u);
        const auto resizedStatistics = waterOptics.GetStatistics();
        if (resizedStatistics.resourceGeneration != 2u
            || resizedStatistics.retiredResourceSets != 1u
            || !resizedStatistics.historyValid
            || !resizedStatistics.rayMarchEnabled
            || resizedStatistics.effectiveRayMarchSamples != 8u
            || resizedStatistics.refractionDispatchCount != 1u
            || resizedStatistics.effectiveRefractionWidth != 72u
            || resizedStatistics.effectiveRefractionHeight != 48u)
        {
            throw std::runtime_error(
                "Water optics resize did not retire and recreate resources.");
        }
        waterSettings.quality = Prism::Renderer::WaterOpticsQuality::Normal;
        waterOptics.EnsureResources(device, 96u, 64u,
            frame.GetDepthStencilTexture(), opaqueDepthView,
            frame.GetDepthStencilTexture(), opaqueDepthView,
            waterSettings, 3u);
        const auto normalQualityStatistics = waterOptics.GetStatistics();
        if (resizedStatistics.effectiveVolumetricWidth != 48u
            || resizedStatistics.effectiveVolumetricHeight != 32u
            || normalQualityStatistics.effectiveVolumetricWidth != 24u
            || normalQualityStatistics.effectiveVolumetricHeight != 16u)
            throw std::runtime_error("Volume half/quarter quality extents are inconsistent.");
        if (normalQualityStatistics.resourceGeneration != 3u
            || normalQualityStatistics.effectiveRefractionWidth != 48u
            || normalQualityStatistics.effectiveRefractionHeight != 32u
            || normalQualityStatistics.rayMarchEnabled
            || normalQualityStatistics.effectiveRayMarchSamples != 0u)
        {
            throw std::runtime_error(
                "Normal water-optics quality did not reduce only optical resolution and samples.");
        }
        waterOptics.UpdateGpuTiming(0.25f, true);
        waterOptics.UpdateCompositeGpuTiming(0.15f, true);
        if (!waterOptics.GetStatistics().gpuTimingAvailable
            || std::abs(waterOptics.GetStatistics().refractionMilliseconds
                - 0.25f) > 1.0e-6f
            || std::abs(waterOptics.GetStatistics().compositeMilliseconds
                - 0.15f) > 1.0e-6f)
        {
            throw std::runtime_error(
                "Water optics refraction timing was not reported.");
        }
        waterOptics.EndFrame(3u + frame.GetFramesInFlight(), true);
        if (waterOptics.GetStatistics().retiredResourceSets != 0u)
            throw std::runtime_error("Water optics retired resources leaked.");
        waterOptics.ReleaseSizeDependentResources(8u, true);
        if (waterOptics.HasResources()
            || waterOptics.GetStatistics().retiredResourceSets != 0u)
        {
            throw std::runtime_error(
                "Water optics size-dependent resources were not released.");
        }
        waterOptics.Shutdown();
        if (waterOptics.IsInitialized())
            throw std::runtime_error("Water optics feature did not shut down.");

        phase = "refraction fixture resource creation";
        const Float4 validUvNormal{0.5f, 0.5f, 1.0f, 0.0f};
        const Float4 validDepths{2.0f, 7.0f, 7.0f, 0.2f};
        const Float4 bounds{0.1f, 0.01f, 10.0f, 0.01f};
        const Float4 fallbackColor{1.0f, 0.0f, 0.0f, 1.0f};
        const Float4 refractedColor{0.0f, 1.0f, 0.0f, 1.0f};
        const std::array<RefractionFixtureInput, 5> refractionInputs{{
            {validUvNormal, validDepths, bounds,
                fallbackColor, refractedColor},
            {{0.98f, 0.5f, 1.0f, 0.0f}, validDepths, bounds,
                fallbackColor, refractedColor},
            {validUvNormal, {2.0f, 7.0f, 2.005f, 0.2f}, bounds,
                fallbackColor, refractedColor},
            {validUvNormal, {2.0f, 2.005f, 7.0f, 0.2f}, bounds,
                fallbackColor, refractedColor},
            {{0.5f, 0.5f,
                 std::numeric_limits<float>::quiet_NaN(), 0.0f},
                validDepths, bounds, fallbackColor, refractedColor}}};
        Prism::RHI::BufferDescription refractionInputDescription{};
        refractionInputDescription.size = sizeof(refractionInputs);
        refractionInputDescription.stride = sizeof(RefractionFixtureInput);
        refractionInputDescription.usage = Prism::RHI::BufferUsage::Storage
            | Prism::RHI::BufferUsage::CopyDestination;
        phase = "refraction input buffer creation";
        const auto refractionInputBuffer = device.CreateBuffer(
            refractionInputDescription, refractionInputs.data());
        Prism::RHI::BufferDescription refractionOutputDescription{};
        refractionOutputDescription.size =
            sizeof(RefractionFixtureOutput) * refractionInputs.size();
        refractionOutputDescription.stride = sizeof(RefractionFixtureOutput);
        refractionOutputDescription.usage = Prism::RHI::BufferUsage::Storage
            | Prism::RHI::BufferUsage::CopySource
            | Prism::RHI::BufferUsage::CopyDestination;
        phase = "refraction output buffer creation";
        const auto refractionOutputBuffer =
            device.CreateBuffer(refractionOutputDescription);
        Prism::RHI::BufferDescription refractionReadbackDescription =
            refractionOutputDescription;
        refractionReadbackDescription.usage =
            Prism::RHI::BufferUsage::CopyDestination;
        refractionReadbackDescription.memoryAccess =
            Prism::RHI::MemoryAccess::GpuToCpu;
        phase = "refraction readback buffer creation";
        const auto refractionReadback =
            device.CreateBuffer(refractionReadbackDescription);
        phase = "refraction fixture shader loading";
        const Prism::RHI::ShaderBinary& refractionFixtureShader =
            shaderManager.LoadShader(
                std::filesystem::path(PRISM_RENDER_SHADER_DIR) / "Ocean"
                    / "WaterOptics.slang",
                "WaterRefractionAnalyticFixtureCS",
                Prism::RHI::ShaderStage::Compute,
                device.GetPreferredShaderBinaryFormat());
        phase = "refraction fixture pipeline creation";
        const std::array refractionStages{Prism::RHI::ShaderLayoutStage{
            &refractionFixtureShader.reflection,
            Prism::RHI::ShaderStage::Compute}};
        const auto refractionLayout = device.CreateDescriptorSetLayout(
            Prism::RHI::BuildDescriptorSetLayout(refractionStages, {}));
        Prism::RHI::ComputePipelineDescription refractionPipelineDescription{};
        refractionPipelineDescription.computeShader = refractionFixtureShader;
        refractionPipelineDescription.descriptorSetLayout = refractionLayout;
        const auto refractionPipeline = pipelineCache.GetOrCreateCompute(
            device, "Test.WaterRefraction.AnalyticFallbacks",
            refractionPipelineDescription);
        const auto refractionSet = device.CreateDescriptorSet(refractionLayout);
        refractionSet->WriteBuffer(16u, refractionInputBuffer);
        refractionSet->WriteBuffer(32u, refractionOutputBuffer);

        phase = "ray-march fixture resource creation";
        const Float4 rayDepthParameters{2.0f, 8.0f, 0.1f, 0.01f};
        const Float4 rayMarchParameters{10.0f, 1.6f, 0.25f, 0.01f};
        const Float4 farDepths{20.0f, 20.0f, 20.0f, 20.0f};
        const Float4 selfDepths{2.001f, 2.001f, 2.001f, 2.001f};
        const std::array<RayMarchFixtureInput, 3> rayMarchInputs{{
            {{0.5f, 0.5f, 1.0f, 0.0f}, rayDepthParameters,
                rayMarchParameters, {20.0f, 4.0f, 20.0f, 20.0f},
                farDepths, fallbackColor, refractedColor},
            {{0.5f, 0.5f, 1.0f, 0.0f}, rayDepthParameters,
                rayMarchParameters, selfDepths, selfDepths,
                fallbackColor, refractedColor},
            {{0.5f, 0.5f,
                 std::numeric_limits<float>::quiet_NaN(), 0.0f},
                rayDepthParameters, rayMarchParameters, farDepths,
                farDepths, fallbackColor, refractedColor}}};
        Prism::RHI::BufferDescription rayMarchInputDescription{};
        rayMarchInputDescription.size = sizeof(rayMarchInputs);
        rayMarchInputDescription.stride = sizeof(RayMarchFixtureInput);
        rayMarchInputDescription.usage = Prism::RHI::BufferUsage::Storage
            | Prism::RHI::BufferUsage::CopyDestination;
        const auto rayMarchInputBuffer = device.CreateBuffer(
            rayMarchInputDescription, rayMarchInputs.data());
        Prism::RHI::BufferDescription rayMarchOutputDescription{};
        rayMarchOutputDescription.size = sizeof(RefractionFixtureOutput)
            * rayMarchInputs.size();
        rayMarchOutputDescription.stride = sizeof(RefractionFixtureOutput);
        rayMarchOutputDescription.usage = Prism::RHI::BufferUsage::Storage
            | Prism::RHI::BufferUsage::CopySource
            | Prism::RHI::BufferUsage::CopyDestination;
        const auto rayMarchOutputBuffer = device.CreateBuffer(
            rayMarchOutputDescription);
        Prism::RHI::BufferDescription rayMarchReadbackDescription =
            rayMarchOutputDescription;
        rayMarchReadbackDescription.usage =
            Prism::RHI::BufferUsage::CopyDestination;
        rayMarchReadbackDescription.memoryAccess =
            Prism::RHI::MemoryAccess::GpuToCpu;
        const auto rayMarchReadback = device.CreateBuffer(
            rayMarchReadbackDescription);
        const Prism::RHI::ShaderBinary& rayMarchFixtureShader =
            shaderManager.LoadShader(
                std::filesystem::path(PRISM_RENDER_SHADER_DIR) / "Ocean"
                    / "WaterOptics.slang",
                "WaterRefractionRayMarchFixtureCS",
                Prism::RHI::ShaderStage::Compute,
                device.GetPreferredShaderBinaryFormat());
        const std::array rayMarchStages{Prism::RHI::ShaderLayoutStage{
            &rayMarchFixtureShader.reflection,
            Prism::RHI::ShaderStage::Compute}};
        const auto rayMarchLayout = device.CreateDescriptorSetLayout(
            Prism::RHI::BuildDescriptorSetLayout(rayMarchStages, {}));
        Prism::RHI::ComputePipelineDescription rayMarchPipelineDescription{};
        rayMarchPipelineDescription.computeShader = rayMarchFixtureShader;
        rayMarchPipelineDescription.descriptorSetLayout = rayMarchLayout;
        const auto rayMarchPipeline = pipelineCache.GetOrCreateCompute(
            device, "Test.WaterRefraction.RayMarchFallbacks",
            rayMarchPipelineDescription);
        const auto rayMarchSet = device.CreateDescriptorSet(rayMarchLayout);
        rayMarchSet->WriteBuffer(17u, rayMarchInputBuffer);
        rayMarchSet->WriteBuffer(33u, rayMarchOutputBuffer);

        const float quietNan =
            std::numeric_limits<float>::quiet_NaN();
        const float infinity =
            std::numeric_limits<float>::infinity();
        const std::array<WaterOpticalFixtureInput, 4> opticalInputs{{
            {{0.0f, 0.0f, 0.0f, 1.333f},
                {0.0f, 0.0f, 0.0f, 0.8f},
                {1.0f, 1.0f, 20.0f, 0.0f},
                {0.35f, 0.2f, 0.0f, 0.0f}},
            {{0.16f, 0.055f, 0.025f, 1.333f},
                {0.018f, 0.065f, 0.09f, 0.8f},
                {1.0f, 1.0f, 1.0f, 0.0f},
                {0.35f, 0.2f, 0.0f, 0.0f}},
            {{0.16f, 0.055f, 0.025f, 1.333f},
                {0.018f, 0.065f, 0.09f, 0.8f},
                {0.0f, -1.0f, 20.0f, 0.0f},
                {0.35f, 0.2f, 0.0f, 0.0f}},
            {{quietNan, 0.055f, 0.025f, infinity},
                {0.018f, infinity, 0.09f, quietNan},
                {quietNan, -infinity, infinity, 0.0f},
                {infinity, quietNan, 0.0f, 0.0f}}}};
        Prism::RHI::BufferDescription opticalInputDescription{};
        opticalInputDescription.size = sizeof(opticalInputs);
        opticalInputDescription.stride = sizeof(WaterOpticalFixtureInput);
        opticalInputDescription.usage = Prism::RHI::BufferUsage::Storage
            | Prism::RHI::BufferUsage::CopyDestination;
        const auto opticalInputBuffer = device.CreateBuffer(
            opticalInputDescription, opticalInputs.data());
        Prism::RHI::BufferDescription opticalOutputDescription{};
        opticalOutputDescription.size =
            sizeof(WaterOpticalFixtureOutput) * opticalInputs.size();
        opticalOutputDescription.stride = sizeof(WaterOpticalFixtureOutput);
        opticalOutputDescription.usage = Prism::RHI::BufferUsage::Storage
            | Prism::RHI::BufferUsage::CopySource
            | Prism::RHI::BufferUsage::CopyDestination;
        const auto opticalOutputBuffer = device.CreateBuffer(
            opticalOutputDescription);
        Prism::RHI::BufferDescription opticalReadbackDescription =
            opticalOutputDescription;
        opticalReadbackDescription.usage =
            Prism::RHI::BufferUsage::CopyDestination;
        opticalReadbackDescription.memoryAccess =
            Prism::RHI::MemoryAccess::GpuToCpu;
        const auto opticalReadback = device.CreateBuffer(
            opticalReadbackDescription);
        const Prism::RHI::ShaderBinary& opticalFixtureShader =
            shaderManager.LoadShader(
                std::filesystem::path(PRISM_RENDER_SHADER_DIR) / "Ocean"
                    / "WaterOptics.slang",
                "WaterOpticalAnalyticFixtureCS",
                Prism::RHI::ShaderStage::Compute,
                device.GetPreferredShaderBinaryFormat());
        const std::array opticalStages{Prism::RHI::ShaderLayoutStage{
            &opticalFixtureShader.reflection,
            Prism::RHI::ShaderStage::Compute}};
        const auto opticalLayout = device.CreateDescriptorSetLayout(
            Prism::RHI::BuildDescriptorSetLayout(opticalStages, {}));
        Prism::RHI::ComputePipelineDescription opticalPipelineDescription{};
        opticalPipelineDescription.computeShader = opticalFixtureShader;
        opticalPipelineDescription.descriptorSetLayout = opticalLayout;
        const auto opticalPipeline = pipelineCache.GetOrCreateCompute(
            device, "Test.WaterOptical.AnalyticAgreement",
            opticalPipelineDescription);
        const auto opticalSet = device.CreateDescriptorSet(opticalLayout);
        opticalSet->WriteBuffer(18u, opticalInputBuffer);
        opticalSet->WriteBuffer(35u, opticalOutputBuffer);

        phase = "caustic fixture resource creation";
        const std::array<CausticFixtureInput, 5> causticInputs{{
            {{0.0f, 1.0f, 0.0f, 2.0f},
                {0.0f, -1.0f, 0.0f, 1.0f},
                {0.1f, 2.0f, 0.96f, 1.04f}},
            {{0.8f, 0.2f, 0.0f, 2.0f},
                {0.0f, -1.0f, 0.0f, 1.0f},
                {0.1f, 2.0f, 0.96f, 1.04f}},
            {{0.0f, 1.0f, 0.0f, -0.1f},
                {0.0f, -1.0f, 0.0f, 1.0f},
                {0.1f, 2.0f, 0.96f, 1.04f}},
            {{0.0f, 1.0f, 0.0f, 2.0f},
                {0.0f, -1.0f, 0.0f, 0.0f},
                {0.1f, 2.0f, 0.96f, 1.04f}},
            {{quietNan, 1.0f, 0.0f, 2.0f},
                {0.0f, -1.0f, 0.0f, 1.0f},
                {0.1f, 2.0f, 0.96f, 1.04f}}}};
        Prism::RHI::BufferDescription causticInputDescription{};
        causticInputDescription.size = sizeof(causticInputs);
        causticInputDescription.stride = sizeof(CausticFixtureInput);
        causticInputDescription.usage = Prism::RHI::BufferUsage::Storage
            | Prism::RHI::BufferUsage::CopyDestination;
        const auto causticInputBuffer = device.CreateBuffer(
            causticInputDescription, causticInputs.data());
        Prism::RHI::BufferDescription causticOutputDescription{};
        causticOutputDescription.size = sizeof(Float4)
            * causticInputs.size();
        causticOutputDescription.stride = sizeof(Float4);
        causticOutputDescription.usage = Prism::RHI::BufferUsage::Storage
            | Prism::RHI::BufferUsage::CopySource
            | Prism::RHI::BufferUsage::CopyDestination;
        const auto causticOutputBuffer = device.CreateBuffer(
            causticOutputDescription);
        Prism::RHI::BufferDescription causticReadbackDescription =
            causticOutputDescription;
        causticReadbackDescription.usage =
            Prism::RHI::BufferUsage::CopyDestination;
        causticReadbackDescription.memoryAccess =
            Prism::RHI::MemoryAccess::GpuToCpu;
        const auto causticReadback = device.CreateBuffer(
            causticReadbackDescription);
        const Prism::RHI::ShaderBinary& causticFixtureShader =
            shaderManager.LoadShader(
                std::filesystem::path(PRISM_RENDER_SHADER_DIR) / "Ocean"
                    / "WaterCaustics.slang",
                "WaterCausticAnalyticFixtureCS",
                Prism::RHI::ShaderStage::Compute,
                device.GetPreferredShaderBinaryFormat());
        const std::array causticStages{Prism::RHI::ShaderLayoutStage{
            &causticFixtureShader.reflection,
            Prism::RHI::ShaderStage::Compute}};
        const auto causticLayout = device.CreateDescriptorSetLayout(
            Prism::RHI::BuildDescriptorSetLayout(causticStages, {}));
        Prism::RHI::ComputePipelineDescription causticPipelineDescription{};
        causticPipelineDescription.computeShader = causticFixtureShader;
        causticPipelineDescription.descriptorSetLayout = causticLayout;
        const auto causticPipeline = pipelineCache.GetOrCreateCompute(
            device, "Test.WaterCaustics.BoundedReceivers",
            causticPipelineDescription);
        const auto causticSet = device.CreateDescriptorSet(causticLayout);
        causticSet->WriteBuffer(18u, causticInputBuffer);
        causticSet->WriteBuffer(34u, causticOutputBuffer);

        phase = "volumetric fixture resource creation";
        const std::array<VolumetricFixtureInput, 5> volumetricInputs{{
            {{0.1f, 0.2f, 0.3f, 8.0f}, {0.02f, 0.04f, 0.06f, 0.5f}},
            {{0.1f, 0.2f, 0.3f, 0.0f}, {0.02f, 0.04f, 0.06f, 0.5f}},
            {{0.1f, 0.2f, 0.3f, 8.0f}, {0.02f, 0.04f, 0.06f, 0.0f}},
            {{0.0f, 0.0f, 0.0f, 8.0f}, {0.02f, 0.04f, 0.06f, 0.5f}},
            {{0.1f, 0.2f, 0.3f, 10000.0f}, {0.02f, 0.04f, 0.06f, 0.5f}}}};
        auto volumetricInputDescription = causticInputDescription;
        volumetricInputDescription.size = sizeof(volumetricInputs);
        volumetricInputDescription.stride = sizeof(VolumetricFixtureInput);
        const auto volumetricInputBuffer = device.CreateBuffer(
            volumetricInputDescription, volumetricInputs.data());
        const auto volumetricOutputBuffer = device.CreateBuffer(
            causticOutputDescription);
        const auto volumetricReadback = device.CreateBuffer(
            causticReadbackDescription);
        const auto& volumetricShader = shaderManager.LoadShader(
            std::filesystem::path(PRISM_RENDER_SHADER_DIR) / "Ocean"
                / "WaterVolumetrics.slang",
            "WaterVolumetricAnalyticFixtureCS", Prism::RHI::ShaderStage::Compute,
            device.GetPreferredShaderBinaryFormat());
        const std::array volumetricStages{Prism::RHI::ShaderLayoutStage{
            &volumetricShader.reflection, Prism::RHI::ShaderStage::Compute}};
        const auto volumetricLayout = device.CreateDescriptorSetLayout(
            Prism::RHI::BuildDescriptorSetLayout(volumetricStages, {}));
        Prism::RHI::ComputePipelineDescription volumetricDescription{};
        volumetricDescription.computeShader = volumetricShader;
        volumetricDescription.descriptorSetLayout = volumetricLayout;
        const auto volumetricPipeline = pipelineCache.GetOrCreateCompute(
            device, "Test.WaterVolumetric.Integration", volumetricDescription);
        const auto volumetricSet = device.CreateDescriptorSet(volumetricLayout);
        volumetricSet->WriteBuffer(22u, volumetricInputBuffer);
        volumetricSet->WriteBuffer(33u, volumetricOutputBuffer);

        phase = "refraction fixture frame recording";
        if (frame.BeginFrame() != Prism::RHI::FrameResult::Ready)
            throw std::runtime_error("Tessellation test could not begin a frame.");
        Prism::RHI::ICommandContext& commandContext = backend->GetCommandContext();
        // All five inputs have initialData; outputs are allocated without it.
        // D3D12 uploads restore Storage buffers to UAV. Vulkan uploads leave
        // a transfer write to publish; untouched output buffers have no prior access.
        const auto inputState = api == Prism::RHI::GraphicsApi::Direct3D12
            ? Prism::RHI::ResourceState::UnorderedAccess
            : Prism::RHI::ResourceState::CopyDestination;
        const auto outputState = api == Prism::RHI::GraphicsApi::Direct3D12
            ? Prism::RHI::ResourceState::UnorderedAccess
            : Prism::RHI::ResourceState::Undefined;
        commandContext.BufferBarrier({refractionInputBuffer.get(),
            inputState,
            Prism::RHI::ResourceState::ShaderResource});
        commandContext.BufferBarrier({refractionOutputBuffer.get(),
            outputState,
            Prism::RHI::ResourceState::UnorderedAccess});
        commandContext.BindComputePipeline(*refractionPipeline);
        commandContext.BindDescriptorSet(*refractionSet);
        commandContext.Dispatch(1u, 1u, 1u);
        commandContext.BufferBarrier({refractionOutputBuffer.get(),
            Prism::RHI::ResourceState::UnorderedAccess,
            Prism::RHI::ResourceState::CopySource});
        commandContext.CopyBuffer(*refractionOutputBuffer,
            *refractionReadback, refractionOutputDescription.size);
        commandContext.BufferBarrier({rayMarchInputBuffer.get(),
            inputState,
            Prism::RHI::ResourceState::ShaderResource});
        commandContext.BufferBarrier({rayMarchOutputBuffer.get(),
            outputState,
            Prism::RHI::ResourceState::UnorderedAccess});
        commandContext.BindComputePipeline(*rayMarchPipeline);
        commandContext.BindDescriptorSet(*rayMarchSet);
        commandContext.Dispatch(1u, 1u, 1u);
        commandContext.BufferBarrier({rayMarchOutputBuffer.get(),
            Prism::RHI::ResourceState::UnorderedAccess,
            Prism::RHI::ResourceState::CopySource});
        commandContext.CopyBuffer(*rayMarchOutputBuffer,
            *rayMarchReadback, rayMarchOutputDescription.size);
        commandContext.BufferBarrier({opticalInputBuffer.get(),
            inputState,
            Prism::RHI::ResourceState::ShaderResource});
        commandContext.BufferBarrier({opticalOutputBuffer.get(),
            outputState,
            Prism::RHI::ResourceState::UnorderedAccess});
        commandContext.BindComputePipeline(*opticalPipeline);
        commandContext.BindDescriptorSet(*opticalSet);
        commandContext.Dispatch(1u, 1u, 1u);
        commandContext.BufferBarrier({opticalOutputBuffer.get(),
            Prism::RHI::ResourceState::UnorderedAccess,
            Prism::RHI::ResourceState::CopySource});
        commandContext.CopyBuffer(*opticalOutputBuffer,
            *opticalReadback, opticalOutputDescription.size);
        commandContext.BufferBarrier({causticInputBuffer.get(),
            inputState,
            Prism::RHI::ResourceState::ShaderResource});
        commandContext.BufferBarrier({causticOutputBuffer.get(),
            outputState,
            Prism::RHI::ResourceState::UnorderedAccess});
        commandContext.BindComputePipeline(*causticPipeline);
        commandContext.BindDescriptorSet(*causticSet);
        commandContext.Dispatch(1u, 1u, 1u);
        commandContext.BufferBarrier({causticOutputBuffer.get(),
            Prism::RHI::ResourceState::UnorderedAccess,
            Prism::RHI::ResourceState::CopySource});
        commandContext.CopyBuffer(*causticOutputBuffer,
            *causticReadback, causticOutputDescription.size);
        commandContext.BufferBarrier({volumetricInputBuffer.get(),
            inputState,
            Prism::RHI::ResourceState::ShaderResource});
        commandContext.BufferBarrier({volumetricOutputBuffer.get(),
            outputState,
            Prism::RHI::ResourceState::UnorderedAccess});
        commandContext.BindComputePipeline(*volumetricPipeline);
        commandContext.BindDescriptorSet(*volumetricSet);
        commandContext.Dispatch(1u, 1u, 1u);
        commandContext.BufferBarrier({volumetricOutputBuffer.get(),
            Prism::RHI::ResourceState::UnorderedAccess,
            Prism::RHI::ResourceState::CopySource});
        commandContext.CopyBuffer(*volumetricOutputBuffer,
            *volumetricReadback, causticOutputDescription.size);
        Prism::RHI::RenderingInfo rendering{};
        rendering.width = frame.GetFrameWidth();
        rendering.height = frame.GetFrameHeight();
        Prism::RHI::RenderingAttachment color{};
        color.view = &frame.GetCurrentBackBufferView();
        color.loadOperation = Prism::RHI::LoadOperation::Clear;
        color.storeOperation = Prism::RHI::StoreOperation::Store;
        color.clearColor = {0.02f, 0.04f, 0.06f, 1.0f};
        // D3D12 BeginFrame performs this transition; Vulkan BeginRendering owns it.
        color.stateBefore = api == Prism::RHI::GraphicsApi::Vulkan
            ? Prism::RHI::ResourceState::Present
            : Prism::RHI::ResourceState::RenderTarget;
        color.stateAfter = Prism::RHI::ResourceState::RenderTarget;
        rendering.colorAttachments.push_back(color);
        commandContext.BeginRendering(rendering);
        commandContext.BindGraphicsPipeline(*pipeline);
        commandContext.Draw(3u, 1u, 0u, 0u);
        commandContext.EndRendering();
        if (frame.EndFrame() != Prism::RHI::FrameResult::Ready)
            throw std::runtime_error("Tessellation test could not present a frame.");
        frame.WaitForGpu();
        phase = "refraction fixture readback validation";
        std::array<RefractionFixtureOutput, 5> refractionOutputs{};
        refractionReadback->Read(
            refractionOutputs.data(), sizeof(refractionOutputs));
        const std::array<float, 5> expectedFallbacks{
            0.0f, 1.0f, 2.0f, 3.0f, 4.0f};
        for (std::size_t index = 0u;
             index < refractionOutputs.size(); ++index)
        {
            const RefractionFixtureOutput& output = refractionOutputs[index];
            const bool finite = std::isfinite(output.uvThicknessFallback.x)
                && std::isfinite(output.uvThicknessFallback.y)
                && std::isfinite(output.uvThicknessFallback.z)
                && std::isfinite(output.uvThicknessFallback.w)
                && std::isfinite(output.resolvedColor.x)
                && std::isfinite(output.resolvedColor.y)
                && std::isfinite(output.resolvedColor.z)
                && std::isfinite(output.resolvedColor.w);
            const bool expectedColor = index == 0u
                ? output.resolvedColor.y > 0.9f
                : output.resolvedColor.x > 0.9f;
            if (!finite || !expectedColor
                || std::abs(output.uvThicknessFallback.w
                    - expectedFallbacks[index]) > 0.01f)
            {
                throw std::runtime_error(
                    "Water refraction GPU fallback fixture disagreed.");
            }
        }
        std::array<RefractionFixtureOutput, 3> rayMarchOutputs{};
        rayMarchReadback->Read(
            rayMarchOutputs.data(), sizeof(rayMarchOutputs));
        const std::array<float, 3> expectedRayFallbacks{0.0f, 2.0f, 4.0f};
        for (std::size_t index = 0u;
             index < rayMarchOutputs.size(); ++index)
        {
            const RefractionFixtureOutput& output = rayMarchOutputs[index];
            const bool finite = std::isfinite(output.uvThicknessFallback.x)
                && std::isfinite(output.uvThicknessFallback.y)
                && std::isfinite(output.uvThicknessFallback.z)
                && std::isfinite(output.uvThicknessFallback.w)
                && std::isfinite(output.resolvedColor.x)
                && std::isfinite(output.resolvedColor.y)
                && std::isfinite(output.resolvedColor.z)
                && std::isfinite(output.resolvedColor.w);
            const bool expectedColor = index == 0u
                ? output.resolvedColor.y > 0.9f
                : output.resolvedColor.x > 0.9f;
            const bool improvedAlignment = index != 0u
                || (output.uvThicknessFallback.x > 0.5f
                    && output.uvThicknessFallback.x < 0.55f);
            if (!finite || !expectedColor || !improvedAlignment
                || std::abs(output.uvThicknessFallback.w
                    - expectedRayFallbacks[index]) > 0.01f)
            {
                throw std::runtime_error(
                    "Water ray-march GPU fixture disagreed.");
            }
        }
        std::array<WaterOpticalFixtureOutput, 4> opticalOutputs{};
        opticalReadback->Read(opticalOutputs.data(), sizeof(opticalOutputs));
        for (std::size_t index = 0u; index < opticalInputs.size(); ++index)
        {
            const WaterOpticalFixtureInput& fixture = opticalInputs[index];
            Prism::Renderer::WaterOpticalParameters parameters{};
            parameters.absorption = {fixture.absorptionIor.x,
                fixture.absorptionIor.y, fixture.absorptionIor.z};
            parameters.scattering = {fixture.scatteringPhase.x,
                fixture.scatteringPhase.y, fixture.scatteringPhase.z};
            parameters.indexOfRefraction = fixture.absorptionIor.w;
            parameters.phaseG = fixture.scatteringPhase.w;
            parameters.thinLayerStrength = fixture.strengths.x;
            parameters.backlitStrength = fixture.strengths.y;
            Prism::Renderer::WaterOpticalInput input{};
            input.viewCosine = fixture.responseParameters.x;
            input.lightCosine = fixture.responseParameters.y;
            input.thickness = fixture.responseParameters.z;
            const Prism::Renderer::WaterOpticalResult expected =
                Prism::Renderer::EvaluateWaterOpticalResponse(
                    input, parameters);
            const WaterOpticalFixtureOutput expectedOutput{
                {expected.transmittance.x, expected.transmittance.y,
                    expected.transmittance.z, expected.fresnel},
                {expected.singleScattering.x,
                    expected.singleScattering.y,
                    expected.singleScattering.z, expected.phase},
                {expected.thinLayer.x, expected.thinLayer.y,
                    expected.thinLayer.z, 0.0f},
                {expected.backlit.x, expected.backlit.y,
                    expected.backlit.z, 0.0f}};
            const float* actualValues =
                &opticalOutputs[index].transmittanceFresnel.x;
            const float* expectedValues =
                &expectedOutput.transmittanceFresnel.x;
            for (std::size_t component = 0u; component < 16u; ++component)
            {
                const float tolerance = 5.0e-4f
                    * (1.0f + std::abs(expectedValues[component]));
                if (!std::isfinite(actualValues[component])
                    || std::abs(actualValues[component]
                        - expectedValues[component]) > tolerance)
                {
                    throw std::runtime_error(
                        "Water optical CPU/GPU fixture disagreed.");
                }
            }
        }
        std::array<Float4, 5> causticOutputs{};
        causticReadback->Read(
            causticOutputs.data(), sizeof(causticOutputs));
        for (const Float4& output : causticOutputs)
        {
            if (!std::isfinite(output.x) || !std::isfinite(output.y)
                || !std::isfinite(output.z) || !std::isfinite(output.w)
                || output.x < 0.0f || output.y < 0.0f || output.z < 0.0f
                || output.x > 16.0f || output.y > 16.0f
                || output.z > 16.0f)
            {
                throw std::runtime_error(
                    "Water caustic GPU fixture produced unbounded energy.");
            }
        }
        if (!(causticOutputs[0].x > causticOutputs[1].x
                && causticOutputs[1].x >= 0.0f
                && causticOutputs[2].x == 0.0f
                && causticOutputs[3].x == 0.0f
                && causticOutputs[4].x == 0.0f
                && causticOutputs[0].y < causticOutputs[0].z))
        {
            throw std::runtime_error(
                "Water caustic GPU receiver/dispersion fixture disagreed.");
        }
        std::array<Float4, 5> volumetricOutputs{};
        volumetricReadback->Read(volumetricOutputs.data(), sizeof(volumetricOutputs));
        for (std::size_t index = 0; index < volumetricInputs.size(); ++index)
        {
            const auto& input = volumetricInputs[index];
            const float extinction = input.extinctionDistance.x;
            const float distance = input.extinctionDistance.w;
            const float expected = input.scatteringPhase.x * input.scatteringPhase.w
                * (extinction > 1.0e-5f
                    ? (1.0f - std::exp(-extinction * distance)) / extinction
                    : distance);
            const auto& output = volumetricOutputs[index];
            if (!std::isfinite(output.x) || !std::isfinite(output.y)
                || !std::isfinite(output.z)
                || std::abs(output.x - expected) > 1.0e-4f)
            {
                throw std::runtime_error(
                    "Water volumetric finite/empty/disabled integration fixture disagreed.");
            }
        }
        std::cout << "Ocean tessellation triangle rendered on "
                  << backend->GetAdapterName() << ".\n";
        return EXIT_SUCCESS;
    }
    catch (const std::exception& exception)
    {
        std::cerr << "Ocean tessellation GPU test failed during " << phase
                  << ": "
                  << exception.what() << '\n';
        return EXIT_FAILURE;
    }
}
