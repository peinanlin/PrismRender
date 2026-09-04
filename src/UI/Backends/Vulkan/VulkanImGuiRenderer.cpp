#include "UI/Backends/Vulkan/VulkanImGuiRenderer.h"

#include "Core/Assert.h"
#include "RHI/Rendering.h"
#include "RHI/Vulkan/VulkanRenderBackend.h"
#include "RHI/Vulkan/VulkanResources.h"
#include "UI/UiDrawPacket.h"

#include <imgui.h>
#include <backends/imgui_impl_vulkan.h>
#include <GLFW/glfw3.h>

#include <string>
#include <type_traits>

namespace Prism::RHI::Vulkan
{
namespace
{
PFN_vkVoidFunction LoadImGuiVulkanFunction(
    const char* functionName,
    void* userData)
{
    const auto instance = reinterpret_cast<VkInstance>(userData);
    return glfwGetInstanceProcAddress(
        instance,
        functionName);
}

void CheckImGuiVkResult(const VkResult result)
{
    Core::Check(
        result == VK_SUCCESS,
        ("Dear ImGui Vulkan operation failed with VkResult "
         + std::to_string(static_cast<int>(result))));
}

template <typename Handle>
std::uint64_t EncodeVulkanHandle(const Handle handle)
{
    if constexpr (std::is_pointer_v<Handle>)
    {
        return reinterpret_cast<std::uint64_t>(handle);
    }
    else
    {
        return static_cast<std::uint64_t>(handle);
    }
}

template <typename Handle>
Handle DecodeVulkanHandle(const std::uint64_t value)
{
    if constexpr (std::is_pointer_v<Handle>)
    {
        return reinterpret_cast<Handle>(value);
    }
    else
    {
        return static_cast<Handle>(value);
    }
}
} // namespace

void VulkanImGuiRenderer::Initialize(
    IRenderBackend& renderBackend)
{
    if (m_initialized)
    {
        return;
    }

    m_backend = dynamic_cast<VulkanRenderBackend*>(
        &renderBackend);
    Core::Check(
        m_backend != nullptr,
        "Vulkan ImGui requires the Vulkan RHI backend.");
    VulkanContext& context = m_backend->GetContext();

    Core::Check(
        ImGui_ImplVulkan_LoadFunctions(
            VK_API_VERSION_1_3,
            LoadImGuiVulkanFunction,
            reinterpret_cast<void*>(context.GetInstance())),
        "Failed to load Vulkan functions for Dear ImGui.");

    VkSamplerCreateInfo samplerInfo{
        VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    samplerInfo.magFilter = VK_FILTER_LINEAR;
    samplerInfo.minFilter = VK_FILTER_LINEAR;
    samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    samplerInfo.addressModeU =
        VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeV =
        VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeW =
        VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.maxLod = VK_LOD_CLAMP_NONE;
    CheckImGuiVkResult(vkCreateSampler(
        context.GetDevice(),
        &samplerInfo,
        nullptr,
        &m_textureSampler));

    const VkFormat colorFormat =
        context.GetSwapChainFormat();
    ImGui_ImplVulkan_InitInfo initInfo{};
    initInfo.ApiVersion = VK_API_VERSION_1_3;
    initInfo.Instance = context.GetInstance();
    initInfo.PhysicalDevice = context.GetPhysicalDevice();
    initInfo.Device = context.GetDevice();
    initInfo.QueueFamily =
        context.GetGraphicsQueueFamilyIndex();
    initInfo.Queue = context.GetGraphicsQueue();
    initInfo.DescriptorPoolSize = 128;
    initInfo.MinImageCount =
        context.GetFramesInFlight();
    initInfo.ImageCount =
        context.GetSwapChainImageCount();
    initInfo.MSAASamples = VK_SAMPLE_COUNT_1_BIT;
    initInfo.UseDynamicRendering = true;
    initInfo.PipelineRenderingCreateInfo = {
        VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
    initInfo.PipelineRenderingCreateInfo
        .colorAttachmentCount = 1;
    initInfo.PipelineRenderingCreateInfo
        .pColorAttachmentFormats = &colorFormat;
    initInfo.CheckVkResultFn = CheckImGuiVkResult;
    Core::Check(
        ImGui_ImplVulkan_Init(&initInfo),
        "Failed to initialize the ImGui Vulkan renderer.");
    Core::Check(
        ImGui_ImplVulkan_CreateFontsTexture(),
        "Failed to upload the ImGui Vulkan font texture.");
    m_initialized = true;
}

void VulkanImGuiRenderer::Shutdown()
{
    if (!m_initialized)
    {
        return;
    }

    ImGui_ImplVulkan_Shutdown();
    if (m_textureSampler != VK_NULL_HANDLE
        && m_backend != nullptr)
    {
        vkDestroySampler(
            m_backend->GetContext().GetDevice(),
            m_textureSampler,
            nullptr);
    }
    m_textureSampler = VK_NULL_HANDLE;
    m_backend = nullptr;
    m_initialized = false;
}

void VulkanImGuiRenderer::BeginFrame()
{
    Core::Check(
        m_initialized,
        "The Vulkan ImGui renderer is not initialized.");
    ImGui_ImplVulkan_NewFrame();
}

std::uint64_t VulkanImGuiRenderer::RegisterTexture(
    const ITextureView& textureView)
{
    Core::Check(
        m_initialized && m_textureSampler != VK_NULL_HANDLE,
        "The Vulkan ImGui renderer is not initialized.");
    const auto* vulkanView = dynamic_cast<
        const VulkanTextureView*>(&textureView);
    Core::Check(
        vulkanView != nullptr,
        "The Vulkan editor requires a Vulkan texture view.");
    const VkDescriptorSet descriptorSet =
        ImGui_ImplVulkan_AddTexture(
            m_textureSampler,
            vulkanView->GetHandle(),
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    return EncodeVulkanHandle(descriptorSet);
}

void VulkanImGuiRenderer::UnregisterTexture(
    const std::uint64_t textureId)
{
    if (textureId == 0 || !m_initialized)
    {
        return;
    }
    ImGui_ImplVulkan_RemoveTexture(
        DecodeVulkanHandle<VkDescriptorSet>(textureId));
}

void VulkanImGuiRenderer::RenderDrawData(
    const UI::UiDrawPacket& drawPacket)
{
    Core::Check(
        m_initialized && m_backend != nullptr,
        "The Vulkan ImGui renderer is not initialized.");

    VulkanContext& context = m_backend->GetContext();
    RenderingInfo renderingInfo{};
    renderingInfo.width = context.GetFrameWidth();
    renderingInfo.height = context.GetFrameHeight();
    RenderingAttachment colorAttachment{};
    colorAttachment.view =
        &context.GetCurrentBackBufferView();
    colorAttachment.loadOperation = LoadOperation::Clear;
    colorAttachment.storeOperation = StoreOperation::Store;
    colorAttachment.clearColor = {
        0.035f,
        0.040f,
        0.048f,
        1.0f};
    colorAttachment.stateBefore = ResourceState::Present;
    colorAttachment.stateAfter = ResourceState::RenderTarget;
    renderingInfo.colorAttachments.push_back(
        colorAttachment);

    context.BeginRendering(renderingInfo);
    ImGui_ImplVulkan_RenderDrawData(
        &drawPacket.GetDrawData(),
        context.GetCommandBuffer());
    context.EndRendering();
}
} // namespace Prism::RHI::Vulkan
