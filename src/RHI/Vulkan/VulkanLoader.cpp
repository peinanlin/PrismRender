#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable: 4551)
#endif

#define GLAD_VULKAN_IMPLEMENTATION
#include "RHI/Vulkan/VulkanLoader.h"

#if defined(_MSC_VER)
#pragma warning(pop)
#endif

#include <GLFW/glfw3.h>

namespace Prism::RHI::Vulkan
{
namespace
{
GLADapiproc LoadGlobalFunction(const char* name)
{
    return reinterpret_cast<GLADapiproc>(glfwGetInstanceProcAddress(VK_NULL_HANDLE, name));
}

struct InstanceLoaderContext
{
    VkInstance instance = VK_NULL_HANDLE;
};

GLADapiproc LoadInstanceFunction(void* userData, const char* name)
{
    const auto* context = static_cast<const InstanceLoaderContext*>(userData);
    return reinterpret_cast<GLADapiproc>(glfwGetInstanceProcAddress(context->instance, name));
}
} // namespace

int LoadGlobalFunctions()
{
    return gladLoadVulkan(VK_NULL_HANDLE, LoadGlobalFunction);
}

int LoadInstanceFunctions(const VkInstance instance, const VkPhysicalDevice physicalDevice)
{
    InstanceLoaderContext context{instance};
    return gladLoadVulkanUserPtr(physicalDevice, LoadInstanceFunction, &context);
}
} // namespace Prism::RHI::Vulkan
