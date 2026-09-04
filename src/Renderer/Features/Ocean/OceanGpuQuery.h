#pragma once

#include "RHI/GraphicsResources.h"
#include "RHI/ShaderTypes.h"

#include <DirectXMath.h>

#include <cstdint>
#include <filesystem>
#include <memory>

namespace Prism::Asset { class ShaderManager; }
namespace Prism::RHI
{
class ICommandContext;
class IGraphicsDevice;
class IComputePipeline;
class IDescriptorSet;
class IDescriptorSetLayout;
}
namespace Prism::Renderer
{
class PipelineCache;

struct OceanGpuQueryConstants
{
    std::uint32_t queryCount = 0u;
    std::uint32_t resolution = 0u;
    std::uint32_t localEnabled = 0u;
    std::uint32_t padding = 0u;
    float simulationTimeSeconds = 0.0f;
    float domainSizeMeters = 0.0f;
    DirectX::XMFLOAT2 domainCenter{};
    DirectX::XMFLOAT4 cascadePatchLengths{};
    DirectX::XMFLOAT4 uvWarpParameters{};
};

struct OceanGpuQueryBinding
{
    std::shared_ptr<RHI::IBuffer> constants;
    std::shared_ptr<RHI::IBuffer> points;
    std::shared_ptr<RHI::IBuffer> results;
    std::shared_ptr<RHI::ITextureView> spectralDisplacement;
    std::shared_ptr<RHI::ITextureView> localDisplacement;
};

// Creates and records the reflected Slang query kernel. Fence ownership and
// the optional CPU readback FIFO remain in OceanQueryService.
class OceanGpuQuery
{
public:
    void InitializeGpu(
        RHI::IGraphicsDevice& device,
        Asset::ShaderManager& shaderManager,
        PipelineCache& pipelineCache,
        const std::filesystem::path& shaderDirectory,
        RHI::ShaderBinaryFormat shaderFormat);
    [[nodiscard]] bool IsReady() const noexcept
    {
        return m_pipeline != nullptr && m_layout != nullptr;
    }
    [[nodiscard]] std::shared_ptr<RHI::IDescriptorSet> Bind(
        const OceanGpuQueryBinding& binding);
    void Execute(RHI::ICommandContext& commandContext,
        const RHI::IDescriptorSet& descriptorSet,
        std::uint32_t queryCount) const;

private:
    RHI::IGraphicsDevice* m_device = nullptr;
    std::shared_ptr<RHI::IDescriptorSetLayout> m_layout;
    std::shared_ptr<RHI::IComputePipeline> m_pipeline;
};
} // namespace Prism::Renderer
