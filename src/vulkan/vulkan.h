#pragma once

// Dear ImGui's Vulkan backend normally includes the Vulkan SDK header. The
// renderer uses the vendored GLAD Vulkan 1.3 header instead, so this shim keeps
// ImGui on the same loader and supplies the promoted KHR aliases it references.
#include <glad/vulkan.h>

#ifndef VK_KHR_dynamic_rendering
#define VK_KHR_dynamic_rendering 1
#endif

using VkPipelineRenderingCreateInfoKHR =
    VkPipelineRenderingCreateInfo;
using VkRenderingAttachmentInfoKHR =
    VkRenderingAttachmentInfo;
using VkRenderingInfoKHR = VkRenderingInfo;
using PFN_vkCmdBeginRenderingKHR =
    PFN_vkCmdBeginRendering;
using PFN_vkCmdEndRenderingKHR =
    PFN_vkCmdEndRendering;

#ifndef VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO_KHR
#define VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO_KHR \
    VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO
#endif
#ifndef VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO_KHR
#define VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO_KHR \
    VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO
#endif
#ifndef VK_STRUCTURE_TYPE_RENDERING_INFO_KHR
#define VK_STRUCTURE_TYPE_RENDERING_INFO_KHR \
    VK_STRUCTURE_TYPE_RENDERING_INFO
#endif
