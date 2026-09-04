#pragma once

#include "Asset/ShaderManager.h"
#include "RHI/GraphicsResources.h"
#include "Renderer/Features/Ocean/OceanFft.h"
#include "Renderer/Features/Ocean/OceanSettings.h"
#include "Renderer/RenderGraph.h"

#include <DirectXMath.h>

#include <array>
#include <bit>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <vector>

namespace Prism::RHI
{
class IComputePipeline;
class IDescriptorSet;
class IDescriptorSetLayout;
class IGraphicsDevice;
class ICommandContext;
class ITextureView;
}

namespace Prism::Renderer
{
class PipelineCache;

struct FftOceanFeatureSlot
{
};

struct FftOceanGraphContribution
{
    std::array<RHI::ITexture*, 2> spectrumA{};
    std::array<RHI::ITexture*, 2> spectrumB{};
    RHI::ITexture* displacement = nullptr;
    RHI::ITexture* normalFoam = nullptr;
    std::array<RHI::ResourceState, 2> spectrumAStates{};
    std::array<RHI::ResourceState, 2> spectrumBStates{};
    RHI::ResourceState displacementState =
        RHI::ResourceState::Undefined;
    RHI::ResourceState normalFoamState =
        RHI::ResourceState::Undefined;
    RenderGraph::ParameterExecuteCallback execute;
};

class FftOcean
{
public:
    // The legacy comparison Lab intentionally remains 128 squared. All FFT
    // planning, stage counts, and dispatch metadata are resolution-derived in
    // OceanFft so the spectral path can select 128/256/512 at runtime.
    static constexpr std::uint32_t Resolution = 128;
    static constexpr std::uint32_t MipLevelCount =
        std::bit_width(Resolution);
    static_assert(
        std::has_single_bit(Resolution),
        "FFT ocean resolution must remain a power of two.");

    void Initialize(
        RHI::IGraphicsDevice& device,
        Asset::ShaderManager& shaderManager,
        PipelineCache& pipelineCache,
        const std::filesystem::path& shaderPath,
        RHI::ShaderBinaryFormat shaderFormat,
        std::uint32_t framesInFlight);
    void Update(
        std::uint32_t frameIndex,
        float timeSeconds,
        float patchLength,
        const DirectX::XMFLOAT2& windDirection,
        float windSpeed,
        float amplitude,
        float choppiness);
    void Update(
        std::uint32_t frameIndex,
        float timeSeconds,
        const OceanSettings& settings);
    void Execute(
        RHI::ICommandContext& commandContext,
        std::uint32_t frameIndex) const;
    [[nodiscard]] FftOceanGraphContribution
        CreateRenderGraphContribution(std::uint32_t frameIndex);
    static void AddPasses(
        RenderGraph& graph,
        std::array<TextureHandle, 2>& spectrumA,
        std::array<TextureHandle, 2>& spectrumB,
        TextureHandle& displacement,
        TextureHandle& normalFoam,
        RenderGraph::ParameterExecuteCallback execute);
    void EndFrame(bool enabled);

    [[nodiscard]] const std::shared_ptr<RHI::ITexture>&
        GetDisplacementTexture() const;
    [[nodiscard]] const std::shared_ptr<RHI::ITexture>&
        GetNormalFoamTexture() const;
    [[nodiscard]] bool IsInitialized() const;

private:
    struct alignas(16) Constants
    {
        std::uint32_t resolution = Resolution;
        std::uint32_t stage = 0;
        std::uint32_t direction = 0;
        float timeSeconds = 0.0f;
        DirectX::XMFLOAT2 baseWindDirection{1.0f, 0.0f};
        float baseWindSpeed = 4.7f;
        float baseFetchMeters = 100.0f;
        float basePeaking = 3.3f;
        float baseAmplitude = 1.0f;
        DirectX::XMFLOAT2 swellDirection{0.0f, 1.0f};
        float swellSpeed = 1.5f;
        float swellFetchMeters = 520000.0f;
        float swellPeaking = 10.0f;
        float swellAmplitude = 1.0f;
        float patchLength = 320.0f;
        float choppiness = 1.25f;
        float baseCutoffLength = 0.0f;
        float baseCutoffPower = 0.0f;
        float swellCutoffLength = 60.0f;
        float swellCutoffPower = 1.0f;
        float baseDependency = 1.0f;
        float swellDependency = 1.0f;
    };

    struct FrameResources
    {
        std::shared_ptr<RHI::IBuffer> generationConstants;
        std::shared_ptr<RHI::IDescriptorSet> generationSet;
    };

    std::shared_ptr<RHI::IBuffer> CreateConstantsBuffer(
        const Constants& constants) const;

    RHI::IGraphicsDevice* m_device = nullptr;
    std::shared_ptr<RHI::IDescriptorSetLayout> m_generationLayout;
    std::shared_ptr<RHI::IDescriptorSetLayout> m_buildLayout;
    std::shared_ptr<RHI::IDescriptorSetLayout> m_downsampleLayout;
    std::shared_ptr<RHI::IComputePipeline> m_generationPipeline;
    std::shared_ptr<RHI::IComputePipeline> m_buildPipeline;
    std::shared_ptr<RHI::IComputePipeline> m_downsamplePipeline;
    std::array<std::shared_ptr<RHI::ITexture>, 2> m_spectrumA;
    std::array<std::shared_ptr<RHI::ITexture>, 2> m_spectrumB;
    std::array<std::shared_ptr<RHI::ITextureView>, 2> m_spectrumAStorage;
    std::array<std::shared_ptr<RHI::ITextureView>, 2> m_spectrumBStorage;
    std::shared_ptr<RHI::ITexture> m_displacement;
    std::shared_ptr<RHI::ITexture> m_normalFoam;
    std::array<std::shared_ptr<RHI::ITextureView>, MipLevelCount>
        m_displacementStorage;
    std::array<std::shared_ptr<RHI::ITextureView>, MipLevelCount>
        m_normalFoamStorage;
    std::vector<FrameResources> m_frames;
    OceanFft m_fft;
    std::shared_ptr<RHI::IBuffer> m_buildConstants;
    std::shared_ptr<RHI::IDescriptorSet> m_buildSet;
    std::array<
        std::shared_ptr<RHI::IDescriptorSet>,
        MipLevelCount - 1u> m_downsampleSets;
    std::array<RHI::ResourceState, 2> m_spectrumAStates{
        RHI::ResourceState::Undefined,
        RHI::ResourceState::Undefined};
    std::array<RHI::ResourceState, 2> m_spectrumBStates{
        RHI::ResourceState::Undefined,
        RHI::ResourceState::Undefined};
    RHI::ResourceState m_displacementState =
        RHI::ResourceState::Undefined;
    RHI::ResourceState m_normalFoamState =
        RHI::ResourceState::Undefined;
};
} // namespace Prism::Renderer
