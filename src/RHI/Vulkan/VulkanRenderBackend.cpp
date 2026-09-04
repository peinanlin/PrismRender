#include "RHI/Vulkan/VulkanRenderBackend.h"

#include <utility>

namespace Prism::RHI::Vulkan
{
VulkanRenderBackend::VulkanRenderBackend(
    const FramePacingConfiguration& framePacing)
    : m_context(framePacing)
{
}

void VulkanRenderBackend::Initialize(
    Platform::Window& window)
{
    m_context.Initialize(window);
}

IFrameContext& VulkanRenderBackend::GetFrameContext()
{
    return m_context;
}

const IFrameContext&
VulkanRenderBackend::GetFrameContext() const
{
    return m_context;
}

IGraphicsDevice& VulkanRenderBackend::GetGraphicsDevice()
{
    return m_context;
}

const IGraphicsDevice&
VulkanRenderBackend::GetGraphicsDevice() const
{
    return m_context;
}

ICommandContext& VulkanRenderBackend::GetCommandContext()
{
    return m_context;
}

const std::string& VulkanRenderBackend::GetAdapterName() const
{
    return m_context.GetAdapterName();
}

const GraphicsAdapterInfo&
VulkanRenderBackend::GetAdapterInfo() const
{
    return m_context.GetAdapterInfo();
}

FrameAdmissionResult VulkanRenderBackend::WaitForFrameAdmission()
{
    return m_context.WaitForFrameAdmission();
}

bool VulkanRenderBackend::ApplyFramePacingConfiguration(
    const FramePacingConfiguration& configuration,
    const std::uint64_t generation,
    std::string* outErrorMessage)
{
    return m_context.ApplyFramePacingConfiguration(
        configuration, generation, outErrorMessage);
}

const FramePacingState& VulkanRenderBackend::GetFramePacingState() const
{
    return m_context.GetFramePacingState();
}

void VulkanRenderBackend::RequestTextureCapture(
    std::filesystem::path outputPath)
{
    m_context.RequestCapture(std::move(outputPath));
}

void VulkanRenderBackend::RecordTextureCapture(
    const ITexture* texture)
{
    m_context.SetCaptureTexture(texture);
}

bool VulkanRenderBackend::ResolveTextureCapture(
    std::string* outErrorMessage)
{
    if (outErrorMessage != nullptr)
    {
        *outErrorMessage = m_context.GetCaptureError();
    }
    return m_context.IsCaptureComplete();
}

VulkanContext& VulkanRenderBackend::GetContext()
{
    return m_context;
}

const VulkanContext& VulkanRenderBackend::GetContext() const
{
    return m_context;
}
} // namespace Prism::RHI::Vulkan
