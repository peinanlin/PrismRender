#pragma once

#include "Asset/ShaderManager.h"
#include "RHI/GraphicsResources.h"
#include "Renderer/RenderGraph.h"

#include <DirectXMath.h>

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

struct InteractiveTerrainFeatureSlot
{
};

struct InteractiveTerrainGraphContribution
{
    RHI::ITexture* rawHeight = nullptr;
    RHI::ITexture* erodedHeight = nullptr;
    RHI::ResourceState rawHeightInitialState =
        RHI::ResourceState::Undefined;
    RHI::ResourceState erodedHeightInitialState =
        RHI::ResourceState::Undefined;
    RenderGraph::ParameterExecuteCallback execute;
};

struct InteractiveTerrainCommand
{
    std::uint64_t revision = 0;
    DirectX::XMFLOAT2 brushUv{0.5f, 0.5f};
    float brushRadius = 0.05f;
    float brushDelta = 0.0f;
    bool brushActive = false;
    bool reset = false;
    bool fullUpdate = false;
    bool erosionEnabled = true;
};

// Owns the persistent, view-independent terrain heightfield. The point-wise
// erosion pass is intentionally independent from the terrain mesh so both the
// Game and Scene renderers sample exactly the same edited result.
class InteractiveTerrain
{
public:
    static constexpr std::uint32_t Resolution = 512;

    void Initialize(
        RHI::IGraphicsDevice& device,
        Asset::ShaderManager& shaderManager,
        PipelineCache& pipelineCache,
        const std::filesystem::path& shaderPath,
        RHI::ShaderBinaryFormat shaderFormat,
        std::uint32_t framesInFlight);
    void UpdateCommand(const InteractiveTerrainCommand& command);
    void Execute(
        RHI::ICommandContext& commandContext,
        std::uint32_t frameIndex);
    [[nodiscard]] InteractiveTerrainGraphContribution
        CreateRenderGraphContribution(std::uint32_t frameIndex);
    static void AddPasses(
        RenderGraph& graph,
        TextureHandle& rawHeight,
        TextureHandle& erodedHeight,
        RenderGraph::ParameterExecuteCallback execute);
    void EndFrame(bool enabled);
    void RequestReset() noexcept;

    [[nodiscard]] const std::shared_ptr<RHI::ITexture>&
        GetHeightTexture() const;
    [[nodiscard]] bool IsInitialized() const;
    [[nodiscard]] bool HasPendingUpdate() const noexcept
    {
        return m_needsInitialization
            || m_pendingCommand.revision != m_lastExecutedRevision;
    }
    [[nodiscard]] std::uint64_t PendingCommandRevision() const noexcept
    {
        return m_pendingCommand.revision;
    }

private:
    struct alignas(16) Constants
    {
        DirectX::XMUINT4 dispatchParams{};
        DirectX::XMFLOAT4 brushParams{};
        DirectX::XMFLOAT4 erosionParams{};
    };

    struct FrameResources
    {
        std::shared_ptr<RHI::IBuffer> constants;
        std::shared_ptr<RHI::IDescriptorSet> descriptorSet;
    };

    std::shared_ptr<RHI::IBuffer> CreateConstantsBuffer() const;

    RHI::IGraphicsDevice* m_device = nullptr;
    std::shared_ptr<RHI::IDescriptorSetLayout> m_layout;
    std::shared_ptr<RHI::IComputePipeline> m_brushPipeline;
    std::shared_ptr<RHI::IComputePipeline> m_erosionPipeline;
    std::shared_ptr<RHI::ITexture> m_rawHeight;
    std::shared_ptr<RHI::ITexture> m_erodedHeight;
    std::shared_ptr<RHI::ITextureView> m_rawHeightStorage;
    std::shared_ptr<RHI::ITextureView> m_erodedHeightStorage;
    std::vector<FrameResources> m_frames;
    InteractiveTerrainCommand m_pendingCommand{};
    std::uint64_t m_lastExecutedRevision = 0;
    bool m_needsInitialization = true;
    RHI::ResourceState m_rawHeightState =
        RHI::ResourceState::Undefined;
    RHI::ResourceState m_erodedHeightState =
        RHI::ResourceState::Undefined;
};
} // namespace Prism::Renderer
