#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include "RHI/GraphicsApi.h"
#include "RHI/FramePacing.h"
#include "RHI/D3D12/D3D12Presentation.h"
#include "RHI/GraphicsAdapterInfo.h"
#include "RHI/DeviceCapabilities.h"
#include "RHI/GraphicsTypes.h"
#include "RHI/ICommandContext.h"
#include "RHI/IFrameContext.h"

namespace Prism::Platform
{
class Window;
}

namespace Prism::RHI
{
class D3D12Context final : public IFrameContext
{
public:
    static constexpr std::uint32_t FrameCount = 2;
    static constexpr std::uint32_t
        ShaderVisibleSrvCapacity = 65'536;
    static constexpr std::uint32_t
        ShaderVisibleSamplerCapacity = 2'048;

    struct ShaderVisibleDescriptor
    {
        std::uint32_t index = 0;
        D3D12_CPU_DESCRIPTOR_HANDLE cpuHandle{};
        D3D12_GPU_DESCRIPTOR_HANDLE gpuHandle{};
    };

    struct UploadAllocation
    {
        Microsoft::WRL::ComPtr<ID3D12Resource>
            resource;
        std::uint64_t offset = 0;
        std::uint64_t size = 0;
        std::byte* cpuAddress = nullptr;
        std::uint32_t pageIndex = 0;
    };

    explicit D3D12Context(
        const FramePacingConfiguration& framePacing = {});
    ~D3D12Context();

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

    ID3D12Device* GetDevice() const;
    ID3D12CommandQueue* GetCommandQueue() const;
    ID3D12CommandQueue* GetComputeCommandQueue() const;
    ID3D12GraphicsCommandList* GetCommandList() const;
    ID3D12DescriptorHeap* GetShaderVisibleSrvHeap() const;
    ID3D12DescriptorHeap* GetShaderVisibleSamplerHeap() const;
    ID3D12CommandSignature* GetDrawIndexedCommandSignature(
        std::uint32_t stride);
    ShaderVisibleDescriptor AllocateShaderVisibleSrv();
    ShaderVisibleDescriptor AllocateShaderVisibleSrvRange(std::uint32_t descriptorCount);
    ShaderVisibleDescriptor AllocateShaderVisibleSamplerRange(std::uint32_t descriptorCount);
    void RetireShaderVisibleSrvRange(
        std::uint32_t firstDescriptor,
        std::uint32_t descriptorCount);
    void RetireShaderVisibleSamplerRange(
        std::uint32_t firstDescriptor,
        std::uint32_t descriptorCount);
    void QueueUpload(
        const UploadAllocation& allocation,
        const std::function<void(
            ID3D12GraphicsCommandList*)>&
            recordCopyCommands,
        const std::function<void(
            ID3D12GraphicsCommandList*)>&
            recordFinalizeCommands);
    UploadAllocation AllocateUpload(
        std::uint64_t byteCount,
        std::uint64_t alignment);
    void RetireResource(
        Microsoft::WRL::ComPtr<ID3D12Resource>
            resource,
        std::shared_ptr<void> allocationOwner = {});
    void ExecuteImmediate(const std::function<void(ID3D12GraphicsCommandList*)>& recordCommands);
    void ExecuteComputeImmediate(
        const std::function<void(
            ID3D12GraphicsCommandList*)>& recordCommands);
    bool SupportsNativeComputeQueue() const;
    bool IsComputeQueueValidated() const;
    CommandQueueType GetActiveCommandQueue() const;
    bool SwitchCommandQueue(CommandQueueType queue);
    bool BeginQueueBatch(
        CommandQueueType queue,
        std::span<const QueueSyncPoint> waits);
    QueueSyncPoint EndQueueBatch();
    bool FlushQueueBatches();
    bool ResumeGraphicsQueue(
        std::span<const QueueSyncPoint> waits);
    std::unique_ptr<IParallelCommandRecording>
        CreateParallelCommandRecording(
            CommandQueueType queue);
    bool AppendParallelCommandRecording(
        std::unique_ptr<IParallelCommandRecording>
            recording);
    D3D12_CPU_DESCRIPTOR_HANDLE GetCurrentRtvHandle() const;
    D3D12_CPU_DESCRIPTOR_HANDLE GetDsvHandle() const;
    ID3D12Resource* GetDepthStencilResource() const;
    D3D12_CPU_DESCRIPTOR_HANDLE GetImGuiCpuDescriptorHandle() const;
    D3D12_GPU_DESCRIPTOR_HANDLE GetImGuiGpuDescriptorHandle() const;
    DXGI_FORMAT GetBackBufferFormat() const;
    DXGI_FORMAT GetDepthFormat() const;
    std::uint32_t GetCurrentFrameIndex() const override;
    std::uint32_t GetFramesInFlight() const override;
    std::uint32_t GetFrameWidth() const override;
    std::uint32_t GetFrameHeight() const override;
    const std::shared_ptr<ITexture>&
        GetCurrentBackBufferTexture() const override;
    const ITextureView&
        GetCurrentBackBufferView() const override;
    Format GetBackBufferRhiFormat() const override;
    const std::shared_ptr<ITexture>&
        GetDepthStencilTexture() const override;
    const ITextureView&
        GetDepthStencilView() const override;
    const std::string& GetAdapterName() const;
    const GraphicsAdapterInfo& GetAdapterInfo() const;
    GraphicsApi GetGraphicsApi() const override;
    const GraphicsDeviceCapabilities&
        GetDeviceCapabilities() const;
    DescriptorAllocatorStatistics
        GetDescriptorAllocatorStatistics() const;
    UploadQueueStatistics
        GetUploadQueueStatistics() const;
    UploadTicket GetPendingUploadTicket() const;
    bool IsUploadComplete(
        UploadTicket ticket) const;
    ResourceRetirementStatistics
        GetResourceRetirementStatistics() const;

private:
    struct DescriptorRange
    {
        std::uint32_t first = 0;
        std::uint32_t count = 0;
    };

    struct FrameContext
    {
        Microsoft::WRL::ComPtr<ID3D12CommandAllocator> commandAllocator;
        std::vector<Microsoft::WRL::ComPtr<ID3D12CommandAllocator>>
            retainedCommandAllocators;
        std::vector<Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList>>
            retainedCommandLists;
        std::vector<DescriptorRange>
            retiredSrvDescriptors;
        std::vector<DescriptorRange>
            retiredSamplerDescriptors;
        std::vector<Microsoft::WRL::ComPtr<
            ID3D12Resource>>
            retainedUploadResources;
        // Placed resources must be released before their backing heaps. Keep
        // owners before resources so reverse member destruction preserves
        // that order even during context shutdown.
        std::vector<std::shared_ptr<void>>
            retiredAllocationOwners;
        std::vector<Microsoft::WRL::ComPtr<
            ID3D12Resource>>
            retiredResources;
        std::uint64_t fenceValue = 0;
    };

    struct PendingQueueBatch
    {
        std::vector<Microsoft::WRL::ComPtr<
            ID3D12CommandAllocator>> commandAllocators;
        std::vector<Microsoft::WRL::ComPtr<
            ID3D12GraphicsCommandList>> commandLists;
        CommandQueueType queue =
            CommandQueueType::Graphics;
        std::vector<QueueSyncPoint> waits;
        QueueSyncPoint signal;
    };

    struct UploadPage
    {
        Microsoft::WRL::ComPtr<ID3D12Resource>
            resource;
        std::byte* cpuAddress = nullptr;
        std::uint64_t capacity = 0;
        std::uint64_t cursor = 0;
        std::uint64_t lastTicket = 0;
        bool pending = false;
    };

    void EnableDebugLayer() const;
    void CreateFactory();
    void PickAdapter();
    void CreateDevice();
    void CreateCommandObjects();
    void CreateSwapChain();
    void CreateDescriptorHeaps();
    void CreateRenderTargets();
    void CreateDepthStencil();
    void CreateFence();
    void BuildDeviceCapabilities();
    ShaderVisibleDescriptor AllocateShaderVisibleRange(
        std::vector<DescriptorRange>& freeRanges,
        std::uint32_t descriptorCount,
        bool sampler);
    void ReclaimDescriptorRanges(FrameContext& frameContext);
    static void InsertAndCoalesceDescriptorRange(
        std::vector<DescriptorRange>& freeRanges,
        DescriptorRange range);
    void FlushPendingUploads(bool waitForCompletion);
    void ReleaseSizeDependentResources();
    void TransitionCurrentBackBuffer(ResourceState before, ResourceState after);
    void WaitForFrame(std::uint32_t frameIndex);
    void RetainActiveCommandList(FrameContext& frameContext);
    void CreateActiveCommandList(D3D12_COMMAND_LIST_TYPE type);
    void CloseActiveBatchSegment();
    void ApplyQueueWaits(
        CommandQueueType queue,
        std::span<const QueueSyncPoint> waits);

    Platform::Window* m_window = nullptr;
    std::uint32_t m_width = 0;
    std::uint32_t m_height = 0;
    std::uint32_t m_currentFrameIndex = 0;
    std::uint64_t m_nextFenceValue = 1;
    std::uint64_t m_totalSubmittedFrames = 0;
    std::string m_adapterName;
    GraphicsAdapterInfo m_adapterInfo;
    GraphicsDeviceCapabilities m_deviceCapabilities;

    Microsoft::WRL::ComPtr<IDXGIFactory6> m_factory;
    Microsoft::WRL::ComPtr<IDXGIAdapter4> m_adapter;
    Microsoft::WRL::ComPtr<ID3D12Device> m_device;
    Microsoft::WRL::ComPtr<ID3D12CommandQueue> m_commandQueue;
    Microsoft::WRL::ComPtr<ID3D12CommandQueue> m_computeCommandQueue;
    Microsoft::WRL::ComPtr<ID3D12CommandQueue> m_copyCommandQueue;
    Microsoft::WRL::ComPtr<IDXGISwapChain4> m_swapChain;
    HANDLE m_frameLatencyWaitableObject = nullptr;
    FramePacingConfiguration m_framePacingConfiguration;
    FramePacingState m_framePacingState;
    D3D12::D3D12PresentationPlan m_presentationPlan;
    bool m_tearingSupported = false;
    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> m_commandList;
    Microsoft::WRL::ComPtr<ID3D12CommandAllocator>
        m_activeTransientCommandAllocator;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> m_rtvHeap;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> m_dsvHeap;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> m_srvHeap;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> m_samplerHeap;
    Microsoft::WRL::ComPtr<ID3D12Fence> m_fence;
    Microsoft::WRL::ComPtr<ID3D12Fence> m_computeFence;
    Microsoft::WRL::ComPtr<ID3D12Fence> m_copyFence;
    Microsoft::WRL::ComPtr<ID3D12Fence> m_uploadFence;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_depthStencil;
    std::shared_ptr<ITexture> m_depthStencilTexture;
    std::shared_ptr<ITextureView> m_depthStencilView;
    std::array<Microsoft::WRL::ComPtr<ID3D12Resource>, FrameCount> m_renderTargets;
    std::array<std::shared_ptr<ITexture>, FrameCount>
        m_backBufferTextures;
    std::array<std::shared_ptr<ITextureView>, FrameCount>
        m_backBufferViews;
    std::array<FrameContext, FrameCount> m_frameContexts;
    HANDLE m_fenceEvent = nullptr;
    HANDLE m_computeFenceEvent = nullptr;
    HANDLE m_uploadFenceEvent = nullptr;
    std::uint64_t m_nextComputeFenceValue = 1;
    bool m_computeQueueValidated = false;
    CommandQueueType m_activeCommandQueue =
        CommandQueueType::Graphics;
    bool m_queueBatchExecutionActive = false;
    bool m_queueBatchOpen = false;
    std::vector<QueueSyncPoint> m_activeBatchWaits;
    std::vector<Microsoft::WRL::ComPtr<
        ID3D12CommandAllocator>>
        m_activeBatchCommandAllocators;
    std::vector<Microsoft::WRL::ComPtr<
        ID3D12GraphicsCommandList>>
        m_activeBatchCommandLists;
    std::vector<PendingQueueBatch>
        m_pendingQueueBatches;
    std::uint32_t m_rtvDescriptorSize = 0;
    std::uint32_t m_srvDescriptorSize = 0;
    std::uint32_t m_samplerDescriptorSize = 0;
    mutable std::mutex m_descriptorAllocatorMutex;
    std::vector<DescriptorRange> m_freeSrvDescriptors;
    std::vector<DescriptorRange> m_freeSamplerDescriptors;
    DescriptorAllocatorStatistics m_descriptorAllocatorStatistics;
    mutable std::mutex m_uploadMutex;
    Microsoft::WRL::ComPtr<ID3D12CommandAllocator>
        m_pendingUploadAllocator;
    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList>
        m_pendingUploadCommandList;
    Microsoft::WRL::ComPtr<ID3D12CommandAllocator>
        m_pendingUploadFinalizeAllocator;
    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList>
        m_pendingUploadFinalizeCommandList;
    std::vector<Microsoft::WRL::ComPtr<
        ID3D12Resource>>
        m_pendingUploadResources;
    std::vector<UploadPage> m_uploadPages;
    UploadTicket m_pendingUploadTicket;
    std::uint64_t m_nextUploadTicket = 1;
    UploadQueueStatistics m_uploadQueueStatistics;
    mutable std::mutex m_resourceRetirementMutex;
    ResourceRetirementStatistics
        m_resourceRetirementStatistics;
    D3D12_VIEWPORT m_viewport{};
    D3D12_RECT m_scissorRect{};
    DXGI_FORMAT m_backBufferFormat = DXGI_FORMAT_UNKNOWN;
    DXGI_FORMAT m_depthFormat = DXGI_FORMAT_UNKNOWN;
    std::mutex m_commandSignatureMutex;
    std::unordered_map<
        std::uint32_t,
        Microsoft::WRL::ComPtr<
            ID3D12CommandSignature>>
        m_drawIndexedCommandSignatures;
};
} // namespace Prism::RHI
