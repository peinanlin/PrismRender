#pragma once

#include "RHI/IRenderBackend.h"
#include "RHI/Vulkan/VulkanContext.h"

namespace Prism::RHI::Vulkan
{
class VulkanRenderBackend final : public IRenderBackend
{
public:
    explicit VulkanRenderBackend(
        const FramePacingConfiguration& framePacing = {});
    void Initialize(Platform::Window& window) override;
    IFrameContext& GetFrameContext() override;
    const IFrameContext& GetFrameContext() const override;
    IGraphicsDevice& GetGraphicsDevice() override;
    const IGraphicsDevice& GetGraphicsDevice() const override;
    ICommandContext& GetCommandContext() override;
    const std::string& GetAdapterName() const override;
    const GraphicsAdapterInfo& GetAdapterInfo() const override;
    FrameAdmissionResult WaitForFrameAdmission() override;
    bool ApplyFramePacingConfiguration(
        const FramePacingConfiguration& configuration,
        std::uint64_t generation,
        std::string* outErrorMessage = nullptr) override;
    const FramePacingState& GetFramePacingState() const override;
    void RequestTextureCapture(
        std::filesystem::path outputPath) override;
    void RecordTextureCapture(
        const ITexture* texture) override;
    bool ResolveTextureCapture(
        std::string* outErrorMessage = nullptr) override;

    VulkanContext& GetContext();
    const VulkanContext& GetContext() const;

private:
    VulkanContext m_context;
};
} // namespace Prism::RHI::Vulkan
