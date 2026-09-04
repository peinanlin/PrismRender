#pragma once

#include "RHI/Vulkan/VulkanLoader.h"
#include "RHI/FramePacing.h"

#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

namespace Prism::RHI::Vulkan
{
struct VulkanQueueFamilyIndices
{
    std::uint32_t graphics = UINT32_MAX;
    std::uint32_t present = UINT32_MAX;
    std::uint32_t compute = UINT32_MAX;
    std::uint32_t transfer = UINT32_MAX;

    [[nodiscard]] bool IsComplete() const;
};

struct VulkanSwapChainSupport
{
    VkSurfaceCapabilitiesKHR capabilities{};
    std::vector<VkSurfaceFormatKHR> formats;
    std::vector<VkPresentModeKHR> presentModes;

    [[nodiscard]] bool IsAdequate() const;
};

[[nodiscard]] VulkanQueueFamilyIndices
FindVulkanQueueFamilies(
    VkPhysicalDevice device,
    VkSurfaceKHR surface);
[[nodiscard]] bool SupportsVulkanDeviceExtensions(
    VkPhysicalDevice device,
    std::span<const char* const> requiredExtensions);
[[nodiscard]] VulkanSwapChainSupport
QueryVulkanSwapChainSupport(
    VkPhysicalDevice device,
    VkSurfaceKHR surface);
[[nodiscard]] VkSurfaceFormatKHR
ChooseVulkanSurfaceFormat(
    std::span<const VkSurfaceFormatKHR> formats);
[[nodiscard]] VkPresentModeKHR
ChooseVulkanPresentMode(
    std::span<const VkPresentModeKHR> presentModes,
    PresentationIntent intent);
} // namespace Prism::RHI::Vulkan
