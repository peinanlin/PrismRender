#include "RHI/Vulkan/VulkanContext.h"

#include "Core/Assert.h"
#include "RHI/Vulkan/VulkanResources.h"

#include <array>
#include <algorithm>
#include <stdexcept>
#include <string>
#include <utility>

namespace Prism::RHI::Vulkan
{
namespace
{
void CheckVk(const VkResult result, const char* message)
{
    if (result != VK_SUCCESS)
    {
        throw std::runtime_error(
            std::string(message) + " VkResult="
            + std::to_string(static_cast<int>(result)));
    }
}
}

void VulkanContext::CreateSwapChainResources()
{
    CreateSwapChain();
    CreateImageViews();

    TextureDescription depthDescription{};
    depthDescription.width = m_swapChainExtent.width;
    depthDescription.height = m_swapChainExtent.height;
    depthDescription.format = Format::D32Float;
    depthDescription.usage =
        TextureUsage::DepthStencil
        | TextureUsage::ShaderResource;
    m_depthTexture = CreateTexture(depthDescription);

    TextureViewDescription depthViewDescription{};
    depthViewDescription.type = TextureViewType::DepthStencil;
    m_depthTextureView =
        CreateTextureView(m_depthTexture, depthViewDescription);

    TextureDescription backBufferDescription{};
    backBufferDescription.width = m_swapChainExtent.width;
    backBufferDescription.height = m_swapChainExtent.height;
    backBufferDescription.format = GetSwapChainRhiFormat();
    backBufferDescription.usage =
        TextureUsage::RenderTarget | TextureUsage::CopySource;

    TextureViewDescription backBufferViewDescription{};
    backBufferViewDescription.type = TextureViewType::RenderTarget;
    m_swapChainTextureViews.clear();
    m_swapChainTextureViews.reserve(m_swapChainImages.size());
    m_swapChainTextures.clear();
    m_swapChainTextures.reserve(m_swapChainImages.size());
    for (std::size_t index = 0;
         index < m_swapChainImages.size();
         ++index)
    {
        auto texture = std::make_shared<VulkanTexture>(
            *this,
            backBufferDescription,
            m_swapChainImages[index]);
        m_swapChainTextures.push_back(texture);
        m_swapChainTextureViews.push_back(
            std::make_shared<VulkanTextureView>(
                *this,
                std::move(texture),
                m_swapChainImageViews[index],
                backBufferViewDescription));
    }

    CreateRenderPass();
    CreateFramebuffers();
    m_imageInFlightFences.assign(
        m_swapChainImages.size(),
        VK_NULL_HANDLE);
    m_presentSemaphores.assign(m_swapChainImages.size(), VK_NULL_HANDLE);
    const VkSemaphoreCreateInfo semaphoreInfo{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    for (auto& semaphore : m_presentSemaphores)
        CheckVk(vkCreateSemaphore(m_device, &semaphoreInfo, nullptr, &semaphore),
            "Failed to create a Vulkan per-image presentation semaphore.");
}

void VulkanContext::CreateSwapChain()
{
    const SwapChainSupport support =
        QuerySwapChainSupport(m_physicalDevice);
    const VkSurfaceFormatKHR surfaceFormat =
        ChooseSurfaceFormat(support.formats);
    const VkPresentModeKHR presentMode =
        ChoosePresentMode(support.presentModes);
    const VkExtent2D extent =
        ChooseExtent(support.capabilities);

    std::uint32_t imageCount =
        support.capabilities.minImageCount + 1u;
    if (support.capabilities.maxImageCount > 0
        && imageCount > support.capabilities.maxImageCount)
    {
        imageCount = support.capabilities.maxImageCount;
    }

    m_swapChainSupportsTransferSource =
        (support.capabilities.supportedUsageFlags
         & VK_IMAGE_USAGE_TRANSFER_SRC_BIT)
        != 0;
    VkSwapchainCreateInfoKHR createInfo{
        VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
    createInfo.surface = m_surface;
    createInfo.minImageCount = imageCount;
    createInfo.imageFormat = surfaceFormat.format;
    createInfo.imageColorSpace = surfaceFormat.colorSpace;
    createInfo.imageExtent = extent;
    createInfo.imageArrayLayers = 1;
    createInfo.imageUsage =
        VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT
        | (m_swapChainSupportsTransferSource
               ? VK_IMAGE_USAGE_TRANSFER_SRC_BIT
               : 0u);

    const std::array<std::uint32_t, 2> queueFamilies = {
        m_queueFamilies.graphics,
        m_queueFamilies.present};
    if (m_queueFamilies.graphics != m_queueFamilies.present)
    {
        createInfo.imageSharingMode =
            VK_SHARING_MODE_CONCURRENT;
        createInfo.queueFamilyIndexCount =
            static_cast<std::uint32_t>(queueFamilies.size());
        createInfo.pQueueFamilyIndices = queueFamilies.data();
    }
    else
    {
        createInfo.imageSharingMode =
            VK_SHARING_MODE_EXCLUSIVE;
    }

    createInfo.preTransform =
        support.capabilities.currentTransform;
    constexpr std::array<VkCompositeAlphaFlagBitsKHR, 4>
        CompositeAlphaModes = {
            VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR,
            VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR,
            VK_COMPOSITE_ALPHA_POST_MULTIPLIED_BIT_KHR,
            VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR};
    for (const VkCompositeAlphaFlagBitsKHR mode
         : CompositeAlphaModes)
    {
        if ((support.capabilities.supportedCompositeAlpha & mode)
            != 0)
        {
            createInfo.compositeAlpha = mode;
            break;
        }
    }
    createInfo.presentMode = presentMode;
    createInfo.clipped = VK_TRUE;
    CheckVk(
        vkCreateSwapchainKHR(
            m_device,
            &createInfo,
            nullptr,
            &m_swapChain),
        "Failed to create Vulkan swap chain.");

    CheckVk(
        vkGetSwapchainImagesKHR(
            m_device,
            m_swapChain,
            &imageCount,
            nullptr),
        "Failed to count Vulkan swap-chain images.");
    m_swapChainImages.resize(imageCount);
    CheckVk(
        vkGetSwapchainImagesKHR(
            m_device,
            m_swapChain,
            &imageCount,
            m_swapChainImages.data()),
        "Failed to get Vulkan swap-chain images.");
    m_swapChainFormat = surfaceFormat.format;
    m_swapChainExtent = extent;
    m_swapChainPresentMode = presentMode;
    m_framePacingState.requested = m_framePacingConfiguration;
    m_framePacingState.effectivePresentation =
        presentMode == VK_PRESENT_MODE_IMMEDIATE_KHR
            ? PresentationIntent::Immediate
            : presentMode == VK_PRESENT_MODE_MAILBOX_KHR
                ? PresentationIntent::LowLatencySynchronized
                : PresentationIntent::Synchronized;
    m_framePacingState.effectiveMaxQueuedFrames =
        (std::min)(
            m_framePacingConfiguration.presentation
                    == PresentationIntent::LowLatencySynchronized
                ? 1u
                : m_framePacingConfiguration.maxQueuedFrames,
            FrameCount);
    m_framePacingState.swapchainImageCount = imageCount;
    m_framePacingState.frameResourceSlotCount = FrameCount;
    m_framePacingState.syncInterval =
        presentMode == VK_PRESENT_MODE_IMMEDIATE_KHR ? 0u : 1u;
    m_framePacingState.nativePresentMode =
        presentMode == VK_PRESENT_MODE_IMMEDIATE_KHR ? "IMMEDIATE"
        : presentMode == VK_PRESENT_MODE_MAILBOX_KHR ? "MAILBOX"
        : "FIFO";
    m_framePacingState.admissionSource =
        FrameAdmissionSource::FrameQueueFallback;
    m_framePacingState.tearingSupported =
        std::find(support.presentModes.begin(),
            support.presentModes.end(),
            VK_PRESENT_MODE_IMMEDIATE_KHR)
        != support.presentModes.end();
    m_framePacingState.tearingEnabled =
        presentMode == VK_PRESENT_MODE_IMMEDIATE_KHR;
    m_framePacingState.fallbackReason.clear();
    if (m_framePacingConfiguration.presentation
            == PresentationIntent::Immediate
        && presentMode != VK_PRESENT_MODE_IMMEDIATE_KHR)
    {
        m_framePacingState.fallbackReason =
            "Vulkan IMMEDIATE is unavailable; using a synchronized fallback.";
    }
    else if (m_framePacingConfiguration.presentation
            == PresentationIntent::LowLatencySynchronized
        && presentMode != VK_PRESENT_MODE_MAILBOX_KHR)
    {
        m_framePacingState.fallbackReason =
            "Vulkan MAILBOX is unavailable; using FIFO.";
    }
    if (m_framePacingConfiguration.maxQueuedFrames > FrameCount)
    {
        if (!m_framePacingState.fallbackReason.empty())
        {
            m_framePacingState.fallbackReason += ' ';
        }
        m_framePacingState.fallbackReason +=
            "Maximum queued frames is limited by Vulkan frame-resource slots.";
    }
}

void VulkanContext::CreateImageViews()
{
    m_swapChainImageViews.resize(m_swapChainImages.size());
    for (std::size_t index = 0;
         index < m_swapChainImages.size();
         ++index)
    {
        VkImageViewCreateInfo viewInfo{
            VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        viewInfo.image = m_swapChainImages[index];
        viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        viewInfo.format = m_swapChainFormat;
        viewInfo.components = {
            VK_COMPONENT_SWIZZLE_IDENTITY,
            VK_COMPONENT_SWIZZLE_IDENTITY,
            VK_COMPONENT_SWIZZLE_IDENTITY,
            VK_COMPONENT_SWIZZLE_IDENTITY};
        viewInfo.subresourceRange.aspectMask =
            VK_IMAGE_ASPECT_COLOR_BIT;
        viewInfo.subresourceRange.levelCount = 1;
        viewInfo.subresourceRange.layerCount = 1;
        CheckVk(
            vkCreateImageView(
                m_device,
                &viewInfo,
                nullptr,
                &m_swapChainImageViews[index]),
            "Failed to create Vulkan image view.");
    }
}

void VulkanContext::CreateRenderPass()
{
    VkAttachmentDescription colorAttachment{};
    colorAttachment.format = m_swapChainFormat;
    colorAttachment.samples = VK_SAMPLE_COUNT_1_BIT;
    colorAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    colorAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    colorAttachment.stencilLoadOp =
        VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    colorAttachment.stencilStoreOp =
        VK_ATTACHMENT_STORE_OP_DONT_CARE;
    colorAttachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    colorAttachment.finalLayout =
        VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

    VkAttachmentDescription depthAttachment{};
    depthAttachment.format = VK_FORMAT_D32_SFLOAT;
    depthAttachment.samples = VK_SAMPLE_COUNT_1_BIT;
    depthAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    depthAttachment.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    depthAttachment.stencilLoadOp =
        VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    depthAttachment.stencilStoreOp =
        VK_ATTACHMENT_STORE_OP_DONT_CARE;
    depthAttachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    depthAttachment.finalLayout =
        VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

    const std::array<VkAttachmentDescription, 2> attachments = {
        colorAttachment,
        depthAttachment};
    const VkAttachmentReference colorReference{
        0,
        VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    const VkAttachmentReference depthReference{
        1,
        VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint =
        VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments = &colorReference;
    subpass.pDepthStencilAttachment = &depthReference;

    VkSubpassDependency dependency{};
    dependency.srcSubpass = VK_SUBPASS_EXTERNAL;
    dependency.dstSubpass = 0;
    dependency.srcStageMask =
        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dependency.dstStageMask =
        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT
        | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
    dependency.dstAccessMask =
        VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT
        | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;

    VkRenderPassCreateInfo renderPassInfo{
        VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
    renderPassInfo.attachmentCount =
        static_cast<std::uint32_t>(attachments.size());
    renderPassInfo.pAttachments = attachments.data();
    renderPassInfo.subpassCount = 1;
    renderPassInfo.pSubpasses = &subpass;
    renderPassInfo.dependencyCount = 1;
    renderPassInfo.pDependencies = &dependency;
    CheckVk(
        vkCreateRenderPass(
            m_device,
            &renderPassInfo,
            nullptr,
            &m_renderPass),
        "Failed to create Vulkan render pass.");
}

void VulkanContext::CreateFramebuffers()
{
    const auto depthTexture =
        std::dynamic_pointer_cast<VulkanTexture>(m_depthTexture);
    Core::Check(
        depthTexture != nullptr,
        "Vulkan framebuffer requires a Vulkan depth texture.");
    m_swapChainFramebuffers.resize(m_swapChainImageViews.size());
    for (std::size_t index = 0;
         index < m_swapChainImageViews.size();
         ++index)
    {
        const std::array<VkImageView, 2> attachments = {
            m_swapChainImageViews[index],
            depthTexture->GetImageView()};
        VkFramebufferCreateInfo framebufferInfo{
            VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
        framebufferInfo.renderPass = m_renderPass;
        framebufferInfo.attachmentCount =
            static_cast<std::uint32_t>(attachments.size());
        framebufferInfo.pAttachments = attachments.data();
        framebufferInfo.width = m_swapChainExtent.width;
        framebufferInfo.height = m_swapChainExtent.height;
        framebufferInfo.layers = 1;
        CheckVk(
            vkCreateFramebuffer(
                m_device,
                &framebufferInfo,
                nullptr,
                &m_swapChainFramebuffers[index]),
            "Failed to create Vulkan framebuffer.");
    }
}

void VulkanContext::CreateFrameResources()
{
    std::array<VkCommandBuffer, FrameCount> commandBuffers{};
    VkCommandBufferAllocateInfo allocateInfo{
        VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    allocateInfo.commandPool = m_commandPool;
    allocateInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocateInfo.commandBufferCount = FrameCount;
    CheckVk(
        vkAllocateCommandBuffers(
            m_device,
            &allocateInfo,
            commandBuffers.data()),
        "Failed to allocate Vulkan command buffers.");

    const VkSemaphoreCreateInfo semaphoreInfo{
        VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    VkFenceCreateInfo fenceInfo{
        VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;
    for (std::uint32_t index = 0; index < FrameCount; ++index)
    {
        FrameContext& frame = m_frames[index];
        frame.commandBuffer = commandBuffers[index];
        CheckVk(
            vkCreateSemaphore(
                m_device,
                &semaphoreInfo,
                nullptr,
                &frame.imageAvailable),
            "Failed to create Vulkan acquire semaphore.");
        CheckVk(
            vkCreateFence(
                m_device,
                &fenceInfo,
                nullptr,
                &frame.inFlight),
            "Failed to create Vulkan frame fence.");
    }
}

void VulkanContext::DestroySwapChainResources()
{
    if (m_device == VK_NULL_HANDLE)
    {
        return;
    }
    for (const VkFramebuffer framebuffer
         : m_swapChainFramebuffers)
    {
        vkDestroyFramebuffer(m_device, framebuffer, nullptr);
    }
    m_swapChainFramebuffers.clear();
    if (m_renderPass != VK_NULL_HANDLE)
    {
        vkDestroyRenderPass(m_device, m_renderPass, nullptr);
        m_renderPass = VK_NULL_HANDLE;
    }
    m_swapChainTextureViews.clear();
    m_swapChainTextures.clear();
    m_depthTextureView.reset();
    m_depthTexture.reset();
    for (const VkImageView imageView : m_swapChainImageViews)
    {
        vkDestroyImageView(m_device, imageView, nullptr);
    }
    m_swapChainImageViews.clear();
    m_swapChainImages.clear();
    for (const auto semaphore : m_presentSemaphores)
        vkDestroySemaphore(m_device, semaphore, nullptr);
    m_presentSemaphores.clear();
    m_imageInFlightFences.clear();
    if (m_swapChain != VK_NULL_HANDLE)
    {
        vkDestroySwapchainKHR(m_device, m_swapChain, nullptr);
        m_swapChain = VK_NULL_HANDLE;
    }
}
}
