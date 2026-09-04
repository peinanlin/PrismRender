#pragma once

#include "RHI/ShaderTypes.h"

#include <array>
#include <complex>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>
#include <vector>

namespace Prism::Asset
{
class ShaderManager;
}

namespace Prism::RHI
{
class IBuffer;
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

struct OceanFftPass
{
    std::uint32_t stage = 0;
    std::uint32_t direction = 0;
    std::uint32_t sourceIndex = 0;
    std::uint32_t destinationIndex = 1;
    bool inverse = true;
    bool bitReverseInput = false;
    bool normalizeOutput = false;
};

// Owns the portable radix-2 FFT plan and the texture butterfly executor used
// by the ocean simulation. Spectrum generation supplies bit-reversed input to
// the production inverse transform; validation callers may request that work
// in the first pass of each dimension.
class OceanFft
{
public:
    static constexpr std::array<std::uint32_t, 3> SupportedSizes{
        128u, 256u, 512u};
    // Cross-backend FP32 regression budget for normalized CPU/GPU fixtures.
    // Typical RTX 5060 errors are below 1e-6; 2e-4 leaves headroom for other
    // conformant shader compilers without hiding visible numerical failure.
    static constexpr float Fp32RegressionTolerance = 2.0e-4f;

    OceanFft() = default;
    ~OceanFft();

    OceanFft(const OceanFft&) = delete;
    OceanFft& operator=(const OceanFft&) = delete;

    [[nodiscard]] bool Configure(std::uint32_t size) noexcept;
    void Reset() noexcept;

    [[nodiscard]] std::uint32_t Size() const noexcept { return m_size; }
    [[nodiscard]] std::uint32_t StageCount() const noexcept
    {
        return m_stageCount;
    }
    [[nodiscard]] std::uint32_t BitReverse(
        std::uint32_t value) const noexcept;
    [[nodiscard]] std::complex<float> Twiddle(
        std::uint32_t stage,
        std::uint32_t lane,
        bool inverse) const noexcept;
    [[nodiscard]] std::vector<OceanFftPass> BuildPasses(
        std::uint32_t dimensions,
        bool inverse,
        std::uint32_t initialSourceIndex = 0u,
        bool bitReverseEachDimension = false,
        bool normalizeEachDimension = false) const;

    // In-place radix-2 transform used as the CPU numerical reference. The
    // inverse path applies 1/N normalization.
    [[nodiscard]] bool Transform(
        std::span<std::complex<float>> values,
        bool inverse) const noexcept;

    void InitializeGpu(
        RHI::IGraphicsDevice& device,
        Asset::ShaderManager& shaderManager,
        PipelineCache& pipelineCache,
        const std::filesystem::path& shaderPath,
        RHI::ShaderBinaryFormat shaderFormat);
    void BindTextureWorkingSet(
        const std::array<std::shared_ptr<RHI::ITextureView>, 2>& spectrumA,
        const std::array<std::shared_ptr<RHI::ITextureView>, 2>& spectrumB);
    void BindTextureArrayWorkingSet(
        const std::array<std::shared_ptr<RHI::ITextureView>, 2>& spectrumA,
        const std::array<std::shared_ptr<RHI::ITextureView>, 2>& spectrumB,
        std::uint32_t arrayLayerCount);
    void ExecuteInverse2D(RHI::ICommandContext& commandContext) const;
    void ExecuteInverseHorizontalArray(
        RHI::ICommandContext& commandContext) const;
    void ExecuteInverseVerticalArray(
        RHI::ICommandContext& commandContext) const;
    [[nodiscard]] bool IsGpuReady() const noexcept;
    [[nodiscard]] bool IsArrayGpuReady() const noexcept;

private:
    struct alignas(16) GpuConstants
    {
        std::uint32_t resolution = 0;
        std::uint32_t stage = 0;
        std::uint32_t direction = 0;
        std::uint32_t inverse = 1;
        std::uint32_t bitReverseInput = 0;
        std::uint32_t normalizeOutput = 0;
        std::uint32_t padding0 = 0;
        std::uint32_t padding1 = 0;
    };

    std::uint32_t m_size = 0;
    std::uint32_t m_stageCount = 0;
    RHI::IGraphicsDevice* m_device = nullptr;
    std::shared_ptr<RHI::IDescriptorSetLayout> m_textureLayout;
    std::shared_ptr<RHI::IComputePipeline> m_texturePipeline;
    std::shared_ptr<RHI::IDescriptorSetLayout> m_arrayTextureLayout;
    std::shared_ptr<RHI::IDescriptorSetLayout> m_arrayRadixLayout;
    std::shared_ptr<RHI::IComputePipeline> m_arrayTexturePipeline;
    std::shared_ptr<RHI::IComputePipeline> m_arrayRadixPipeline;
    std::vector<std::shared_ptr<RHI::IBuffer>> m_stageConstants;
    std::vector<std::shared_ptr<RHI::IDescriptorSet>> m_stageSets;
    std::vector<std::shared_ptr<RHI::IBuffer>> m_arrayStageConstants;
    std::vector<std::shared_ptr<RHI::IDescriptorSet>> m_arrayStageSets;
    std::array<std::shared_ptr<RHI::IBuffer>, 2> m_arrayRadixConstants;
    std::array<std::shared_ptr<RHI::IDescriptorSet>, 2> m_arrayRadixSets;
    std::uint32_t m_arrayLayerCount = 0u;
};
} // namespace Prism::Renderer
