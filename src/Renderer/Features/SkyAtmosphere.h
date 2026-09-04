#pragma once

#include "Asset/ShaderManager.h"
#include "RHI/ICommandContext.h"
#include "Renderer/RenderGraph.h"

#include <DirectXMath.h>

#include <cstdint>
#include <filesystem>
#include <memory>
#include <vector>

namespace Prism::RHI
{
class IBuffer;
class IComputePipeline;
class IDescriptorSet;
class IDescriptorSetLayout;
class IGraphicsDevice;
class ISampler;
class ITexture;
class ITextureView;
}

namespace Prism::Renderer
{
class PipelineCache;
struct RenderSettings;

struct SkyAtmosphereFeatureSlot
{
};

struct SkyAtmosphereGraphContribution
{
    RHI::ITexture* transmittance = nullptr;
    RHI::ITexture* skyView = nullptr;
    RHI::ResourceState transmittanceInitialState =
        RHI::ResourceState::Undefined;
    RHI::ResourceState skyViewInitialState =
        RHI::ResourceState::Undefined;
    RenderGraph::ParameterExecuteCallback transmittanceExecute;
    RenderGraph::ParameterExecuteCallback skyViewExecute;
};

class SkyAtmosphere
{
public:
    static constexpr std::uint32_t
        TransmittanceWidth = 256;
    static constexpr std::uint32_t
        TransmittanceHeight = 64;
    static constexpr std::uint32_t
        SkyViewWidth = 192;
    static constexpr std::uint32_t
        SkyViewHeight = 108;

    void Initialize(
        RHI::IGraphicsDevice& device,
        Asset::ShaderManager& shaderManager,
        PipelineCache& pipelineCache,
        const std::filesystem::path& shaderPath,
        RHI::ShaderBinaryFormat shaderFormat,
        std::uint32_t framesInFlight);
    void Update(
        std::uint32_t frameIndex,
        const RenderSettings& settings,
        const DirectX::XMFLOAT3& cameraPosition,
        const DirectX::XMFLOAT3& sunDirection,
        float sunIntensity);
    void ExecuteTransmittance(
        RHI::ICommandContext& commandContext,
        std::uint32_t frameIndex) const;
    void ExecuteSkyView(
        RHI::ICommandContext& commandContext,
        std::uint32_t frameIndex) const;
    [[nodiscard]] SkyAtmosphereGraphContribution
        CreateRenderGraphContribution(std::uint32_t frameIndex);
    static void AddPasses(
        RenderGraph& graph,
        TextureHandle& transmittance,
        TextureHandle& skyView,
        RenderGraph::ParameterExecuteCallback
            transmittanceExecute,
        RenderGraph::ParameterExecuteCallback
            skyViewExecute);
    void EndFrame(bool enabled);

    [[nodiscard]] bool IsInitialized() const;
    [[nodiscard]] std::shared_ptr<RHI::ITexture>
        GetSkyViewTexture() const;
    [[nodiscard]] std::shared_ptr<RHI::ITextureView>
        GetSkyViewSampledView(bool enabled) const;

private:
    struct alignas(16) Constants
    {
        // x: planet radius, y: atmosphere radius,
        // z: Rayleigh scale height, w: Mie scale height (kilometres).
        DirectX::XMFLOAT4 planetParameters{};
        // xyz: Rayleigh scattering (1/km), w: solar illuminance.
        DirectX::XMFLOAT4 rayleighScattering{};
        // x: Mie scattering, y: Mie absorption,
        // z: anisotropy, w: multiple-scattering approximation strength.
        DirectX::XMFLOAT4 mieParameters{};
        // xyz: normalized direction toward the sun, w: camera altitude (km).
        DirectX::XMFLOAT4 sunDirectionCameraAltitude{};
        DirectX::XMFLOAT4 lutDimensions{};
    };

    struct FrameResources
    {
        std::shared_ptr<RHI::IBuffer> constants;
        std::shared_ptr<RHI::IDescriptorSet>
            transmittanceDescriptorSet;
        std::shared_ptr<RHI::IDescriptorSet>
            skyViewDescriptorSet;
    };

    RHI::IGraphicsDevice* m_device = nullptr;
    std::shared_ptr<RHI::IDescriptorSetLayout>
        m_transmittanceLayout;
    std::shared_ptr<RHI::IDescriptorSetLayout>
        m_skyViewLayout;
    std::shared_ptr<RHI::IComputePipeline>
        m_transmittancePipeline;
    std::shared_ptr<RHI::IComputePipeline>
        m_skyViewPipeline;
    std::shared_ptr<RHI::ITexture>
        m_transmittanceTexture;
    std::shared_ptr<RHI::ITextureView>
        m_transmittanceSampledView;
    std::shared_ptr<RHI::ITextureView>
        m_transmittanceStorageView;
    std::shared_ptr<RHI::ITexture>
        m_skyViewTexture;
    std::shared_ptr<RHI::ITextureView>
        m_skyViewSampledView;
    std::shared_ptr<RHI::ITextureView>
        m_skyViewStorageView;
    std::shared_ptr<RHI::ITextureView> m_disabledSkyView;
    std::shared_ptr<RHI::ISampler> m_sampler;
    std::vector<FrameResources> m_frames;
    RHI::ResourceState m_transmittanceState =
        RHI::ResourceState::Undefined;
    RHI::ResourceState m_skyViewState =
        RHI::ResourceState::Undefined;
};
} // namespace Prism::Renderer
