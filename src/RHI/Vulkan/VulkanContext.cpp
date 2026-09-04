#include "RHI/Vulkan/VulkanContext.h"
#include "RHI/Vulkan/VulkanDeviceSelection.h"

#include "Core/Assert.h"
#include "Core/Environment.h"
#include "Platform/Window.h"
#include "RHI/Vulkan/VulkanPipeline.h"
#include "RHI/Vulkan/VulkanParallelCommandRecording.h"
#include "RHI/Vulkan/VulkanResources.h"
#include "RHI/Vulkan/VulkanTransientResources.h"
#include "RHI/Vulkan/VulkanTypeConversions.h"

#include <GLFW/glfw3.h>

#include <algorithm>
#include <atomic>
#include <iostream>
#include <array>
#include <cstring>
#include <fstream>
#include <limits>
#include <optional>
#include <set>
#include <stdexcept>
#include <utility>

namespace Prism::RHI::Vulkan
{
VulkanContext::VulkanContext(
    const FramePacingConfiguration& framePacing)
    : m_framePacingConfiguration(framePacing)
{
    ValidateFramePacingConfiguration(m_framePacingConfiguration);
    m_framePacingState.requested = m_framePacingConfiguration;
}

namespace
{
std::atomic_uint VulkanValidationErrors{0u};

VkBool32 GLAD_API_PTR ValidationCallback(VkDebugReportFlagsEXT flags,
    VkDebugReportObjectTypeEXT, std::uint64_t, std::size_t, std::int32_t,
    const char*, const char* message, void*)
{
    std::cerr << "[Vulkan validation] " << message << '\n';
    if (flags & VK_DEBUG_REPORT_ERROR_BIT_EXT)
        ++VulkanValidationErrors;
    return VK_FALSE;
}

class VulkanAccelerationStructure final
    : public IRayTracingAccelerationStructure
{
public:
    VulkanAccelerationStructure(
        VulkanContext& context,
        AccelerationStructureBuildDescription
            description,
        const AccelerationStructureBuildSizes sizes,
        std::shared_ptr<IBuffer> storageBuffer,
        const VkAccelerationStructureKHR handle,
        const VkDeviceAddress deviceAddress)
        : m_context(&context)
        , m_description(std::move(description))
        , m_sizes(sizes)
        , m_storageBuffer(std::move(storageBuffer))
        , m_handle(handle)
        , m_deviceAddress(deviceAddress)
    {
    }

    ~VulkanAccelerationStructure() override
    {
        if (m_context == nullptr
            || m_handle == VK_NULL_HANDLE)
        {
            return;
        }
        const VkAccelerationStructureKHR handle =
            std::exchange(
                m_handle,
                VK_NULL_HANDLE);
        m_context->RetireGpuObject(
            [handle](const VkDevice device)
            {
                vkDestroyAccelerationStructureKHR(
                    device,
                    handle,
                    nullptr);
            });
    }

    GraphicsApi GetGraphicsApi() const override
    {
        return GraphicsApi::Vulkan;
    }

    const AccelerationStructureBuildDescription&
    GetDescription() const override
    {
        return m_description;
    }

    const AccelerationStructureBuildSizes&
    GetBuildSizes() const override
    {
        return m_sizes;
    }

    std::uint64_t GetDeviceAddress() const override
    {
        return m_deviceAddress;
    }

    std::uint64_t GetNativeHandleBits() const override
    {
        return static_cast<std::uint64_t>(
            reinterpret_cast<std::uintptr_t>(m_handle));
    }

    VkAccelerationStructureKHR GetHandle() const
    {
        return m_handle;
    }

private:
    VulkanContext* m_context = nullptr;
    AccelerationStructureBuildDescription
        m_description;
    AccelerationStructureBuildSizes m_sizes;
    std::shared_ptr<IBuffer> m_storageBuffer;
    VkAccelerationStructureKHR m_handle =
        VK_NULL_HANDLE;
    VkDeviceAddress m_deviceAddress = 0;
};

constexpr std::array<const char*, 1> RequiredDeviceExtensions = {
    VK_KHR_SWAPCHAIN_EXTENSION_NAME,
};
constexpr std::array<const char*, 2>
    AccelerationStructureDeviceExtensions = {
        VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME,
        VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME};
void CheckVk(const VkResult result, const char* message)
{
    if (result != VK_SUCCESS)
    {
        throw std::runtime_error(std::string(message) + " VkResult=" + std::to_string(static_cast<int>(result)));
    }
}

VkBuildAccelerationStructureFlagsKHR
ToNativeBuildFlags(
    const AccelerationStructureBuildFlags flags)
{
    VkBuildAccelerationStructureFlagsKHR result = 0;
    if (HasAnyFlag(
            flags,
            AccelerationStructureBuildFlags::AllowUpdate))
    {
        result |=
            VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_UPDATE_BIT_KHR;
    }
    if (HasAnyFlag(
            flags,
            AccelerationStructureBuildFlags::AllowCompaction))
    {
        result |=
            VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_COMPACTION_BIT_KHR;
    }
    if (HasAnyFlag(
            flags,
            AccelerationStructureBuildFlags::PreferFastTrace))
    {
        result |=
            VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
    }
    if (HasAnyFlag(
            flags,
            AccelerationStructureBuildFlags::PreferFastBuild))
    {
        result |=
            VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_BUILD_BIT_KHR;
    }
    if (HasAnyFlag(
            flags,
            AccelerationStructureBuildFlags::MinimizeMemory))
    {
        result |=
            VK_BUILD_ACCELERATION_STRUCTURE_LOW_MEMORY_BIT_KHR;
    }
    return result;
}

VkGeometryInstanceFlagsKHR ToNativeInstanceFlags(
    const RayTracingInstanceFlags flags)
{
    VkGeometryInstanceFlagsKHR result = 0;
    if (HasAnyFlag(
            flags,
            RayTracingInstanceFlags::
                TriangleCullDisable))
    {
        result |=
            VK_GEOMETRY_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT_KHR;
    }
    if (HasAnyFlag(
            flags,
            RayTracingInstanceFlags::
                TriangleFrontCounterClockwise))
    {
        result |=
            VK_GEOMETRY_INSTANCE_TRIANGLE_FRONT_COUNTERCLOCKWISE_BIT_KHR;
    }
    if (HasAnyFlag(
            flags,
            RayTracingInstanceFlags::ForceOpaque))
    {
        result |=
            VK_GEOMETRY_INSTANCE_FORCE_OPAQUE_BIT_KHR;
    }
    if (HasAnyFlag(
            flags,
            RayTracingInstanceFlags::
                ForceNonOpaque))
    {
        result |=
            VK_GEOMETRY_INSTANCE_FORCE_NO_OPAQUE_BIT_KHR;
    }
    return result;
}

VkDeviceAddress GetBufferDeviceAddress(
    const VkDevice device,
    const VulkanBuffer& buffer)
{
    VkBufferDeviceAddressInfo addressInfo{
        VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO};
    addressInfo.buffer = buffer.GetHandle();
    return vkGetBufferDeviceAddress(
        device,
        &addressInfo);
}

std::set<std::string> EnumerateDeviceExtensions(
    const VkPhysicalDevice device)
{
    std::uint32_t count = 0;
    CheckVk(
        vkEnumerateDeviceExtensionProperties(
            device,
            nullptr,
            &count,
            nullptr),
        "Failed to count Vulkan device extensions.");
    std::vector<VkExtensionProperties> properties(
        count);
    CheckVk(
        vkEnumerateDeviceExtensionProperties(
            device,
            nullptr,
            &count,
            properties.data()),
        "Failed to enumerate Vulkan device extensions.");
    std::set<std::string> extensions;
    for (const VkExtensionProperties& property :
         properties)
    {
        extensions.emplace(property.extensionName);
    }
    return extensions;
}

std::size_t QueueIndex(
    const CommandQueueType queue)
{
    return queue == CommandQueueType::Compute
        ? 1u
        : 0u;
}

VkAttachmentLoadOp ToNativeLoadOperation(const LoadOperation operation)
{
    switch (operation)
    {
    case LoadOperation::Load: return VK_ATTACHMENT_LOAD_OP_LOAD;
    case LoadOperation::Clear: return VK_ATTACHMENT_LOAD_OP_CLEAR;
    case LoadOperation::Discard: return VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    }
    throw std::invalid_argument("Unsupported Vulkan attachment load operation.");
}

VkAttachmentStoreOp ToNativeStoreOperation(const StoreOperation operation)
{
    switch (operation)
    {
    case StoreOperation::Store: return VK_ATTACHMENT_STORE_OP_STORE;
    case StoreOperation::Discard: return VK_ATTACHMENT_STORE_OP_DONT_CARE;
    }
    throw std::invalid_argument("Unsupported Vulkan attachment store operation.");
}

void WriteUint16(std::ostream& stream, const std::uint16_t value)
{
    const std::array<char, 2> bytes = {
        static_cast<char>(value & 0xffu),
        static_cast<char>((value >> 8u) & 0xffu),
    };
    stream.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}

void WriteUint32(std::ostream& stream, const std::uint32_t value)
{
    const std::array<char, 4> bytes = {
        static_cast<char>(value & 0xffu),
        static_cast<char>((value >> 8u) & 0xffu),
        static_cast<char>((value >> 16u) & 0xffu),
        static_cast<char>((value >> 24u) & 0xffu),
    };
    stream.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}
} // namespace

bool VulkanContext::QueueFamilyIndices::IsComplete() const
{
    return graphics != UINT32_MAX
        && present != UINT32_MAX
        && compute != UINT32_MAX
        && transfer != UINT32_MAX;
}

bool VulkanContext::SwapChainSupport::IsAdequate() const
{
    return !formats.empty() && !presentModes.empty();
}

VulkanContext::~VulkanContext()
{
    if (m_device != VK_NULL_HANDLE)
    {
        FlushPendingUploads(true);
        vkDeviceWaitIdle(m_device);
    }

    DestroyCaptureResources();
    DestroySwapChainResources();

    m_descriptorAllocator.reset();

    if (m_device != VK_NULL_HANDLE)
    {
        {
            std::scoped_lock lock(
                m_resourceRetirementMutex);
            for (FrameContext& frame : m_frames)
            {
                for (auto& destroy :
                     frame.retiredGpuObjects)
                {
                    destroy(m_device);
                }
                frame.retiredGpuObjects.clear();
            }
            m_resourceRetirementStatistics
                .totalReclaimedObjectCount +=
                m_resourceRetirementStatistics
                    .pendingObjectCount;
            m_resourceRetirementStatistics
                .pendingObjectCount = 0;
        }
        for (FrameContext& frame : m_frames)
        {
            for (const RetiredUpload& upload :
                 frame.retiredUploads)
            {
                if (upload.buffer != VK_NULL_HANDLE)
                {
                    vkDestroyBuffer(
                        m_device,
                        upload.buffer,
                        nullptr);
                }
                if (upload.memory != VK_NULL_HANDLE)
                {
                    vkFreeMemory(
                        m_device,
                        upload.memory,
                        nullptr);
                }
            }
            for (const VkCommandPool pool :
                 frame.retiredCommandPools)
            {
                if (pool != VK_NULL_HANDLE)
                {
                    vkDestroyCommandPool(
                        m_device,
                        pool,
                        nullptr);
                }
            }
            if (frame.imageAvailable != VK_NULL_HANDLE)
            {
                vkDestroySemaphore(m_device, frame.imageAvailable, nullptr);
            }
            if (frame.inFlight != VK_NULL_HANDLE)
            {
                vkDestroyFence(m_device, frame.inFlight, nullptr);
            }
        }
        for (UploadPage& page : m_uploadPages)
        {
            if (page.memory != VK_NULL_HANDLE
                && page.cpuAddress != nullptr)
            {
                vkUnmapMemory(
                    m_device,
                    page.memory);
                page.cpuAddress = nullptr;
            }
            if (page.buffer != VK_NULL_HANDLE)
            {
                vkDestroyBuffer(
                    m_device,
                    page.buffer,
                    nullptr);
            }
            if (page.memory != VK_NULL_HANDLE)
            {
                vkFreeMemory(
                    m_device,
                    page.memory,
                    nullptr);
            }
        }
        m_uploadPages.clear();
        if (m_commandPool != VK_NULL_HANDLE)
        {
            vkDestroyCommandPool(m_device, m_commandPool, nullptr);
        }
        if (m_computeCommandPool != VK_NULL_HANDLE)
        {
            vkDestroyCommandPool(
                m_device,
                m_computeCommandPool,
                nullptr);
        }
        if (m_queueTimelineSemaphore != VK_NULL_HANDLE)
        {
            vkDestroySemaphore(
                m_device,
                m_queueTimelineSemaphore,
                nullptr);
        }
        if (m_uploadTimelineSemaphore != VK_NULL_HANDLE)
        {
            vkDestroySemaphore(
                m_device,
                m_uploadTimelineSemaphore,
                nullptr);
        }
        for (VkSemaphore semaphore :
             m_batchTimelineSemaphores)
        {
            if (semaphore != VK_NULL_HANDLE)
            {
                vkDestroySemaphore(
                    m_device,
                    semaphore,
                    nullptr);
            }
        }
        vkDestroyDevice(m_device, nullptr);
        m_device = VK_NULL_HANDLE;
    }
    if (m_surface != VK_NULL_HANDLE && m_instance != VK_NULL_HANDLE)
    {
        vkDestroySurfaceKHR(m_instance, m_surface, nullptr);
        m_surface = VK_NULL_HANDLE;
    }
    if (m_instance != VK_NULL_HANDLE)
    {
        if (m_debugMessenger != VK_NULL_HANDLE)
        {
            const auto destroy = reinterpret_cast<PFN_vkDestroyDebugReportCallbackEXT>(
                vkGetInstanceProcAddr(m_instance, "vkDestroyDebugReportCallbackEXT"));
            if (destroy) destroy(m_instance, m_debugMessenger, nullptr);
            m_debugMessenger = VK_NULL_HANDLE;
        }
        vkDestroyInstance(m_instance, nullptr);
        m_instance = VK_NULL_HANDLE;
    }
}

void VulkanContext::Initialize(Platform::Window& window)
{
    m_window = &window;
    Core::Check(glfwVulkanSupported() == GLFW_TRUE, "GLFW could not find a Vulkan runtime or compatible driver.");
    const int loaderVersion = LoadGlobalFunctions();
    Core::Check(loaderVersion >= GLAD_MAKE_VERSION(1, 3), "PrismRender requires a Vulkan 1.3 runtime.");

    CreateInstance();
    Core::Check(LoadInstanceFunctions(m_instance, VK_NULL_HANDLE) != 0, "Failed to load Vulkan instance functions.");
    CreateSurface();
    PickPhysicalDevice();
    Core::Check(LoadInstanceFunctions(m_instance, m_physicalDevice) != 0, "Failed to load Vulkan device functions.");
    CreateLogicalDevice();
    m_descriptorAllocator = std::make_unique<VulkanDescriptorAllocator>(*this);
    CreateCommandPool();
    ExecuteComputeImmediate(
        [](VkCommandBuffer) {});
    CreateSwapChainResources();
    CreateFrameResources();
}

void VulkanContext::Resize(const std::uint32_t width, const std::uint32_t height)
{
    if (width == 0 || height == 0 || m_device == VK_NULL_HANDLE)
    {
        return;
    }

    WaitForGpu();
    DestroyCaptureResources();
    DestroySwapChainResources();
    CreateSwapChainResources();
}

FrameResult VulkanContext::BeginFrame()
{
    Core::Check(!m_frameInProgress, "Vulkan frame is already in progress.");
    FrameContext& frame = m_frames[m_currentFrame];
    auto* pacing = FramePacing();
    if (pacing)
    {
        pacing->Reset(m_currentFrame);
        pacing->imageCount = static_cast<std::uint32_t>(m_swapChainImages.size());
        pacing->presentMode = static_cast<std::int32_t>(m_swapChainPresentMode);
        pacing->frameFenceWaitCalled = true;
    }
    FramePacingScope fenceScope(pacing, FramePacingStatistics::Phase::FrameFence);
    const auto frameWaitResult = vkWaitForFences(m_device, 1, &frame.inFlight, VK_TRUE, UINT64_MAX);
    fenceScope.End();
    if (pacing) pacing->waitResult = frameWaitResult;
    CheckVk(frameWaitResult, "Failed to wait for Vulkan frame fence.");
    FramePacingScope reclaimScope(pacing, FramePacingStatistics::Phase::Reclaim);
    if (m_descriptorAllocator != nullptr)
    {
        m_descriptorAllocator->ReclaimFrame(
            m_currentFrame);
    }
    {
        std::scoped_lock lock(
            m_resourceRetirementMutex);
        const std::uint32_t reclaimed =
            static_cast<std::uint32_t>(
                frame.retiredGpuObjects.size());
        for (auto& destroy :
             frame.retiredGpuObjects)
        {
            destroy(m_device);
        }
        frame.retiredGpuObjects.clear();
        m_resourceRetirementStatistics
            .totalReclaimedObjectCount += reclaimed;
        m_resourceRetirementStatistics
            .pendingObjectCount -= reclaimed;
    }
    for (const RetiredCommandBuffer& retired :
         frame.retiredCommandBuffers)
    {
        if (retired.commandBuffer != VK_NULL_HANDLE)
        {
            vkFreeCommandBuffers(
                m_device,
                retired.pool,
                1,
                &retired.commandBuffer);
        }
    }
    frame.retiredCommandBuffers.clear();
    for (const RetiredUpload& upload :
         frame.retiredUploads)
    {
        if (upload.buffer != VK_NULL_HANDLE)
        {
            vkDestroyBuffer(
                m_device,
                upload.buffer,
                nullptr);
        }
        if (upload.memory != VK_NULL_HANDLE)
        {
            vkFreeMemory(
                m_device,
                upload.memory,
                nullptr);
        }
    }
    frame.retiredUploads.clear();
    for (const VkCommandPool pool :
         frame.retiredCommandPools)
    {
        if (pool != VK_NULL_HANDLE)
        {
            vkDestroyCommandPool(
                m_device,
                pool,
                nullptr);
        }
    }
    frame.retiredCommandPools.clear();
    frame.pendingTimelineWaitValue = 0;
    frame.pendingUploadWaitValue = 0;
    frame.pendingBatchWaitValues = {};
    frame.imageAvailableConsumed = false;
    m_activeQueueWaitValue = 0;
    m_activeCommandQueue =
        CommandQueueType::Graphics;
    m_activeBatchWaits.clear();
    m_activeBatchCommandBuffers.clear();
    m_pendingQueueBatches.clear();
    m_queueBatchExecutionActive = false;
    m_queueBatchOpen = false;

    reclaimScope.End();
    FramePacingScope acquireScope(pacing, FramePacingStatistics::Phase::Acquire);
    const VkResult acquireResult = vkAcquireNextImageKHR(
        m_device,
        m_swapChain,
        UINT64_MAX,
        frame.imageAvailable,
        VK_NULL_HANDLE,
        &m_currentImage);
    acquireScope.End();
    if (pacing) { pacing->acquireResult = acquireResult; pacing->image = m_currentImage; }
    if (acquireResult == VK_ERROR_OUT_OF_DATE_KHR)
    {
        return FrameResult::SwapChainOutOfDate;
    }
    if (acquireResult != VK_SUCCESS && acquireResult != VK_SUBOPTIMAL_KHR)
    {
        CheckVk(acquireResult, "Failed to acquire a Vulkan swap-chain image.");
    }
    m_swapChainSuboptimal = acquireResult == VK_SUBOPTIMAL_KHR;

    const bool firstAcquire = m_imageInFlightFences[m_currentImage] == VK_NULL_HANDLE;
    FramePacingScope imageScope(pacing, FramePacingStatistics::Phase::ImageFence);
    if (!firstAcquire)
    {
        if (pacing) pacing->imageFenceWaitCalled = true;
        CheckVk(
            vkWaitForFences(m_device, 1, &m_imageInFlightFences[m_currentImage], VK_TRUE, UINT64_MAX),
            "Failed to wait for a Vulkan swap-chain image fence.");
    }
    m_imageInFlightFences[m_currentImage] = frame.inFlight;
    imageScope.End();

    FramePacingScope uploadScope(pacing, FramePacingStatistics::Phase::Upload);
    FlushPendingUploads(false);
    uploadScope.End();
    FramePacingScope prepareScope(pacing, FramePacingStatistics::Phase::Prepare);
    CheckVk(vkResetFences(m_device, 1, &frame.inFlight), "Failed to reset Vulkan frame fence.");
    CheckVk(vkResetCommandBuffer(frame.commandBuffer, 0), "Failed to reset Vulkan command buffer.");

    VkCommandBufferBeginInfo beginInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    CheckVk(vkBeginCommandBuffer(frame.commandBuffer, &beginInfo), "Failed to begin Vulkan command buffer.");

    // RHI callers import acquired images as Present. Newly created swapchain
    // images start Undefined, including each image after a resize.
    if (firstAcquire)
    {
        VkImageMemoryBarrier ready{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        ready.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        ready.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        ready.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        ready.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        ready.image = m_swapChainImages[m_currentImage];
        ready.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0u, 1u, 0u, 1u};
        vkCmdPipelineBarrier(frame.commandBuffer, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
            VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0u, 0u, nullptr,
            0u, nullptr, 1u, &ready);
    }

    m_activePipelineLayout = VK_NULL_HANDLE;
    m_activePipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    m_frameInProgress = true;
    if (pacing) pacing->beginCompleted = true;
    return FrameResult::Ready;
}

FrameResult VulkanContext::EndFrame()
{
    Core::Check(VulkanValidationErrors.load() == 0u,
        "Vulkan validation reported errors; see stderr diagnostics.");
    Core::Check(m_frameInProgress, "Vulkan frame was not begun.");
    Core::Check(!m_renderingInProgress, "EndRendering must be called before ending a Vulkan frame.");
    Core::Check(
        !m_queueBatchExecutionActive
            && !m_queueBatchOpen,
        "Vulkan queue-batch execution must resume graphics before EndFrame.");
    FrameContext& frame = m_frames[m_currentFrame];

    const bool canCapture = m_captureSourceImage != VK_NULL_HANDLE || m_swapChainSupportsTransferSource;
    const bool captureFrame = m_captureRequested && canCapture;
    if (captureFrame)
    {
        if (m_captureSourceImage == VK_NULL_HANDLE)
        {
            m_captureExtent = m_swapChainExtent;
            m_captureFormat = m_swapChainFormat;
        }
        EnsureCaptureBuffer();
    }
    RecordPresentTransition(frame.commandBuffer, captureFrame);
    CheckVk(vkEndCommandBuffer(frame.commandBuffer), "Failed to end Vulkan command buffer.");

    std::array<VkSemaphore, 5> waitSemaphores{};
    std::array<VkPipelineStageFlags, 5> waitStages{};
    std::array<std::uint64_t, 5> waitValues{};
    std::uint32_t waitCount = 0;
    if (!frame.imageAvailableConsumed)
    {
        waitSemaphores[waitCount] =
            frame.imageAvailable;
        waitStages[waitCount] =
            VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        waitValues[waitCount] = 0;
        ++waitCount;
    }
    if (frame.pendingUploadWaitValue > 0)
    {
        waitSemaphores[waitCount] =
            m_uploadTimelineSemaphore;
        waitStages[waitCount] =
            VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
        waitValues[waitCount] =
            frame.pendingUploadWaitValue;
        ++waitCount;
    }
    if (frame.pendingTimelineWaitValue > 0)
    {
        waitSemaphores[waitCount] =
            m_queueTimelineSemaphore;
        waitStages[waitCount] =
            VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
        waitValues[waitCount] =
            frame.pendingTimelineWaitValue;
        ++waitCount;
    }
    for (std::size_t queueIndex = 0;
         queueIndex < frame.pendingBatchWaitValues.size();
         ++queueIndex)
    {
        if (frame.pendingBatchWaitValues[queueIndex] == 0)
        {
            continue;
        }
        waitSemaphores[waitCount] =
            m_batchTimelineSemaphores[queueIndex];
        waitStages[waitCount] =
            VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
        waitValues[waitCount] =
            frame.pendingBatchWaitValues[queueIndex];
        ++waitCount;
    }
    const std::uint64_t renderFinishedValue = 0;
    VkTimelineSemaphoreSubmitInfo timelineInfo{
        VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO};
    timelineInfo.waitSemaphoreValueCount = waitCount;
    timelineInfo.pWaitSemaphoreValues =
        waitValues.data();
    timelineInfo.signalSemaphoreValueCount = 1;
    timelineInfo.pSignalSemaphoreValues =
        &renderFinishedValue;
    VkSubmitInfo submitInfo{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submitInfo.pNext = &timelineInfo;
    submitInfo.waitSemaphoreCount = waitCount;
    submitInfo.pWaitSemaphores =
        waitSemaphores.data();
    submitInfo.pWaitDstStageMask =
        waitStages.data();
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers = &frame.commandBuffer;
    submitInfo.signalSemaphoreCount = 1;
    submitInfo.pSignalSemaphores = &m_presentSemaphores[m_currentImage];
    FramePacingScope submitScope(FramePacing(), FramePacingStatistics::Phase::Submit);
    CheckVk(vkQueueSubmit(m_graphicsQueue, 1, &submitInfo, frame.inFlight), "Failed to submit Vulkan command buffer.");
    ++m_totalSubmittedFrames;
    submitScope.End();

    VkPresentInfoKHR presentInfo{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
    presentInfo.waitSemaphoreCount = 1;
    presentInfo.pWaitSemaphores = &m_presentSemaphores[m_currentImage];
    presentInfo.swapchainCount = 1;
    presentInfo.pSwapchains = &m_swapChain;
    presentInfo.pImageIndices = &m_currentImage;
    FramePacingScope presentScope(FramePacing(), FramePacingStatistics::Phase::Present);
    const VkResult presentResult = vkQueuePresentKHR(m_presentQueue, &presentInfo);
    presentScope.End();
    if (auto* pacing = FramePacing()) { pacing->presentResult = presentResult; pacing->endCompleted = true; }

    if (m_captureRequested && !canCapture)
    {
        m_captureError = "The selected Vulkan surface does not support transfer-source swap-chain images.";
        m_captureRequested = false;
        m_captureComplete = true;
    }
    else if (captureFrame)
    {
        CheckVk(vkWaitForFences(m_device, 1, &frame.inFlight, VK_TRUE, UINT64_MAX), "Failed to wait for Vulkan capture.");
        SaveCapture();
    }

    m_frameInProgress = false;
    frame.pendingTimelineWaitValue = 0;
    frame.pendingUploadWaitValue = 0;
    frame.pendingBatchWaitValues = {};
    m_currentFrame = (m_currentFrame + 1u) % FrameCount;

    if (presentResult == VK_ERROR_OUT_OF_DATE_KHR || presentResult == VK_SUBOPTIMAL_KHR || m_swapChainSuboptimal)
    {
        return FrameResult::SwapChainOutOfDate;
    }
    CheckVk(presentResult, "Failed to present Vulkan swap-chain image.");
    return FrameResult::Ready;
}

void VulkanContext::WaitForGpu()
{
    FlushPendingUploads(true);
    if (m_device != VK_NULL_HANDLE)
    {
        CheckVk(vkDeviceWaitIdle(m_device), "Failed to wait for the Vulkan device.");
    }
}

FrameAdmissionResult VulkanContext::WaitForFrameAdmission()
{
    FrameAdmissionResult result{};
    result.configurationGeneration =
        m_framePacingState.effectiveGeneration;
    if (m_device == VK_NULL_HANDLE)
    {
        return result;
    }
    if (m_framePacingState.effectiveMaxQueuedFrames == 1
        && m_totalSubmittedFrames > 0)
    {
        const std::uint32_t previousFrame =
            (m_currentFrame + FrameCount - 1u) % FrameCount;
        const auto start = std::chrono::steady_clock::now();
        CheckVk(vkWaitForFences(m_device, 1,
            &m_frames[previousFrame].inFlight,
            VK_TRUE, UINT64_MAX),
            "Failed to enforce Vulkan low-latency queue depth.");
        result.queueWaitMilliseconds =
            std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - start).count();
    }
    result.submittedFrames = m_totalSubmittedFrames;
    for (const FrameContext& frame : m_frames)
    {
        if (vkGetFenceStatus(m_device, frame.inFlight) == VK_NOT_READY)
        {
            ++result.outstandingFrames;
        }
    }
    return result;
}

bool VulkanContext::ApplyFramePacingConfiguration(
    const FramePacingConfiguration& configuration,
    const std::uint64_t generation,
    std::string* outErrorMessage)
{
    try
    {
        ValidateFramePacingConfiguration(configuration);
        if (generation <= m_framePacingState.requestedGeneration)
        {
            throw std::invalid_argument(
                "Frame-pacing generation must advance monotonically.");
        }
        const SwapChainSupport support =
            QuerySwapChainSupport(m_physicalDevice);
        const VkPresentModeKHR requestedMode =
            Prism::RHI::Vulkan::ChooseVulkanPresentMode(
                support.presentModes, configuration.presentation);
        if (requestedMode != m_swapChainPresentMode)
        {
            const FramePacingConfiguration previousConfiguration =
                m_framePacingConfiguration;
            const FramePacingState previousState = m_framePacingState;
            m_framePacingState.requested = configuration;
            m_framePacingState.requestedGeneration = generation;
            m_framePacingState.transitionPending = true;
            WaitForGpu();
            DestroyCaptureResources();
            DestroySwapChainResources();
            m_framePacingConfiguration = configuration;
            try
            {
                CreateSwapChainResources();
            }
            catch (...)
            {
                DestroySwapChainResources();
                m_framePacingConfiguration = previousConfiguration;
                CreateSwapChainResources();
                m_framePacingState = previousState;
                throw;
            }
        }
        else
        {
            m_framePacingConfiguration = configuration;
        }
        m_framePacingState.requested = configuration;
        m_framePacingState.effectiveMaxQueuedFrames =
            (std::min)(
                configuration.presentation
                        == PresentationIntent::LowLatencySynchronized
                    ? 1u
                    : configuration.maxQueuedFrames,
                FrameCount);
        m_framePacingState.fallbackReason.clear();
        if (configuration.presentation == PresentationIntent::Immediate
            && m_swapChainPresentMode != VK_PRESENT_MODE_IMMEDIATE_KHR)
        {
            m_framePacingState.fallbackReason =
                "Vulkan IMMEDIATE is unavailable; using a synchronized fallback.";
        }
        else if (configuration.presentation
                    == PresentationIntent::LowLatencySynchronized
            && m_swapChainPresentMode != VK_PRESENT_MODE_MAILBOX_KHR)
        {
            m_framePacingState.fallbackReason =
                "Vulkan MAILBOX is unavailable; using FIFO.";
        }
        if (configuration.maxQueuedFrames > FrameCount)
        {
            if (!m_framePacingState.fallbackReason.empty())
            {
                m_framePacingState.fallbackReason += ' ';
            }
            m_framePacingState.fallbackReason +=
                "Maximum queued frames is limited by Vulkan frame-resource slots.";
        }
        m_framePacingState.requestedGeneration = generation;
        m_framePacingState.effectiveGeneration = generation;
        m_framePacingState.transitionPending = false;
        return true;
    }
    catch (const std::exception& error)
    {
        if (outErrorMessage != nullptr) *outErrorMessage = error.what();
        return false;
    }
}

const FramePacingState& VulkanContext::GetFramePacingState() const noexcept
{
    return m_framePacingState;
}

std::shared_ptr<IBuffer> VulkanContext::CreateBuffer(
    const BufferDescription& description,
    const void* initialData)
{
    return std::make_shared<VulkanBuffer>(*this, description, initialData);
}

std::shared_ptr<ITexture> VulkanContext::CreateTexture(
    const TextureDescription& description,
    const TextureInitialData* initialData)
{
    return std::make_shared<VulkanTexture>(*this, description, initialData);
}

std::shared_ptr<ITransientTexturePool>
VulkanContext::CreateTransientTexturePool(
    const std::vector<TransientTextureRequest>& requests)
{
    return std::make_shared<VulkanTransientTexturePool>(
        *this,
        requests);
}

std::shared_ptr<ITransientBufferPool>
VulkanContext::CreateTransientBufferPool(
    const std::vector<TransientBufferRequest>& requests)
{
    return std::make_shared<VulkanTransientBufferPool>(
        *this,
        requests);
}

std::shared_ptr<ITextureView> VulkanContext::CreateTextureView(
    std::shared_ptr<ITexture> texture,
    const TextureViewDescription& description)
{
    auto nativeTexture = std::dynamic_pointer_cast<VulkanTexture>(texture);
    Core::Check(nativeTexture != nullptr, "Vulkan texture views require Vulkan textures.");
    return std::make_shared<VulkanTextureView>(*this, std::move(nativeTexture), description);
}

std::shared_ptr<ISampler> VulkanContext::CreateSampler(const SamplerDescription& description)
{
    return std::make_shared<VulkanSampler>(*this, description);
}

std::shared_ptr<IDescriptorSetLayout> VulkanContext::CreateDescriptorSetLayout(
    const DescriptorSetLayoutDescription& description)
{
    return std::make_shared<VulkanDescriptorSetLayout>(*this, description);
}

std::shared_ptr<IDescriptorSet> VulkanContext::CreateDescriptorSet(
    std::shared_ptr<IDescriptorSetLayout> layout)
{
    auto nativeLayout = std::dynamic_pointer_cast<VulkanDescriptorSetLayout>(layout);
    Core::Check(nativeLayout != nullptr, "VulkanContext requires a Vulkan descriptor-set layout.");
    Core::Check(m_descriptorAllocator != nullptr, "Vulkan descriptor allocator is not initialized.");
    return m_descriptorAllocator->Allocate(std::move(nativeLayout));
}

std::shared_ptr<IGraphicsPipeline> VulkanContext::CreateGraphicsPipeline(
    const GraphicsPipelineDescription& description)
{
    auto pipeline = std::make_shared<VulkanGraphicsPipeline>(*this, description);
    m_pipelineCreationCounters.RecordGraphics();
    return pipeline;
}

std::shared_ptr<IComputePipeline> VulkanContext::CreateComputePipeline(
    const ComputePipelineDescription& description)
{
    auto pipeline = std::make_shared<VulkanComputePipeline>(*this, description);
    m_pipelineCreationCounters.RecordCompute();
    return pipeline;
}

AccelerationStructureBuildSizes
VulkanContext::QueryAccelerationStructureBuildSizes(
    const AccelerationStructureBuildDescription&
        description) const
{
    std::string validationError;
    Core::Check(
        ValidateAccelerationStructureBuildDescription(
            description,
            &validationError),
        validationError.c_str());
    Core::Check(
        m_deviceCapabilities.features
            .rayTracingAccelerationStructure,
        "The selected Vulkan adapter does not support acceleration structures.");
    Core::Check(
        vkGetAccelerationStructureBuildSizesKHR
            != nullptr,
        "The Vulkan acceleration-structure build-size entry point is unavailable.");

    std::vector<VkAccelerationStructureGeometryKHR>
        nativeGeometries;
    std::vector<std::uint32_t> primitiveCounts;
    VkAccelerationStructureBuildGeometryInfoKHR
        buildInfo{
            VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR};
    buildInfo.type =
        description.type
                == AccelerationStructureType::BottomLevel
            ? VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR
            : VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
    buildInfo.flags =
        ToNativeBuildFlags(description.flags);
    buildInfo.mode =
        VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;

    if (description.type
        == AccelerationStructureType::BottomLevel)
    {
        nativeGeometries.reserve(
            description.geometries.size());
        primitiveCounts.reserve(
            description.geometries.size());
        for (const RayTracingTrianglesDescription& geometry :
             description.geometries)
        {
            VkAccelerationStructureGeometryKHR native{
                VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR};
            native.geometryType =
                VK_GEOMETRY_TYPE_TRIANGLES_KHR;
            if (HasAnyFlag(
                    geometry.flags,
                    RayTracingGeometryFlags::Opaque))
            {
                native.flags |=
                    VK_GEOMETRY_OPAQUE_BIT_KHR;
            }
            if (HasAnyFlag(
                    geometry.flags,
                    RayTracingGeometryFlags::
                        NoDuplicateAnyHitInvocation))
            {
                native.flags |=
                    VK_GEOMETRY_NO_DUPLICATE_ANY_HIT_INVOCATION_BIT_KHR;
            }
            native.geometry.triangles.sType =
                VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR;
            native.geometry.triangles.vertexFormat =
                VK_FORMAT_R32G32B32_SFLOAT;
            native.geometry.triangles.vertexStride =
                geometry.vertexStride;
            native.geometry.triangles.maxVertex =
                geometry.vertexCount - 1;
            native.geometry.triangles.indexType =
                geometry.indexCount == 0
                ? VK_INDEX_TYPE_NONE_KHR
                : geometry.indexFormat
                          == IndexFormat::UInt16
                    ? VK_INDEX_TYPE_UINT16
                    : VK_INDEX_TYPE_UINT32;
            nativeGeometries.push_back(native);
            primitiveCounts.push_back(
                (geometry.indexCount == 0
                     ? geometry.vertexCount
                     : geometry.indexCount)
                / 3);
        }
    }
    else
    {
        VkAccelerationStructureGeometryKHR native{
            VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR};
        native.geometryType =
            VK_GEOMETRY_TYPE_INSTANCES_KHR;
        native.geometry.instances.sType =
            VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR;
        nativeGeometries.push_back(native);
        primitiveCounts.push_back(
            description.instanceCount);
    }

    buildInfo.geometryCount =
        static_cast<std::uint32_t>(
            nativeGeometries.size());
    buildInfo.pGeometries =
        nativeGeometries.data();
    VkAccelerationStructureBuildSizesInfoKHR
        nativeSizes{
            VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR};
    vkGetAccelerationStructureBuildSizesKHR(
        m_device,
        VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR,
        &buildInfo,
        primitiveCounts.data(),
        &nativeSizes);
    Core::Check(
        nativeSizes.accelerationStructureSize > 0
            && nativeSizes.buildScratchSize > 0,
        "Vulkan returned invalid acceleration-structure build sizes.");
    return {
        nativeSizes.accelerationStructureSize,
        nativeSizes.buildScratchSize,
        nativeSizes.updateScratchSize};
}

std::shared_ptr<IRayTracingAccelerationStructure>
VulkanContext::CreateAccelerationStructure(
    const AccelerationStructureBuildRequest&
        request)
{
    std::string validationError;
    Core::Check(
        ValidateAccelerationStructureBuildRequest(
            request,
            GraphicsApi::Vulkan,
            &validationError),
        validationError.c_str());
    Core::Check(
        vkCreateAccelerationStructureKHR != nullptr
            && vkCmdBuildAccelerationStructuresKHR
                != nullptr
            && vkGetAccelerationStructureDeviceAddressKHR
                != nullptr,
        "The Vulkan acceleration-structure entry points are unavailable.");
    const AccelerationStructureBuildSizes sizes =
        QueryAccelerationStructureBuildSizes(
            request.description);

    BufferDescription storageDescription{};
    storageDescription.size =
        static_cast<std::size_t>(
            sizes.accelerationStructureBytes);
    storageDescription.usage =
        BufferUsage::
            AccelerationStructureStorage;
    storageDescription.memoryAccess =
        MemoryAccess::GpuOnly;
    std::shared_ptr<IBuffer> storageBuffer =
        CreateBuffer(storageDescription);
    const auto* nativeStorageBuffer =
        dynamic_cast<const VulkanBuffer*>(
            storageBuffer.get());
    Core::Check(
        nativeStorageBuffer != nullptr,
        "Vulkan acceleration structures require a Vulkan storage buffer.");

    VkAccelerationStructureCreateInfoKHR createInfo{
        VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR};
    createInfo.buffer =
        nativeStorageBuffer->GetHandle();
    createInfo.size =
        sizes.accelerationStructureBytes;
    createInfo.type =
        request.description.type
                == AccelerationStructureType::BottomLevel
            ? VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR
            : VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
    VkAccelerationStructureKHR handle =
        VK_NULL_HANDLE;
    CheckVk(
        vkCreateAccelerationStructureKHR(
            m_device,
            &createInfo,
            nullptr,
            &handle),
        "Failed to create a Vulkan acceleration structure.");
    VkAccelerationStructureDeviceAddressInfoKHR
        addressInfo{
            VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR};
    addressInfo.accelerationStructure = handle;
    const VkDeviceAddress accelerationStructureAddress =
        vkGetAccelerationStructureDeviceAddressKHR(
            m_device,
            &addressInfo);
    auto result =
        std::make_shared<
            VulkanAccelerationStructure>(
            *this,
            request.description,
            sizes,
            std::move(storageBuffer),
            handle,
            accelerationStructureAddress);

    BufferDescription scratchDescription{};
    scratchDescription.size =
        static_cast<std::size_t>(
            sizes.buildScratchBytes);
    scratchDescription.usage =
        BufferUsage::
            AccelerationStructureScratch;
    scratchDescription.memoryAccess =
        MemoryAccess::GpuOnly;
    std::shared_ptr<IBuffer> scratchBuffer =
        CreateBuffer(scratchDescription);
    const auto* nativeScratchBuffer =
        dynamic_cast<const VulkanBuffer*>(
            scratchBuffer.get());
    const VkDeviceAddress scratchAddress =
        GetBufferDeviceAddress(
            m_device,
            *nativeScratchBuffer);

    std::vector<VkAccelerationStructureGeometryKHR>
        nativeGeometries;
    std::vector<
        VkAccelerationStructureBuildRangeInfoKHR>
        ranges;
    std::vector<VkAccelerationStructureInstanceKHR>
        nativeInstances;
    std::shared_ptr<IBuffer> instanceBuffer;
    VkAccelerationStructureBuildGeometryInfoKHR
        buildInfo{
            VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR};
    buildInfo.type = createInfo.type;
    buildInfo.flags =
        ToNativeBuildFlags(
            request.description.flags);
    buildInfo.mode =
        VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
    buildInfo.dstAccelerationStructure =
        result->GetHandle();
    buildInfo.scratchData.deviceAddress =
        scratchAddress;

    if (request.description.type
        == AccelerationStructureType::BottomLevel)
    {
        nativeGeometries.reserve(
            request.geometries.size());
        ranges.reserve(request.geometries.size());
        for (const RayTracingGeometryBuildInput& input :
             request.geometries)
        {
            const auto* vertexBuffer =
                dynamic_cast<const VulkanBuffer*>(
                    input.vertexBuffer.get());
            const auto* indexBuffer =
                dynamic_cast<const VulkanBuffer*>(
                    input.indexBuffer.get());
            Core::Check(
                vertexBuffer != nullptr,
                "Vulkan BLAS builds require Vulkan vertex buffers.");
            Core::Check(
                input.vertexBufferOffset
                        + static_cast<std::uint64_t>(
                              input.description
                                  .vertexCount)
                              * input.description
                                    .vertexStride
                    <= input.vertexBuffer
                           ->GetDescription()
                           .size,
                "Vulkan BLAS vertex input exceeds its buffer.");
            VkAccelerationStructureGeometryKHR native{
                VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR};
            native.geometryType =
                VK_GEOMETRY_TYPE_TRIANGLES_KHR;
            native.geometry.triangles.sType =
                VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR;
            native.geometry.triangles.vertexFormat =
                VK_FORMAT_R32G32B32_SFLOAT;
            native.geometry.triangles.vertexData
                .deviceAddress =
                GetBufferDeviceAddress(
                    m_device,
                    *vertexBuffer)
                + input.vertexBufferOffset;
            native.geometry.triangles.vertexStride =
                input.description.vertexStride;
            native.geometry.triangles.maxVertex =
                input.description.vertexCount - 1;
            if (HasAnyFlag(
                    input.description.flags,
                    RayTracingGeometryFlags::Opaque))
            {
                native.flags |=
                    VK_GEOMETRY_OPAQUE_BIT_KHR;
            }
            if (HasAnyFlag(
                    input.description.flags,
                    RayTracingGeometryFlags::
                        NoDuplicateAnyHitInvocation))
            {
                native.flags |=
                    VK_GEOMETRY_NO_DUPLICATE_ANY_HIT_INVOCATION_BIT_KHR;
            }
            if (input.description.indexCount > 0)
            {
                Core::Check(
                    indexBuffer != nullptr,
                    "Indexed Vulkan BLAS geometry requires a Vulkan index buffer.");
                const std::uint64_t indexStride =
                    input.description.indexFormat
                            == IndexFormat::UInt16
                        ? 2
                        : 4;
                Core::Check(
                    input.indexBufferOffset
                            + indexStride
                                  * input.description
                                        .indexCount
                        <= input.indexBuffer
                               ->GetDescription()
                               .size,
                    "Vulkan BLAS index input exceeds its buffer.");
                native.geometry.triangles.indexType =
                    input.description.indexFormat
                            == IndexFormat::UInt16
                        ? VK_INDEX_TYPE_UINT16
                        : VK_INDEX_TYPE_UINT32;
                native.geometry.triangles.indexData
                    .deviceAddress =
                    GetBufferDeviceAddress(
                        m_device,
                        *indexBuffer)
                    + input.indexBufferOffset;
            }
            else
            {
                native.geometry.triangles.indexType =
                    VK_INDEX_TYPE_NONE_KHR;
            }
            nativeGeometries.push_back(native);
            VkAccelerationStructureBuildRangeInfoKHR
                range{};
            range.primitiveCount =
                (input.description.indexCount == 0
                     ? input.description.vertexCount
                     : input.description.indexCount)
                / 3;
            ranges.push_back(range);
        }
    }
    else
    {
        nativeInstances.reserve(
            request.instances.size());
        for (const RayTracingInstanceDescription& instance :
             request.instances)
        {
            VkAccelerationStructureInstanceKHR native{};
            std::memcpy(
                &native.transform,
                instance.transform.data(),
                sizeof(native.transform));
            native.instanceCustomIndex =
                instance.instanceId;
            native.mask = instance.mask;
            native.instanceShaderBindingTableRecordOffset =
                instance.hitGroupOffset;
            native.flags =
                ToNativeInstanceFlags(instance.flags);
            native.accelerationStructureReference =
                instance.bottomLevel
                    ->GetDeviceAddress();
            nativeInstances.push_back(native);
        }
        BufferDescription instanceDescription{};
        instanceDescription.size =
            nativeInstances.size()
            * sizeof(
                VkAccelerationStructureInstanceKHR);
        instanceDescription.stride =
            sizeof(
                VkAccelerationStructureInstanceKHR);
        instanceDescription.usage =
            BufferUsage::
                AccelerationStructureBuildInput;
        instanceDescription.memoryAccess =
            MemoryAccess::CpuToGpu;
        instanceBuffer = CreateBuffer(
            instanceDescription,
            nativeInstances.data());
        const auto* nativeInstanceBuffer =
            dynamic_cast<const VulkanBuffer*>(
                instanceBuffer.get());
        VkAccelerationStructureGeometryKHR native{
            VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR};
        native.geometryType =
            VK_GEOMETRY_TYPE_INSTANCES_KHR;
        native.geometry.instances.sType =
            VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR;
        native.geometry.instances.data.deviceAddress =
            GetBufferDeviceAddress(
                m_device,
                *nativeInstanceBuffer);
        nativeGeometries.push_back(native);
        VkAccelerationStructureBuildRangeInfoKHR range{};
        range.primitiveCount =
            static_cast<std::uint32_t>(
                nativeInstances.size());
        ranges.push_back(range);
    }

    buildInfo.geometryCount =
        static_cast<std::uint32_t>(
            nativeGeometries.size());
    buildInfo.pGeometries =
        nativeGeometries.data();
    ExecuteImmediate(
        [&](const VkCommandBuffer commandBuffer)
        {
            const VkAccelerationStructureBuildRangeInfoKHR*
                rangePointer = ranges.data();
            vkCmdBuildAccelerationStructuresKHR(
                commandBuffer,
                1,
                &buildInfo,
                &rangePointer);
            VkMemoryBarrier barrier{
                VK_STRUCTURE_TYPE_MEMORY_BARRIER};
            barrier.srcAccessMask =
                VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR;
            barrier.dstAccessMask =
                VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR;
            vkCmdPipelineBarrier(
                commandBuffer,
                VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
                VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR
                    | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                0,
                1,
                &barrier,
                0,
                nullptr,
                0,
                nullptr);
        });
    return result;
}

void VulkanContext::BeginRendering(const RenderingInfo& renderingInfo)
{
    Core::Check(m_frameInProgress, "Vulkan rendering requires an active frame.");
    Core::Check(!m_renderingInProgress, "Nested Vulkan rendering scopes are not supported.");
    std::string validationError;
    Core::Check(ValidateRenderingInfo(renderingInfo, &validationError), validationError.c_str());

    std::vector<VkRenderingAttachmentInfo> colorAttachments;
    colorAttachments.reserve(renderingInfo.colorAttachments.size());
    m_pendingAttachmentTransitions.clear();
    for (const RenderingAttachment& attachment : renderingInfo.colorAttachments)
    {
        const auto* view = dynamic_cast<const VulkanTextureView*>(attachment.view);
        Core::Check(view != nullptr, "Vulkan rendering requires Vulkan texture views.");
        TransitionTextureView(*view, attachment.stateBefore, ResourceState::RenderTarget);

        VkRenderingAttachmentInfo native{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        native.imageView = view->GetHandle();
        native.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        native.loadOp = ToNativeLoadOperation(attachment.loadOperation);
        native.storeOp = ToNativeStoreOperation(attachment.storeOperation);
        native.clearValue.color = {{attachment.clearColor.red, attachment.clearColor.green,
                                    attachment.clearColor.blue, attachment.clearColor.alpha}};
        colorAttachments.push_back(native);
        m_pendingAttachmentTransitions.push_back({attachment.view, ResourceState::RenderTarget,
                                                   attachment.stateAfter});
    }

    VkRenderingAttachmentInfo depthAttachment{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    const VkRenderingAttachmentInfo* nativeDepthAttachment = nullptr;
    if (renderingInfo.depthAttachment.has_value())
    {
        const RenderingAttachment& attachment = *renderingInfo.depthAttachment;
        const auto* view = dynamic_cast<const VulkanTextureView*>(attachment.view);
        Core::Check(view != nullptr, "Vulkan rendering requires a Vulkan depth texture view.");
        TransitionTextureView(*view, attachment.stateBefore, ResourceState::DepthWrite);
        depthAttachment.imageView = view->GetHandle();
        depthAttachment.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        depthAttachment.loadOp = ToNativeLoadOperation(attachment.loadOperation);
        depthAttachment.storeOp = ToNativeStoreOperation(attachment.storeOperation);
        depthAttachment.clearValue.depthStencil = {
            attachment.clearDepthStencil.depth,
            attachment.clearDepthStencil.stencil};
        nativeDepthAttachment = &depthAttachment;
        m_pendingAttachmentTransitions.push_back({attachment.view, ResourceState::DepthWrite,
                                                   attachment.stateAfter});
    }

    VkRenderingInfo native{VK_STRUCTURE_TYPE_RENDERING_INFO};
    native.renderArea.extent = {renderingInfo.width, renderingInfo.height};
    native.layerCount = renderingInfo.layerCount;
    native.colorAttachmentCount = static_cast<std::uint32_t>(colorAttachments.size());
    native.pColorAttachments = colorAttachments.data();
    native.pDepthAttachment = nativeDepthAttachment;
    vkCmdBeginRendering(GetCommandBuffer(), &native);

    VkViewport viewport{};
    viewport.y = static_cast<float>(renderingInfo.height);
    viewport.width = static_cast<float>(renderingInfo.width);
    viewport.height = -static_cast<float>(renderingInfo.height);
    viewport.minDepth = 0.0f;
    viewport.maxDepth = 1.0f;
    vkCmdSetViewport(GetCommandBuffer(), 0, 1, &viewport);
    const VkRect2D scissor{{0, 0}, {renderingInfo.width, renderingInfo.height}};
    vkCmdSetScissor(GetCommandBuffer(), 0, 1, &scissor);
    m_renderingInProgress = true;
}

void VulkanContext::EndRendering()
{
    Core::Check(m_renderingInProgress, "No Vulkan rendering scope is active.");
    vkCmdEndRendering(GetCommandBuffer());
    m_renderingInProgress = false;
    for (const PendingAttachmentTransition& transition : m_pendingAttachmentTransitions)
    {
        if (transition.stateAfter != ResourceState::Undefined)
        {
            TransitionTextureView(*transition.view, transition.stateBefore, transition.stateAfter);
        }
    }
    m_pendingAttachmentTransitions.clear();
}

void VulkanContext::BindGraphicsPipeline(const IGraphicsPipeline& pipeline)
{
    const auto* nativePipeline = dynamic_cast<const VulkanGraphicsPipeline*>(&pipeline);
    Core::Check(nativePipeline != nullptr, "VulkanContext requires a Vulkan graphics pipeline.");
    BindGraphicsPipeline(nativePipeline->GetHandle(), nativePipeline->GetLayout());
}

void VulkanContext::BindComputePipeline(const IComputePipeline& pipeline)
{
    const auto* nativePipeline = dynamic_cast<const VulkanComputePipeline*>(&pipeline);
    Core::Check(nativePipeline != nullptr, "VulkanContext requires a Vulkan compute pipeline.");
    vkCmdBindPipeline(GetCommandBuffer(), VK_PIPELINE_BIND_POINT_COMPUTE, nativePipeline->GetHandle());
    m_activePipelineLayout = nativePipeline->GetLayout();
    m_activePipelineBindPoint = VK_PIPELINE_BIND_POINT_COMPUTE;
}

void VulkanContext::BindVertexBuffer(const IBuffer& buffer, const std::uint32_t slot)
{
    const auto* nativeBuffer = dynamic_cast<const VulkanBuffer*>(&buffer);
    Core::Check(nativeBuffer != nullptr, "VulkanContext requires a Vulkan vertex buffer.");
    const VkBuffer handle = nativeBuffer->GetHandle();
    const VkDeviceSize offset = 0;
    vkCmdBindVertexBuffers(GetCommandBuffer(), slot, 1, &handle, &offset);
}

void VulkanContext::BindIndexBuffer(const IBuffer& buffer, const IndexFormat format)
{
    const auto* nativeBuffer = dynamic_cast<const VulkanBuffer*>(&buffer);
    Core::Check(nativeBuffer != nullptr, "VulkanContext requires a Vulkan index buffer.");
    vkCmdBindIndexBuffer(
        GetCommandBuffer(),
        nativeBuffer->GetHandle(),
        0,
        format == IndexFormat::UInt16 ? VK_INDEX_TYPE_UINT16 : VK_INDEX_TYPE_UINT32);
}

void VulkanContext::BindDescriptorSet(
    const IDescriptorSet& descriptorSet,
    const std::span<const DynamicBufferOffset> dynamicOffsets)
{
    const auto* nativeSet = dynamic_cast<const VulkanDescriptorSet*>(&descriptorSet);
    Core::Check(nativeSet != nullptr, "VulkanContext requires a Vulkan descriptor set.");
    Core::Check(m_activePipelineLayout != VK_NULL_HANDLE, "BindGraphicsPipeline must be called before BindDescriptorSet.");
    const VkDescriptorSet handle = nativeSet->GetHandle();
    const std::vector<std::uint32_t> nativeOffsets = nativeSet->BuildDynamicOffsets(dynamicOffsets);
    vkCmdBindDescriptorSets(
        GetCommandBuffer(),
        m_activePipelineBindPoint,
        m_activePipelineLayout,
        0,
        1,
        &handle,
        static_cast<std::uint32_t>(nativeOffsets.size()),
        nativeOffsets.data());
}

void VulkanContext::DrawIndexed(
    const std::uint32_t indexCount,
    const std::uint32_t instanceCount,
    const std::uint32_t firstIndex,
    const std::int32_t vertexOffset,
    const std::uint32_t firstInstance)
{
    vkCmdDrawIndexed(GetCommandBuffer(), indexCount, instanceCount, firstIndex, vertexOffset, firstInstance);
}

void VulkanContext::DrawIndexedIndirect(
    const IBuffer& argumentBuffer,
    const std::size_t argumentOffset,
    const std::uint32_t maxDrawCount,
    const std::uint32_t stride,
    const IBuffer* countBuffer,
    const std::size_t countOffset)
{
    const auto* nativeArguments =
        dynamic_cast<const VulkanBuffer*>(
            &argumentBuffer);
    const auto* nativeCount =
        dynamic_cast<const VulkanBuffer*>(
            countBuffer);
    Core::Check(
        nativeArguments != nullptr,
        "Vulkan indirect draws require a Vulkan argument buffer.");
    Core::Check(
        countBuffer == nullptr
            || nativeCount != nullptr,
        "Vulkan indirect draw counts require a Vulkan count buffer.");
    Core::Check(
        HasAnyFlag(
            nativeArguments->GetDescription().usage,
            BufferUsage::Indirect),
        "Vulkan indirect argument buffers require Indirect usage.");
    Core::Check(
        stride
                >= sizeof(
                    DrawIndexedIndirectArguments)
            && stride % 4u == 0u,
        "Vulkan indexed-indirect strides must be at least 20 bytes and four-byte aligned.");
    Core::Check(
        argumentOffset % 4u == 0u
            && argumentOffset
                    + static_cast<std::size_t>(
                          maxDrawCount)
                          * stride
                <= nativeArguments
                       ->GetDescription()
                       .size,
        "Vulkan indexed-indirect arguments exceed their buffer.");
    Core::Check(
        nativeCount == nullptr
            || (countOffset % 4u == 0u
                && countOffset
                        + sizeof(std::uint32_t)
                    <= nativeCount
                           ->GetDescription()
                           .size),
        "Vulkan indirect count arguments exceed their buffer.");

    if (nativeCount != nullptr)
    {
        Core::Check(
            m_deviceCapabilities.features
                .drawIndirectCount,
            "This Vulkan device does not support indirect draw counts.");
        vkCmdDrawIndexedIndirectCount(
            GetCommandBuffer(),
            nativeArguments->GetHandle(),
            argumentOffset,
            nativeCount->GetHandle(),
            countOffset,
            maxDrawCount,
            stride);
        return;
    }
    vkCmdDrawIndexedIndirect(
        GetCommandBuffer(),
        nativeArguments->GetHandle(),
        argumentOffset,
        maxDrawCount,
        stride);
}

void VulkanContext::Draw(
    const std::uint32_t vertexCount,
    const std::uint32_t instanceCount,
    const std::uint32_t firstVertex,
    const std::uint32_t firstInstance)
{
    vkCmdDraw(GetCommandBuffer(), vertexCount, instanceCount, firstVertex, firstInstance);
}

void VulkanContext::Dispatch(
    const std::uint32_t groupCountX,
    const std::uint32_t groupCountY,
    const std::uint32_t groupCountZ)
{
    vkCmdDispatch(GetCommandBuffer(), groupCountX, groupCountY, groupCountZ);
}

void VulkanContext::CopyBuffer(
    const IBuffer& source,
    IBuffer& destination,
    const std::size_t size,
    const std::size_t sourceOffset,
    const std::size_t destinationOffset)
{
    const auto* sourceBuffer =
        dynamic_cast<const VulkanBuffer*>(&source);
    auto* destinationBuffer =
        dynamic_cast<VulkanBuffer*>(&destination);
    Core::Check(
        sourceBuffer != nullptr && destinationBuffer != nullptr,
        "Vulkan buffer copies require Vulkan buffers.");
    Core::Check(
        sourceOffset + size <= source.GetDescription().size
            && destinationOffset + size
                <= destination.GetDescription().size,
        "Vulkan buffer copy range exceeds its resource.");
    const VkBufferCopy region{
        sourceOffset,
        destinationOffset,
        size};
    vkCmdCopyBuffer(
        GetCommandBuffer(),
        sourceBuffer->GetHandle(),
        destinationBuffer->GetHandle(),
        1,
        &region);
}

void VulkanContext::TextureBarrier(const RHI::TextureBarrier& barrier)
{
    auto* nativeTexture = dynamic_cast<VulkanTexture*>(barrier.texture);
    Core::Check(nativeTexture != nullptr, "Vulkan texture barriers require a Vulkan texture.");
    const bool depthResource = IsDepthFormat(nativeTexture->GetDescription().format);
    const ResourceStateMapping before = ToNativeResourceState(barrier.before, depthResource);
    const ResourceStateMapping after = ToNativeResourceState(barrier.after, depthResource);

    VkImageMemoryBarrier nativeBarrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    nativeBarrier.srcAccessMask = before.accessMask;
    nativeBarrier.dstAccessMask = after.accessMask;
    nativeBarrier.oldLayout = before.imageLayout;
    nativeBarrier.newLayout = after.imageLayout;
    nativeBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    nativeBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    nativeBarrier.image = nativeTexture->GetImage();
    nativeBarrier.subresourceRange.aspectMask = depthResource ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
    const TextureDescription& description =
        nativeTexture->GetDescription();
    Core::Check(
        barrier.baseMipLevel
                < description.mipLevels
            && barrier.baseArrayLayer
                < description.arrayLayers,
        "A Vulkan texture barrier starts outside its subresources.");
    nativeBarrier.subresourceRange.baseMipLevel =
        barrier.baseMipLevel;
    nativeBarrier.subresourceRange.levelCount =
        barrier.mipLevelCount == 0
        ? description.mipLevels
            - barrier.baseMipLevel
        : barrier.mipLevelCount;
    nativeBarrier.subresourceRange.baseArrayLayer =
        barrier.baseArrayLayer;
    nativeBarrier.subresourceRange.layerCount =
        barrier.arrayLayerCount == 0
        ? description.arrayLayers
            - barrier.baseArrayLayer
        : barrier.arrayLayerCount;
    Core::Check(
        nativeBarrier.subresourceRange.baseMipLevel
                + nativeBarrier.subresourceRange.levelCount
            <= description.mipLevels
            && nativeBarrier.subresourceRange
                    .baseArrayLayer
                    + nativeBarrier.subresourceRange
                          .layerCount
                <= description.arrayLayers,
        "A Vulkan texture barrier exceeds its subresources.");
    vkCmdPipelineBarrier(
        GetCommandBuffer(),
        QueueCompatiblePipelineStages(before.pipelineStages, GetActiveCommandQueue()),
        QueueCompatiblePipelineStages(after.pipelineStages, GetActiveCommandQueue()),
        0,
        0,
        nullptr,
        0,
        nullptr,
        1,
        &nativeBarrier);
}

void VulkanContext::BufferBarrier(
    const RHI::BufferBarrier& barrier)
{
    auto* nativeBuffer =
        dynamic_cast<VulkanBuffer*>(barrier.buffer);
    Core::Check(
        nativeBuffer != nullptr,
        "Vulkan buffer barriers require a Vulkan buffer.");
    const std::size_t effectiveSize =
        barrier.size == 0
        ? nativeBuffer->GetDescription().size
              - barrier.offset
        : barrier.size;
    Core::Check(
        barrier.offset + effectiveSize
            <= nativeBuffer->GetDescription().size,
        "A Vulkan buffer barrier exceeds its resource.");
    const ResourceStateMapping before =
        ToNativeResourceState(
            barrier.before,
            false);
    const ResourceStateMapping after =
        ToNativeResourceState(
            barrier.after,
            false);
    VkBufferMemoryBarrier native{
        VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
    native.srcAccessMask = before.accessMask;
    native.dstAccessMask = after.accessMask;
    native.srcQueueFamilyIndex =
        VK_QUEUE_FAMILY_IGNORED;
    native.dstQueueFamilyIndex =
        VK_QUEUE_FAMILY_IGNORED;
    native.buffer = nativeBuffer->GetHandle();
    native.offset = barrier.offset;
    native.size = effectiveSize;
    vkCmdPipelineBarrier(
        GetCommandBuffer(),
        QueueCompatiblePipelineStages(before.pipelineStages, GetActiveCommandQueue()),
        QueueCompatiblePipelineStages(after.pipelineStages, GetActiveCommandQueue()),
        0,
        0,
        nullptr,
        1,
        &native,
        0,
        nullptr);
}

void VulkanContext::GlobalBarrier(
    const RHI::GlobalBarrier& barrier)
{
    const ResourceStateMapping before =
        ToNativeResourceState(
            barrier.before,
            false);
    const ResourceStateMapping after =
        ToNativeResourceState(
            barrier.after,
            false);
    VkMemoryBarrier native{
        VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    native.srcAccessMask = before.accessMask;
    native.dstAccessMask = after.accessMask;
    vkCmdPipelineBarrier(
        GetCommandBuffer(),
        QueueCompatiblePipelineStages(before.pipelineStages, GetActiveCommandQueue()),
        QueueCompatiblePipelineStages(after.pipelineStages, GetActiveCommandQueue()),
        0,
        1,
        &native,
        0,
        nullptr,
        0,
        nullptr);
}

void VulkanContext::TextureViewBarrier(
    const ITextureView& textureView,
    const ResourceState before,
    const ResourceState after)
{
    TransitionTextureView(
        textureView,
        before,
        after);
}

void VulkanContext::TextureAliasingBarrier(
    const ITexture* before,
    ITexture& after)
{
    Core::Check(
        before == nullptr
            || dynamic_cast<const VulkanTexture*>(before)
                != nullptr,
        "Vulkan aliasing barriers require Vulkan textures.");
    Core::Check(
        dynamic_cast<VulkanTexture*>(&after) != nullptr,
        "Vulkan aliasing barriers require a Vulkan destination texture.");

    VkMemoryBarrier memoryBarrier{
        VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    memoryBarrier.srcAccessMask =
        VK_ACCESS_MEMORY_WRITE_BIT;
    memoryBarrier.dstAccessMask =
        VK_ACCESS_MEMORY_READ_BIT
        | VK_ACCESS_MEMORY_WRITE_BIT;
    vkCmdPipelineBarrier(
        GetCommandBuffer(),
        VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
        VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
        0,
        1,
        &memoryBarrier,
        0,
        nullptr,
        0,
        nullptr);
}

void VulkanContext::BufferAliasingBarrier(
    const IBuffer* before,
    IBuffer& after)
{
    Core::Check(
        before == nullptr
            || dynamic_cast<const VulkanBuffer*>(before)
                != nullptr,
        "Vulkan aliasing barriers require Vulkan buffers.");
    Core::Check(
        dynamic_cast<VulkanBuffer*>(&after) != nullptr,
        "Vulkan aliasing barriers require a Vulkan destination buffer.");

    VkMemoryBarrier memoryBarrier{
        VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    memoryBarrier.srcAccessMask =
        VK_ACCESS_MEMORY_WRITE_BIT;
    memoryBarrier.dstAccessMask =
        VK_ACCESS_MEMORY_READ_BIT
        | VK_ACCESS_MEMORY_WRITE_BIT;
    vkCmdPipelineBarrier(
        GetCommandBuffer(),
        VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
        VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
        0,
        1,
        &memoryBarrier,
        0,
        nullptr,
        0,
        nullptr);
}

void VulkanContext::TransitionTextureView(
    const ITextureView& textureView,
    const ResourceState beforeState,
    const ResourceState afterState)
{
    if (beforeState == afterState)
    {
        return;
    }
    const auto* view = dynamic_cast<const VulkanTextureView*>(&textureView);
    Core::Check(view != nullptr, "Vulkan attachment transitions require Vulkan texture views.");
    const bool depthResource = IsDepthFormat(view->GetTextureDescription().format);
    const ResourceStateMapping before = ToNativeResourceState(beforeState, depthResource);
    const ResourceStateMapping after = ToNativeResourceState(afterState, depthResource);
    const TextureViewDescription& description = view->GetDescription();

    VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    barrier.srcAccessMask = before.accessMask;
    barrier.dstAccessMask = after.accessMask;
    barrier.oldLayout = before.imageLayout;
    barrier.newLayout = after.imageLayout;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = view->GetImage();
    barrier.subresourceRange.aspectMask = depthResource ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
    barrier.subresourceRange.baseMipLevel = description.baseMipLevel;
    barrier.subresourceRange.levelCount = description.mipLevelCount;
    barrier.subresourceRange.baseArrayLayer = description.baseArrayLayer;
    barrier.subresourceRange.layerCount = description.arrayLayerCount;
    vkCmdPipelineBarrier(
        GetCommandBuffer(),
        QueueCompatiblePipelineStages(before.pipelineStages, GetActiveCommandQueue()),
        QueueCompatiblePipelineStages(after.pipelineStages, GetActiveCommandQueue()),
        0,
        0,
        nullptr,
        0,
        nullptr,
        1,
        &barrier);
}

void VulkanContext::RetireGpuObject(
    std::function<void(VkDevice)> destroy)
{
    Core::Check(
        static_cast<bool>(destroy),
        "Vulkan GPU retirement requires a destruction callback.");
    std::scoped_lock lock(
        m_resourceRetirementMutex);
    m_frames[m_currentFrame]
        .retiredGpuObjects.push_back(
            std::move(destroy));
    ++m_resourceRetirementStatistics
          .totalRetiredObjectCount;
    ++m_resourceRetirementStatistics
          .pendingObjectCount;
    m_resourceRetirementStatistics
        .pendingObjectHighWatermark =
        std::max(
            m_resourceRetirementStatistics
                .pendingObjectHighWatermark,
            m_resourceRetirementStatistics
                .pendingObjectCount);
}

void VulkanContext::ExecuteImmediate(
    const std::function<void(VkCommandBuffer)>&
        recordCommands)
{
    Core::Check(static_cast<bool>(recordCommands), "Immediate Vulkan submissions require a recording callback.");
    FlushPendingUploads(true);
    VkCommandBufferAllocateInfo allocateInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    allocateInfo.commandPool = m_commandPool;
    allocateInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocateInfo.commandBufferCount = 1;
    VkCommandBuffer commandBuffer = VK_NULL_HANDLE;
    CheckVk(vkAllocateCommandBuffers(m_device, &allocateInfo, &commandBuffer), "Failed to allocate immediate Vulkan command buffer.");

    VkCommandBufferBeginInfo beginInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    CheckVk(vkBeginCommandBuffer(commandBuffer, &beginInfo), "Failed to begin immediate Vulkan command buffer.");
    recordCommands(commandBuffer);
    CheckVk(vkEndCommandBuffer(commandBuffer), "Failed to end immediate Vulkan command buffer.");

    VkSubmitInfo submitInfo{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers = &commandBuffer;
    CheckVk(vkQueueSubmit(m_graphicsQueue, 1, &submitInfo, VK_NULL_HANDLE), "Failed to submit immediate Vulkan commands.");
    CheckVk(vkQueueWaitIdle(m_graphicsQueue), "Failed to wait for immediate Vulkan commands.");
    vkFreeCommandBuffers(m_device, m_commandPool, 1, &commandBuffer);
}

void VulkanContext::ExecuteComputeImmediate(
    const std::function<void(VkCommandBuffer)>&
        recordCommands)
{
    Core::Check(
        static_cast<bool>(recordCommands),
        "Immediate Vulkan compute submissions require a recording callback.");
    Core::Check(
        m_computeQueue != VK_NULL_HANDLE
            && m_computeCommandPool != VK_NULL_HANDLE,
        "The Vulkan compute queue is not initialized.");

    VkCommandBufferAllocateInfo allocateInfo{
        VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    allocateInfo.commandPool = m_computeCommandPool;
    allocateInfo.level =
        VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocateInfo.commandBufferCount = 1;
    VkCommandBuffer commandBuffer = VK_NULL_HANDLE;
    CheckVk(
        vkAllocateCommandBuffers(
            m_device,
            &allocateInfo,
            &commandBuffer),
        "Failed to allocate an immediate Vulkan compute command buffer.");

    VkCommandBufferBeginInfo beginInfo{
        VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    beginInfo.flags =
        VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    CheckVk(
        vkBeginCommandBuffer(
            commandBuffer,
            &beginInfo),
        "Failed to begin an immediate Vulkan compute command buffer.");
    recordCommands(commandBuffer);
    CheckVk(
        vkEndCommandBuffer(commandBuffer),
        "Failed to end an immediate Vulkan compute command buffer.");

    VkSubmitInfo submitInfo{
        VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers = &commandBuffer;
    const std::uint64_t signalValue =
        m_nextQueueTimelineValue++;
    VkTimelineSemaphoreSubmitInfo timelineInfo{
        VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO};
    timelineInfo.signalSemaphoreValueCount = 1;
    timelineInfo.pSignalSemaphoreValues =
        &signalValue;
    submitInfo.pNext = &timelineInfo;
    submitInfo.signalSemaphoreCount = 1;
    submitInfo.pSignalSemaphores =
        &m_queueTimelineSemaphore;
    VkFenceCreateInfo fenceInfo{
        VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    VkFence fence = VK_NULL_HANDLE;
    CheckVk(
        vkCreateFence(
            m_device,
            &fenceInfo,
            nullptr,
            &fence),
        "Failed to create an immediate Vulkan compute fence.");
    CheckVk(
        vkQueueSubmit(
            m_computeQueue,
            1,
            &submitInfo,
            fence),
        "Failed to submit an immediate Vulkan compute command buffer.");
    CheckVk(
        vkWaitForFences(
            m_device,
            1,
            &fence,
            VK_TRUE,
            UINT64_MAX),
        "Failed to wait for an immediate Vulkan compute submission.");
    std::uint64_t completedValue = 0;
    CheckVk(
        vkGetSemaphoreCounterValue(
            m_device,
            m_queueTimelineSemaphore,
            &completedValue),
        "Failed to query the Vulkan queue timeline semaphore.");
    Core::Check(
        completedValue >= signalValue,
        "The Vulkan compute queue did not signal its timeline semaphore.");
    m_computeQueueValidated = true;
    vkDestroyFence(m_device, fence, nullptr);
    vkFreeCommandBuffers(
        m_device,
        m_computeCommandPool,
        1,
        &commandBuffer);
}

void VulkanContext::BindGraphicsPipeline(const VkPipeline pipeline, const VkPipelineLayout layout)
{
    Core::Check(pipeline != VK_NULL_HANDLE && layout != VK_NULL_HANDLE, "Vulkan graphics pipeline binding requires valid handles.");
    m_activePipelineLayout = layout;
    m_activePipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    vkCmdBindPipeline(GetCommandBuffer(), VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
}

void VulkanContext::RequestCapture(const std::filesystem::path& outputPath)
{
    m_captureSourceImage = VK_NULL_HANDLE;
    m_capturePath = outputPath;
    m_captureRequested = !outputPath.empty();
    m_captureComplete = false;
    m_captureError.clear();
}

void VulkanContext::SetCaptureTexture(const ITexture* texture)
{
    if (!m_captureRequested) return;
    const auto* native = dynamic_cast<const VulkanTexture*>(texture);
    Core::Check(native != nullptr, "Vulkan capture requires a Vulkan texture.");
    const auto& description = native->GetDescription();
    Core::Check(description.format == Format::Rgba8Unorm || description.format == Format::Rgba8UnormSrgb
            || description.format == Format::Bgra8Unorm || description.format == Format::Bgra8UnormSrgb,
        "Vulkan final-output capture requires RGBA8/BGRA8.");
    m_captureSourceImage = native->GetImage();
    m_captureExtent = {description.width, description.height};
    m_captureFormat = ToNativeFormat(description.format);
}

bool VulkanContext::IsCaptureComplete() const
{
    return m_captureComplete;
}

const std::string& VulkanContext::GetCaptureError() const
{
    return m_captureError;
}

void VulkanContext::CreateBuffer(
    const VkDeviceSize size,
    const VkBufferUsageFlags usage,
    const VkMemoryPropertyFlags memoryProperties,
    VkBuffer& outBuffer,
    VkDeviceMemory& outMemory,
    const bool enableDeviceAddress) const
{
    VkBufferCreateInfo bufferInfo{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bufferInfo.size = size;
    bufferInfo.usage = usage;
    std::array<std::uint32_t, 3> queueFamilies{};
    std::uint32_t queueFamilyCount = 0;
    for (const std::uint32_t family : {
             m_queueFamilies.graphics,
             m_queueFamilies.compute,
             m_queueFamilies.transfer})
    {
        if (std::find(
                queueFamilies.begin(),
                queueFamilies.begin()
                    + queueFamilyCount,
                family)
            == queueFamilies.begin()
                + queueFamilyCount)
        {
            queueFamilies[queueFamilyCount++] =
                family;
        }
    }
    if (queueFamilyCount > 1)
    {
        bufferInfo.sharingMode =
            VK_SHARING_MODE_CONCURRENT;
        bufferInfo.queueFamilyIndexCount =
            queueFamilyCount;
        bufferInfo.pQueueFamilyIndices =
            queueFamilies.data();
    }
    else
    {
        bufferInfo.sharingMode =
            VK_SHARING_MODE_EXCLUSIVE;
    }
    CheckVk(vkCreateBuffer(m_device, &bufferInfo, nullptr, &outBuffer), "Failed to create Vulkan buffer.");

    VkMemoryRequirements memoryRequirements{};
    vkGetBufferMemoryRequirements(m_device, outBuffer, &memoryRequirements);

    VkMemoryAllocateInfo allocateInfo{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    allocateInfo.allocationSize = memoryRequirements.size;
    allocateInfo.memoryTypeIndex = FindMemoryType(memoryRequirements.memoryTypeBits, memoryProperties);
    VkMemoryAllocateFlagsInfo allocateFlags{
        VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO};
    if (enableDeviceAddress)
    {
        Core::Check(
            m_deviceCapabilities.features
                .bufferDeviceAddress,
            "Vulkan device-address buffers require bufferDeviceAddress support.");
        allocateFlags.flags =
            VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT;
        allocateInfo.pNext = &allocateFlags;
    }
    try
    {
        CheckVk(vkAllocateMemory(m_device, &allocateInfo, nullptr, &outMemory), "Failed to allocate Vulkan buffer memory.");
        CheckVk(vkBindBufferMemory(m_device, outBuffer, outMemory, 0), "Failed to bind Vulkan buffer memory.");
    }
    catch (...)
    {
        if (outMemory != VK_NULL_HANDLE)
        {
            vkFreeMemory(m_device, outMemory, nullptr);
            outMemory = VK_NULL_HANDLE;
        }
        vkDestroyBuffer(m_device, outBuffer, nullptr);
        outBuffer = VK_NULL_HANDLE;
        throw;
    }
}

std::uint32_t VulkanContext::FindMemoryType(const std::uint32_t typeFilter, const VkMemoryPropertyFlags properties) const
{
    VkPhysicalDeviceMemoryProperties memoryProperties{};
    vkGetPhysicalDeviceMemoryProperties(m_physicalDevice, &memoryProperties);
    for (std::uint32_t index = 0; index < memoryProperties.memoryTypeCount; ++index)
    {
        if ((typeFilter & (1u << index)) != 0
            && (memoryProperties.memoryTypes[index].propertyFlags & properties) == properties)
        {
            return index;
        }
    }
    throw std::runtime_error("No compatible Vulkan memory type was found.");
}

VkInstance VulkanContext::GetInstance() const { return m_instance; }
VkPhysicalDevice VulkanContext::GetPhysicalDevice() const { return m_physicalDevice; }
VkDevice VulkanContext::GetDevice() const { return m_device; }
VkQueue VulkanContext::GetGraphicsQueue() const { return m_graphicsQueue; }
VkQueue VulkanContext::GetComputeQueue() const { return m_computeQueue; }
VkSemaphore VulkanContext::GetQueueTimelineSemaphore() const
{
    return m_queueTimelineSemaphore;
}
VkCommandBuffer VulkanContext::GetCommandBuffer() const { return m_frames[m_currentFrame].commandBuffer; }
VkRenderPass VulkanContext::GetRenderPass() const { return m_renderPass; }
VkExtent2D VulkanContext::GetSwapChainExtent() const { return m_swapChainExtent; }
std::uint32_t VulkanContext::GetSwapChainImageCount() const
{
    return static_cast<std::uint32_t>(m_swapChainImages.size());
}
VkFormat VulkanContext::GetSwapChainFormat() const { return m_swapChainFormat; }
Format VulkanContext::GetSwapChainRhiFormat() const
{
    switch (m_swapChainFormat)
    {
    case VK_FORMAT_R8G8B8A8_UNORM: return Format::Rgba8Unorm;
    case VK_FORMAT_R8G8B8A8_SRGB: return Format::Rgba8UnormSrgb;
    case VK_FORMAT_B8G8R8A8_UNORM: return Format::Bgra8Unorm;
    case VK_FORMAT_B8G8R8A8_SRGB: return Format::Bgra8UnormSrgb;
    default: throw std::runtime_error("The Vulkan swap-chain format has no Prism RHI mapping.");
    }
}
Format VulkanContext::GetBackBufferRhiFormat() const
{
    return GetSwapChainRhiFormat();
}
const ITextureView& VulkanContext::GetCurrentBackBufferView() const
{
    Core::Check(m_currentImage < m_swapChainTextureViews.size(), "Vulkan back-buffer view is unavailable.");
    return *m_swapChainTextureViews[m_currentImage];
}
const std::shared_ptr<ITexture>&
VulkanContext::GetCurrentBackBufferTexture() const
{
    Core::Check(
        m_currentImage < m_swapChainTextures.size(),
        "Vulkan back-buffer texture is unavailable.");
    return m_swapChainTextures[m_currentImage];
}
const ITextureView& VulkanContext::GetDepthStencilView() const
{
    Core::Check(m_depthTextureView != nullptr, "Vulkan depth-stencil view is unavailable.");
    return *m_depthTextureView;
}

const std::shared_ptr<ITexture>&
VulkanContext::GetDepthStencilTexture() const
{
    Core::Check(
        m_depthTexture != nullptr,
        "Vulkan depth-stencil texture is unavailable.");
    return m_depthTexture;
}
std::uint32_t VulkanContext::GetCurrentFrameIndex() const { return m_currentFrame; }

std::uint32_t VulkanContext::GetFramesInFlight() const { return FrameCount; }
std::uint32_t VulkanContext::GetFrameWidth() const
{
    return m_swapChainExtent.width;
}
std::uint32_t VulkanContext::GetFrameHeight() const
{
    return m_swapChainExtent.height;
}
const std::string& VulkanContext::GetAdapterName() const { return m_adapterName; }
const GraphicsAdapterInfo& VulkanContext::GetAdapterInfo() const { return m_adapterInfo; }
bool VulkanContext::SupportsSamplerAnisotropy() const { return m_samplerAnisotropySupported; }
GraphicsApi VulkanContext::GetGraphicsApi() const { return GraphicsApi::Vulkan; }
const GraphicsDeviceCapabilities&
VulkanContext::GetCapabilities() const
{
    return m_deviceCapabilities;
}
DescriptorAllocatorStatistics
VulkanContext::GetDescriptorAllocatorStatistics() const
{
    return m_descriptorAllocator != nullptr
        ? m_descriptorAllocator->GetStatistics()
        : DescriptorAllocatorStatistics{};
}
UploadQueueStatistics
VulkanContext::GetUploadQueueStatistics() const
{
    std::scoped_lock lock(m_uploadMutex);
    UploadQueueStatistics statistics =
        m_uploadQueueStatistics;
    if (m_uploadTimelineSemaphore != VK_NULL_HANDLE)
    {
        CheckVk(
            vkGetSemaphoreCounterValue(
                m_device,
                m_uploadTimelineSemaphore,
                &statistics.completedTicket),
            "Failed to query the Vulkan upload timeline.");
    }
    statistics.outstandingBatchCount =
        statistics.lastSubmittedTicket
            >= statistics.completedTicket
        ? statistics.lastSubmittedTicket
            - statistics.completedTicket
        : 0;
    return statistics;
}

UploadTicket
VulkanContext::GetPendingUploadTicket() const
{
    std::scoped_lock lock(m_uploadMutex);
    return m_pendingUploadTicket;
}

bool VulkanContext::IsUploadComplete(
    const UploadTicket ticket) const
{
    if (!ticket.IsValid())
    {
        return true;
    }
    if (m_uploadTimelineSemaphore == VK_NULL_HANDLE)
    {
        return false;
    }
    std::uint64_t completedValue = 0;
    CheckVk(
        vkGetSemaphoreCounterValue(
            m_device,
            m_uploadTimelineSemaphore,
            &completedValue),
        "Failed to query a Vulkan upload ticket.");
    return completedValue >= ticket.value;
}
ResourceRetirementStatistics
VulkanContext::GetResourceRetirementStatistics() const
{
    std::scoped_lock lock(
        m_resourceRetirementMutex);
    return m_resourceRetirementStatistics;
}
CommandQueueCapabilities
VulkanContext::GetQueueCapabilities() const
{
    const bool batchSubmission =
        m_batchTimelineSemaphores[0] != VK_NULL_HANDLE
        && m_batchTimelineSemaphores[1] != VK_NULL_HANDLE
        && m_computeQueueValidated;
    return {
        true,
        m_computeQueue != VK_NULL_HANDLE
            && m_computeQueueValidated,
        m_queueFamilies.compute
                != m_queueFamilies.graphics
            && m_computeQueueValidated,
        m_queueTimelineSemaphore != VK_NULL_HANDLE
            && m_computeQueueValidated,
        m_queueTimelineSemaphore != VK_NULL_HANDLE
            && m_computeQueueValidated,
        batchSubmission,
        batchSubmission,
        batchSubmission};
}

CommandQueueType
VulkanContext::GetActiveCommandQueue() const
{
    return m_activeCommandQueue;
}

std::uint32_t
VulkanContext::GetGraphicsQueueFamilyIndex() const
{
    return m_queueFamilies.graphics;
}

std::uint32_t
VulkanContext::GetComputeQueueFamilyIndex() const
{
    return m_queueFamilies.compute;
}

std::uint32_t
VulkanContext::GetTransferQueueFamilyIndex() const
{
    return m_queueFamilies.transfer;
}

VkCommandBuffer
VulkanContext::AllocateAndBeginCommandBuffer(
    const VkCommandPool pool) const
{
    VkCommandBufferAllocateInfo allocateInfo{
        VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    allocateInfo.commandPool = pool;
    allocateInfo.level =
        VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocateInfo.commandBufferCount = 1;
    VkCommandBuffer commandBuffer = VK_NULL_HANDLE;
    CheckVk(
        vkAllocateCommandBuffers(
            m_device,
            &allocateInfo,
            &commandBuffer),
        "Failed to allocate a Vulkan queue-segment command buffer.");
    VkCommandBufferBeginInfo beginInfo{
        VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    beginInfo.flags =
        VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    CheckVk(
        vkBeginCommandBuffer(
            commandBuffer,
            &beginInfo),
        "Failed to begin a Vulkan queue-segment command buffer.");
    return commandBuffer;
}

void VulkanContext::RetireCurrentCommandBuffer(
    FrameContext& frame,
    const VkCommandPool pool)
{
    frame.retiredCommandBuffers.push_back({
        pool,
        frame.commandBuffer});
}

void VulkanContext::CloseActiveBatchSegment()
{
    Core::Check(
        m_queueBatchOpen,
        "A Vulkan queue batch must be open before closing a segment.");
    FrameContext& frame =
        m_frames[m_currentFrame];
    CheckVk(
        vkEndCommandBuffer(frame.commandBuffer),
        "Failed to end a Vulkan queue-batch segment.");
    m_activeBatchCommandBuffers.push_back(
        frame.commandBuffer);
    RetireCurrentCommandBuffer(
        frame,
        m_activeCommandQueue
                == CommandQueueType::Compute
            ? m_computeCommandPool
            : m_commandPool);
}

bool VulkanContext::SwitchCommandQueue(
    const CommandQueueType queue)
{
    Core::Check(
        !m_queueBatchExecutionActive,
        "Legacy Vulkan queue switching cannot run during queue-batch execution.");
    Core::Check(
        m_frameInProgress,
        "Vulkan queue switching requires an active frame.");
    Core::Check(
        !m_renderingInProgress,
        "Vulkan command queues cannot switch inside a rendering scope.");
    if (queue == m_activeCommandQueue)
    {
        return true;
    }
    Core::Check(
        m_computeQueueValidated
            && m_queueTimelineSemaphore
                != VK_NULL_HANDLE,
        "Vulkan native queue switching is unavailable.");

    FrameContext& frame = m_frames[m_currentFrame];
    CheckVk(
        vkEndCommandBuffer(frame.commandBuffer),
        "Failed to end a Vulkan queue-segment command buffer.");

    if (m_activeCommandQueue
            == CommandQueueType::Graphics
        && queue == CommandQueueType::Compute)
    {
        std::array<VkSemaphore, 3> waits{};
        std::array<VkPipelineStageFlags, 3> stages{};
        std::array<std::uint64_t, 3> waitValues{};
        std::uint32_t waitCount = 0;
        if (!frame.imageAvailableConsumed)
        {
            waits[waitCount] = frame.imageAvailable;
            stages[waitCount] =
                VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
            waitValues[waitCount] = 0;
            ++waitCount;
            frame.imageAvailableConsumed = true;
        }
        if (frame.pendingUploadWaitValue > 0)
        {
            waits[waitCount] =
                m_uploadTimelineSemaphore;
            stages[waitCount] =
                VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
            waitValues[waitCount] =
                frame.pendingUploadWaitValue;
            ++waitCount;
        }
        if (frame.pendingTimelineWaitValue > 0)
        {
            waits[waitCount] =
                m_queueTimelineSemaphore;
            stages[waitCount] =
                VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
            waitValues[waitCount] =
                frame.pendingTimelineWaitValue;
            ++waitCount;
        }

        const std::uint64_t signalValue =
            m_nextQueueTimelineValue++;
        VkTimelineSemaphoreSubmitInfo timelineInfo{
            VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO};
        timelineInfo.waitSemaphoreValueCount =
            waitCount;
        timelineInfo.pWaitSemaphoreValues =
            waitValues.data();
        timelineInfo.signalSemaphoreValueCount = 1;
        timelineInfo.pSignalSemaphoreValues =
            &signalValue;
        VkSubmitInfo submitInfo{
            VK_STRUCTURE_TYPE_SUBMIT_INFO};
        submitInfo.pNext = &timelineInfo;
        submitInfo.waitSemaphoreCount = waitCount;
        submitInfo.pWaitSemaphores = waits.data();
        submitInfo.pWaitDstStageMask = stages.data();
        submitInfo.commandBufferCount = 1;
        submitInfo.pCommandBuffers =
            &frame.commandBuffer;
        submitInfo.signalSemaphoreCount = 1;
        submitInfo.pSignalSemaphores =
            &m_queueTimelineSemaphore;
        CheckVk(
            vkQueueSubmit(
                m_graphicsQueue,
                1,
                &submitInfo,
                VK_NULL_HANDLE),
            "Failed to submit a Vulkan graphics RDG segment.");
        RetireCurrentCommandBuffer(
            frame,
            m_commandPool);
        frame.commandBuffer =
            AllocateAndBeginCommandBuffer(
                m_computeCommandPool);
        frame.pendingTimelineWaitValue = 0;
        frame.pendingUploadWaitValue = 0;
        m_activeQueueWaitValue = signalValue;
        m_activeCommandQueue =
            CommandQueueType::Compute;
        m_activePipelineLayout = VK_NULL_HANDLE;
        m_activePipelineBindPoint =
            VK_PIPELINE_BIND_POINT_COMPUTE;
        return true;
    }

    Core::Check(
        m_activeCommandQueue
                == CommandQueueType::Compute
            && queue == CommandQueueType::Graphics,
        "Unsupported Vulkan command-queue transition.");
    const std::uint64_t signalValue =
        m_nextQueueTimelineValue++;
    VkTimelineSemaphoreSubmitInfo timelineInfo{
        VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO};
    timelineInfo.waitSemaphoreValueCount = 1;
    timelineInfo.pWaitSemaphoreValues =
        &m_activeQueueWaitValue;
    timelineInfo.signalSemaphoreValueCount = 1;
    timelineInfo.pSignalSemaphoreValues =
        &signalValue;
    VkSubmitInfo submitInfo{
        VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submitInfo.pNext = &timelineInfo;
    submitInfo.waitSemaphoreCount = 1;
    submitInfo.pWaitSemaphores =
        &m_queueTimelineSemaphore;
    constexpr VkPipelineStageFlags waitStage =
        VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    submitInfo.pWaitDstStageMask = &waitStage;
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers =
        &frame.commandBuffer;
    submitInfo.signalSemaphoreCount = 1;
    submitInfo.pSignalSemaphores =
        &m_queueTimelineSemaphore;
    CheckVk(
        vkQueueSubmit(
            m_computeQueue,
            1,
            &submitInfo,
            VK_NULL_HANDLE),
        "Failed to submit a Vulkan compute RDG segment.");
    RetireCurrentCommandBuffer(
        frame,
        m_computeCommandPool);
    frame.commandBuffer =
        AllocateAndBeginCommandBuffer(m_commandPool);
    frame.pendingTimelineWaitValue = signalValue;
    m_activeQueueWaitValue = 0;
    m_activeCommandQueue =
        CommandQueueType::Graphics;
    m_activePipelineLayout = VK_NULL_HANDLE;
    m_activePipelineBindPoint =
        VK_PIPELINE_BIND_POINT_GRAPHICS;
    return true;
}

bool VulkanContext::BeginQueueBatch(
    const CommandQueueType queue,
    const std::span<const QueueSyncPoint> waits)
{
    Core::Check(
        m_frameInProgress,
        "Vulkan queue batches require an active frame.");
    Core::Check(
        !m_renderingInProgress,
        "Vulkan queue batches cannot begin inside a rendering scope.");
    Core::Check(
        m_computeQueueValidated
            && m_batchTimelineSemaphores[0]
                != VK_NULL_HANDLE
            && m_batchTimelineSemaphores[1]
                != VK_NULL_HANDLE,
        "Vulkan independent queue batches are unavailable.");
    Core::Check(
        !m_queueBatchOpen,
        "A Vulkan queue batch is already open.");

    FrameContext& frame =
        m_frames[m_currentFrame];
    if (!m_queueBatchExecutionActive)
    {
        Core::Check(
            queue == CommandQueueType::Graphics
                && m_activeCommandQueue
                    == CommandQueueType::Graphics,
            "Vulkan queue-batch execution must begin with the active graphics command buffer.");
        m_queueBatchExecutionActive = true;
        m_activeBatchCommandBuffers.clear();
    }
    else
    {
        frame.commandBuffer =
            AllocateAndBeginCommandBuffer(
                queue == CommandQueueType::Compute
                    ? m_computeCommandPool
                    : m_commandPool);
        m_activeBatchCommandBuffers.clear();
    }

    m_activeCommandQueue = queue;
    m_activeBatchWaits.assign(
        waits.begin(),
        waits.end());
    m_activePipelineLayout = VK_NULL_HANDLE;
    m_activePipelineBindPoint =
        queue == CommandQueueType::Compute
            ? VK_PIPELINE_BIND_POINT_COMPUTE
            : VK_PIPELINE_BIND_POINT_GRAPHICS;
    m_queueBatchOpen = true;
    return true;
}

QueueSyncPoint VulkanContext::EndQueueBatch()
{
    Core::Check(
        m_queueBatchExecutionActive
            && m_queueBatchOpen,
        "No Vulkan queue batch is open.");
    Core::Check(
        !m_renderingInProgress,
        "Vulkan queue batches cannot end inside a rendering scope.");

    CloseActiveBatchSegment();

    const std::size_t activeQueueIndex =
        QueueIndex(m_activeCommandQueue);
    QueueSyncPoint signal{
        m_activeCommandQueue,
        m_nextBatchTimelineValues[
            activeQueueIndex]++};
    PendingQueueBatch pending{};
    pending.commandBuffers =
        std::move(m_activeBatchCommandBuffers);
    pending.queue = m_activeCommandQueue;
    pending.waits =
        std::move(m_activeBatchWaits);
    pending.signal = signal;
    m_pendingQueueBatches.push_back(
        std::move(pending));
    m_queueBatchOpen = false;
    return signal;
}

bool VulkanContext::FlushQueueBatches()
{
    Core::Check(
        m_queueBatchExecutionActive
            && !m_queueBatchOpen,
        "Vulkan queue batches must be closed before submission.");
    Core::Check(
        !m_pendingQueueBatches.empty(),
        "No Vulkan queue batches are pending submission.");

    FrameContext& frame =
        m_frames[m_currentFrame];
    for (const PendingQueueBatch& batch :
         m_pendingQueueBatches)
    {
        std::array<VkSemaphore, 4>
            waitSemaphores{};
        std::array<VkPipelineStageFlags, 4>
            waitStages{};
        std::array<std::uint64_t, 4>
            waitValues{};
        std::uint32_t waitCount = 0;
        if (batch.queue
                == CommandQueueType::Graphics
            && !frame.imageAvailableConsumed)
        {
            waitSemaphores[waitCount] =
                frame.imageAvailable;
            waitStages[waitCount] =
                VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
            waitValues[waitCount] = 0;
            ++waitCount;
            frame.imageAvailableConsumed = true;
        }
        if (frame.pendingUploadWaitValue > 0)
        {
            waitSemaphores[waitCount] =
                m_uploadTimelineSemaphore;
            waitStages[waitCount] =
                VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
            waitValues[waitCount] =
                frame.pendingUploadWaitValue;
            ++waitCount;
        }

        std::array<std::uint64_t, 2>
            queueWaitValues{};
        for (const QueueSyncPoint wait :
             batch.waits)
        {
            if (!wait.IsValid()
                || wait.queue == batch.queue)
            {
                continue;
            }
            const std::size_t queueIndex =
                QueueIndex(wait.queue);
            queueWaitValues[queueIndex] =
                std::max(
                    queueWaitValues[queueIndex],
                    wait.value);
        }
        for (std::size_t queueIndex = 0;
             queueIndex < queueWaitValues.size();
             ++queueIndex)
        {
            if (queueWaitValues[queueIndex] == 0)
            {
                continue;
            }
            waitSemaphores[waitCount] =
                m_batchTimelineSemaphores[
                    queueIndex];
            waitStages[waitCount] =
                VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
            waitValues[waitCount] =
                queueWaitValues[queueIndex];
            ++waitCount;
        }

        const std::size_t activeQueueIndex =
            QueueIndex(batch.queue);
        VkTimelineSemaphoreSubmitInfo timelineInfo{
            VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO};
        timelineInfo.waitSemaphoreValueCount =
            waitCount;
        timelineInfo.pWaitSemaphoreValues =
            waitValues.data();
        timelineInfo.signalSemaphoreValueCount = 1;
        timelineInfo.pSignalSemaphoreValues =
            &batch.signal.value;
        VkSubmitInfo submitInfo{
            VK_STRUCTURE_TYPE_SUBMIT_INFO};
        submitInfo.pNext = &timelineInfo;
        submitInfo.waitSemaphoreCount = waitCount;
        submitInfo.pWaitSemaphores =
            waitSemaphores.data();
        submitInfo.pWaitDstStageMask =
            waitStages.data();
        submitInfo.commandBufferCount =
            static_cast<std::uint32_t>(
                batch.commandBuffers.size());
        submitInfo.pCommandBuffers =
            batch.commandBuffers.data();
        submitInfo.signalSemaphoreCount = 1;
        submitInfo.pSignalSemaphores =
            &m_batchTimelineSemaphores[
                activeQueueIndex];
        VkQueue targetQueue =
            batch.queue == CommandQueueType::Compute
            ? m_computeQueue
            : m_graphicsQueue;
        CheckVk(
            vkQueueSubmit(
                targetQueue,
                1,
                &submitInfo,
                VK_NULL_HANDLE),
            "Failed to submit a deferred Vulkan RDG queue batch.");
    }
    m_pendingQueueBatches.clear();
    frame.pendingUploadWaitValue = 0;
    return true;
}

std::unique_ptr<IParallelCommandRecording>
VulkanContext::CreateParallelCommandRecording(
    const CommandQueueType queue)
{
    Core::Check(
        m_frameInProgress,
        "Vulkan parallel command recording requires an active frame.");
    Core::Check(
        queue == CommandQueueType::Graphics
            || (queue == CommandQueueType::Compute
                && m_computeQueueValidated),
        "The requested Vulkan parallel recording queue is unavailable.");
    return std::make_unique<
        VulkanParallelCommandRecording>(
        *this,
        queue);
}

bool VulkanContext::AppendParallelCommandRecording(
    std::unique_ptr<IParallelCommandRecording>
        recording)
{
    Core::Check(
        m_queueBatchExecutionActive
            && m_queueBatchOpen,
        "Vulkan parallel recordings can only be appended to an open queue batch.");
    auto* nativeRecording =
        dynamic_cast<
            VulkanParallelCommandRecording*>(
            recording.get());
    Core::Check(
        nativeRecording != nullptr,
        "VulkanContext received a non-Vulkan parallel command recording.");
    Core::Check(
        nativeRecording->GetQueue()
            == m_activeCommandQueue,
        "A Vulkan parallel command recording was appended to the wrong queue.");

    CloseActiveBatchSegment();
    m_activeBatchCommandBuffers.push_back(
        nativeRecording->GetCommandBuffer());
    FrameContext& frame =
        m_frames[m_currentFrame];
    frame.retiredCommandPools.push_back(
        nativeRecording->ReleaseCommandPool());
    frame.commandBuffer =
        AllocateAndBeginCommandBuffer(
            m_activeCommandQueue
                    == CommandQueueType::Compute
                ? m_computeCommandPool
                : m_commandPool);
    m_activePipelineLayout = VK_NULL_HANDLE;
    m_activePipelineBindPoint =
        m_activeCommandQueue
                == CommandQueueType::Compute
            ? VK_PIPELINE_BIND_POINT_COMPUTE
            : VK_PIPELINE_BIND_POINT_GRAPHICS;
    return true;
}

bool VulkanContext::ResumeGraphicsQueue(
    const std::span<const QueueSyncPoint> waits)
{
    Core::Check(
        m_queueBatchExecutionActive
            && !m_queueBatchOpen
            && m_pendingQueueBatches.empty(),
        "Vulkan graphics continuation requires completed queue batches.");
    FrameContext& frame =
        m_frames[m_currentFrame];
    frame.commandBuffer =
        AllocateAndBeginCommandBuffer(
            m_commandPool);
    frame.pendingBatchWaitValues = {};
    for (const QueueSyncPoint wait : waits)
    {
        if (!wait.IsValid()
            || wait.queue
                == CommandQueueType::Graphics)
        {
            continue;
        }
        const std::size_t queueIndex =
            QueueIndex(wait.queue);
        frame.pendingBatchWaitValues[queueIndex] =
            std::max(
                frame.pendingBatchWaitValues[queueIndex],
                wait.value);
    }
    m_activeCommandQueue =
        CommandQueueType::Graphics;
    m_activePipelineLayout = VK_NULL_HANDLE;
    m_activePipelineBindPoint =
        VK_PIPELINE_BIND_POINT_GRAPHICS;
    m_activeBatchWaits.clear();
    m_queueBatchExecutionActive = false;
    return true;
}

void VulkanContext::CreateInstance()
{
    std::uint32_t extensionCount = 0;
    const char** glfwExtensions = glfwGetRequiredInstanceExtensions(&extensionCount);
    Core::Check(glfwExtensions != nullptr && extensionCount > 0, "GLFW did not provide Vulkan surface extensions.");

    VkApplicationInfo applicationInfo{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    applicationInfo.pApplicationName = "PrismRender";
    applicationInfo.applicationVersion = VK_MAKE_API_VERSION(0, 0, 1, 0);
    applicationInfo.pEngineName = "PrismRender";
    applicationInfo.engineVersion = VK_MAKE_API_VERSION(0, 0, 1, 0);
    applicationInfo.apiVersion = VK_API_VERSION_1_3;

    VkInstanceCreateInfo createInfo{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    const bool validation = Core::IsEnvironmentVariableEnabled("PRISM_RENDER_GPU_VALIDATION");
    std::vector<const char*> extensions(glfwExtensions, glfwExtensions + extensionCount);
    const char* validationLayer = "VK_LAYER_KHRONOS_validation";
    VkDebugReportCallbackCreateInfoEXT debugInfo{VK_STRUCTURE_TYPE_DEBUG_REPORT_CALLBACK_CREATE_INFO_EXT};
    if (validation)
    {
        extensions.push_back(VK_EXT_DEBUG_REPORT_EXTENSION_NAME);
        createInfo.enabledLayerCount = 1u;
        createInfo.ppEnabledLayerNames = &validationLayer;
        debugInfo.flags = VK_DEBUG_REPORT_WARNING_BIT_EXT | VK_DEBUG_REPORT_ERROR_BIT_EXT;
        debugInfo.pfnCallback = ValidationCallback;
        VulkanValidationErrors = 0u;
    }
    createInfo.pApplicationInfo = &applicationInfo;
    createInfo.enabledExtensionCount = static_cast<std::uint32_t>(extensions.size());
    createInfo.ppEnabledExtensionNames = extensions.data();
    CheckVk(vkCreateInstance(&createInfo, nullptr, &m_instance), "Failed to create Vulkan instance.");
    if (validation)
    {
        const auto create = reinterpret_cast<PFN_vkCreateDebugReportCallbackEXT>(
            vkGetInstanceProcAddr(m_instance, "vkCreateDebugReportCallbackEXT"));
        Core::Check(create != nullptr, "Vulkan debug messenger extension unavailable.");
        CheckVk(create(m_instance, &debugInfo, nullptr, &m_debugMessenger), "Failed to create Vulkan debug messenger.");
        std::cerr << "Vulkan Khronos validation layer enabled.\n";
    }
}

void VulkanContext::CreateSurface()
{
    CheckVk(
        glfwCreateWindowSurface(m_instance, m_window->GetNativeWindow(), nullptr, &m_surface),
        "Failed to create GLFW Vulkan surface.");
}

void VulkanContext::PickPhysicalDevice()
{
    std::uint32_t deviceCount = 0;
    CheckVk(vkEnumeratePhysicalDevices(m_instance, &deviceCount, nullptr), "Failed to count Vulkan physical devices.");
    Core::Check(deviceCount > 0, "No Vulkan physical device was found.");

    std::vector<VkPhysicalDevice> devices(deviceCount);
    CheckVk(vkEnumeratePhysicalDevices(m_instance, &deviceCount, devices.data()), "Failed to enumerate Vulkan physical devices.");

    std::uint32_t bestScore = 0;
    for (const VkPhysicalDevice device : devices)
    {
        VkPhysicalDeviceProperties properties{};
        vkGetPhysicalDeviceProperties(device, &properties);
        if (properties.apiVersion < VK_API_VERSION_1_3)
        {
            continue;
        }
        const QueueFamilyIndices indices = FindQueueFamilies(device);
        if (!indices.IsComplete() || !SupportsRequiredDeviceExtensions(device)
            || !QuerySwapChainSupport(device).IsAdequate())
        {
            continue;
        }

        std::uint32_t score = properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU ? 1000u : 100u;
        score += properties.limits.maxImageDimension2D;
        if (m_physicalDevice == VK_NULL_HANDLE || score > bestScore)
        {
            m_physicalDevice = device;
            m_queueFamilies = indices;
            m_adapterName = properties.deviceName;
            m_adapterInfo.name = properties.deviceName;
            m_adapterInfo.vendorId = properties.vendorID;
            m_adapterInfo.deviceId = properties.deviceID;
            m_adapterInfo.driverVersionRaw =
                properties.driverVersion;
            m_adapterInfo.driverVersion =
                std::to_string(VK_VERSION_MAJOR(
                    properties.driverVersion))
                + '.'
                + std::to_string(VK_VERSION_MINOR(
                    properties.driverVersion))
                + '.'
                + std::to_string(VK_VERSION_PATCH(
                    properties.driverVersion));
            m_adapterInfo.apiVersion =
                std::to_string(VK_VERSION_MAJOR(
                    properties.apiVersion))
                + '.'
                + std::to_string(VK_VERSION_MINOR(
                    properties.apiVersion))
                + '.'
                + std::to_string(VK_VERSION_PATCH(
                    properties.apiVersion));
            VkPhysicalDeviceMemoryProperties memory{};
            vkGetPhysicalDeviceMemoryProperties(device, &memory);
            m_adapterInfo.dedicatedVideoMemoryBytes = 0;
            m_adapterInfo.sharedSystemMemoryBytes = 0;
            for (std::uint32_t heapIndex = 0;
                 heapIndex < memory.memoryHeapCount;
                 ++heapIndex)
            {
                const VkMemoryHeap& heap =
                    memory.memoryHeaps[heapIndex];
                if ((heap.flags
                     & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) != 0)
                {
                    m_adapterInfo.dedicatedVideoMemoryBytes
                        += heap.size;
                }
                else
                {
                    m_adapterInfo.sharedSystemMemoryBytes
                        += heap.size;
                }
            }
            bestScore = score;
        }
    }
    Core::Check(m_physicalDevice != VK_NULL_HANDLE, "No Vulkan device supports graphics, presentation, and VK_KHR_swapchain.");
}

void VulkanContext::CreateLogicalDevice()
{
    const std::set<std::uint32_t> uniqueFamilies = {
        m_queueFamilies.graphics,
        m_queueFamilies.present,
        m_queueFamilies.compute,
        m_queueFamilies.transfer};
    constexpr float queuePriority = 1.0f;
    std::vector<VkDeviceQueueCreateInfo> queueCreateInfos;
    queueCreateInfos.reserve(uniqueFamilies.size());
    for (const std::uint32_t family : uniqueFamilies)
    {
        VkDeviceQueueCreateInfo queueInfo{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
        queueInfo.queueFamilyIndex = family;
        queueInfo.queueCount = 1;
        queueInfo.pQueuePriorities = &queuePriority;
        queueCreateInfos.push_back(queueInfo);
    }

    VkPhysicalDeviceFeatures supportedFeatures{};
    vkGetPhysicalDeviceFeatures(m_physicalDevice, &supportedFeatures);
    VkPhysicalDeviceFeatures features{};
    features.samplerAnisotropy = supportedFeatures.samplerAnisotropy;
    // VK_POLYGON_MODE_LINE requires fillModeNonSolid to be enabled when the
    // Scene viewport creates its wireframe graphics pipelines.
    features.fillModeNonSolid = supportedFeatures.fillModeNonSolid;
    features.tessellationShader = supportedFeatures.tessellationShader;
    features.imageCubeArray = supportedFeatures.imageCubeArray;
    features.shaderClipDistance = supportedFeatures.shaderClipDistance;
    m_samplerAnisotropySupported = supportedFeatures.samplerAnisotropy == VK_TRUE;
    const std::set<std::string> availableExtensions =
        EnumerateDeviceExtensions(m_physicalDevice);
    const bool accelerationStructureExtensionsAvailable =
        std::all_of(
            AccelerationStructureDeviceExtensions.begin(),
            AccelerationStructureDeviceExtensions.end(),
            [&](const char* extension)
            {
                return availableExtensions.contains(
                    extension);
            });
    const bool rayTracingPipelineExtensionAvailable =
        availableExtensions.contains(
            VK_KHR_RAY_TRACING_PIPELINE_EXTENSION_NAME);
    const bool rayQueryExtensionAvailable =
        availableExtensions.contains(
            VK_KHR_RAY_QUERY_EXTENSION_NAME);

    VkPhysicalDeviceVulkan12Features supportedVulkan12{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    VkPhysicalDeviceVulkan13Features supportedVulkan13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    VkPhysicalDeviceAccelerationStructureFeaturesKHR
        supportedAccelerationStructure{
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR};
    VkPhysicalDeviceRayTracingPipelineFeaturesKHR
        supportedRayTracingPipeline{
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_FEATURES_KHR};
    VkPhysicalDeviceRayQueryFeaturesKHR
        supportedRayQuery{
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_QUERY_FEATURES_KHR};
    supportedVulkan12.pNext = &supportedVulkan13;
    if (accelerationStructureExtensionsAvailable)
    {
        supportedVulkan13.pNext =
            &supportedAccelerationStructure;
        if (rayTracingPipelineExtensionAvailable)
        {
            supportedAccelerationStructure.pNext =
                &supportedRayTracingPipeline;
            if (rayQueryExtensionAvailable)
            {
                supportedRayTracingPipeline.pNext =
                    &supportedRayQuery;
            }
        }
        else if (rayQueryExtensionAvailable)
        {
            supportedAccelerationStructure.pNext =
                &supportedRayQuery;
        }
    }
    VkPhysicalDeviceFeatures2 supportedFeatures2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    VkPhysicalDeviceVulkan11Features supportedVulkan11{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES};
    supportedVulkan11.pNext = &supportedVulkan12;
    supportedFeatures2.pNext = &supportedVulkan11;
    vkGetPhysicalDeviceFeatures2(m_physicalDevice, &supportedFeatures2);
    Core::Check(supportedVulkan13.dynamicRendering == VK_TRUE,
                "The selected Vulkan 1.3 device does not support dynamic rendering.");
    Core::Check(
        supportedVulkan12.timelineSemaphore == VK_TRUE,
        "The selected Vulkan device does not support timeline semaphores.");
    m_accelerationStructureSupported =
        accelerationStructureExtensionsAvailable
        && supportedVulkan12.bufferDeviceAddress
            == VK_TRUE
        && supportedAccelerationStructure
               .accelerationStructure
            == VK_TRUE;
    m_rayTracingPipelineSupported =
        m_accelerationStructureSupported
        && rayTracingPipelineExtensionAvailable
        && supportedRayTracingPipeline
               .rayTracingPipeline
            == VK_TRUE;
    m_rayQuerySupported =
        m_accelerationStructureSupported
        && rayQueryExtensionAvailable
        && supportedRayQuery.rayQuery == VK_TRUE;

    VkPhysicalDeviceVulkan12Features vulkan12Features{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    vulkan12Features.timelineSemaphore = VK_TRUE;
    vulkan12Features.descriptorIndexing =
        supportedVulkan12.descriptorIndexing;
    vulkan12Features
        .shaderSampledImageArrayNonUniformIndexing =
        supportedVulkan12
            .shaderSampledImageArrayNonUniformIndexing;
    vulkan12Features.runtimeDescriptorArray =
        supportedVulkan12.runtimeDescriptorArray;
    vulkan12Features.descriptorBindingPartiallyBound =
        supportedVulkan12
            .descriptorBindingPartiallyBound;
    vulkan12Features
        .descriptorBindingVariableDescriptorCount =
        supportedVulkan12
            .descriptorBindingVariableDescriptorCount;
    vulkan12Features.bufferDeviceAddress =
        supportedVulkan12.bufferDeviceAddress;
    vulkan12Features.drawIndirectCount =
        supportedVulkan12.drawIndirectCount;
    VkPhysicalDeviceVulkan13Features vulkan13Features{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    vulkan13Features.dynamicRendering = VK_TRUE;
    vulkan13Features.maintenance4 = supportedVulkan13.maintenance4;
    vulkan12Features.pNext = &vulkan13Features;
    VkPhysicalDeviceAccelerationStructureFeaturesKHR
        accelerationStructureFeatures{
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR};
    VkPhysicalDeviceRayTracingPipelineFeaturesKHR
        rayTracingPipelineFeatures{
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_FEATURES_KHR};
    VkPhysicalDeviceRayQueryFeaturesKHR
        rayQueryFeatures{
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_QUERY_FEATURES_KHR};
    if (m_accelerationStructureSupported)
    {
        accelerationStructureFeatures
            .accelerationStructure = VK_TRUE;
        vulkan13Features.pNext =
            &accelerationStructureFeatures;
        if (m_rayTracingPipelineSupported)
        {
            rayTracingPipelineFeatures
                .rayTracingPipeline = VK_TRUE;
            accelerationStructureFeatures.pNext =
                &rayTracingPipelineFeatures;
            if (m_rayQuerySupported)
            {
                rayQueryFeatures.rayQuery = VK_TRUE;
                rayTracingPipelineFeatures.pNext =
                    &rayQueryFeatures;
            }
        }
        else if (m_rayQuerySupported)
        {
            rayQueryFeatures.rayQuery = VK_TRUE;
            accelerationStructureFeatures.pNext =
                &rayQueryFeatures;
        }
    }
    std::vector<const char*> enabledExtensions(
        RequiredDeviceExtensions.begin(),
        RequiredDeviceExtensions.end());
    if (m_accelerationStructureSupported)
    {
        enabledExtensions.insert(
            enabledExtensions.end(),
            AccelerationStructureDeviceExtensions.begin(),
            AccelerationStructureDeviceExtensions.end());
    }
    if (m_rayTracingPipelineSupported)
    {
        enabledExtensions.push_back(
            VK_KHR_RAY_TRACING_PIPELINE_EXTENSION_NAME);
    }
    if (m_rayQuerySupported)
    {
        enabledExtensions.push_back(
            VK_KHR_RAY_QUERY_EXTENSION_NAME);
    }
    VkDeviceCreateInfo createInfo{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    VkPhysicalDeviceVulkan11Features vulkan11Features{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES};
    vulkan11Features.shaderDrawParameters = supportedVulkan11.shaderDrawParameters;
    vulkan11Features.pNext = &vulkan12Features;
    createInfo.pNext = &vulkan11Features;
    createInfo.queueCreateInfoCount = static_cast<std::uint32_t>(queueCreateInfos.size());
    createInfo.pQueueCreateInfos = queueCreateInfos.data();
    createInfo.pEnabledFeatures = &features;
    createInfo.enabledExtensionCount =
        static_cast<std::uint32_t>(
            enabledExtensions.size());
    createInfo.ppEnabledExtensionNames =
        enabledExtensions.data();
    CheckVk(vkCreateDevice(m_physicalDevice, &createInfo, nullptr, &m_device), "Failed to create Vulkan logical device.");

    vkGetDeviceQueue(m_device, m_queueFamilies.graphics, 0, &m_graphicsQueue);
    vkGetDeviceQueue(m_device, m_queueFamilies.compute, 0, &m_computeQueue);
    vkGetDeviceQueue(m_device, m_queueFamilies.transfer, 0, &m_transferQueue);
    vkGetDeviceQueue(m_device, m_queueFamilies.present, 0, &m_presentQueue);

    VkSemaphoreTypeCreateInfo typeInfo{
        VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO};
    typeInfo.semaphoreType =
        VK_SEMAPHORE_TYPE_TIMELINE;
    typeInfo.initialValue = 0;
    VkSemaphoreCreateInfo semaphoreInfo{
        VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    semaphoreInfo.pNext = &typeInfo;
    CheckVk(
        vkCreateSemaphore(
            m_device,
            &semaphoreInfo,
            nullptr,
            &m_queueTimelineSemaphore),
        "Failed to create the Vulkan queue timeline semaphore.");
    CheckVk(
        vkCreateSemaphore(
            m_device,
            &semaphoreInfo,
            nullptr,
            &m_uploadTimelineSemaphore),
        "Failed to create the Vulkan upload timeline semaphore.");
    for (VkSemaphore& semaphore :
         m_batchTimelineSemaphores)
    {
        CheckVk(
            vkCreateSemaphore(
                m_device,
                &semaphoreInfo,
                nullptr,
                &semaphore),
            "Failed to create a Vulkan queue-batch timeline semaphore.");
    }
    BuildDeviceCapabilities();
}

void VulkanContext::BuildDeviceCapabilities()
{
    VkPhysicalDeviceRayTracingPipelinePropertiesKHR
        rayTracingProperties{
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_PROPERTIES_KHR};
    VkPhysicalDeviceAccelerationStructurePropertiesKHR
        accelerationStructureProperties{
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_PROPERTIES_KHR};
    VkPhysicalDeviceProperties2 properties2{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
    if (m_accelerationStructureSupported)
    {
        properties2.pNext =
            &accelerationStructureProperties;
        if (m_rayTracingPipelineSupported)
        {
            accelerationStructureProperties.pNext =
                &rayTracingProperties;
        }
    }
    vkGetPhysicalDeviceProperties2(
        m_physicalDevice,
        &properties2);
    const VkPhysicalDeviceProperties& properties =
        properties2.properties;
    VkPhysicalDeviceVulkan12Features vulkan12{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    VkPhysicalDeviceFeatures2 features{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    features.pNext = &vulkan12;
    vkGetPhysicalDeviceFeatures2(
        m_physicalDevice,
        &features);

    m_deviceCapabilities = {};
    m_deviceCapabilities.graphicsApi =
        GraphicsApi::Vulkan;
    m_deviceCapabilities.adapterName =
        m_adapterName;
    m_deviceCapabilities.limits = {
        properties.limits.maxImageDimension2D,
        properties.limits.maxImageArrayLayers,
        properties.limits.maxColorAttachments,
        static_cast<std::uint32_t>(
            std::max(
                1.0f,
                properties.limits
                    .maxSamplerAnisotropy)),
        properties.limits
            .minUniformBufferOffsetAlignment,
        properties.limits
            .minStorageBufferOffsetAlignment,
        properties.limits
            .maxDescriptorSetSampledImages
            + properties.limits
                  .maxDescriptorSetStorageImages
            + properties.limits
                  .maxDescriptorSetUniformBuffers
            + properties.limits
                  .maxDescriptorSetStorageBuffers,
        properties.limits
            .maxDescriptorSetSamplers,
        m_rayTracingPipelineSupported
            ? rayTracingProperties
                  .maxRayRecursionDepth
            : 0,
        m_rayTracingPipelineSupported
            ? rayTracingProperties
                  .shaderGroupHandleSize
            : 0,
        m_rayTracingPipelineSupported
            ? rayTracingProperties
                  .shaderGroupBaseAlignment
            : 0,
        m_accelerationStructureSupported
            ? accelerationStructureProperties
                  .minAccelerationStructureScratchOffsetAlignment
            : 0,
        properties.limits.maxTessellationPatchSize};
    m_deviceCapabilities.features = {
        true,
        m_computeQueue != VK_NULL_HANDLE,
        m_queueFamilies.compute
            != m_queueFamilies.graphics,
        m_transferQueue != VK_NULL_HANDLE,
        m_queueFamilies.transfer
            != m_queueFamilies.graphics
            && m_queueFamilies.transfer
                != m_queueFamilies.compute,
        vulkan12.timelineSemaphore == VK_TRUE,
        true,
        vulkan12.descriptorIndexing == VK_TRUE,
        vulkan12.bufferDeviceAddress == VK_TRUE,
        vulkan12.drawIndirectCount == VK_TRUE,
        features.features.samplerAnisotropy
            == VK_TRUE,
        true,
        m_accelerationStructureSupported,
        m_rayTracingPipelineSupported,
        m_rayQuerySupported};
    m_deviceCapabilities.features.tessellationShader =
        features.features.tessellationShader == VK_TRUE;
    m_deviceCapabilities.features.patchListTopology =
        m_deviceCapabilities.features.tessellationShader;
    m_deviceCapabilities.rayTracingTier =
        m_rayQuerySupported
        ? RayTracingTier::Tier1_1
        : m_accelerationStructureSupported
              && m_rayTracingPipelineSupported
        ? RayTracingTier::Tier1_0
        : RayTracingTier::Unsupported;
}

void VulkanContext::CreateCommandPool()
{
    VkCommandPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    poolInfo.queueFamilyIndex = m_queueFamilies.graphics;
    CheckVk(vkCreateCommandPool(m_device, &poolInfo, nullptr, &m_commandPool), "Failed to create Vulkan command pool.");
    poolInfo.queueFamilyIndex =
        m_queueFamilies.compute;
    CheckVk(
        vkCreateCommandPool(
            m_device,
            &poolInfo,
            nullptr,
            &m_computeCommandPool),
        "Failed to create Vulkan compute command pool.");
}

void VulkanContext::DestroyCaptureResources()
{
    if (m_device != VK_NULL_HANDLE && m_captureBuffer != VK_NULL_HANDLE)
    {
        vkDestroyBuffer(m_device, m_captureBuffer, nullptr);
    }
    if (m_device != VK_NULL_HANDLE && m_captureMemory != VK_NULL_HANDLE)
    {
        vkFreeMemory(m_device, m_captureMemory, nullptr);
    }
    m_captureBuffer = VK_NULL_HANDLE;
    m_captureMemory = VK_NULL_HANDLE;
    m_captureBufferSize = 0;
}

VulkanContext::QueueFamilyIndices VulkanContext::FindQueueFamilies(const VkPhysicalDevice device) const
{
    const VulkanQueueFamilyIndices indices =
        FindVulkanQueueFamilies(device, m_surface);
    return {
        indices.graphics,
        indices.present,
        indices.compute,
        indices.transfer};
}

bool VulkanContext::SupportsRequiredDeviceExtensions(const VkPhysicalDevice device) const
{
    return SupportsVulkanDeviceExtensions(
        device,
        RequiredDeviceExtensions);
}

VulkanContext::SwapChainSupport VulkanContext::QuerySwapChainSupport(const VkPhysicalDevice device) const
{
    VulkanSwapChainSupport support =
        QueryVulkanSwapChainSupport(
            device,
            m_surface);
    return {
        support.capabilities,
        std::move(support.formats),
        std::move(support.presentModes)};
}

VkSurfaceFormatKHR VulkanContext::ChooseSurfaceFormat(const std::vector<VkSurfaceFormatKHR>& formats) const
{
    return ChooseVulkanSurfaceFormat(formats);
}

VkPresentModeKHR VulkanContext::ChoosePresentMode(const std::vector<VkPresentModeKHR>& presentModes) const
{
    return Prism::RHI::Vulkan::ChooseVulkanPresentMode(
        presentModes, m_framePacingConfiguration.presentation);
}

VkExtent2D VulkanContext::ChooseExtent(const VkSurfaceCapabilitiesKHR& capabilities) const
{
    if (capabilities.currentExtent.width != std::numeric_limits<std::uint32_t>::max())
    {
        return capabilities.currentExtent;
    }
    VkExtent2D extent{m_window->GetWidth(), m_window->GetHeight()};
    extent.width = std::clamp(extent.width, capabilities.minImageExtent.width, capabilities.maxImageExtent.width);
    extent.height = std::clamp(extent.height, capabilities.minImageExtent.height, capabilities.maxImageExtent.height);
    return extent;
}

void VulkanContext::EnsureCaptureBuffer()
{
    const VkDeviceSize requiredSize = static_cast<VkDeviceSize>(m_captureExtent.width)
                                      * static_cast<VkDeviceSize>(m_captureExtent.height) * 4u;
    if (m_captureBuffer != VK_NULL_HANDLE && m_captureBufferSize == requiredSize)
    {
        return;
    }
    DestroyCaptureResources();
    CreateBuffer(
        requiredSize,
        VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
        m_captureBuffer,
        m_captureMemory);
    m_captureBufferSize = requiredSize;
}

void VulkanContext::RecordPresentTransition(const VkCommandBuffer commandBuffer, const bool captureFrame)
{
    const bool captureSwapchain = captureFrame && m_captureSourceImage == VK_NULL_HANDLE;
    VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    barrier.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    barrier.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    barrier.newLayout = captureSwapchain ? VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL : VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = m_swapChainImages[m_currentImage];
    barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    barrier.subresourceRange.levelCount = 1;
    barrier.subresourceRange.layerCount = 1;
    barrier.dstAccessMask = captureSwapchain ? VK_ACCESS_TRANSFER_READ_BIT : 0;
    vkCmdPipelineBarrier(
        commandBuffer,
        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
        captureSwapchain ? VK_PIPELINE_STAGE_TRANSFER_BIT : VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
        0,
        0,
        nullptr,
        0,
        nullptr,
        1,
        &barrier);

    if (!captureFrame)
    {
        return;
    }

    if (!captureSwapchain)
    {
        // The selected Game/Scene output has already returned to shader-read
        // after tonemapping and UI sampling. Restore it after the copy.
        barrier.image = m_captureSourceImage;
        barrier.oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        barrier.srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
        barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT, 0u, 0u, nullptr, 0u, nullptr, 1u, &barrier);
    }

    VkBufferImageCopy copyRegion{};
    copyRegion.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    copyRegion.imageSubresource.layerCount = 1;
    copyRegion.imageExtent = {m_captureExtent.width, m_captureExtent.height, 1};
    vkCmdCopyImageToBuffer(
        commandBuffer,
        captureSwapchain ? m_swapChainImages[m_currentImage] : m_captureSourceImage,
        VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        m_captureBuffer,
        1,
        &copyRegion);

    barrier.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    barrier.dstAccessMask = captureSwapchain ? 0u : VK_ACCESS_SHADER_READ_BIT;
    barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    barrier.newLayout = captureSwapchain ? VK_IMAGE_LAYOUT_PRESENT_SRC_KHR : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    vkCmdPipelineBarrier(
        commandBuffer,
        VK_PIPELINE_STAGE_TRANSFER_BIT,
        captureSwapchain ? VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT : VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
        0,
        0,
        nullptr,
        0,
        nullptr,
        1,
        &barrier);
}

void VulkanContext::SaveCapture()
{
    try
    {
        if (m_capturePath.has_parent_path())
        {
            std::filesystem::create_directories(m_capturePath.parent_path());
        }
        void* mappedData = nullptr;
        CheckVk(vkMapMemory(m_device, m_captureMemory, 0, m_captureBufferSize, 0, &mappedData), "Failed to map Vulkan capture buffer.");

        std::ofstream output(m_capturePath, std::ios::binary | std::ios::trunc);
        if (!output)
        {
            vkUnmapMemory(m_device, m_captureMemory);
            throw std::runtime_error("Failed to open Vulkan capture output: " + m_capturePath.string());
        }

        const std::uint32_t width = m_captureExtent.width;
        const std::uint32_t height = m_captureExtent.height;
        const std::uint32_t pixelBytes = width * height * 4u;
        output.put('B');
        output.put('M');
        WriteUint32(output, 54u + pixelBytes);
        WriteUint16(output, 0);
        WriteUint16(output, 0);
        WriteUint32(output, 54);
        WriteUint32(output, 40);
        WriteUint32(output, width);
        WriteUint32(output, height);
        WriteUint16(output, 1);
        WriteUint16(output, 32);
        WriteUint32(output, 0);
        WriteUint32(output, pixelBytes);
        WriteUint32(output, 2835);
        WriteUint32(output, 2835);
        WriteUint32(output, 0);
        WriteUint32(output, 0);

        const auto* source = static_cast<const std::uint8_t*>(mappedData);
        std::vector<std::uint8_t> row(static_cast<std::size_t>(width) * 4u);
        const bool sourceIsRgba = m_captureFormat == VK_FORMAT_R8G8B8A8_UNORM
                                  || m_captureFormat == VK_FORMAT_R8G8B8A8_SRGB;
        for (std::uint32_t rowIndex = 0; rowIndex < height; ++rowIndex)
        {
            const std::uint32_t sourceRow = height - 1u - rowIndex;
            const std::uint8_t* sourcePixels = source + static_cast<std::size_t>(sourceRow) * width * 4u;
            std::memcpy(row.data(), sourcePixels, row.size());
            if (sourceIsRgba)
            {
                for (std::uint32_t x = 0; x < width; ++x)
                {
                    std::swap(row[x * 4u], row[x * 4u + 2u]);
                }
            }
            output.write(reinterpret_cast<const char*>(row.data()), static_cast<std::streamsize>(row.size()));
        }
        vkUnmapMemory(m_device, m_captureMemory);
        if (!output)
        {
            throw std::runtime_error("Failed while writing Vulkan capture: " + m_capturePath.string());
        }
        m_captureError.clear();
    }
    catch (const std::exception& exception)
    {
        m_captureError = exception.what();
    }
    m_captureRequested = false;
    m_captureComplete = true;
}
} // namespace Prism::RHI::Vulkan
