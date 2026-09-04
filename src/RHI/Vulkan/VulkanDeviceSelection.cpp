#include "RHI/Vulkan/VulkanDeviceSelection.h"

#include <ranges>
#include <set>
#include <stdexcept>
#include <string>

namespace Prism::RHI::Vulkan
{
namespace
{
void CheckVk(const VkResult result, const char* message)
{
    if (result != VK_SUCCESS)
    {
        throw std::runtime_error(
            std::string(message)
            + " VkResult="
            + std::to_string(static_cast<int>(result)));
    }
}
} // namespace

bool VulkanQueueFamilyIndices::IsComplete() const
{
    return graphics != UINT32_MAX
        && present != UINT32_MAX
        && compute != UINT32_MAX
        && transfer != UINT32_MAX;
}

bool VulkanSwapChainSupport::IsAdequate() const
{
    return !formats.empty()
        && !presentModes.empty();
}

VulkanQueueFamilyIndices FindVulkanQueueFamilies(
    const VkPhysicalDevice device,
    const VkSurfaceKHR surface)
{
    std::uint32_t count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(
        device,
        &count,
        nullptr);
    std::vector<VkQueueFamilyProperties> properties(count);
    vkGetPhysicalDeviceQueueFamilyProperties(
        device,
        &count,
        properties.data());

    VulkanQueueFamilyIndices indices{};
    int transferPreference = -1;
    for (std::uint32_t index = 0; index < count; ++index)
    {
        const VkQueueFlags flags =
            properties[index].queueFlags;
        if ((flags & VK_QUEUE_GRAPHICS_BIT) != 0)
        {
            indices.graphics = index;
        }
        if ((flags & VK_QUEUE_COMPUTE_BIT) != 0)
        {
            const bool dedicated =
                (flags & VK_QUEUE_GRAPHICS_BIT) == 0;
            if (indices.compute == UINT32_MAX
                || dedicated)
            {
                indices.compute = index;
            }
        }
        if ((flags & VK_QUEUE_TRANSFER_BIT) != 0)
        {
            const bool hasGraphics =
                (flags & VK_QUEUE_GRAPHICS_BIT) != 0;
            const bool hasCompute =
                (flags & VK_QUEUE_COMPUTE_BIT) != 0;
            const int preference =
                !hasGraphics && !hasCompute
                ? 2
                : (!hasGraphics ? 1 : 0);
            if (preference > transferPreference)
            {
                indices.transfer = index;
                transferPreference = preference;
            }
        }
        VkBool32 presentSupport = VK_FALSE;
        CheckVk(
            vkGetPhysicalDeviceSurfaceSupportKHR(
                device,
                index,
                surface,
                &presentSupport),
            "Failed to query Vulkan present support.");
        if (presentSupport == VK_TRUE)
        {
            indices.present = index;
        }
    }
    if (indices.transfer == UINT32_MAX)
    {
        indices.transfer = indices.graphics;
    }
    return indices;
}

bool SupportsVulkanDeviceExtensions(
    const VkPhysicalDevice device,
    const std::span<const char* const> requiredExtensions)
{
    std::uint32_t count = 0;
    CheckVk(
        vkEnumerateDeviceExtensionProperties(
            device,
            nullptr,
            &count,
            nullptr),
        "Failed to count Vulkan device extensions.");
    std::vector<VkExtensionProperties> available(count);
    CheckVk(
        vkEnumerateDeviceExtensionProperties(
            device,
            nullptr,
            &count,
            available.data()),
        "Failed to enumerate Vulkan device extensions.");

    std::set<std::string> required;
    for (const char* extension : requiredExtensions)
    {
        required.emplace(extension);
    }
    for (const VkExtensionProperties& extension : available)
    {
        required.erase(extension.extensionName);
    }
    return required.empty();
}

VulkanSwapChainSupport QueryVulkanSwapChainSupport(
    const VkPhysicalDevice device,
    const VkSurfaceKHR surface)
{
    VulkanSwapChainSupport support{};
    CheckVk(
        vkGetPhysicalDeviceSurfaceCapabilitiesKHR(
            device,
            surface,
            &support.capabilities),
        "Failed to query Vulkan surface capabilities.");

    std::uint32_t formatCount = 0;
    CheckVk(
        vkGetPhysicalDeviceSurfaceFormatsKHR(
            device,
            surface,
            &formatCount,
            nullptr),
        "Failed to count Vulkan surface formats.");
    support.formats.resize(formatCount);
    if (formatCount > 0)
    {
        CheckVk(
            vkGetPhysicalDeviceSurfaceFormatsKHR(
                device,
                surface,
                &formatCount,
                support.formats.data()),
            "Failed to query Vulkan surface formats.");
    }

    std::uint32_t presentModeCount = 0;
    CheckVk(
        vkGetPhysicalDeviceSurfacePresentModesKHR(
            device,
            surface,
            &presentModeCount,
            nullptr),
        "Failed to count Vulkan present modes.");
    support.presentModes.resize(presentModeCount);
    if (presentModeCount > 0)
    {
        CheckVk(
            vkGetPhysicalDeviceSurfacePresentModesKHR(
                device,
                surface,
                &presentModeCount,
                support.presentModes.data()),
            "Failed to query Vulkan present modes.");
    }
    return support;
}

VkSurfaceFormatKHR ChooseVulkanSurfaceFormat(
    const std::span<const VkSurfaceFormatKHR> formats)
{
    const auto findFormat =
        [formats](const VkFormat requested)
        {
            return std::ranges::find_if(
                formats,
                [requested](const VkSurfaceFormatKHR format)
                {
                    return format.format == requested
                        && format.colorSpace
                            == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
                });
        };
    for (const VkFormat requested : {
             VK_FORMAT_B8G8R8A8_UNORM,
             VK_FORMAT_R8G8B8A8_UNORM,
             VK_FORMAT_B8G8R8A8_SRGB})
    {
        if (const auto found = findFormat(requested);
            found != formats.end())
        {
            return *found;
        }
    }
    if (formats.empty())
    {
        throw std::runtime_error(
            "Vulkan surface format selection requires at least one format.");
    }
    return formats.front();
}

VkPresentModeKHR ChooseVulkanPresentMode(
    const std::span<const VkPresentModeKHR> presentModes,
    const PresentationIntent intent)
{
    const auto supports = [presentModes](const VkPresentModeKHR mode)
    {
        return std::ranges::find(presentModes, mode)
            != presentModes.end();
    };
    if (intent == PresentationIntent::Immediate
        && supports(VK_PRESENT_MODE_IMMEDIATE_KHR))
    {
        return VK_PRESENT_MODE_IMMEDIATE_KHR;
    }
    if (intent != PresentationIntent::Synchronized
        && supports(VK_PRESENT_MODE_MAILBOX_KHR))
    {
        return VK_PRESENT_MODE_MAILBOX_KHR;
    }
    return VK_PRESENT_MODE_FIFO_KHR;
}
} // namespace Prism::RHI::Vulkan
