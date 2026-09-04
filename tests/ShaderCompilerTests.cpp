#include "Asset/ShaderManager.h"
#include "Asset/SlangShaderCompiler.h"
#include "RHI/ShaderTypes.h"
#include "RHI/ShaderLayoutBuilder.h"
#include "Renderer/SharedRenderData.h"

#include <array>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <sstream>

namespace
{
using Prism::Asset::ShaderCompileRequest;
using Prism::Asset::SlangShaderCompiler;
using Prism::RHI::ShaderBinary;
using Prism::RHI::ShaderBinaryFormat;
using Prism::RHI::ShaderResourceBinding;
using Prism::RHI::ShaderStage;

struct ShaderEntry
{
    const char* fileName;
    const char* entryPoint;
    ShaderStage stage;
    const char* dxilProfile = nullptr;
    bool spirvOnly = false;
};

// Runtime pipeline creation covers every entry point. This focused matrix keeps CTest fast
// while still compiling every shared shader module to both DXIL and SPIR-V.
constexpr auto ShaderEntries = std::to_array<ShaderEntry>({
    {"Shadow.hlsl", "VSMain", ShaderStage::Vertex},
    {"Shadow.hlsl", "PSMain", ShaderStage::Pixel},
    {"Mesh.hlsl", "VSMain", ShaderStage::Vertex},
    {"Mesh.hlsl", "VSIndexedMain", ShaderStage::Vertex},
    {"Mesh.hlsl", "PSMain", ShaderStage::Pixel},
    {"Mesh.hlsl", "GBufferPS", ShaderStage::Pixel},
    {"Deferred.hlsl", "DeferredLightingPS", ShaderStage::Pixel},
    {"PostProcess.hlsl", "TonemapPS", ShaderStage::Pixel},
    {"PostProcess.hlsl", "SkyboxPS", ShaderStage::Pixel},
    {"PostProcess.hlsl", "BrightExtractCS", ShaderStage::Compute},
    {"PostProcess.hlsl", "BlurHorizontalCS", ShaderStage::Compute},
    {"PostProcess.hlsl", "BlurVerticalCS", ShaderStage::Compute},
    {"HiZ.hlsl", "CopyDepthCS", ShaderStage::Compute},
    {"HiZ.hlsl", "DownsampleDepthCS", ShaderStage::Compute},
    {"DebugView.hlsl", "ShadowDebugPS", ShaderStage::Pixel},
    {"GpuCulling.hlsl", "BuildDrawArgumentsCS", ShaderStage::Compute},
    {"GpuCulling.hlsl", "ClearDrawCountsCS", ShaderStage::Compute},
    {"FftOcean.hlsl", "GenerateSpectrumCS", ShaderStage::Compute},
    {"Ocean/OceanFft.slang", "TextureButterflyCS", ShaderStage::Compute},
    {"Ocean/OceanFft.slang", "TextureArrayButterflyCS", ShaderStage::Compute},
    {"Ocean/OceanFft.slang", "TextureArrayRadix2CS", ShaderStage::Compute},
    {"Ocean/OceanFft.slang", "BufferButterflyCS", ShaderStage::Compute},
    {"Ocean/OceanSpectrumResourceValidation.slang", "WriteSpectrumSlicesCS", ShaderStage::Compute},
    {"Ocean/OceanSpectrumResourceValidation.slang", "ReadSpectrumSlicesCS", ShaderStage::Compute},
    {"Ocean/OceanInitialSpectrum.slang", "GenerateInitialSpectrumCS", ShaderStage::Compute},
    {"Ocean/OceanInitialSpectrum.slang", "SampleInitialSpectrumCS", ShaderStage::Compute},
    {"Ocean/OceanSpectrumEvolution.slang", "EvolveSpectrumCS", ShaderStage::Compute},
    {"Ocean/OceanBuildMaps.slang", "BuildOceanMapsCS", ShaderStage::Compute},
    {"Ocean/OceanBuildMaps.slang", "DownsampleOceanMapsCS", ShaderStage::Compute},
    {"Ocean/OceanDisplacementQuery.slang", "SampleOceanDisplacementCS", ShaderStage::Compute},
    {"Ocean/OceanFoam.slang", "UpdateSpectralFoamCS", ShaderStage::Compute},
    {"Ocean/OceanSimulationValidation.slang", "SampleOceanSimulationCS", ShaderStage::Compute},
    {"Ocean/LocalWave.slang", "SimulateLocalWaveCS", ShaderStage::Compute},
    {"Ocean/OceanSurface.slang", "OceanVSMain", ShaderStage::Vertex},
    {"Ocean/OceanSurface.slang", "OceanVSIndexedMain", ShaderStage::Vertex},
    {"Ocean/OceanSurface.slang", "OceanPSMain", ShaderStage::Pixel},
    {"Ocean/OceanSurface.slang", "OceanGBufferPS", ShaderStage::Pixel},
    {"Ocean/WaterVisibility.slang", "WaterVisibilityPS", ShaderStage::Pixel},
    {"Ocean/WaterDepthCopy.slang", "FullscreenVS", ShaderStage::Vertex},
    {"Ocean/WaterDepthCopy.slang", "WaterDepthCopyPS", ShaderStage::Pixel},
    {"Ocean/WaterOptics.slang", "WaterRefractionAnalyticFixtureCS", ShaderStage::Compute},
    {"Ocean/WaterOptics.slang", "WaterRefractionRayMarchFixtureCS", ShaderStage::Compute},
    {"Ocean/WaterOptics.slang", "WaterRefractionCS", ShaderStage::Compute},
    {"Ocean/WaterOptics.slang", "WaterOpticalAnalyticFixtureCS", ShaderStage::Compute},
    {"Ocean/WaterOptics.slang", "WaterCompositeCS", ShaderStage::Compute},
    {"Ocean/WaterOptics.slang", "WaterCompositePublishCS", ShaderStage::Compute},
    {"Ocean/WaterCaustics.slang", "WaterCausticAccumulateCS", ShaderStage::Compute},
    {"Ocean/WaterCaustics.slang", "WaterCausticAnalyticFixtureCS", ShaderStage::Compute},
    {"Ocean/WaterVolumetrics.slang", "WaterVolumetricAccumulateCS", ShaderStage::Compute},
    {"Ocean/WaterVolumetrics.slang", "WaterVolumetricTemporalCS", ShaderStage::Compute},
    {"Ocean/WaterVolumetrics.slang", "WaterVolumetricReconstructCS", ShaderStage::Compute},
    {"Ocean/WaterVolumetrics.slang", "WaterVolumetricAnalyticFixtureCS", ShaderStage::Compute},
    {"Ocean/WaterCoverage.slang", "WaterCoverageCS", ShaderStage::Compute},
    {"Ocean/OceanSurface.slang", "OceanHullMain", ShaderStage::Hull},
    {"Ocean/OceanSurface.slang", "OceanDomainMain", ShaderStage::Domain},
    {"Ocean/OceanHull.slang", "OceanHullMain", ShaderStage::Hull},
    {"Ocean/OceanDomain.slang", "OceanDomainMain", ShaderStage::Domain},
    {"Ocean/OceanTessellationVertex.slang", "OceanTessellationVS", ShaderStage::Vertex},
    {"Ocean/OceanTessellationPixel.slang", "OceanTessellationPS", ShaderStage::Pixel},
    {"FftOcean.hlsl", "BuildOceanMapsCS", ShaderStage::Compute},
    {"FftOceanDownsample.hlsl", "DownsampleOceanMapsCS", ShaderStage::Compute},
    {"InteractiveTerrain.hlsl", "TerrainBrushCS", ShaderStage::Compute},
    {"InteractiveTerrain.hlsl", "TerrainErosionCS", ShaderStage::Compute},
    {"TemporalAA.hlsl", "TemporalResolveCS", ShaderStage::Compute},
    {"ScreenSpaceEffects.hlsl", "GtaoCS", ShaderStage::Compute},
    {"ScreenSpaceEffects.hlsl", "ScreenSpaceReflectionsCS", ShaderStage::Compute},
    {"ClusteredLighting.hlsl", "BuildLightClustersCS", ShaderStage::Compute},
    {"SkyAtmosphere.hlsl", "TransmittanceCS", ShaderStage::Compute},
    {"SkyAtmosphere.hlsl", "SkyViewCS", ShaderStage::Compute},
    {"LocalLightShadow.hlsl", "LocalShadowVS", ShaderStage::Vertex},
    {"Fluid/PbfIntegrate.hlsl", "InitializeParticlesCS", ShaderStage::Compute},
    {"Fluid/PbfIntegrate.hlsl", "PredictPositionsCS", ShaderStage::Compute},
    {"Fluid/PbfGrid.hlsl", "ClearGridCS", ShaderStage::Compute},
    {"Fluid/PbfGrid.hlsl", "ClearDiagnosticsCS", ShaderStage::Compute},
    {"Fluid/PbfGrid.hlsl", "BuildGridCS", ShaderStage::Compute},
    {"Fluid/PbfGrid.hlsl", "BuildNeighborsCS", ShaderStage::Compute},
    {"Fluid/PbfGrid.hlsl", "ReduceGridDiagnosticsCS", ShaderStage::Compute},
    {"Fluid/PbfConstraints.hlsl", "ComputeLambdaCS", ShaderStage::Compute},
    {"Fluid/PbfConstraints.hlsl", "ComputeDeltaCS", ShaderStage::Compute},
    {"Fluid/PbfConstraints.hlsl", "ApplyDeltaCS", ShaderStage::Compute},
    {"Fluid/PbfVelocity.hlsl", "UpdateVelocityCS", ShaderStage::Compute},
    {"Fluid/PbfVelocity.hlsl", "ComputeVorticityCS", ShaderStage::Compute},
    {"Fluid/PbfVelocity.hlsl", "FinalizeParticlesCS", ShaderStage::Compute},
    {"Fluid/FluidSurface.hlsl", "ParticleBillboardVS", ShaderStage::Vertex},
    {"Fluid/FluidSurface.hlsl", "ParticleDepthPS", ShaderStage::Pixel},
    {"Fluid/FluidSurface.hlsl", "ParticleThicknessPS", ShaderStage::Pixel},
    {"Fluid/FluidFilter.hlsl", "BilateralHorizontalCS", ShaderStage::Compute},
    {"Fluid/FluidFilter.hlsl", "BilateralVerticalCS", ShaderStage::Compute},
    {"Fluid/FluidToonFoam.hlsl", "ReconstructNormalFoamCS", ShaderStage::Compute},
    {"Fluid/FluidToonFoam.hlsl", "RefineFoamCS", ShaderStage::Compute},
    {"Fluid/FluidComposite.hlsl", "FluidCompositeCS", ShaderStage::Compute},
    {"Fluid/FluidCaustics.hlsl", "GenerateCausticsCS", ShaderStage::Compute},
    {"Fluid/FluidCaustics.hlsl", "BlurCausticsHorizontalCS", ShaderStage::Compute},
    {"Fluid/FluidCaustics.hlsl", "BlurCausticsVerticalCS", ShaderStage::Compute},
    {"RayTracingBaseline.hlsl", "RayGenerationMain", ShaderStage::RayGeneration},
    {"RayTracingBaseline.hlsl", "MissMain", ShaderStage::Miss},
    {"RayTracingBaseline.hlsl", "ClosestHitMain", ShaderStage::ClosestHit},
});

void Expect(const bool condition, const std::string& message)
{
    if (!condition)
    {
        throw std::runtime_error(message);
    }
}

std::uint32_t ReadMagic(const ShaderBinary& binary)
{
    Expect(binary.Size() >= sizeof(std::uint32_t), "Shader binary is too small to contain a magic value.");
    std::uint32_t magic = 0;
    std::memcpy(&magic, binary.Data(), sizeof(magic));
    return magic;
}

bool HasResource(const ShaderBinary& binary, const std::string_view name)
{
    for (const ShaderResourceBinding& resource : binary.reflection.resources)
    {
        if (resource.name == name)
        {
            return true;
        }
    }
    return false;
}

const ShaderResourceBinding* FindResource(
    const ShaderBinary& binary,
    const std::string_view name)
{
    for (const ShaderResourceBinding& resource : binary.reflection.resources)
    {
        if (resource.name == name)
            return &resource;
    }
    return nullptr;
}

const Prism::RHI::DescriptorBindingDescription* FindBinding(
    const Prism::RHI::DescriptorSetLayoutDescription& layout,
    const std::uint32_t binding)
{
    for (const Prism::RHI::DescriptorBindingDescription& candidate : layout.bindings)
    {
        if (candidate.binding == binding)
        {
            return &candidate;
        }
    }
    return nullptr;
}

ShaderBinary Compile(
    SlangShaderCompiler& compiler,
    const std::filesystem::path& shaderDirectory,
    const ShaderEntry& entry,
    const ShaderBinaryFormat format)
{
    ShaderCompileRequest request;
    request.filePath = shaderDirectory / entry.fileName;
    request.entryPoint = entry.entryPoint;
    request.stage = entry.stage;
    request.format = format;
    request.debug = true;
    if (format == ShaderBinaryFormat::Dxil
        && entry.dxilProfile != nullptr)
    {
        request.targetProfile = entry.dxilProfile;
    }
    return compiler.Compile(request);
}
} // namespace

int main()
{
    try
    {
        const std::filesystem::path shaderDirectory = PRISM_RENDER_SHADER_DIR;
        SlangShaderCompiler compiler;

        std::size_t compiledProgramCount = 0;
        for (const ShaderEntry& entry : ShaderEntries)
        {
#if defined(PRISM_RENDER_HAS_D3D12)
            if (!entry.spirvOnly)
            {
                const ShaderBinary dxil = Compile(compiler, shaderDirectory, entry, ShaderBinaryFormat::Dxil);
                Expect(dxil.IsValid(), std::string("Invalid DXIL for ") + entry.fileName + ":" + entry.entryPoint);
                Expect(ReadMagic(dxil) == 0x43425844u, "DXIL container must use the DXBC container magic.");
                ++compiledProgramCount;
            }
#endif

            const ShaderBinary spirv = Compile(compiler, shaderDirectory, entry, ShaderBinaryFormat::SpirV);
            Expect(spirv.IsValid(), std::string("Invalid SPIR-V for ") + entry.fileName + ":" + entry.entryPoint);
            Expect(ReadMagic(spirv) == 0x07230203u, "SPIR-V magic mismatch.");
            Expect(spirv.emittedEntryPoint == "main", "Slang SPIR-V entry point must be exposed as main.");
            Expect(spirv.diagnostics.find("explicit binding overlap") == std::string::npos,
                std::string("Vulkan descriptor bindings overlap for ")
                    + entry.fileName + ":" + entry.entryPoint + "\n"
                    + spirv.diagnostics);
            Expect(spirv.stage == entry.stage,
                "Tessellation shader reflection returned the wrong stage identity.");
            ++compiledProgramCount;
        }

        const ShaderEntry meshPixelEntry{"Mesh.hlsl", "PSMain", ShaderStage::Pixel};
        const ShaderEntry meshVertexEntry{"Mesh.hlsl", "VSMain", ShaderStage::Vertex};
        const ShaderEntry meshIndexedVertexEntry{
            "Mesh.hlsl",
            "VSIndexedMain",
            ShaderStage::Vertex};
        const ShaderBinary meshVertex = Compile(
            compiler, shaderDirectory, meshVertexEntry, ShaderBinaryFormat::SpirV);
        const ShaderBinary meshIndexedVertex = Compile(
            compiler,
            shaderDirectory,
            meshIndexedVertexEntry,
            ShaderBinaryFormat::SpirV);
        const ShaderEntry oceanSurfaceEntry{
            "Ocean/OceanSurface.slang", "OceanVSIndexedMain", ShaderStage::Vertex};
        const ShaderEntry oceanRegularSurfaceEntry{
            "Ocean/OceanSurface.slang", "OceanVSMain", ShaderStage::Vertex};
        const ShaderBinary oceanSurface = Compile(
            compiler, shaderDirectory, oceanSurfaceEntry, ShaderBinaryFormat::SpirV);
        const ShaderBinary oceanRegularSurface = Compile(
            compiler, shaderDirectory, oceanRegularSurfaceEntry,
            ShaderBinaryFormat::SpirV);
        const ShaderEntry oceanPixelEntry{
            "Ocean/OceanSurface.slang", "OceanPSMain", ShaderStage::Pixel};
        const ShaderBinary oceanPixel = Compile(
            compiler, shaderDirectory, oceanPixelEntry, ShaderBinaryFormat::SpirV);
        const ShaderEntry oceanGBufferEntry{
            "Ocean/OceanSurface.slang", "OceanGBufferPS", ShaderStage::Pixel};
        const ShaderBinary oceanGBuffer = Compile(
            compiler, shaderDirectory, oceanGBufferEntry, ShaderBinaryFormat::SpirV);
        const ShaderEntry waterVisibilityEntry{
            "Ocean/WaterVisibility.slang", "WaterVisibilityPS", ShaderStage::Pixel};
        const ShaderBinary waterVisibility = Compile(
            compiler, shaderDirectory, waterVisibilityEntry,
            ShaderBinaryFormat::SpirV);
        const ShaderResourceBinding* waterConstants =
            FindResource(waterVisibility, "WaterOpticsConstants");
        Expect(HasResource(waterVisibility, "FrameConstants")
                && HasResource(waterVisibility, "spectralOceanGradient")
                && HasResource(waterVisibility, "spectralOceanFoam")
                && waterConstants != nullptr
                && waterConstants->bindingIndex == 5u
                && waterConstants->byteSize == 256u
                && !HasResource(waterVisibility, "albedoTexture"),
            "Water visibility reflection must expose the 256-byte optical CBV and spectral data without generic mesh textures.");
        const ShaderEntry waterRefractionFixtureEntry{
            "Ocean/WaterOptics.slang",
            "WaterRefractionAnalyticFixtureCS",
            ShaderStage::Compute};
        const ShaderBinary waterRefractionFixture = Compile(
            compiler, shaderDirectory, waterRefractionFixtureEntry,
            ShaderBinaryFormat::SpirV);
        Expect(HasResource(waterRefractionFixture,
                   "refractionFixtureInputs")
                && HasResource(waterRefractionFixture,
                    "refractionFixtureOutputs"),
            "Water refraction analytic fixture reflection is incomplete.");
        const ShaderEntry waterRayMarchFixtureEntry{
            "Ocean/WaterOptics.slang",
            "WaterRefractionRayMarchFixtureCS",
            ShaderStage::Compute};
        const ShaderBinary waterRayMarchFixture = Compile(
            compiler, shaderDirectory, waterRayMarchFixtureEntry,
            ShaderBinaryFormat::SpirV);
        Expect(HasResource(waterRayMarchFixture,
                   "rayMarchFixtureInputs")
                && HasResource(waterRayMarchFixture,
                    "rayMarchFixtureOutputs"),
            "Water ray-march fixture reflection is incomplete.");
        const ShaderEntry waterRefractionEntry{
            "Ocean/WaterOptics.slang",
            "WaterRefractionCS",
            ShaderStage::Compute};
        const ShaderBinary waterRefraction = Compile(
            compiler, shaderDirectory, waterRefractionEntry,
            ShaderBinaryFormat::SpirV);
        Expect(HasResource(waterRefraction, "WaterOpticsConstants")
                && HasResource(waterRefraction, "waterSceneColor")
                && HasResource(waterRefraction, "waterOpaqueDepth")
                && HasResource(waterRefraction, "waterCompositeDepth")
                && HasResource(waterRefraction, "waterMaterialNormal")
                && HasResource(waterRefraction, "waterMaterialMask")
                && HasResource(waterRefraction, "waterRefractionOutput")
                && HasResource(waterRefraction, "waterLinearClampSampler"),
            "Production water refraction reflection is incomplete.");
        const ShaderEntry waterOpticalFixtureEntry{
            "Ocean/WaterOptics.slang",
            "WaterOpticalAnalyticFixtureCS",
            ShaderStage::Compute};
        const ShaderBinary waterOpticalFixture = Compile(
            compiler, shaderDirectory, waterOpticalFixtureEntry,
            ShaderBinaryFormat::SpirV);
        Expect(HasResource(waterOpticalFixture,
                   "waterOpticalFixtureInputs")
                && HasResource(waterOpticalFixture,
                    "waterOpticalFixtureOutputs"),
            "Water optical analytic fixture reflection is incomplete.");
        const ShaderEntry waterCompositeEntry{
            "Ocean/WaterOptics.slang",
            "WaterCompositeCS",
            ShaderStage::Compute};
        const ShaderBinary waterComposite = Compile(
            compiler, shaderDirectory, waterCompositeEntry,
            ShaderBinaryFormat::SpirV);
        const ShaderResourceBinding* waterFrameConstants =
            FindResource(waterComposite, "WaterFrameConstants");
        const ShaderResourceBinding* waterCompositeConstants =
            FindResource(waterComposite, "WaterOpticsConstants");
        Expect(waterFrameConstants != nullptr
                && waterFrameConstants->bindingIndex == 0u
                && waterFrameConstants->byteSize
                    == sizeof(Prism::Renderer::SharedFrameConstants)
                && waterCompositeConstants != nullptr
                && waterCompositeConstants->bindingIndex == 5u
                && waterCompositeConstants->byteSize == 256u
                && HasResource(waterComposite, "waterSceneColor")
                && HasResource(waterComposite, "waterOpaqueDepth")
                && HasResource(waterComposite, "waterCompositeDepth")
                && HasResource(waterComposite, "waterCompositeRefraction")
                && HasResource(waterComposite, "waterMaterialNormal")
                && HasResource(waterComposite, "waterCompositeGBuffer1")
                && HasResource(waterComposite, "waterMaterialMask")
                && HasResource(waterComposite,
                    "waterCompositeSlopeMoments")
                && HasResource(waterComposite, "waterCompositeShadowMap")
                && HasResource(waterComposite,
                    "waterCompositeShadowMoments")
                && HasResource(waterComposite,
                    "waterCompositeAtmosphere")
                && HasResource(waterComposite,
                    "waterCompositeCausticsNear")
                && HasResource(waterComposite,
                    "waterCompositeCausticsMiddle")
                && HasResource(waterComposite,
                    "waterCompositeEnvironment")
                && HasResource(waterComposite, "waterCompositeOutput")
                && !HasResource(waterComposite, "albedoTexture")
                && !HasResource(waterComposite,
                    "metallicRoughnessTexture"),
            "Dedicated water composite reflection includes missing inputs or ordinary mesh material resources.");
        const ShaderEntry waterCausticEntry{
            "Ocean/WaterCaustics.slang",
            "WaterCausticAccumulateCS",
            ShaderStage::Compute};
        const ShaderBinary waterCaustic = Compile(
            compiler, shaderDirectory, waterCausticEntry,
            ShaderBinaryFormat::SpirV);
        Expect(HasResource(waterCaustic, "CausticFrameConstants")
                && HasResource(waterCaustic, "CausticWaterConstants")
                && HasResource(waterCaustic, "causticSpectralGradient")
                && HasResource(waterCaustic, "causticLocalGradient")
                && HasResource(waterCaustic, "causticNearOutput")
                && HasResource(waterCaustic, "causticMiddleOutput"),
            "Production water caustic reflection is incomplete.");
        const ShaderEntry waterCausticFixtureEntry{
            "Ocean/WaterCaustics.slang",
            "WaterCausticAnalyticFixtureCS",
            ShaderStage::Compute};
        const ShaderBinary waterCausticFixture = Compile(
            compiler, shaderDirectory, waterCausticFixtureEntry,
            ShaderBinaryFormat::SpirV);
        Expect(HasResource(waterCausticFixture, "causticFixtureInputs")
                && HasResource(waterCausticFixture,
                    "causticFixtureOutputs"),
            "Water caustic analytic fixture reflection is incomplete.");
        const ShaderEntry waterVolumeEntry{
            "Ocean/WaterVolumetrics.slang",
            "WaterVolumetricAccumulateCS",
            ShaderStage::Compute};
        const ShaderBinary waterVolume = Compile(
            compiler, shaderDirectory, waterVolumeEntry,
            ShaderBinaryFormat::SpirV);
        Expect(HasResource(waterVolume, "VolumeFrameConstants")
                && HasResource(waterVolume, "VolumeWaterConstants")
                && HasResource(waterVolume, "volumeOpaqueDepth")
                && HasResource(waterVolume, "volumeCompositeDepth")
                && HasResource(waterVolume, "volumeWaterMask")
                && HasResource(waterVolume, "volumeCurrentOutput"),
            "Production water volumetric reflection is incomplete.");
        const ShaderEntry waterVolumeFixtureEntry{
            "Ocean/WaterVolumetrics.slang",
            "WaterVolumetricAnalyticFixtureCS",
            ShaderStage::Compute};
        const ShaderBinary waterVolumeFixture = Compile(
            compiler, shaderDirectory, waterVolumeFixtureEntry,
            ShaderBinaryFormat::SpirV);
        Expect(HasResource(waterVolumeFixture, "volumeFixtureInputs")
                && HasResource(waterVolumeFixture,
                    "volumeFixtureOutputs"),
            "Water volumetric analytic fixture reflection is incomplete.");
        const ShaderEntry waterPublishEntry{
            "Ocean/WaterOptics.slang",
            "WaterCompositePublishCS",
            ShaderStage::Compute};
        const ShaderBinary waterPublish = Compile(
            compiler, shaderDirectory, waterPublishEntry,
            ShaderBinaryFormat::SpirV);
        Expect(HasResource(waterPublish, "waterSceneColor")
                && HasResource(waterPublish, "waterMaterialNormal")
                && HasResource(waterPublish, "waterMaterialMask")
                && HasResource(waterPublish, "waterPublishHdr")
                && HasResource(waterPublish,
                    "waterPublishSceneMotion"),
            "Water composite HDR/motion publication reflection is incomplete.");
        {
            std::ifstream sourceFile(
                shaderDirectory / "Ocean" / "WaterOptics.slang");
            std::ostringstream sourceStream;
            sourceStream << sourceFile.rdbuf();
            const std::string source = sourceStream.str();
            for (const std::string_view contractToken : {
                     "EvaluateApproximateWaterRefraction",
                     "WaterRefractionOffScreen",
                     "WaterRefractionForeground",
                     "WaterRefractionZeroThickness",
                     "WaterRefractionNonFinite",
                     "WaterRefractionNoIntersection",
                     "EvaluateRayMarchedWaterRefraction",
                     "WaterRefractionFrameNoise",
                     "EvaluateWaterOpticalResponse",
                     "EvaluateWaterFoamResponse",
                     "WaterFoamWorldDetail",
                     "foam.refractionWeight",
                     "foam.effectiveFoam.xxx",
                     "aeratedDiffuse",
                     "WaterCompositeCS",
                     "WaterCompositePublishCS",
                     "WaterSlopeVariance",
                     "WaterDirectionalShadow",
                     "singleScattering",
                     "thinLayer",
                     "backlit",
                     "maximumUvOffset"})
            {
                Expect(source.find(contractToken) != std::string::npos,
                    "Water refraction fallback contract is incomplete.");
            }
            for (const std::string_view forbiddenToken : {
                     "albedoTexture",
                     "metallicRoughnessTexture",
                     "EvaluateDirectPbrLighting"})
            {
                Expect(source.find(forbiddenToken) == std::string::npos,
                    "Dedicated water optics source must not include ordinary mesh material or generic opaque BRDF entry points.");
            }
        }
        {
            std::ifstream sourceFile(
                shaderDirectory / "Ocean" / "OceanSurface.slang");
            Expect(sourceFile.good(),
                "Water visibility shader source is unavailable.");
            std::ostringstream sourceStream;
            sourceStream << sourceFile.rdbuf();
            std::string source = sourceStream.str();
            // The Slang module includes the implementation from its parent.
            if (source.find("WaterVisibilityOutput") == std::string::npos)
            {
                std::ifstream implementationFile(
                    shaderDirectory / "OceanSurface.hlsl");
                sourceStream.str({});
                sourceStream.clear();
                sourceStream << implementationFile.rdbuf();
                source = sourceStream.str();
            }
            for (const std::string_view contractToken : {
                     "normalRoughness : SV_Target0",
                     "absorptionFoam : SV_Target1",
                     "scatteringMask : SV_Target2",
                     "motionVector : SV_Target3",
                     "absorptionPerMeter",
                     "scatteringPerMeter",
                     "currentClipPosition",
                     "previousClipPosition"})
            {
                Expect(source.find(contractToken) != std::string::npos,
                    "Water visibility shader output contract is incomplete.");
            }
        }
        Expect(HasResource(oceanSurface, "oceanPatchInstances"),
            "Adaptive ocean vertex reflection did not expose its instance payload.");
        const ShaderBinary meshPixelSpirv = Compile(
            compiler, shaderDirectory, meshPixelEntry, ShaderBinaryFormat::SpirV);
        Expect(HasResource(meshPixelSpirv, "FrameConstants"), "Slang reflection did not report FrameConstants.");
        Expect(HasResource(meshPixelSpirv, "albedoTexture"), "Slang reflection did not report albedoTexture.");
        Expect(HasResource(meshPixelSpirv, "linearWrapSampler"), "Slang reflection did not report linearWrapSampler.");
        const std::array layoutStages = {
            Prism::RHI::ShaderLayoutStage{&meshVertex.reflection, ShaderStage::Vertex},
            Prism::RHI::ShaderLayoutStage{&meshIndexedVertex.reflection, ShaderStage::Vertex},
            Prism::RHI::ShaderLayoutStage{&meshPixelSpirv.reflection, ShaderStage::Pixel}};
        const std::array<std::uint32_t, 1> dynamicBindings = {1};
        const Prism::RHI::DescriptorSetLayoutDescription meshLayout =
            Prism::RHI::BuildDescriptorSetLayout(layoutStages, dynamicBindings);
        const auto* objectBinding = FindBinding(meshLayout, 1);
        Expect(objectBinding != nullptr
                   && objectBinding->type == Prism::RHI::DescriptorType::DynamicConstantBuffer,
               "ObjectConstants must be reflected as dynamic binding 1.");
        for (std::uint32_t binding = 16; binding <= 25; ++binding)
        {
            Expect(FindBinding(meshLayout, binding) != nullptr,
                   "The shared PBR descriptor layout is missing a texture binding.");
        }
        Expect(FindBinding(meshLayout, 48) != nullptr && FindBinding(meshLayout, 49) != nullptr,
               "The shared PBR descriptor layout is missing its samplers.");
        Expect(
            FindBinding(meshLayout, 32) != nullptr
                && FindBinding(meshLayout, 33) != nullptr
                && FindBinding(meshLayout, 34) != nullptr
                && FindBinding(meshLayout, 35) != nullptr,
            "The indexed-object or live-atmosphere path is missing a reflected binding.");
        Expect(FindBinding(meshLayout, 5u) == nullptr,
            "Water optical constants changed the ordinary mesh layout.");
        const std::array ordinaryOceanLayoutStages = {
            Prism::RHI::ShaderLayoutStage{
                &oceanRegularSurface.reflection, ShaderStage::Vertex},
            Prism::RHI::ShaderLayoutStage{
                &oceanSurface.reflection, ShaderStage::Vertex},
            Prism::RHI::ShaderLayoutStage{
                &oceanPixel.reflection, ShaderStage::Pixel},
            Prism::RHI::ShaderLayoutStage{
                &oceanGBuffer.reflection, ShaderStage::Pixel}};
        const auto ordinaryOceanLayout =
            Prism::RHI::BuildDescriptorSetLayout(
                ordinaryOceanLayoutStages, dynamicBindings);
        Expect(FindBinding(ordinaryOceanLayout, 5u) == nullptr,
            "Water optical constants changed ordinary ocean PSO reflection.");
        const std::array oceanLayoutStages = {
            Prism::RHI::ShaderLayoutStage{
                &oceanRegularSurface.reflection, ShaderStage::Vertex},
            Prism::RHI::ShaderLayoutStage{&oceanSurface.reflection, ShaderStage::Vertex},
            Prism::RHI::ShaderLayoutStage{&oceanPixel.reflection, ShaderStage::Pixel},
            Prism::RHI::ShaderLayoutStage{
                &oceanGBuffer.reflection, ShaderStage::Pixel},
            Prism::RHI::ShaderLayoutStage{
                &waterVisibility.reflection, ShaderStage::Pixel}};
        const auto oceanLayout = Prism::RHI::BuildDescriptorSetLayout(
            oceanLayoutStages, dynamicBindings);
        Expect(FindBinding(oceanLayout, 42) != nullptr,
            "Ocean descriptor layout did not merge the adaptive instance binding.");
        for (const std::uint32_t binding : {
                 0u, 1u, 5u, 21u, 22u, 23u, 24u, 25u, 29u, 34u,
                 36u, 37u, 38u, 39u, 40u, 41u, 42u, 48u, 49u, 50u})
        {
            Expect(FindBinding(oceanLayout, binding) != nullptr,
                "Reduced ocean descriptor layout is missing required binding "
                    + std::to_string(binding) + ".");
        }
        for (const std::uint32_t binding : {
                 2u, 3u, 4u, 16u, 17u, 18u, 19u, 20u,
                 26u, 27u, 28u, 30u, 31u, 32u, 33u, 35u})
        {
            Expect(FindBinding(oceanLayout, binding) == nullptr,
                "Reduced ocean descriptor layout retained unrelated mesh binding "
                    + std::to_string(binding) + ".");
        }
#if defined(PRISM_RENDER_HAS_D3D12)
        const ShaderBinary meshVertexDxil = Compile(
            compiler,
            shaderDirectory,
            meshVertexEntry,
            ShaderBinaryFormat::Dxil);
        const ShaderBinary meshIndexedVertexDxil = Compile(
            compiler,
            shaderDirectory,
            meshIndexedVertexEntry,
            ShaderBinaryFormat::Dxil);
        const ShaderBinary meshPixelDxil = Compile(
            compiler,
            shaderDirectory,
            meshPixelEntry,
            ShaderBinaryFormat::Dxil);
        const std::array dxilLayoutStages = {
            Prism::RHI::ShaderLayoutStage{
                &meshVertexDxil.reflection,
                ShaderStage::Vertex},
            Prism::RHI::ShaderLayoutStage{
                &meshIndexedVertexDxil.reflection,
                ShaderStage::Vertex},
            Prism::RHI::ShaderLayoutStage{
                &meshPixelDxil.reflection,
                ShaderStage::Pixel}};
        const auto dxilMeshLayout =
            Prism::RHI::BuildDescriptorSetLayout(
                dxilLayoutStages,
                dynamicBindings);
        Expect(
            FindBinding(dxilMeshLayout, 16) != nullptr
                && FindBinding(dxilMeshLayout, 32) != nullptr
                && FindBinding(dxilMeshLayout, 33) != nullptr
                && FindBinding(dxilMeshLayout, 34) != nullptr
                && FindBinding(dxilMeshLayout, 35) != nullptr
                && FindBinding(dxilMeshLayout, 48) != nullptr,
            "DXIL reflection did not normalize D3D register classes to canonical RHI bindings.");
        const ShaderBinary oceanVertexDxil = Compile(
            compiler,
            shaderDirectory,
            oceanSurfaceEntry,
            ShaderBinaryFormat::Dxil);
        const ShaderBinary oceanPixelDxil = Compile(
            compiler,
            shaderDirectory,
            oceanPixelEntry,
            ShaderBinaryFormat::Dxil);
        const ShaderBinary oceanGBufferDxil = Compile(
            compiler,
            shaderDirectory,
            oceanGBufferEntry,
            ShaderBinaryFormat::Dxil);
        const ShaderBinary waterVisibilityDxil = Compile(
            compiler,
            shaderDirectory,
            waterVisibilityEntry,
            ShaderBinaryFormat::Dxil);
        const ShaderResourceBinding* waterConstantsDxil =
            FindResource(waterVisibilityDxil, "WaterOpticsConstants");
        Expect(waterConstantsDxil != nullptr
                && waterConstantsDxil->bindingIndex
                    == waterConstants->bindingIndex
                && waterConstantsDxil->byteSize == waterConstants->byteSize,
            "DXIL/SPIR-V water optical constant reflection disagrees.");
        const std::array oceanDxilStages = {
            Prism::RHI::ShaderLayoutStage{
                &oceanVertexDxil.reflection, ShaderStage::Vertex},
            Prism::RHI::ShaderLayoutStage{
                &oceanPixelDxil.reflection, ShaderStage::Pixel},
            Prism::RHI::ShaderLayoutStage{
                &oceanGBufferDxil.reflection, ShaderStage::Pixel},
            Prism::RHI::ShaderLayoutStage{
                &waterVisibilityDxil.reflection, ShaderStage::Pixel}};
        const auto oceanDxilLayout =
            Prism::RHI::BuildDescriptorSetLayout(
                oceanDxilStages, dynamicBindings);
        Expect(oceanDxilLayout.bindings.size() == oceanLayout.bindings.size(),
            "DXIL/SPIR-V ocean layouts expose different binding counts.");
        for (const auto& binding : oceanLayout.bindings)
        {
            const auto* dxilBinding = FindBinding(
                oceanDxilLayout, binding.binding);
            Expect(dxilBinding != nullptr
                    && dxilBinding->type == binding.type
                    && dxilBinding->descriptorCount
                        == binding.descriptorCount,
                "DXIL/SPIR-V ocean descriptor reflection disagrees.");
        }
#endif
        Expect(sizeof(Prism::Renderer::SharedFrameConstants) == 768,
               "Shared FrameConstants no longer match the Slang cbuffer contract.");

        const ShaderEntry shadowVertexEntry{
            "Shadow.hlsl", "VSMain", ShaderStage::Vertex};
        const ShaderEntry shadowPixelEntry{
            "Shadow.hlsl", "PSMain", ShaderStage::Pixel};
        const ShaderBinary shadowVertex = Compile(
            compiler,
            shaderDirectory,
            shadowVertexEntry,
            ShaderBinaryFormat::SpirV);
        const ShaderBinary shadowPixel = Compile(
            compiler,
            shaderDirectory,
            shadowPixelEntry,
            ShaderBinaryFormat::SpirV);
        const std::array shadowLayoutStages = {
            Prism::RHI::ShaderLayoutStage{
                &shadowVertex.reflection,
                ShaderStage::Vertex},
            Prism::RHI::ShaderLayoutStage{
                &shadowPixel.reflection,
                ShaderStage::Pixel}};
        const std::array<std::uint32_t, 2>
            shadowDynamicBindings = {1, 3};
        const auto shadowLayout =
            Prism::RHI::BuildDescriptorSetLayout(
                shadowLayoutStages,
                shadowDynamicBindings);
        Expect(
            FindBinding(shadowLayout, 1) != nullptr
                && FindBinding(shadowLayout, 2) != nullptr
                && FindBinding(shadowLayout, 3) != nullptr
                && FindBinding(shadowLayout, 16) != nullptr
                && FindBinding(shadowLayout, 17) != nullptr
                && FindBinding(shadowLayout, 48) != nullptr
                && FindBinding(shadowLayout, 49) != nullptr,
            "Alpha-mask shadow layout is missing object, material, texture, or sampler bindings.");

        const ShaderEntry hiZEntry{
            "HiZ.hlsl",
            "DownsampleDepthCS",
            ShaderStage::Compute};
        const ShaderBinary hiZ = Compile(
            compiler,
            shaderDirectory,
            hiZEntry,
            ShaderBinaryFormat::SpirV);
        Expect(
            HasResource(hiZ, "sourceDepth")
                && HasResource(hiZ, "outputHiZ"),
            "Hi-Z reflection must expose sampled input and storage output.");

        const ShaderEntry oceanDownsampleEntry{
            "FftOceanDownsample.hlsl",
            "DownsampleOceanMapsCS",
            ShaderStage::Compute};
        const ShaderBinary oceanDownsample = Compile(
            compiler,
            shaderDirectory,
            oceanDownsampleEntry,
            ShaderBinaryFormat::SpirV);
        Expect(
            HasResource(oceanDownsample, "sourceDisplacement")
                && HasResource(oceanDownsample, "sourceNormalFoam")
                && HasResource(oceanDownsample, "destinationDisplacement")
                && HasResource(oceanDownsample, "destinationNormalFoam"),
            "Ocean mip reflection must expose only its source and destination maps.");

        const ShaderEntry gtaoEntry{
            "ScreenSpaceEffects.hlsl",
            "GtaoCS",
            ShaderStage::Compute};
        const ShaderBinary gtao = Compile(
            compiler,
            shaderDirectory,
            gtaoEntry,
            ShaderBinaryFormat::SpirV);
        Expect(
            HasResource(gtao, "positionRoughness")
                && HasResource(gtao, "normalMetallic")
                && HasResource(gtao, "screenSpaceOutput"),
            "GTAO reflection must expose GBuffer inputs and its storage output.");
        const ShaderEntry ssrEntry{
            "ScreenSpaceEffects.hlsl",
            "ScreenSpaceReflectionsCS",
            ShaderStage::Compute};
        const ShaderBinary ssr = Compile(
            compiler,
            shaderDirectory,
            ssrEntry,
            ShaderBinaryFormat::SpirV);
        Expect(
            HasResource(ssr, "hiZ")
                && HasResource(ssr, "hdrColor")
                && HasResource(ssr, "screenSpaceOutput"),
            "SSR reflection must expose Hi-Z, HDR, and its storage output.");
        const ShaderEntry clusterEntry{
            "ClusteredLighting.hlsl",
            "BuildLightClustersCS",
            ShaderStage::Compute};
        const ShaderBinary cluster = Compile(
            compiler,
            shaderDirectory,
            clusterEntry,
            ShaderBinaryFormat::SpirV);
        Expect(
            HasResource(cluster, "pointLights")
                && HasResource(
                    cluster,
                    "clusterLightCounts")
                && HasResource(
                    cluster,
                    "clusterLightIndices"),
            "Clustered-light reflection must expose lights, counts, and indices.");
        const ShaderEntry deferredEntry{
            "Deferred.hlsl",
            "DeferredLightingPS",
            ShaderStage::Pixel};
        const ShaderBinary deferred = Compile(
            compiler,
            shaderDirectory,
            deferredEntry,
            ShaderBinaryFormat::SpirV);
        Expect(
            HasResource(
                deferred,
                "LocalLightConstants")
                && HasResource(
                    deferred,
                    "spotShadowMap")
                && HasResource(
                    deferred,
                    "pointShadowMap"),
            "Deferred local-light reflection must expose constants and both shadow maps.");
        const ShaderEntry rayGenerationEntry{
            "RayTracingBaseline.hlsl",
            "RayGenerationMain",
            ShaderStage::RayGeneration};
        const ShaderBinary rayGeneration =
            Compile(
                compiler,
                shaderDirectory,
                rayGenerationEntry,
                ShaderBinaryFormat::SpirV);
        Expect(
            HasResource(
                rayGeneration,
                "SceneAccelerationStructure")
                && HasResource(
                    rayGeneration,
                    "RayTracingOutput"),
            "Ray-generation reflection must expose the TLAS and output image.");
        const std::array rayTracingStages = {
            Prism::RHI::ShaderLayoutStage{
                &rayGeneration.reflection,
                ShaderStage::RayGeneration}};
        const auto rayTracingLayout =
            Prism::RHI::BuildDescriptorSetLayout(
                rayTracingStages,
                {});
        Expect(
            FindBinding(rayTracingLayout, 16)
                    != nullptr
                && FindBinding(
                       rayTracingLayout,
                       16)
                       ->type
                    == Prism::RHI::
                        DescriptorType::
                            AccelerationStructure
                && FindBinding(
                       rayTracingLayout,
                       32)
                       != nullptr
                && FindBinding(
                       rayTracingLayout,
                       32)
                       ->type
                    == Prism::RHI::
                        DescriptorType::
                            StorageTexture,
            "Ray-tracing reflection did not preserve TLAS and UAV descriptor types.");
        const std::array clusterStages = {
            Prism::RHI::ShaderLayoutStage{
                &cluster.reflection,
                ShaderStage::Compute}};
        const auto clusterLayout =
            Prism::RHI::BuildDescriptorSetLayout(
                clusterStages,
                {});
        const auto* clusterLightsBinding =
            FindBinding(clusterLayout, 16);
        const auto* clusterCountsBinding =
            FindBinding(clusterLayout, 32);
        const auto* clusterIndicesBinding =
            FindBinding(clusterLayout, 33);
        Expect(
            clusterLightsBinding != nullptr
                && clusterLightsBinding->type
                    == Prism::RHI::DescriptorType::
                        ReadOnlyStorageBuffer
                && clusterCountsBinding != nullptr
                && clusterCountsBinding->type
                    == Prism::RHI::DescriptorType::
                        StorageBuffer
                && clusterIndicesBinding != nullptr
                && clusterIndicesBinding->type
                    == Prism::RHI::DescriptorType::
                        StorageBuffer,
            "Clustered-light read-only/read-write buffer reflection is incorrect.");

        const ShaderEntry gpuCullingEntry{
            "GpuCulling.hlsl",
            "BuildDrawArgumentsCS",
            ShaderStage::Compute};
        const ShaderBinary gpuCulling =
            Compile(
                compiler,
                shaderDirectory,
                gpuCullingEntry,
                ShaderBinaryFormat::SpirV);
        const std::array gpuCullingStages = {
            Prism::RHI::ShaderLayoutStage{
                &gpuCulling.reflection,
                ShaderStage::Compute}};
        const auto gpuCullingLayout =
            Prism::RHI::BuildDescriptorSetLayout(
                gpuCullingStages,
                {});
        Expect(
            FindBinding(gpuCullingLayout, 16)
                    != nullptr
                && FindBinding(
                       gpuCullingLayout,
                       16)
                       ->type
                    == Prism::RHI::
                        DescriptorType::
                            SampledTexture
                && FindBinding(gpuCullingLayout, 32)
                    != nullptr
                && FindBinding(
                       gpuCullingLayout,
                       32)
                       ->type
                    == Prism::RHI::
                        DescriptorType::
                            StorageBuffer
                && FindBinding(
                       gpuCullingLayout,
                       33)
                       != nullptr
                && FindBinding(
                       gpuCullingLayout,
                       33)
                       ->type
                    == Prism::RHI::
                        DescriptorType::
                            StorageBuffer,
            "Slang reflection did not classify GPU culling structured buffers.");

        Prism::Asset::ShaderManager shaderManager;
        const ShaderBinary& cachedA =
            shaderManager.LoadShader(
                shaderDirectory / "Triangle.hlsl",
                "VSMain",
                ShaderStage::Vertex,
#if defined(PRISM_RENDER_HAS_D3D12)
                ShaderBinaryFormat::Dxil);
#else
                ShaderBinaryFormat::SpirV);
#endif
        const ShaderBinary& cachedB =
            shaderManager.LoadShader(
                shaderDirectory / "Triangle.hlsl",
                "VSMain",
                ShaderStage::Vertex,
#if defined(PRISM_RENDER_HAS_D3D12)
                ShaderBinaryFormat::Dxil);
#else
                ShaderBinaryFormat::SpirV);
#endif
        Expect(&cachedA == &cachedB, "ShaderManager did not reuse its cached ShaderBinary.");

        std::cout << "Slang compiled " << compiledProgramCount
#if defined(PRISM_RENDER_HAS_D3D12)
                  << " DXIL/SPIR-V shader programs and reflection validation passed.\n";
#else
                  << " SPIR-V shader programs and reflection validation passed.\n";
#endif
        return 0;
    }
    catch (const std::exception& exception)
    {
        std::cerr << "Shader compiler test failure: " << exception.what() << '\n';
        return 1;
    }
}
