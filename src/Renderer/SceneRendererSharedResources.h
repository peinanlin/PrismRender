#pragma once

#include "Asset/ShaderManager.h"
#include "Renderer/Pipeline/PipelineCache.h"
#include "Renderer/Features/Ocean/OceanFallbackResources.h"

#include <cstdint>
#include <filesystem>

#include <memory>

namespace Prism::Asset
{
class Texture;
}

namespace Prism::RHI
{
class IGraphicsDevice;
enum class ShaderBinaryFormat;
} // namespace Prism::RHI

namespace Prism::Renderer
{
class InteractiveTerrain;
class LocalWaveGpuResources;
class SpectralOceanSimulation;
// View-independent renderer assets. A composition root owns one instance and
// shares it between game and editor views so expensive IBL textures and ocean
// simulation resources are built once for a graphics device.
class SceneRendererSharedResources
{
public:
    void Initialize(RHI::IGraphicsDevice& device,
        std::shared_ptr<Asset::Texture> environmentCubemap);
    void ConfigureInteractiveTerrain(RHI::IGraphicsDevice& device,
        const std::filesystem::path& shaderPath,
        RHI::ShaderBinaryFormat shaderFormat,
        std::uint32_t framesInFlight);
    void EnsureInteractiveTerrainInitialized();
    void InitializeOceanSimulation(RHI::IGraphicsDevice& device,
        const std::filesystem::path& shaderDirectory,
        RHI::ShaderBinaryFormat shaderFormat,
        std::uint32_t framesInFlight);

    [[nodiscard]] bool IsInitialized() const;
    [[nodiscard]] const std::shared_ptr<Asset::Texture>&
    GetEnvironmentCubemap() const;
    [[nodiscard]] const std::shared_ptr<Asset::Texture>&
    GetIrradianceCubemap() const;
    [[nodiscard]] const std::shared_ptr<Asset::Texture>&
    GetPrefilteredSpecularCubemapArray() const;
    [[nodiscard]] const std::shared_ptr<Asset::Texture>&
    GetBrdfLutTexture() const;
    [[nodiscard]] Asset::ShaderManager& GetShaderManager();
    [[nodiscard]] PipelineCache& GetPipelineCache();
    [[nodiscard]] InteractiveTerrain& GetInteractiveTerrain();
    [[nodiscard]] bool IsInteractiveTerrainInitialized() const;
    [[nodiscard]] SpectralOceanSimulation& GetSpectralOcean();
    [[nodiscard]] LocalWaveGpuResources& GetLocalWaveGpuResources();
    [[nodiscard]] const OceanFallbackResources& GetOceanFallbackResources() const
    {
        return m_oceanFallbackResources;
    }

private:
    std::shared_ptr<Asset::Texture> m_environmentCubemap;
    std::shared_ptr<Asset::Texture> m_irradianceCubemap;
    std::shared_ptr<Asset::Texture> m_prefilteredSpecularCubemapArray;
    std::shared_ptr<Asset::Texture> m_brdfLutTexture;
    Asset::ShaderManager m_shaderManager;
    PipelineCache m_pipelineCache;
    std::shared_ptr<InteractiveTerrain> m_interactiveTerrain;
    RHI::IGraphicsDevice* m_terrainDevice = nullptr;
    std::filesystem::path m_terrainShaderPath;
    RHI::ShaderBinaryFormat m_terrainShaderFormat{};
    std::uint32_t m_terrainFramesInFlight = 0;
    std::shared_ptr<SpectralOceanSimulation> m_spectralOcean;
    std::shared_ptr<LocalWaveGpuResources> m_localWaveGpuResources;
    OceanFallbackResources m_oceanFallbackResources;
};
} // namespace Prism::Renderer
