#pragma once

#include <glad/vulkan.h>

namespace Prism::RHI::Vulkan
{
int LoadGlobalFunctions();
int LoadInstanceFunctions(VkInstance instance, VkPhysicalDevice physicalDevice);
} // namespace Prism::RHI::Vulkan

