#include "RHI/Vulkan/VulkanLoader.h"

#include <GLFW/glfw3.h>

#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace
{
void CheckVk(const VkResult result, const char* message)
{
    if (result != VK_SUCCESS)
    {
        throw std::runtime_error(std::string(message) + " VkResult=" + std::to_string(static_cast<int>(result)));
    }
}
} // namespace

int main()
{
    VkInstance instance = VK_NULL_HANDLE;
    bool glfwInitialized = false;
    try
    {
        if (glfwInit() != GLFW_TRUE)
        {
            throw std::runtime_error("GLFW initialization failed.");
        }
        glfwInitialized = true;
        if (glfwVulkanSupported() != GLFW_TRUE)
        {
            throw std::runtime_error("No Vulkan runtime was detected.");
        }
        if (Prism::RHI::Vulkan::LoadGlobalFunctions() < GLAD_MAKE_VERSION(1, 3))
        {
            throw std::runtime_error("A Vulkan 1.3 runtime is required.");
        }

        VkApplicationInfo applicationInfo{VK_STRUCTURE_TYPE_APPLICATION_INFO};
        applicationInfo.pApplicationName = "PrismVulkanRuntimeTests";
        applicationInfo.apiVersion = VK_API_VERSION_1_3;
        VkInstanceCreateInfo createInfo{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
        createInfo.pApplicationInfo = &applicationInfo;
        CheckVk(vkCreateInstance(&createInfo, nullptr, &instance), "Vulkan instance creation failed.");
        if (Prism::RHI::Vulkan::LoadInstanceFunctions(instance, VK_NULL_HANDLE) == 0)
        {
            throw std::runtime_error("Vulkan instance function loading failed.");
        }

        std::uint32_t physicalDeviceCount = 0;
        CheckVk(vkEnumeratePhysicalDevices(instance, &physicalDeviceCount, nullptr), "Physical-device enumeration failed.");
        if (physicalDeviceCount == 0)
        {
            throw std::runtime_error("Vulkan reported no physical devices.");
        }
        std::vector<VkPhysicalDevice> devices(physicalDeviceCount);
        CheckVk(vkEnumeratePhysicalDevices(instance, &physicalDeviceCount, devices.data()), "Physical-device retrieval failed.");
        VkPhysicalDeviceProperties properties{};
        vkGetPhysicalDeviceProperties(devices.front(), &properties);
        std::cout << "Vulkan runtime test passed on: " << properties.deviceName << '\n';

        vkDestroyInstance(instance, nullptr);
        instance = VK_NULL_HANDLE;
        glfwTerminate();
        return EXIT_SUCCESS;
    }
    catch (const std::exception& exception)
    {
        if (instance != VK_NULL_HANDLE && vkDestroyInstance != nullptr)
        {
            vkDestroyInstance(instance, nullptr);
        }
        if (glfwInitialized)
        {
            glfwTerminate();
        }
        std::cerr << "Vulkan runtime test failed: " << exception.what() << '\n';
        return EXIT_FAILURE;
    }
}
