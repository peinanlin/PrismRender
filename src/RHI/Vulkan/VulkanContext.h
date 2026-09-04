#pragma once

#include "RHI/GraphicsApi.h"
#include "RHI/FramePacing.h"
#include "RHI/GraphicsAdapterInfo.h"
#include "RHI/IGraphicsDevice.h"
#include "RHI/ICommandContext.h"
#include "RHI/IFrameContext.h"
#include "RHI/Vulkan/VulkanLoader.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace Prism::Platform
{
class Window;
}

namespace Prism::RHI::Vulkan
{
class VulkanDescriptorAllocator;
class VulkanParallelCommandRecording;

class VulkanContext final
    : public IGraphicsDevice,
      public ICommandContext,
      public IFrameContext
{
public:
    struct UploadAllocation
    {
        VkBuffer buffer = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        VkDeviceSize offset = 0;
        VkDeviceSize size = 0;
        std::byte* cpuAddress = nullptr;
        std::uint32_t pageIndex = 0;
    };

    static constexpr std::uint32_t FrameCount = 2;

    explicit VulkanContext(
        const FramePacingConfiguration& framePacing = {});
    ~VulkanContext();

    VulkanContext(const VulkanContext&) = delete;
    VulkanContext& operator=(const VulkanContext&) = delete;

    void Initialize(Platform::Window& window);
    void Resize(std::uint32_t width, std::uint32_t height) override;
    FrameResult BeginFrame() override;
    FrameResult EndFrame() override;
    void WaitForGpu() override;
    FrameAdmissionResult WaitForFrameAdmission();
    bool ApplyFramePacingConfiguration(
        const FramePacingConfiguration& configuration,
        std::uint64_t generation,
        std::string* outErrorMessage = nullptr);
    const FramePacingState& GetFramePacingState() const noexcept;

    std::shared_ptr<IBuffer> CreateBuffer(
        const BufferDescription& description,
        const void* initialData = nullptr) override;
    std::shared_ptr<ITexture> CreateTexture(
        const TextureDescription& description,
        const TextureInitialData* initialData = nullptr) override;
    std::shared_ptr<ITransientTexturePool>
        CreateTransientTexturePool(
            const std::vector<TransientTextureRequest>&
                requests) override;
    std::shared_ptr<ITransientBufferPool>
        CreateTransientBufferPool(
            const std::vector<TransientBufferRequest>&
                requests) override;
    std::shared_ptr<ITextureView> CreateTextureView(
        std::shared_ptr<ITexture> texture,
        const TextureViewDescription& description) override;
    std::shared_ptr<ISampler> CreateSampler(const SamplerDescription& description) override;
    std::shared_ptr<IDescriptorSetLayout> CreateDescriptorSetLayout(
        const DescriptorSetLayoutDescription& description) override;
    std::shared_ptr<IDescriptorSet> CreateDescriptorSet(
        std::shared_ptr<IDescriptorSetLayout> layout) override;
    std::shared_ptr<IGraphicsPipeline> CreateGraphicsPipeline(
        const GraphicsPipelineDescription& description) override;
    std::shared_ptr<IComputePipeline> CreateComputePipeline(
        const ComputePipelineDescription& description) override;
    AccelerationStructureBuildSizes
        QueryAccelerationStructureBuildSizes(
            const AccelerationStructureBuildDescription&
                description) const override;
    std::shared_ptr<
        IRayTracingAccelerationStructure>
        CreateAccelerationStructure(
            const AccelerationStructureBuildRequest&
                request) override;

    void BeginRendering(const RenderingInfo& renderingInfo) override;
    void EndRendering() override;
    void BindGraphicsPipeline(const IGraphicsPipeline& pipeline) override;
    void BindComputePipeline(const IComputePipeline& pipeline) override;
    void BindVertexBuffer(const IBuffer& buffer, std::uint32_t slot = 0) override;
    void BindIndexBuffer(const IBuffer& buffer, IndexFormat format) override;
    void BindDescriptorSet(
        const IDescriptorSet& descriptorSet,
        std::span<const DynamicBufferOffset> dynamicOffsets = {}) override;
    void DrawIndexed(
        std::uint32_t indexCount,
        std::uint32_t instanceCount = 1,
        std::uint32_t firstIndex = 0,
        std::int32_t vertexOffset = 0,
        std::uint32_t firstInstance = 0) override;
    void DrawIndexedIndirect(
        const IBuffer& argumentBuffer,
        std::size_t argumentOffset = 0,
        std::uint32_t maxDrawCount = 1,
        std::uint32_t stride =
            sizeof(DrawIndexedIndirectArguments),
        const IBuffer* countBuffer = nullptr,
        std::size_t countOffset = 0) override;
    void Draw(
        std::uint32_t vertexCount,
        std::uint32_t instanceCount = 1,
        std::uint32_t firstVertex = 0,
        std::uint32_t firstInstance = 0) override;
    void Dispatch(
        std::uint32_t groupCountX,
        std::uint32_t groupCountY = 1,
        std::uint32_t groupCountZ = 1) override;
    void CopyBuffer(
        const IBuffer& source,
        IBuffer& destination,
        std::size_t size,
        std::size_t sourceOffset = 0,
        std::size_t destinationOffset = 0) override;
    void TextureBarrier(const RHI::TextureBarrier& barrier) override;
    void BufferBarrier(
        const RHI::BufferBarrier& barrier) override;
    void GlobalBarrier(
        const RHI::GlobalBarrier& barrier) override;
    void TextureViewBarrier(
        const ITextureView& textureView,
        ResourceState before,
        ResourceState after) override;
    void TextureAliasingBarrier(
        const ITexture* before,
        ITexture& after) override;
    void BufferAliasingBarrier(
        const IBuffer* before,
        IBuffer& after) override;

    void ExecuteImmediate(
        const std::function<void(VkCommandBuffer)>&
            recordCommands);
    void ExecuteComputeImmediate(
        const std::function<void(VkCommandBuffer)>&
            recordCommands);
    void BindGraphicsPipeline(VkPipeline pipeline, VkPipelineLayout layout);

    void RequestCapture(const std::filesystem::path& outputPath);
    void SetCaptureTexture(const ITexture* texture);
    bool IsCaptureComplete() const;
    const std::string& GetCaptureError() const;

    void CreateBuffer(
        VkDeviceSize size,
        VkBufferUsageFlags usage,
        VkMemoryPropertyFlags memoryProperties,
        VkBuffer& outBuffer,
        VkDeviceMemory& outMemory,
        bool enableDeviceAddress = false) const;
    void QueueUpload(
        const UploadAllocation& allocation,
        const std::function<void(VkCommandBuffer)>&
            recordCommands);
    UploadAllocation AllocateUpload(
        std::uint64_t byteCount,
        std::uint64_t alignment);
    void RetireGpuObject(
        std::function<void(VkDevice)> destroy);
    std::uint32_t FindMemoryType(std::uint32_t typeFilter, VkMemoryPropertyFlags properties) const;

    VkInstance GetInstance() const;
    VkPhysicalDevice GetPhysicalDevice() const;
    VkDevice GetDevice() const;
    VkQueue GetGraphicsQueue() const;
    VkQueue GetComputeQueue() const;
    VkSemaphore GetQueueTimelineSemaphore() const;
    VkCommandBuffer GetCommandBuffer() const;
    VkRenderPass GetRenderPass() const;
    VkExtent2D GetSwapChainExtent() const;
    std::uint32_t GetSwapChainImageCount() const;
    VkFormat GetSwapChainFormat() const;
    Format GetSwapChainRhiFormat() const;
    Format GetBackBufferRhiFormat() const override;
    const std::shared_ptr<ITexture>&
        GetCurrentBackBufferTexture() const override;
    const ITextureView&
        GetCurrentBackBufferView() const override;
    const ITextureView& GetDepthStencilView() const override;
    const std::shared_ptr<ITexture>&
        GetDepthStencilTexture() const override;
    std::uint32_t GetCurrentFrameIndex() const override;
    std::uint32_t GetFramesInFlight() const override;
    std::uint32_t GetFrameWidth() const override;
    std::uint32_t GetFrameHeight() const override;
    const std::string& GetAdapterName() const;
    const GraphicsAdapterInfo& GetAdapterInfo() const;
    bool SupportsSamplerAnisotropy() const;
    GraphicsApi GetGraphicsApi() const override;
    ShaderBinaryFormat
        GetPreferredShaderBinaryFormat() const override
    {
        return ShaderBinaryFormat::SpirV;
    }
    const GraphicsDeviceCapabilities&
        GetCapabilities() const override;
    DescriptorAllocatorStatistics
        GetDescriptorAllocatorStatistics() const override;
    UploadQueueStatistics
        GetUploadQueueStatistics() const override;
    UploadTicket GetPendingUploadTicket() const override;
    bool IsUploadComplete(
        UploadTicket ticket) const override;
    ResourceRetirementStatistics
        GetResourceRetirementStatistics() const override;
    CommandQueueCapabilities
        GetQueueCapabilities() const override;
    CommandQueueType GetActiveCommandQueue() const override;
    bool SwitchCommandQueue(CommandQueueType queue) override;
    bool BeginQueueBatch(
        CommandQueueType queue,
        std::span<const QueueSyncPoint> waits) override;
    QueueSyncPoint EndQueueBatch() override;
    bool FlushQueueBatches() override;
    bool ResumeGraphicsQueue(
        std::span<const QueueSyncPoint> waits) override;
    std::unique_ptr<IParallelCommandRecording>
        CreateParallelCommandRecording(
            CommandQueueType queue) override;
    bool AppendParallelCommandRecording(
        std::unique_ptr<IParallelCommandRecording>
            recording) override;
    std::uint32_t GetGraphicsQueueFamilyIndex() const;
    std::uint32_t GetComputeQueueFamilyIndex() const;
    std::uint32_t GetTransferQueueFamilyIndex() const;

private:
    struct QueueFamilyIndices
    {
        std::uint32_t graphics = UINT32_MAX;
        std::uint32_t present = UINT32_MAX;
        std::uint32_t compute = UINT32_MAX;
        std::uint32_t transfer = UINT32_MAX;

        bool IsComplete() const;
    };

    struct SwapChainSupport
    {
        VkSurfaceCapabilitiesKHR capabilities{};
        std::vector<VkSurfaceFormatKHR> formats;
        std::vector<VkPresentModeKHR> presentModes;

        bool IsAdequate() const;
    };

    struct RetiredCommandBuffer
    {
        VkCommandPool pool = VK_NULL_HANDLE;
        VkCommandBuffer commandBuffer = VK_NULL_HANDLE;
    };

    struct RetiredUpload
    {
        VkBuffer buffer = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
    };

    struct UploadPage
    {
        VkBuffer buffer = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        std::byte* cpuAddress = nullptr;
        VkDeviceSize capacity = 0;
        VkDeviceSize cursor = 0;
        std::uint64_t lastTicket = 0;
        bool pending = false;
    };

    struct FrameContext
    {
        VkCommandBuffer commandBuffer = VK_NULL_HANDLE;
        VkSemaphore imageAvailable = VK_NULL_HANDLE;
        VkFence inFlight = VK_NULL_HANDLE;
        std::vector<RetiredCommandBuffer>
            retiredCommandBuffers;
        std::vector<VkCommandPool>
            retiredCommandPools;
        std::vector<RetiredUpload>
            retiredUploads;
        std::vector<std::function<void(VkDevice)>>
            retiredGpuObjects;
        std::uint64_t pendingTimelineWaitValue = 0;
        std::uint64_t pendingUploadWaitValue = 0;
        std::array<std::uint64_t, 2>
            pendingBatchWaitValues{};
        bool imageAvailableConsumed = false;
    };

    struct PendingQueueBatch
    {
        std::vector<VkCommandBuffer>
            commandBuffers;
        CommandQueueType queue =
            CommandQueueType::Graphics;
        std::vector<QueueSyncPoint> waits;
        QueueSyncPoint signal;
    };

    void CreateInstance();
    void CreateSurface();
    void PickPhysicalDevice();
    void CreateLogicalDevice();
    void CreateCommandPool();
    void FlushPendingUploads(bool waitForCompletion);
    void CreateSwapChainResources();
    void CreateSwapChain();
    void CreateImageViews();
    void CreateRenderPass();
    void CreateFramebuffers();
    void CreateFrameResources();
    void BuildDeviceCapabilities();
    void DestroySwapChainResources();
    void DestroyCaptureResources();
    VkCommandBuffer AllocateAndBeginCommandBuffer(
        VkCommandPool pool) const;
    void RetireCurrentCommandBuffer(
        FrameContext& frame,
        VkCommandPool pool);
    void CloseActiveBatchSegment();

    QueueFamilyIndices FindQueueFamilies(VkPhysicalDevice device) const;
    bool SupportsRequiredDeviceExtensions(VkPhysicalDevice device) const;
    SwapChainSupport QuerySwapChainSupport(VkPhysicalDevice device) const;
    VkSurfaceFormatKHR ChooseSurfaceFormat(const std::vector<VkSurfaceFormatKHR>& formats) const;
    VkPresentModeKHR ChoosePresentMode(const std::vector<VkPresentModeKHR>& presentModes) const;
    VkExtent2D ChooseExtent(const VkSurfaceCapabilitiesKHR& capabilities) const;
    void EnsureCaptureBuffer();
    void RecordPresentTransition(VkCommandBuffer commandBuffer, bool captureFrame);
    void TransitionTextureView(
        const ITextureView& textureView,
        ResourceState before,
        ResourceState after);
    void SaveCapture();

    struct PendingAttachmentTransition
    {
        const ITextureView* view = nullptr;
        ResourceState stateBefore = ResourceState::Undefined;
        ResourceState stateAfter = ResourceState::Undefined;
    };

    Platform::Window* m_window = nullptr;
    VkInstance m_instance = VK_NULL_HANDLE;
    VkDebugReportCallbackEXT m_debugMessenger = VK_NULL_HANDLE;
    VkSurfaceKHR m_surface = VK_NULL_HANDLE;
    VkPhysicalDevice m_physicalDevice = VK_NULL_HANDLE;
    VkDevice m_device = VK_NULL_HANDLE;
    VkQueue m_graphicsQueue = VK_NULL_HANDLE;
    VkQueue m_computeQueue = VK_NULL_HANDLE;
    VkQueue m_transferQueue = VK_NULL_HANDLE;
    VkQueue m_presentQueue = VK_NULL_HANDLE;
    VkSwapchainKHR m_swapChain = VK_NULL_HANDLE;
    VkRenderPass m_renderPass = VK_NULL_HANDLE;
    VkCommandPool m_commandPool = VK_NULL_HANDLE;
    VkCommandPool m_computeCommandPool = VK_NULL_HANDLE;
    mutable std::mutex m_uploadMutex;
    VkCommandPool m_pendingUploadCommandPool =
        VK_NULL_HANDLE;
    VkCommandBuffer m_pendingUploadCommandBuffer =
        VK_NULL_HANDLE;
    std::vector<RetiredUpload> m_pendingUploads;
    std::vector<UploadPage> m_uploadPages;
    UploadTicket m_pendingUploadTicket;
    std::uint64_t m_nextUploadTicket = 1;
    UploadQueueStatistics m_uploadQueueStatistics;
    mutable std::mutex m_resourceRetirementMutex;
    ResourceRetirementStatistics
        m_resourceRetirementStatistics;
    VkSemaphore m_queueTimelineSemaphore =
        VK_NULL_HANDLE;
    VkSemaphore m_uploadTimelineSemaphore =
        VK_NULL_HANDLE;
    std::uint64_t m_nextQueueTimelineValue = 1;
    std::array<VkSemaphore, 2>
        m_batchTimelineSemaphores{};
    std::array<std::uint64_t, 2>
        m_nextBatchTimelineValues{1, 1};
    bool m_computeQueueValidated = false;
    CommandQueueType m_activeCommandQueue =
        CommandQueueType::Graphics;
    std::uint64_t m_activeQueueWaitValue = 0;
    std::vector<QueueSyncPoint> m_activeBatchWaits;
    std::vector<VkCommandBuffer>
        m_activeBatchCommandBuffers;
    std::vector<PendingQueueBatch>
        m_pendingQueueBatches;
    bool m_queueBatchExecutionActive = false;
    bool m_queueBatchOpen = false;
    VkFormat m_swapChainFormat = VK_FORMAT_UNDEFINED;
    VkExtent2D m_swapChainExtent{};
    VkPresentModeKHR m_swapChainPresentMode = VK_PRESENT_MODE_FIFO_KHR;
    FramePacingConfiguration m_framePacingConfiguration;
    FramePacingState m_framePacingState;
    QueueFamilyIndices m_queueFamilies{};
    std::vector<VkImage> m_swapChainImages;
    // Presentation completion is tied to reacquiring an image, not a frame fence.
    std::vector<VkSemaphore> m_presentSemaphores;
    std::vector<VkImageView> m_swapChainImageViews;
    std::vector<std::shared_ptr<ITexture>>
        m_swapChainTextures;
    std::vector<std::shared_ptr<ITextureView>> m_swapChainTextureViews;
    std::vector<VkFramebuffer> m_swapChainFramebuffers;
    std::shared_ptr<ITexture> m_depthTexture;
    std::shared_ptr<ITextureView> m_depthTextureView;
    std::vector<VkFence> m_imageInFlightFences;
    std::array<FrameContext, FrameCount> m_frames{};
    std::uint32_t m_currentFrame = 0;
    std::uint32_t m_currentImage = 0;
    std::uint64_t m_totalSubmittedFrames = 0;
    bool m_frameInProgress = false;
    bool m_renderingInProgress = false;
    bool m_swapChainSuboptimal = false;
    bool m_swapChainSupportsTransferSource = false;
    bool m_samplerAnisotropySupported = false;
    bool m_accelerationStructureSupported = false;
    bool m_rayTracingPipelineSupported = false;
    bool m_rayQuerySupported = false;
    std::string m_adapterName;
    GraphicsAdapterInfo m_adapterInfo;
    GraphicsDeviceCapabilities m_deviceCapabilities;
    std::unique_ptr<VulkanDescriptorAllocator> m_descriptorAllocator;
    VkPipelineLayout m_activePipelineLayout = VK_NULL_HANDLE;
    VkPipelineBindPoint m_activePipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    std::vector<PendingAttachmentTransition> m_pendingAttachmentTransitions;

    std::filesystem::path m_capturePath;
    VkBuffer m_captureBuffer = VK_NULL_HANDLE;
    VkDeviceMemory m_captureMemory = VK_NULL_HANDLE;
    VkDeviceSize m_captureBufferSize = 0;
    VkImage m_captureSourceImage = VK_NULL_HANDLE;
    VkExtent2D m_captureExtent{};
    VkFormat m_captureFormat = VK_FORMAT_UNDEFINED;
    bool m_captureRequested = false;
    bool m_captureComplete = false;
    std::string m_captureError;

    friend class VulkanParallelCommandRecording;
};
} // namespace Prism::RHI::Vulkan
