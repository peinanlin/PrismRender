#include "Renderer/SceneRendererSharedResources.h"

#include "Asset/EnvironmentMapLoader.h"
#include "Asset/IblEnvironmentBuilder.h"
#include "Asset/Texture.h"
#include "Core/Assert.h"
#include "Renderer/Features/InteractiveTerrain.h"
#include "Renderer/Features/Ocean/LocalWaveGpuResources.h"
#include "Renderer/Features/Ocean/SpectralOceanSimulation.h"

#include <utility>

namespace Prism::Renderer
{
void SceneRendererSharedResources::Initialize(RHI::IGraphicsDevice& device,
    std::shared_ptr<Asset::Texture> environmentCubemap)
{
    if (IsInitialized())
    {
        return;
    }

    m_environmentCubemap = std::move(environmentCubemap);
    if (m_environmentCubemap == nullptr)
    {
        Asset::EnvironmentMapLoader loader;
        m_environmentCubemap = loader.CreateFallbackCubemap(device);
    }

    Asset::IblEnvironmentBuilder builder;
    Asset::IblEnvironmentBuilder::Resources resources =
        builder.Build(device, *m_environmentCubemap);
    m_irradianceCubemap = std::move(resources.irradianceCubemap);
    m_prefilteredSpecularCubemapArray =
        std::move(resources.prefilteredSpecularCubemapArray);
    m_brdfLutTexture = std::move(resources.brdfLutTexture);
    Core::Check(IsInitialized(), resources.statusMessage.c_str());
}

void SceneRendererSharedResources::ConfigureInteractiveTerrain(
    RHI::IGraphicsDevice& device,
    const std::filesystem::path& shaderPath,
    const RHI::ShaderBinaryFormat shaderFormat,
    const std::uint32_t framesInFlight)
{
    if (m_interactiveTerrain == nullptr)
    {
        m_interactiveTerrain = std::make_shared<InteractiveTerrain>();
    }
    m_terrainDevice = &device;
    m_terrainShaderPath = shaderPath;
    m_terrainShaderFormat = shaderFormat;
    m_terrainFramesInFlight = framesInFlight;
}

void SceneRendererSharedResources::EnsureInteractiveTerrainInitialized()
{
    Core::Check(m_interactiveTerrain != nullptr
            && m_terrainDevice != nullptr
            && !m_terrainShaderPath.empty()
            && m_terrainFramesInFlight > 0,
        "Interactive terrain shared resources were not configured.");
    if (m_interactiveTerrain->IsInitialized())
    {
        return;
    }
    m_interactiveTerrain->Initialize(*m_terrainDevice,
        m_shaderManager,
        m_pipelineCache,
        m_terrainShaderPath,
        m_terrainShaderFormat,
        m_terrainFramesInFlight);
}

void SceneRendererSharedResources::InitializeOceanSimulation(
    RHI::IGraphicsDevice& device,
    const std::filesystem::path& shaderDirectory,
    const RHI::ShaderBinaryFormat shaderFormat,
    const std::uint32_t framesInFlight)
{
    if (m_spectralOcean != nullptr)
    {
        Core::Check(m_localWaveGpuResources != nullptr,
            "Shared local-wave resources are unavailable.");
        return;
    }

    m_oceanFallbackResources.Initialize(device);
    m_spectralOcean = std::make_shared<SpectralOceanSimulation>();
    m_spectralOcean->InitializeGpu(device,
        m_shaderManager,
        m_pipelineCache,
        shaderDirectory,
        shaderFormat,
        OceanSimulationQuality::Normal,
        framesInFlight);

    m_localWaveGpuResources = std::make_shared<LocalWaveGpuResources>();
    m_localWaveGpuResources->InitializeGpu(device,
        m_shaderManager,
        m_pipelineCache,
        shaderDirectory / "Ocean",
        shaderFormat,
        framesInFlight);
}

bool SceneRendererSharedResources::IsInitialized() const
{
    return m_environmentCubemap != nullptr && m_irradianceCubemap != nullptr &&
           m_prefilteredSpecularCubemapArray != nullptr &&
           m_brdfLutTexture != nullptr;
}

const std::shared_ptr<Asset::Texture>&
SceneRendererSharedResources::GetEnvironmentCubemap() const
{
    return m_environmentCubemap;
}

const std::shared_ptr<Asset::Texture>&
SceneRendererSharedResources::GetIrradianceCubemap() const
{
    return m_irradianceCubemap;
}

const std::shared_ptr<Asset::Texture>&
SceneRendererSharedResources::GetPrefilteredSpecularCubemapArray() const
{
    return m_prefilteredSpecularCubemapArray;
}

const std::shared_ptr<Asset::Texture>&
SceneRendererSharedResources::GetBrdfLutTexture() const
{
    return m_brdfLutTexture;
}

Asset::ShaderManager& SceneRendererSharedResources::GetShaderManager()
{
    return m_shaderManager;
}

PipelineCache& SceneRendererSharedResources::GetPipelineCache()
{
    return m_pipelineCache;
}

InteractiveTerrain& SceneRendererSharedResources::GetInteractiveTerrain()
{
    Core::Check(m_interactiveTerrain != nullptr,
        "Interactive terrain shared resources are not configured.");
    return *m_interactiveTerrain;
}

bool SceneRendererSharedResources::IsInteractiveTerrainInitialized() const
{
    return m_interactiveTerrain != nullptr
        && m_interactiveTerrain->IsInitialized();
}

SpectralOceanSimulation& SceneRendererSharedResources::GetSpectralOcean()
{
    Core::Check(m_spectralOcean != nullptr && m_spectralOcean->IsGpuReady(),
        "Shared spectral-ocean resources are unavailable.");
    return *m_spectralOcean;
}

LocalWaveGpuResources& SceneRendererSharedResources::GetLocalWaveGpuResources()
{
    Core::Check(m_localWaveGpuResources != nullptr &&
                    m_localWaveGpuResources->IsGpuReady(),
        "Shared local-wave resources are unavailable.");
    return *m_localWaveGpuResources;
}
} // namespace Prism::Renderer
