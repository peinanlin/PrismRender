#include "RHI/D3D12/D3D12Context.h"

#include "Core/Assert.h"
#include "Core/Environment.h"
#include "Platform/Window.h"
#include "RHI/D3D12/D3D12CommandContextAdapter.h"
#include "RHI/D3D12/D3D12Resources.h"
#include "RHI/D3D12/D3D12TypeConversions.h"

#include <algorithm>
#include <iostream>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace Prism::RHI
{
namespace
{
constexpr DXGI_USAGE SwapChainUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
constexpr std::uint64_t UploadPageSize =
    16ull * 1024ull * 1024ull;

std::uint64_t AlignUp(
    const std::uint64_t value,
    const std::uint64_t alignment)
{
    Core::Check(
        alignment > 0,
        "Upload allocation alignment must be positive.");
    return ((value + alignment - 1) / alignment)
        * alignment;
}

D3D12_CPU_DESCRIPTOR_HANDLE OffsetCpuHandle(
    const D3D12_CPU_DESCRIPTOR_HANDLE start,
    const std::uint32_t index,
    const std::uint32_t increment)
{
    D3D12_CPU_DESCRIPTOR_HANDLE handle = start;
    handle.ptr += static_cast<SIZE_T>(index) * increment;
    return handle;
}

class D3D12ParallelCommandRecording final
    : public IParallelCommandRecording
{
public:
    D3D12ParallelCommandRecording(
        D3D12Context& context,
        const CommandQueueType queue)
        : m_queue(queue)
    {
        const D3D12_COMMAND_LIST_TYPE type =
            queue == CommandQueueType::Compute
            ? D3D12_COMMAND_LIST_TYPE_COMPUTE
            : D3D12_COMMAND_LIST_TYPE_DIRECT;
        Core::ThrowIfFailed(
            context.GetDevice()
                ->CreateCommandAllocator(
                    type,
                    IID_PPV_ARGS(&m_allocator)),
            "Failed to create a parallel D3D12 command allocator.");
        Core::ThrowIfFailed(
            context.GetDevice()->CreateCommandList(
                0,
                type,
                m_allocator.Get(),
                nullptr,
                IID_PPV_ARGS(&m_commandList)),
            "Failed to create a parallel D3D12 command list.");
        m_context = std::make_unique<
            D3D12::D3D12CommandContextAdapter>(
            context,
            m_commandList.Get(),
            queue);
    }

    ICommandContext& GetCommandContext() override
    {
        return *m_context;
    }

    bool Close() override
    {
        if (m_closed)
        {
            return true;
        }
        m_closed = SUCCEEDED(m_commandList->Close());
        return m_closed;
    }

    [[nodiscard]] bool IsClosed() const
    {
        return m_closed;
    }

    [[nodiscard]] CommandQueueType GetQueue() const
    {
        return m_queue;
    }

    ComPtr<ID3D12CommandAllocator> TakeAllocator()
    {
        return std::move(m_allocator);
    }

    ComPtr<ID3D12GraphicsCommandList> TakeCommandList()
    {
        return std::move(m_commandList);
    }

private:
    CommandQueueType m_queue =
        CommandQueueType::Graphics;
    ComPtr<ID3D12CommandAllocator> m_allocator;
    ComPtr<ID3D12GraphicsCommandList> m_commandList;
    std::unique_ptr<
        D3D12::D3D12CommandContextAdapter> m_context;
    bool m_closed = false;
};
} // namespace

D3D12Context::D3D12Context(
    const FramePacingConfiguration& framePacing)
    : m_framePacingConfiguration(framePacing),
      m_backBufferFormat(D3D12::ToNativeFormat(Format::Rgba8Unorm)),
      m_depthFormat(D3D12::ToNativeFormat(Format::D32Float))
{
    ValidateFramePacingConfiguration(m_framePacingConfiguration);
    m_framePacingState.requested = m_framePacingConfiguration;
}

D3D12Context::~D3D12Context()
{
    FlushPendingUploads(true);
    WaitForGpu();
    for (UploadPage& page : m_uploadPages)
    {
        if (page.resource != nullptr
            && page.cpuAddress != nullptr)
        {
            page.resource->Unmap(0, nullptr);
            page.cpuAddress = nullptr;
        }
    }

    if (m_fenceEvent != nullptr)
    {
        CloseHandle(m_fenceEvent);
        m_fenceEvent = nullptr;
    }
    if (m_computeFenceEvent != nullptr)
    {
        CloseHandle(m_computeFenceEvent);
        m_computeFenceEvent = nullptr;
    }
    if (m_uploadFenceEvent != nullptr)
    {
        CloseHandle(m_uploadFenceEvent);
        m_uploadFenceEvent = nullptr;
    }
}

void D3D12Context::Initialize(Platform::Window& window)
{
    m_window = &window;
    m_width = window.GetWidth();
    m_height = window.GetHeight();

    EnableDebugLayer();
    CreateFactory();
    PickAdapter();
    CreateDevice();
    CreateCommandObjects();
    CreateSwapChain();
    CreateDescriptorHeaps();
    CreateRenderTargets();
    CreateDepthStencil();
    CreateFence();
    ExecuteComputeImmediate(
        [](ID3D12GraphicsCommandList*) {});
    BuildDeviceCapabilities();
}

void D3D12Context::Resize(const std::uint32_t width, const std::uint32_t height)
{
    if (width == 0 || height == 0)
    {
        return;
    }

    if (m_swapChain == nullptr)
    {
        m_width = width;
        m_height = height;
        return;
    }

    if (m_width == width && m_height == height)
    {
        return;
    }

    WaitForGpu();
    ReleaseSizeDependentResources();

    m_width = width;
    m_height = height;

    Core::ThrowIfFailed(
        m_swapChain->ResizeBuffers(FrameCount, m_width, m_height,
            m_backBufferFormat, m_presentationPlan.swapChainFlags),
        "Failed to resize swap chain buffers.");

    m_frameLatencyWaitableObject =
        m_swapChain->GetFrameLatencyWaitableObject();
    Core::Check(m_frameLatencyWaitableObject != nullptr,
        "DXGI frame-latency waitable object became unavailable after resize.");

    m_currentFrameIndex = m_swapChain->GetCurrentBackBufferIndex();
    CreateRenderTargets();
    CreateDepthStencil();
}

FrameResult D3D12Context::BeginFrame()
{
    m_currentFrameIndex = m_swapChain->GetCurrentBackBufferIndex();
    auto* pacing = FramePacing();
    if (pacing)
    {
        pacing->Reset(m_currentFrameIndex);
        pacing->image = m_currentFrameIndex;
        pacing->imageCount = FrameCount;
        pacing->syncInterval = m_presentationPlan.syncInterval;
        pacing->presentFlags = m_presentationPlan.presentFlags;
    }
    FramePacingScope fenceScope(pacing, FramePacingStatistics::Phase::FrameFence);
    WaitForFrame(m_currentFrameIndex);
    fenceScope.End();

    FrameContext& frameContext = m_frameContexts[m_currentFrameIndex];
    FramePacingScope reclaimScope(pacing, FramePacingStatistics::Phase::Reclaim);
    ReclaimDescriptorRanges(frameContext);
    {
        std::scoped_lock lock(
            m_resourceRetirementMutex);
        const std::uint32_t reclaimed =
            static_cast<std::uint32_t>(
                frameContext.retiredResources.size());
        frameContext.retiredResources.clear();
        frameContext.retiredAllocationOwners.clear();
        m_resourceRetirementStatistics
            .totalReclaimedObjectCount += reclaimed;
        m_resourceRetirementStatistics
            .pendingObjectCount -= reclaimed;
    }
    frameContext.retainedCommandLists.clear();
    frameContext.retainedCommandAllocators.clear();
    frameContext.retainedUploadResources.clear();
    reclaimScope.End();
    FramePacingScope uploadScope(pacing, FramePacingStatistics::Phase::Upload);
    FlushPendingUploads(false);
    uploadScope.End();
    FramePacingScope prepareScope(pacing, FramePacingStatistics::Phase::Prepare);
    m_activeTransientCommandAllocator.Reset();
    m_activeCommandQueue = CommandQueueType::Graphics;
    m_queueBatchExecutionActive = false;
    m_queueBatchOpen = false;
    m_activeBatchWaits.clear();
    m_activeBatchCommandAllocators.clear();
    m_activeBatchCommandLists.clear();
    m_pendingQueueBatches.clear();
    Core::ThrowIfFailed(frameContext.commandAllocator->Reset(), "Failed to reset command allocator.");
    Core::ThrowIfFailed(m_commandList->Reset(frameContext.commandAllocator.Get(), nullptr), "Failed to reset command list.");

    TransitionCurrentBackBuffer(ResourceState::Present, ResourceState::RenderTarget);

    m_viewport.TopLeftX = 0.0f;
    m_viewport.TopLeftY = 0.0f;
    m_viewport.Width = static_cast<float>(m_width);
    m_viewport.Height = static_cast<float>(m_height);
    m_viewport.MinDepth = 0.0f;
    m_viewport.MaxDepth = 1.0f;

    m_scissorRect.left = 0;
    m_scissorRect.top = 0;
    m_scissorRect.right = static_cast<LONG>(m_width);
    m_scissorRect.bottom = static_cast<LONG>(m_height);

    const D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle = GetCurrentRtvHandle();
    const D3D12_CPU_DESCRIPTOR_HANDLE dsvHandle = GetDsvHandle();
    m_commandList->RSSetViewports(1, &m_viewport);
    m_commandList->RSSetScissorRects(1, &m_scissorRect);
    m_commandList->OMSetRenderTargets(1, &rtvHandle, FALSE, &dsvHandle);
    if (pacing) pacing->beginCompleted = true;
    return FrameResult::Ready;
}

FrameResult D3D12Context::EndFrame()
{
    Core::Check(
        !m_queueBatchExecutionActive
            && !m_queueBatchOpen,
        "D3D12 queue-batch execution must resume graphics before EndFrame.");
    Core::Check(
        m_activeCommandQueue
            == CommandQueueType::Graphics,
        "A D3D12 frame must end on the graphics queue.");
    TransitionCurrentBackBuffer(ResourceState::RenderTarget, ResourceState::Present);

    Core::ThrowIfFailed(m_commandList->Close(), "Failed to close command list.");

    ID3D12CommandList* commandLists[] = {m_commandList.Get()};
    FramePacingScope submitScope(FramePacing(), FramePacingStatistics::Phase::Submit);
    m_commandQueue->ExecuteCommandLists(1, commandLists);
    submitScope.End();
    if (m_activeTransientCommandAllocator != nullptr)
    {
        m_frameContexts[m_currentFrameIndex]
            .retainedCommandAllocators.push_back(
                m_activeTransientCommandAllocator);
        m_activeTransientCommandAllocator.Reset();
    }

    FramePacingScope presentScope(FramePacing(), FramePacingStatistics::Phase::Present);
    const HRESULT presentResult = m_swapChain->Present(
        m_presentationPlan.syncInterval,
        m_presentationPlan.presentFlags);
    presentScope.End();
    if (auto* pacing = FramePacing()) pacing->presentResult = presentResult;
    Core::ThrowIfFailed(presentResult, "Failed to present swap chain.");

    FramePacingScope signalScope(FramePacing(), FramePacingStatistics::Phase::Signal);
    FrameContext& frameContext = m_frameContexts[m_currentFrameIndex];
    frameContext.fenceValue = m_nextFenceValue;
    Core::ThrowIfFailed(m_commandQueue->Signal(m_fence.Get(), m_nextFenceValue), "Failed to signal fence.");
    ++m_nextFenceValue;
    ++m_totalSubmittedFrames;
    signalScope.End();
    if (Core::IsEnvironmentVariableEnabled("PRISM_RENDER_GPU_VALIDATION"))
    {
        ComPtr<ID3D12InfoQueue> queue;
        if (SUCCEEDED(m_device.As(&queue)))
        {
            bool errors = false;
            for (UINT64 i = 0; i < queue->GetNumStoredMessages(); ++i)
            {
                SIZE_T bytes = 0;
                queue->GetMessage(i, nullptr, &bytes);
                std::vector<std::byte> storage(bytes);
                auto* message = reinterpret_cast<D3D12_MESSAGE*>(storage.data());
                if (SUCCEEDED(queue->GetMessage(i, message, &bytes))
                    && message->Severity <= D3D12_MESSAGE_SEVERITY_WARNING)
                {
                    std::cerr << "[D3D12 validation " << message->ID << "] " << message->pDescription << '\n';
                    errors |= message->Severity <= D3D12_MESSAGE_SEVERITY_ERROR;
                }
            }
            queue->ClearStoredMessages();
            Core::Check(!errors, "D3D12 validation reported errors; see stderr diagnostics.");
        }
    }
    if (auto* pacing = FramePacing()) pacing->endCompleted = true;
    return FrameResult::Ready;
}

void D3D12Context::WaitForGpu()
{
    FlushPendingUploads(false);
    if (m_commandQueue == nullptr || m_fence == nullptr)
    {
        return;
    }

    const std::uint64_t fenceValue = m_nextFenceValue;
    Core::ThrowIfFailed(m_commandQueue->Signal(m_fence.Get(), fenceValue), "Failed to signal fence for WaitForGpu.");
    ++m_nextFenceValue;

    if (m_fence->GetCompletedValue() < fenceValue)
    {
        Core::ThrowIfFailed(m_fence->SetEventOnCompletion(fenceValue, m_fenceEvent), "Failed to set fence event.");
        WaitForSingleObject(m_fenceEvent, INFINITE);
    }

    if (m_computeCommandQueue != nullptr
        && m_computeFence != nullptr)
    {
        const std::uint64_t computeFenceValue =
            m_nextComputeFenceValue++;
        Core::ThrowIfFailed(
            m_computeCommandQueue->Signal(
                m_computeFence.Get(),
                computeFenceValue),
            "Failed to signal the D3D12 compute fence.");
        if (m_computeFence->GetCompletedValue()
            < computeFenceValue)
        {
            Core::ThrowIfFailed(
                m_computeFence->SetEventOnCompletion(
                    computeFenceValue,
                    m_computeFenceEvent),
                "Failed to set the D3D12 compute fence event.");
            WaitForSingleObject(
                m_computeFenceEvent,
                INFINITE);
        }
    }
}

FrameAdmissionResult D3D12Context::WaitForFrameAdmission()
{
    FrameAdmissionResult result{};
    result.configurationGeneration =
        m_framePacingState.effectiveGeneration;
    if (m_framePacingState.effectivePresentation
            != PresentationIntent::Immediate)
    {
        Core::Check(m_frameLatencyWaitableObject != nullptr,
            "DXGI frame admission requires a waitable object.");
        const auto start = std::chrono::steady_clock::now();
        const DWORD waitResult = WaitForSingleObjectEx(
            m_frameLatencyWaitableObject, INFINITE, FALSE);
        Core::Check(waitResult == WAIT_OBJECT_0,
            "Failed to wait for DXGI frame admission.");
        result.displayWaitMilliseconds =
            std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - start).count();
    }
    result.submittedFrames = m_totalSubmittedFrames;
    const std::uint64_t completed = m_fence != nullptr
        ? m_fence->GetCompletedValue() : 0;
    for (const FrameContext& frame : m_frameContexts)
    {
        if (frame.fenceValue > completed)
        {
            ++result.outstandingFrames;
        }
    }
    return result;
}

bool D3D12Context::ApplyFramePacingConfiguration(
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
        const D3D12::D3D12PresentationPlan plan =
            D3D12::BuildD3D12PresentationPlan(
                configuration, m_tearingSupported);
        if (m_swapChain != nullptr)
        {
            Core::ThrowIfFailed(
                m_swapChain->SetMaximumFrameLatency(
                    plan.maximumFrameLatency),
                "Failed to set DXGI maximum frame latency.");
        }
        m_framePacingConfiguration = configuration;
        m_presentationPlan = plan;
        m_framePacingState.requested = configuration;
        m_framePacingState.effectivePresentation =
            plan.effectivePresentation;
        m_framePacingState.effectiveMaxQueuedFrames =
            plan.maximumFrameLatency;
        m_framePacingState.syncInterval = plan.syncInterval;
        m_framePacingState.presentFlags = plan.presentFlags;
        m_framePacingState.tearingEnabled = plan.tearingEnabled;
        m_framePacingState.fallbackReason = plan.fallbackReason;
        m_framePacingState.admissionSource =
            plan.effectivePresentation == PresentationIntent::Immediate
                ? FrameAdmissionSource::FrameQueueFallback
                : FrameAdmissionSource::DxgiFrameLatencyWaitableObject;
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

const FramePacingState& D3D12Context::GetFramePacingState() const noexcept
{
    return m_framePacingState;
}

ID3D12Device* D3D12Context::GetDevice() const
{
    return m_device.Get();
}

ID3D12CommandQueue* D3D12Context::GetCommandQueue() const
{
    return m_commandQueue.Get();
}

ID3D12CommandQueue*
D3D12Context::GetComputeCommandQueue() const
{
    return m_computeCommandQueue.Get();
}

ID3D12GraphicsCommandList* D3D12Context::GetCommandList() const
{
    return m_commandList.Get();
}

ID3D12DescriptorHeap* D3D12Context::GetShaderVisibleSrvHeap() const
{
    return m_srvHeap.Get();
}

ID3D12DescriptorHeap* D3D12Context::GetShaderVisibleSamplerHeap() const
{
    return m_samplerHeap.Get();
}

ID3D12CommandSignature*
D3D12Context::GetDrawIndexedCommandSignature(
    const std::uint32_t stride)
{
    Core::Check(
        stride >= sizeof(DrawIndexedIndirectArguments)
            && stride % 4u == 0u,
        "D3D12 indexed-indirect command signatures require a valid stride.");
    std::scoped_lock lock(m_commandSignatureMutex);
    const auto found =
        m_drawIndexedCommandSignatures.find(stride);
    if (found != m_drawIndexedCommandSignatures.end())
    {
        return found->second.Get();
    }

    D3D12_INDIRECT_ARGUMENT_DESC argument{};
    argument.Type =
        D3D12_INDIRECT_ARGUMENT_TYPE_DRAW_INDEXED;
    D3D12_COMMAND_SIGNATURE_DESC description{};
    description.ByteStride = stride;
    description.NumArgumentDescs = 1;
    description.pArgumentDescs = &argument;
    Microsoft::WRL::ComPtr<ID3D12CommandSignature>
        commandSignature;
    Core::ThrowIfFailed(
        m_device->CreateCommandSignature(
            &description,
            nullptr,
            IID_PPV_ARGS(&commandSignature)),
        "Failed to create the D3D12 indexed-indirect command signature.");
    ID3D12CommandSignature* result =
        commandSignature.Get();
    m_drawIndexedCommandSignatures.emplace(
        stride,
        std::move(commandSignature));
    return result;
}

D3D12Context::ShaderVisibleDescriptor D3D12Context::AllocateShaderVisibleSrv()
{
    return AllocateShaderVisibleSrvRange(1);
}

D3D12Context::ShaderVisibleDescriptor D3D12Context::AllocateShaderVisibleSrvRange(const std::uint32_t descriptorCount)
{
    return AllocateShaderVisibleRange(
        m_freeSrvDescriptors,
        descriptorCount,
        false);
}

D3D12Context::ShaderVisibleDescriptor D3D12Context::AllocateShaderVisibleSamplerRange(
    const std::uint32_t descriptorCount)
{
    return AllocateShaderVisibleRange(
        m_freeSamplerDescriptors,
        descriptorCount,
        true);
}

D3D12Context::ShaderVisibleDescriptor
D3D12Context::AllocateShaderVisibleRange(
    std::vector<DescriptorRange>& freeRanges,
    const std::uint32_t descriptorCount,
    const bool sampler)
{
    Core::Check(
        descriptorCount > 0,
        "Shader-visible descriptor allocations must request at least one descriptor.");
    std::scoped_lock lock(m_descriptorAllocatorMutex);
    const auto range = std::ranges::find_if(
        freeRanges,
        [descriptorCount](const DescriptorRange& candidate)
        {
            return candidate.count >= descriptorCount;
        });
    Core::Check(
        range != freeRanges.end(),
        sampler
            ? "Shader-visible sampler heap is exhausted."
            : "Shader-visible resource heap is exhausted.");

    ShaderVisibleDescriptor descriptor{};
    descriptor.index = range->first;
    ID3D12DescriptorHeap* heap =
        sampler ? m_samplerHeap.Get() : m_srvHeap.Get();
    const std::uint32_t increment =
        sampler ? m_samplerDescriptorSize : m_srvDescriptorSize;
    descriptor.cpuHandle =
        heap->GetCPUDescriptorHandleForHeapStart();
    descriptor.gpuHandle =
        heap->GetGPUDescriptorHandleForHeapStart();
    descriptor.cpuHandle.ptr +=
        static_cast<SIZE_T>(descriptor.index) * increment;
    descriptor.gpuHandle.ptr +=
        static_cast<UINT64>(descriptor.index) * increment;
    range->first += descriptorCount;
    range->count -= descriptorCount;
    if (range->count == 0)
    {
        freeRanges.erase(range);
    }

    std::uint32_t& allocated =
        sampler
            ? m_descriptorAllocatorStatistics
                  .allocatedSamplerDescriptorCount
            : m_descriptorAllocatorStatistics
                  .allocatedResourceDescriptorCount;
    std::uint32_t& highWatermark =
        sampler
            ? m_descriptorAllocatorStatistics
                  .samplerDescriptorHighWatermark
            : m_descriptorAllocatorStatistics
                  .resourceDescriptorHighWatermark;
    allocated += descriptorCount;
    highWatermark = std::max(highWatermark, allocated);
    return descriptor;
}

void D3D12Context::RetireShaderVisibleSrvRange(
    const std::uint32_t firstDescriptor,
    const std::uint32_t descriptorCount)
{
    if (descriptorCount == 0)
    {
        return;
    }
    std::scoped_lock lock(m_descriptorAllocatorMutex);
    m_frameContexts[m_currentFrameIndex]
        .retiredSrvDescriptors.push_back(
            {firstDescriptor, descriptorCount});
    ++m_descriptorAllocatorStatistics.pendingReleaseCount;
}

void D3D12Context::RetireShaderVisibleSamplerRange(
    const std::uint32_t firstDescriptor,
    const std::uint32_t descriptorCount)
{
    if (descriptorCount == 0)
    {
        return;
    }
    std::scoped_lock lock(m_descriptorAllocatorMutex);
    m_frameContexts[m_currentFrameIndex]
        .retiredSamplerDescriptors.push_back(
            {firstDescriptor, descriptorCount});
    ++m_descriptorAllocatorStatistics.pendingReleaseCount;
}

void D3D12Context::InsertAndCoalesceDescriptorRange(
    std::vector<DescriptorRange>& freeRanges,
    const DescriptorRange range)
{
    freeRanges.push_back(range);
    std::ranges::sort(
        freeRanges,
        {},
        &DescriptorRange::first);
    std::vector<DescriptorRange> merged;
    merged.reserve(freeRanges.size());
    for (const DescriptorRange& candidate : freeRanges)
    {
        if (!merged.empty()
            && merged.back().first + merged.back().count
                == candidate.first)
        {
            merged.back().count += candidate.count;
        }
        else
        {
            merged.push_back(candidate);
        }
    }
    freeRanges = std::move(merged);
}

void D3D12Context::ReclaimDescriptorRanges(
    FrameContext& frameContext)
{
    std::scoped_lock lock(m_descriptorAllocatorMutex);
    for (const DescriptorRange range :
         frameContext.retiredSrvDescriptors)
    {
        InsertAndCoalesceDescriptorRange(
            m_freeSrvDescriptors,
            range);
        m_descriptorAllocatorStatistics
            .allocatedResourceDescriptorCount -= range.count;
    }
    for (const DescriptorRange range :
         frameContext.retiredSamplerDescriptors)
    {
        InsertAndCoalesceDescriptorRange(
            m_freeSamplerDescriptors,
            range);
        m_descriptorAllocatorStatistics
            .allocatedSamplerDescriptorCount -= range.count;
    }
    m_descriptorAllocatorStatistics.pendingReleaseCount -=
        static_cast<std::uint32_t>(
            frameContext.retiredSrvDescriptors.size()
            + frameContext.retiredSamplerDescriptors.size());
    frameContext.retiredSrvDescriptors.clear();
    frameContext.retiredSamplerDescriptors.clear();
}

 D3D12Context::UploadAllocation
D3D12Context::AllocateUpload(
    const std::uint64_t byteCount,
    const std::uint64_t alignment)
{
    Core::Check(
        byteCount > 0,
        "D3D12 upload allocations must contain bytes.");
    std::scoped_lock lock(m_uploadMutex);
    const std::uint64_t completedTicket =
        m_uploadFence != nullptr
        ? m_uploadFence->GetCompletedValue()
        : 0;

    const auto allocateFromPage =
        [&](UploadPage& page,
            const std::uint32_t pageIndex,
            const bool resetPage)
            -> std::optional<UploadAllocation>
    {
        if (resetPage)
        {
            page.cursor = 0;
            page.lastTicket = 0;
        }
        const std::uint64_t offset =
            AlignUp(page.cursor, alignment);
        if (offset > page.capacity
            || byteCount > page.capacity - offset)
        {
            return std::nullopt;
        }
        page.cursor = offset + byteCount;
        page.pending = true;
        return UploadAllocation{
            page.resource,
            offset,
            byteCount,
            page.cpuAddress + offset,
            pageIndex};
    };

    for (std::uint32_t pageIndex = 0;
         pageIndex < m_uploadPages.size();
         ++pageIndex)
    {
        UploadPage& page = m_uploadPages[pageIndex];
        if (!page.pending)
        {
            continue;
        }
        if (auto allocation = allocateFromPage(
                page,
                pageIndex,
                false))
        {
            return *allocation;
        }
    }
    for (std::uint32_t pageIndex = 0;
         pageIndex < m_uploadPages.size();
         ++pageIndex)
    {
        UploadPage& page = m_uploadPages[pageIndex];
        if (page.pending
            || page.lastTicket > completedTicket)
        {
            continue;
        }
        if (auto allocation = allocateFromPage(
                page,
                pageIndex,
                true))
        {
            return *allocation;
        }
    }

    const std::uint64_t capacity =
        std::max(
            UploadPageSize,
            AlignUp(byteCount, 64ull * 1024ull));
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_UPLOAD;
    D3D12_RESOURCE_DESC description{};
    description.Dimension =
        D3D12_RESOURCE_DIMENSION_BUFFER;
    description.Width = capacity;
    description.Height = 1;
    description.DepthOrArraySize = 1;
    description.MipLevels = 1;
    description.SampleDesc.Count = 1;
    description.Layout =
        D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    UploadPage page{};
    page.capacity = capacity;
    Core::ThrowIfFailed(
        m_device->CreateCommittedResource(
            &heap,
            D3D12_HEAP_FLAG_NONE,
            &description,
            D3D12_RESOURCE_STATE_GENERIC_READ,
            nullptr,
            IID_PPV_ARGS(&page.resource)),
        "Failed to create a persistent D3D12 upload page.");
    void* mapped = nullptr;
    D3D12_RANGE readRange{0, 0};
    Core::ThrowIfFailed(
        page.resource->Map(
            0,
            &readRange,
            &mapped),
        "Failed to map a persistent D3D12 upload page.");
    page.cpuAddress =
        static_cast<std::byte*>(mapped);
    m_uploadPages.push_back(std::move(page));
    m_uploadQueueStatistics.stagingPageCount =
        static_cast<std::uint32_t>(
            m_uploadPages.size());
    m_uploadQueueStatistics.stagingCapacityBytes +=
        capacity;
    const std::uint32_t pageIndex =
        static_cast<std::uint32_t>(
            m_uploadPages.size() - 1);
    return *allocateFromPage(
        m_uploadPages.back(),
        pageIndex,
        false);
}

void D3D12Context::QueueUpload(
    const UploadAllocation& allocation,
    const std::function<void(
        ID3D12GraphicsCommandList*)>&
        recordCopyCommands,
    const std::function<void(
        ID3D12GraphicsCommandList*)>&
        recordFinalizeCommands)
{
    Core::Check(
        allocation.resource != nullptr
            && allocation.size > 0
            && allocation.cpuAddress != nullptr
            && static_cast<bool>(recordCopyCommands)
            && static_cast<bool>(recordFinalizeCommands),
        "D3D12 upload batches require staging storage, copy commands, and finalize commands.");
    std::scoped_lock lock(m_uploadMutex);
    Core::Check(
        allocation.pageIndex
            < m_uploadPages.size(),
        "D3D12 upload allocation references an invalid staging page.");
    if (m_pendingUploadCommandList == nullptr)
    {
        m_pendingUploadTicket.value =
            m_nextUploadTicket++;
        Core::ThrowIfFailed(
            m_device->CreateCommandAllocator(
                D3D12_COMMAND_LIST_TYPE_COPY,
                IID_PPV_ARGS(
                    &m_pendingUploadAllocator)),
            "Failed to create the D3D12 copy-upload command allocator.");
        Core::ThrowIfFailed(
            m_device->CreateCommandList(
                0,
                D3D12_COMMAND_LIST_TYPE_COPY,
                m_pendingUploadAllocator.Get(),
                nullptr,
                IID_PPV_ARGS(
                    &m_pendingUploadCommandList)),
            "Failed to create the D3D12 copy-upload command list.");
        Core::ThrowIfFailed(
            m_device->CreateCommandAllocator(
                D3D12_COMMAND_LIST_TYPE_DIRECT,
                IID_PPV_ARGS(
                    &m_pendingUploadFinalizeAllocator)),
            "Failed to create the D3D12 upload-finalize command allocator.");
        Core::ThrowIfFailed(
            m_device->CreateCommandList(
                0,
                D3D12_COMMAND_LIST_TYPE_DIRECT,
                m_pendingUploadFinalizeAllocator.Get(),
                nullptr,
                IID_PPV_ARGS(
                    &m_pendingUploadFinalizeCommandList)),
            "Failed to create the D3D12 upload-finalize command list.");
    }
    recordCopyCommands(
        m_pendingUploadCommandList.Get());
    recordFinalizeCommands(
        m_pendingUploadFinalizeCommandList.Get());
    ++m_uploadQueueStatistics.pendingOperationCount;
    m_uploadQueueStatistics.pendingBytes +=
        allocation.size;
    std::uint64_t stagingUsage = 0;
    for (const UploadPage& page : m_uploadPages)
    {
        stagingUsage += page.cursor;
    }
    m_uploadQueueStatistics.stagingHighWatermarkBytes =
        std::max(
            m_uploadQueueStatistics
                .stagingHighWatermarkBytes,
            stagingUsage);
    m_uploadQueueStatistics.pendingTicket =
        m_pendingUploadTicket.value;
}

void D3D12Context::RetireResource(
    ComPtr<ID3D12Resource> resource,
    std::shared_ptr<void> allocationOwner)
{
    if (resource == nullptr)
    {
        return;
    }
    std::scoped_lock lock(
        m_resourceRetirementMutex);
    FrameContext& frame =
        m_frameContexts[m_currentFrameIndex];
    if (allocationOwner != nullptr)
    {
        frame.retiredAllocationOwners.push_back(
            std::move(allocationOwner));
    }
    frame.retiredResources.push_back(
        std::move(resource));
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

void D3D12Context::FlushPendingUploads(
    const bool waitForCompletion)
{
    std::unique_lock lock(m_uploadMutex);
    if (m_pendingUploadCommandList == nullptr)
    {
        return;
    }
    Core::ThrowIfFailed(
        m_pendingUploadCommandList->Close(),
        "Failed to close the D3D12 copy-upload command list.");
    Core::ThrowIfFailed(
        m_pendingUploadFinalizeCommandList->Close(),
        "Failed to close the D3D12 upload-finalize command list.");
    ID3D12CommandList* copyCommandLists[] = {
        m_pendingUploadCommandList.Get()};
    m_copyCommandQueue->ExecuteCommandLists(
        1,
        copyCommandLists);
    const UploadTicket submittedTicket =
        m_pendingUploadTicket;
    Core::Check(
        submittedTicket.IsValid()
            && m_copyFence != nullptr
            && m_uploadFence != nullptr,
        "D3D12 upload submission requires a valid ticket and copy/ready fences.");
    Core::ThrowIfFailed(
        m_copyCommandQueue->Signal(
            m_copyFence.Get(),
            submittedTicket.value),
        "Failed to signal the D3D12 copy-upload fence.");
    Core::ThrowIfFailed(
        m_commandQueue->Wait(
            m_copyFence.Get(),
            submittedTicket.value),
        "Failed to wait for the D3D12 copy-upload batch.");
    ID3D12CommandList* finalizeCommandLists[] = {
        m_pendingUploadFinalizeCommandList.Get()};
    m_commandQueue->ExecuteCommandLists(
        1,
        finalizeCommandLists);
    Core::ThrowIfFailed(
        m_commandQueue->Signal(
            m_uploadFence.Get(),
            submittedTicket.value),
        "Failed to signal the D3D12 upload-ready ticket.");
    for (UploadPage& page : m_uploadPages)
    {
        if (page.pending)
        {
            page.pending = false;
            page.lastTicket =
                submittedTicket.value;
        }
    }

    const std::uint64_t batchBytes =
        m_uploadQueueStatistics.pendingBytes;
    m_uploadQueueStatistics.uploadedBytes +=
        batchBytes;
    ++m_uploadQueueStatistics.submittedBatchCount;
    m_uploadQueueStatistics.maximumBatchBytes =
        std::max(
            m_uploadQueueStatistics.maximumBatchBytes,
            batchBytes);
    m_uploadQueueStatistics.pendingOperationCount = 0;
    m_uploadQueueStatistics.pendingBytes = 0;
    m_uploadQueueStatistics.pendingTicket = 0;
    m_uploadQueueStatistics.lastSubmittedTicket =
        submittedTicket.value;
    m_pendingUploadTicket = {};

    if (waitForCompletion)
    {
        ++m_uploadQueueStatistics
              .synchronousFlushCount;
        std::vector<ComPtr<ID3D12Resource>>
            retainedResources =
                std::move(m_pendingUploadResources);
        ComPtr<ID3D12GraphicsCommandList>
            retainedCommandList =
                std::move(m_pendingUploadCommandList);
        ComPtr<ID3D12CommandAllocator>
            retainedAllocator =
                std::move(m_pendingUploadAllocator);
        ComPtr<ID3D12GraphicsCommandList>
            retainedFinalizeCommandList =
                std::move(
                    m_pendingUploadFinalizeCommandList);
        ComPtr<ID3D12CommandAllocator>
            retainedFinalizeAllocator =
                std::move(
                    m_pendingUploadFinalizeAllocator);
        lock.unlock();
        if (m_uploadFence->GetCompletedValue()
            < submittedTicket.value)
        {
            Core::ThrowIfFailed(
                m_uploadFence->SetEventOnCompletion(
                    submittedTicket.value,
                    m_uploadFenceEvent),
                "Failed to wait for a D3D12 upload ticket.");
            WaitForSingleObject(
                m_uploadFenceEvent,
                INFINITE);
        }
        return;
    }

    FrameContext& frame =
        m_frameContexts[m_currentFrameIndex];
    frame.retainedCommandLists.push_back(
        std::move(m_pendingUploadCommandList));
    frame.retainedCommandLists.push_back(
        std::move(
            m_pendingUploadFinalizeCommandList));
    frame.retainedCommandAllocators.push_back(
        std::move(m_pendingUploadAllocator));
    frame.retainedCommandAllocators.push_back(
        std::move(
            m_pendingUploadFinalizeAllocator));
    frame.retainedUploadResources.insert(
        frame.retainedUploadResources.end(),
        std::make_move_iterator(
            m_pendingUploadResources.begin()),
        std::make_move_iterator(
            m_pendingUploadResources.end()));
    m_pendingUploadResources.clear();
}

void D3D12Context::ExecuteImmediate(const std::function<void(ID3D12GraphicsCommandList*)>& recordCommands)
{
    Core::Check(static_cast<bool>(recordCommands), "Immediate D3D12 submissions require a recording callback.");
    FlushPendingUploads(false);
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> commandList;
    Core::ThrowIfFailed(
        m_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)),
        "Failed to create an immediate D3D12 command allocator.");
    Core::ThrowIfFailed(
        m_device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr,
                                    IID_PPV_ARGS(&commandList)),
        "Failed to create an immediate D3D12 command list.");
    recordCommands(commandList.Get());
    Core::ThrowIfFailed(commandList->Close(), "Failed to close an immediate D3D12 command list.");
    ID3D12CommandList* commandLists[] = {commandList.Get()};
    m_commandQueue->ExecuteCommandLists(1, commandLists);
    WaitForGpu();
}

void D3D12Context::ExecuteComputeImmediate(
    const std::function<void(
        ID3D12GraphicsCommandList*)>& recordCommands)
{
    Core::Check(
        static_cast<bool>(recordCommands),
        "Immediate D3D12 compute submissions require a recording callback.");
    Core::Check(
        m_computeCommandQueue != nullptr
            && m_computeFence != nullptr,
        "The D3D12 compute queue is not initialized.");

    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> commandList;
    Core::ThrowIfFailed(
        m_device->CreateCommandAllocator(
            D3D12_COMMAND_LIST_TYPE_COMPUTE,
            IID_PPV_ARGS(&allocator)),
        "Failed to create an immediate D3D12 compute allocator.");
    Core::ThrowIfFailed(
        m_device->CreateCommandList(
            0,
            D3D12_COMMAND_LIST_TYPE_COMPUTE,
            allocator.Get(),
            nullptr,
            IID_PPV_ARGS(&commandList)),
        "Failed to create an immediate D3D12 compute command list.");
    recordCommands(commandList.Get());
    Core::ThrowIfFailed(
        commandList->Close(),
        "Failed to close an immediate D3D12 compute command list.");
    ID3D12CommandList* commandLists[] = {
        commandList.Get()};
    m_computeCommandQueue->ExecuteCommandLists(
        1,
        commandLists);

    const std::uint64_t fenceValue =
        m_nextComputeFenceValue++;
    Core::ThrowIfFailed(
        m_computeCommandQueue->Signal(
            m_computeFence.Get(),
            fenceValue),
        "Failed to signal an immediate D3D12 compute submission.");
    if (m_computeFence->GetCompletedValue() < fenceValue)
    {
        Core::ThrowIfFailed(
            m_computeFence->SetEventOnCompletion(
                fenceValue,
                m_computeFenceEvent),
            "Failed to set an immediate D3D12 compute fence event.");
        WaitForSingleObject(
            m_computeFenceEvent,
            INFINITE);
    }
    m_computeQueueValidated = true;
}

bool D3D12Context::SupportsNativeComputeQueue() const
{
    return m_computeCommandQueue != nullptr
        && m_computeFence != nullptr;
}

bool D3D12Context::IsComputeQueueValidated() const
{
    return m_computeQueueValidated;
}

CommandQueueType
D3D12Context::GetActiveCommandQueue() const
{
    return m_activeCommandQueue;
}

void D3D12Context::RetainActiveCommandList(
    FrameContext& frameContext)
{
    frameContext.retainedCommandLists.push_back(
        m_commandList);
    if (m_activeTransientCommandAllocator != nullptr)
    {
        frameContext.retainedCommandAllocators.push_back(
            m_activeTransientCommandAllocator);
        m_activeTransientCommandAllocator.Reset();
    }
}

void D3D12Context::CreateActiveCommandList(
    const D3D12_COMMAND_LIST_TYPE type)
{
    Core::ThrowIfFailed(
        m_device->CreateCommandAllocator(
            type,
            IID_PPV_ARGS(
                &m_activeTransientCommandAllocator)),
        "Failed to create a D3D12 queue-segment command allocator.");
    Core::ThrowIfFailed(
        m_device->CreateCommandList(
            0,
            type,
            m_activeTransientCommandAllocator.Get(),
            nullptr,
            IID_PPV_ARGS(&m_commandList)),
        "Failed to create a D3D12 queue-segment command list.");
}

void D3D12Context::CloseActiveBatchSegment()
{
    Core::Check(
        m_commandList != nullptr,
        "A D3D12 queue batch has no active command list.");
    const HRESULT closeResult =
        m_commandList->Close();
    if (FAILED(closeResult))
    {
        std::ostringstream message;
        message
            << "Failed to close a D3D12 queue-batch command-list segment."
            << " (HRESULT=0x"
            << std::hex
            << static_cast<unsigned long>(
                   closeResult)
            << ")";
        ComPtr<ID3D12InfoQueue> infoQueue;
        if (SUCCEEDED(
                m_device.As(&infoQueue)))
        {
            const UINT64 messageCount =
                infoQueue
                    ->GetNumStoredMessagesAllowedByRetrievalFilter();
            const UINT64 firstMessage =
                messageCount > 12
                ? messageCount - 12
                : 0;
            for (UINT64 index = firstMessage;
                 index < messageCount;
                 ++index)
            {
                SIZE_T messageBytes = 0;
                if (FAILED(
                        infoQueue->GetMessage(
                            index,
                            nullptr,
                            &messageBytes))
                    || messageBytes == 0)
                {
                    continue;
                }
                std::vector<std::byte> storage(
                    messageBytes);
                auto* diagnostic =
                    reinterpret_cast<
                        D3D12_MESSAGE*>(
                        storage.data());
                if (SUCCEEDED(
                        infoQueue->GetMessage(
                            index,
                            diagnostic,
                            &messageBytes)))
                {
                    message
                        << "\n[D3D12 "
                        << diagnostic->ID
                        << "] "
                        << diagnostic
                               ->pDescription;
                }
            }
        }
        throw std::runtime_error(
            message.str());
    }
    m_activeBatchCommandLists.push_back(
        std::move(m_commandList));
    if (m_activeTransientCommandAllocator != nullptr)
    {
        m_activeBatchCommandAllocators.push_back(
            std::move(
                m_activeTransientCommandAllocator));
    }
}

bool D3D12Context::SwitchCommandQueue(
    const CommandQueueType queue)
{
    Core::Check(
        !m_queueBatchExecutionActive,
        "Legacy D3D12 queue switching cannot run during queue-batch execution.");
    Core::Check(
        m_swapChain != nullptr,
        "D3D12 queue switching requires an active render context.");
    if (queue == m_activeCommandQueue)
    {
        return true;
    }
    Core::Check(
        SupportsNativeComputeQueue()
            && m_computeQueueValidated,
        "D3D12 native queue switching is unavailable.");

    FrameContext& frameContext =
        m_frameContexts[m_currentFrameIndex];
    Core::ThrowIfFailed(
        m_commandList->Close(),
        "Failed to close a D3D12 queue-segment command list.");
    ID3D12CommandList* commandLists[] = {
        m_commandList.Get()};

    if (m_activeCommandQueue
            == CommandQueueType::Graphics
        && queue == CommandQueueType::Compute)
    {
        m_commandQueue->ExecuteCommandLists(
            1,
            commandLists);
        const std::uint64_t graphicsValue =
            m_nextFenceValue++;
        Core::ThrowIfFailed(
            m_commandQueue->Signal(
                m_fence.Get(),
                graphicsValue),
            "Failed to signal D3D12 graphics work before compute.");
        Core::ThrowIfFailed(
            m_computeCommandQueue->Wait(
                m_fence.Get(),
                graphicsValue),
            "Failed to wait for D3D12 graphics work on the compute queue.");
        RetainActiveCommandList(frameContext);
        CreateActiveCommandList(
            D3D12_COMMAND_LIST_TYPE_COMPUTE);
        m_activeCommandQueue =
            CommandQueueType::Compute;
        return true;
    }

    Core::Check(
        m_activeCommandQueue
                == CommandQueueType::Compute
            && queue == CommandQueueType::Graphics,
        "Unsupported D3D12 command-queue transition.");
    m_computeCommandQueue->ExecuteCommandLists(
        1,
        commandLists);
    const std::uint64_t computeValue =
        m_nextComputeFenceValue++;
    Core::ThrowIfFailed(
        m_computeCommandQueue->Signal(
            m_computeFence.Get(),
            computeValue),
        "Failed to signal D3D12 compute work.");
    Core::ThrowIfFailed(
        m_commandQueue->Wait(
            m_computeFence.Get(),
            computeValue),
        "Failed to wait for D3D12 compute work on the graphics queue.");
    RetainActiveCommandList(frameContext);
    CreateActiveCommandList(
        D3D12_COMMAND_LIST_TYPE_DIRECT);
    m_activeCommandQueue =
        CommandQueueType::Graphics;
    return true;
}

void D3D12Context::ApplyQueueWaits(
    const CommandQueueType queue,
    const std::span<const QueueSyncPoint> waits)
{
    ID3D12CommandQueue* targetQueue =
        queue == CommandQueueType::Compute
            ? m_computeCommandQueue.Get()
            : m_commandQueue.Get();
    Core::Check(
        targetQueue != nullptr,
        "D3D12 queue-batch wait requires a valid target queue.");

    for (const QueueSyncPoint wait : waits)
    {
        if (!wait.IsValid() || wait.queue == queue)
        {
            continue;
        }
        ID3D12Fence* sourceFence =
            wait.queue == CommandQueueType::Compute
                ? m_computeFence.Get()
                : m_fence.Get();
        Core::Check(
            sourceFence != nullptr,
            "D3D12 queue-batch wait requires a valid source fence.");
        Core::ThrowIfFailed(
            targetQueue->Wait(sourceFence, wait.value),
            "Failed to enqueue a D3D12 queue-batch dependency.");
    }
}

bool D3D12Context::BeginQueueBatch(
    const CommandQueueType queue,
    const std::span<const QueueSyncPoint> waits)
{
    Core::Check(
        m_swapChain != nullptr,
        "D3D12 queue batches require an active render context.");
    Core::Check(
        SupportsNativeComputeQueue()
            && m_computeQueueValidated,
        "D3D12 independent queue batches are unavailable.");
    Core::Check(
        !m_queueBatchOpen,
        "A D3D12 queue batch is already open.");

    if (!m_queueBatchExecutionActive)
    {
        Core::Check(
            queue == CommandQueueType::Graphics
                && m_activeCommandQueue
                    == CommandQueueType::Graphics,
            "D3D12 queue-batch execution must begin with the active graphics command list.");
        m_queueBatchExecutionActive = true;
        m_activeBatchCommandAllocators.clear();
        m_activeBatchCommandLists.clear();
    }
    else
    {
        CreateActiveCommandList(
            queue == CommandQueueType::Compute
                ? D3D12_COMMAND_LIST_TYPE_COMPUTE
                : D3D12_COMMAND_LIST_TYPE_DIRECT);
    }

    m_activeCommandQueue = queue;
    m_activeBatchWaits.assign(
        waits.begin(),
        waits.end());
    m_queueBatchOpen = true;
    return true;
}

QueueSyncPoint D3D12Context::EndQueueBatch()
{
    Core::Check(
        m_queueBatchExecutionActive
            && m_queueBatchOpen,
        "No D3D12 queue batch is open.");
    CloseActiveBatchSegment();

    QueueSyncPoint signal{};
    signal.queue = m_activeCommandQueue;
    if (m_activeCommandQueue
        == CommandQueueType::Compute)
    {
        signal.value = m_nextComputeFenceValue++;
    }
    else
    {
        signal.value = m_nextFenceValue++;
    }

    PendingQueueBatch pending{};
    pending.commandAllocators =
        std::move(
            m_activeBatchCommandAllocators);
    pending.commandLists =
        std::move(m_activeBatchCommandLists);
    pending.queue = m_activeCommandQueue;
    pending.waits =
        std::move(m_activeBatchWaits);
    pending.signal = signal;
    m_pendingQueueBatches.push_back(
        std::move(pending));
    m_queueBatchOpen = false;
    return signal;
}

bool D3D12Context::FlushQueueBatches()
{
    Core::Check(
        m_queueBatchExecutionActive
            && !m_queueBatchOpen,
        "D3D12 queue batches must be closed before submission.");
    Core::Check(
        !m_pendingQueueBatches.empty(),
        "No D3D12 queue batches are pending submission.");

    FrameContext& frame =
        m_frameContexts[m_currentFrameIndex];
    for (PendingQueueBatch& batch :
         m_pendingQueueBatches)
    {
        ApplyQueueWaits(
            batch.queue,
            batch.waits);
        std::vector<ID3D12CommandList*>
            commandLists;
        commandLists.reserve(
            batch.commandLists.size());
        for (const auto& commandList :
             batch.commandLists)
        {
            commandLists.push_back(
                commandList.Get());
        }
        ID3D12CommandQueue* queue =
            batch.queue == CommandQueueType::Compute
            ? m_computeCommandQueue.Get()
            : m_commandQueue.Get();
        queue->ExecuteCommandLists(
            static_cast<UINT>(
                commandLists.size()),
            commandLists.data());
        ID3D12Fence* fence =
            batch.queue == CommandQueueType::Compute
            ? m_computeFence.Get()
            : m_fence.Get();
        Core::ThrowIfFailed(
            queue->Signal(
                fence,
                batch.signal.value),
            "Failed to signal a deferred D3D12 queue batch.");
        for (auto& commandList :
             batch.commandLists)
        {
            frame.retainedCommandLists.push_back(
                std::move(commandList));
        }
        for (auto& allocator :
             batch.commandAllocators)
        {
            frame.retainedCommandAllocators.push_back(
                std::move(allocator));
        }
    }
    m_pendingQueueBatches.clear();
    return true;
}

std::unique_ptr<IParallelCommandRecording>
D3D12Context::CreateParallelCommandRecording(
    const CommandQueueType queue)
{
    Core::Check(
        queue == CommandQueueType::Graphics
            || (SupportsNativeComputeQueue()
                && m_computeQueueValidated),
        "The requested D3D12 parallel command queue is unavailable.");
    return std::make_unique<
        D3D12ParallelCommandRecording>(
        *this,
        queue);
}

bool D3D12Context::AppendParallelCommandRecording(
    std::unique_ptr<IParallelCommandRecording>
        recording)
{
    Core::Check(
        m_queueBatchExecutionActive
            && m_queueBatchOpen,
        "D3D12 parallel command recordings require an open queue batch.");
    auto* native = dynamic_cast<
        D3D12ParallelCommandRecording*>(
        recording.get());
    Core::Check(
        native != nullptr
            && native->IsClosed()
            && native->GetQueue()
                == m_activeCommandQueue,
        "The D3D12 parallel command recording is invalid or still open.");

    CloseActiveBatchSegment();
    m_activeBatchCommandAllocators.push_back(
        native->TakeAllocator());
    m_activeBatchCommandLists.push_back(
        native->TakeCommandList());
    CreateActiveCommandList(
        m_activeCommandQueue
                == CommandQueueType::Compute
            ? D3D12_COMMAND_LIST_TYPE_COMPUTE
            : D3D12_COMMAND_LIST_TYPE_DIRECT);
    return true;
}

bool D3D12Context::ResumeGraphicsQueue(
    const std::span<const QueueSyncPoint> waits)
{
    Core::Check(
        m_queueBatchExecutionActive
            && !m_queueBatchOpen
            && m_pendingQueueBatches.empty(),
        "D3D12 graphics continuation requires completed queue batches.");
    CreateActiveCommandList(
        D3D12_COMMAND_LIST_TYPE_DIRECT);
    m_activeCommandQueue =
        CommandQueueType::Graphics;
    ApplyQueueWaits(
        CommandQueueType::Graphics,
        waits);
    m_queueBatchExecutionActive = false;
    return true;
}

D3D12_CPU_DESCRIPTOR_HANDLE D3D12Context::GetCurrentRtvHandle() const
{
    return OffsetCpuHandle(m_rtvHeap->GetCPUDescriptorHandleForHeapStart(), m_currentFrameIndex, m_rtvDescriptorSize);
}

D3D12_CPU_DESCRIPTOR_HANDLE D3D12Context::GetDsvHandle() const
{
    return m_dsvHeap->GetCPUDescriptorHandleForHeapStart();
}

ID3D12Resource* D3D12Context::GetDepthStencilResource() const
{
    return m_depthStencil.Get();
}

D3D12_CPU_DESCRIPTOR_HANDLE D3D12Context::GetImGuiCpuDescriptorHandle() const
{
    return m_srvHeap->GetCPUDescriptorHandleForHeapStart();
}

D3D12_GPU_DESCRIPTOR_HANDLE D3D12Context::GetImGuiGpuDescriptorHandle() const
{
    return m_srvHeap->GetGPUDescriptorHandleForHeapStart();
}

DXGI_FORMAT D3D12Context::GetBackBufferFormat() const
{
    return m_backBufferFormat;
}

DXGI_FORMAT D3D12Context::GetDepthFormat() const
{
    return m_depthFormat;
}

std::uint32_t D3D12Context::GetCurrentFrameIndex() const
{
    return m_currentFrameIndex;
}

std::uint32_t D3D12Context::GetFramesInFlight() const
{
    return FrameCount;
}

std::uint32_t D3D12Context::GetFrameWidth() const
{
    return m_width;
}

std::uint32_t D3D12Context::GetFrameHeight() const
{
    return m_height;
}

const std::shared_ptr<ITexture>&
D3D12Context::GetCurrentBackBufferTexture() const
{
    Core::Check(
        m_currentFrameIndex < m_backBufferTextures.size()
            && m_backBufferTextures[m_currentFrameIndex]
                != nullptr,
        "D3D12 back-buffer texture is unavailable.");
    return m_backBufferTextures[m_currentFrameIndex];
}

const ITextureView&
D3D12Context::GetCurrentBackBufferView() const
{
    Core::Check(
        m_currentFrameIndex < m_backBufferViews.size()
            && m_backBufferViews[m_currentFrameIndex]
                != nullptr,
        "D3D12 back-buffer view is unavailable.");
    return *m_backBufferViews[m_currentFrameIndex];
}

Format D3D12Context::GetBackBufferRhiFormat() const
{
    return Format::Rgba8Unorm;
}

const std::shared_ptr<ITexture>&
D3D12Context::GetDepthStencilTexture() const
{
    Core::Check(
        m_depthStencilTexture != nullptr,
        "D3D12 depth-stencil texture is unavailable.");
    return m_depthStencilTexture;
}

const ITextureView&
D3D12Context::GetDepthStencilView() const
{
    Core::Check(
        m_depthStencilView != nullptr,
        "D3D12 depth-stencil view is unavailable.");
    return *m_depthStencilView;
}

const std::string& D3D12Context::GetAdapterName() const
{
    return m_adapterName;
}

const GraphicsAdapterInfo& D3D12Context::GetAdapterInfo() const
{
    return m_adapterInfo;
}

GraphicsApi D3D12Context::GetGraphicsApi() const
{
    return GraphicsApi::Direct3D12;
}

const GraphicsDeviceCapabilities&
D3D12Context::GetDeviceCapabilities() const
{
    return m_deviceCapabilities;
}

DescriptorAllocatorStatistics
D3D12Context::GetDescriptorAllocatorStatistics() const
{
    std::scoped_lock lock(m_descriptorAllocatorMutex);
    return m_descriptorAllocatorStatistics;
}

UploadQueueStatistics
D3D12Context::GetUploadQueueStatistics() const
{
    std::scoped_lock lock(m_uploadMutex);
    UploadQueueStatistics statistics =
        m_uploadQueueStatistics;
    statistics.completedTicket =
        m_uploadFence != nullptr
        ? m_uploadFence->GetCompletedValue()
        : 0;
    statistics.outstandingBatchCount =
        statistics.lastSubmittedTicket
            >= statistics.completedTicket
        ? statistics.lastSubmittedTicket
            - statistics.completedTicket
        : 0;
    return statistics;
}

UploadTicket
D3D12Context::GetPendingUploadTicket() const
{
    std::scoped_lock lock(m_uploadMutex);
    return m_pendingUploadTicket;
}

bool D3D12Context::IsUploadComplete(
    const UploadTicket ticket) const
{
    return !ticket.IsValid()
        || (m_uploadFence != nullptr
            && m_uploadFence->GetCompletedValue()
                >= ticket.value);
}

ResourceRetirementStatistics
D3D12Context::GetResourceRetirementStatistics() const
{
    std::scoped_lock lock(
        m_resourceRetirementMutex);
    return m_resourceRetirementStatistics;
}

void D3D12Context::BuildDeviceCapabilities()
{
    m_deviceCapabilities = {};
    m_deviceCapabilities.graphicsApi =
        GraphicsApi::Direct3D12;
    m_deviceCapabilities.adapterName =
        m_adapterName;
    m_deviceCapabilities.limits = {
        D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION,
        D3D12_REQ_TEXTURE2D_ARRAY_AXIS_DIMENSION,
        D3D12_SIMULTANEOUS_RENDER_TARGET_COUNT,
        16,
        D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT,
        1,
        ShaderVisibleSrvCapacity,
        ShaderVisibleSamplerCapacity};
    D3D12_FEATURE_DATA_D3D12_OPTIONS options{};
    const bool optionsAvailable =
        SUCCEEDED(m_device->CheckFeatureSupport(
            D3D12_FEATURE_D3D12_OPTIONS,
            &options,
            sizeof(options)));
    m_deviceCapabilities.features = {
        true,
        SupportsNativeComputeQueue()
            && m_computeQueueValidated,
        SupportsNativeComputeQueue()
            && m_computeQueueValidated,
        m_copyCommandQueue != nullptr,
        m_copyCommandQueue != nullptr,
        true,
        true,
        optionsAvailable
            && options.ResourceBindingTier
                >= D3D12_RESOURCE_BINDING_TIER_2,
        true,
        true,
        true,
        true};
    m_deviceCapabilities.limits.maxTessellationControlPoints = 32u;
    // D3D12 exposes hull/domain stages and patch-list IA topology on every
    // feature-level device supported by this renderer.
    m_deviceCapabilities.features.tessellationShader = true;
    m_deviceCapabilities.features.patchListTopology = true;

    D3D12_FEATURE_DATA_D3D12_OPTIONS5
        rayTracingOptions{};
    const bool rayTracingOptionsAvailable =
        SUCCEEDED(m_device->CheckFeatureSupport(
            D3D12_FEATURE_D3D12_OPTIONS5,
            &rayTracingOptions,
            sizeof(rayTracingOptions)));
    if (rayTracingOptionsAvailable
        && rayTracingOptions.RaytracingTier
            >= D3D12_RAYTRACING_TIER_1_0)
    {
        m_deviceCapabilities.features
            .rayTracingAccelerationStructure = true;
        m_deviceCapabilities.features
            .rayTracingPipeline = true;
        m_deviceCapabilities.features.rayQuery =
            rayTracingOptions.RaytracingTier
            >= D3D12_RAYTRACING_TIER_1_1;
        m_deviceCapabilities.rayTracingTier =
            m_deviceCapabilities.features.rayQuery
            ? RayTracingTier::Tier1_1
            : RayTracingTier::Tier1_0;
        m_deviceCapabilities.limits
            .maxRayRecursionDepth =
            D3D12_RAYTRACING_MAX_DECLARABLE_TRACE_RECURSION_DEPTH;
        m_deviceCapabilities.limits
            .rayTracingShaderGroupHandleSize =
            D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES;
        m_deviceCapabilities.limits
            .rayTracingShaderGroupBaseAlignment =
            D3D12_RAYTRACING_SHADER_TABLE_BYTE_ALIGNMENT;
        m_deviceCapabilities.limits
            .accelerationStructureScratchAlignment =
            D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BYTE_ALIGNMENT;
    }
}

void D3D12Context::EnableDebugLayer() const
{
    bool enabled = Core::IsEnvironmentVariableEnabled("PRISM_RENDER_GPU_VALIDATION");
#if defined(_DEBUG)
    enabled = true;
#endif
    if (!enabled) return;
    ComPtr<ID3D12Debug> debugController;
    if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debugController))))
    {
        debugController->EnableDebugLayer();
        std::cerr << "D3D12 debug layer enabled.\n";
    }
    else Core::Check(false, "D3D12 debug layer requested but unavailable. Install Windows Graphics Tools.");
}

void D3D12Context::CreateFactory()
{
    std::uint32_t flags = 0;
#if defined(_DEBUG)
    flags |= DXGI_CREATE_FACTORY_DEBUG;
#endif
    Core::ThrowIfFailed(CreateDXGIFactory2(flags, IID_PPV_ARGS(&m_factory)), "Failed to create DXGI factory.");
    BOOL allowTearing = FALSE;
    m_tearingSupported = SUCCEEDED(m_factory->CheckFeatureSupport(
        DXGI_FEATURE_PRESENT_ALLOW_TEARING,
        &allowTearing,
        sizeof(allowTearing))) && allowTearing == TRUE;
    m_presentationPlan = D3D12::BuildD3D12PresentationPlan(
        m_framePacingConfiguration, m_tearingSupported);
}

void D3D12Context::PickAdapter()
{
    ComPtr<IDXGIAdapter1> candidate;
    ComPtr<ID3D12Device> testDevice;

    for (std::uint32_t adapterIndex = 0;
         m_factory->EnumAdapterByGpuPreference(adapterIndex, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&candidate)) != DXGI_ERROR_NOT_FOUND;
         ++adapterIndex)
    {
        DXGI_ADAPTER_DESC1 description{};
        candidate->GetDesc1(&description);

        if ((description.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0)
        {
            continue;
        }

        testDevice.Reset();
        if (SUCCEEDED(D3D12CreateDevice(candidate.Get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&testDevice))))
        {
            Core::ThrowIfFailed(candidate.As(&m_adapter), "Failed to cast DXGI adapter.");
            const std::wstring wideName(description.Description);
            const int utf8Length = WideCharToMultiByte(CP_UTF8, 0, wideName.c_str(), -1, nullptr, 0, nullptr, nullptr);
            if (utf8Length > 1)
            {
                std::vector<char> utf8Name(
                    static_cast<std::size_t>(utf8Length));
                WideCharToMultiByte(
                    CP_UTF8,
                    0,
                    wideName.c_str(),
                    -1,
                    utf8Name.data(),
                    utf8Length,
                    nullptr,
                    nullptr);
                m_adapterName.assign(utf8Name.data());
            }
            else
            {
                m_adapterName = "Unknown Adapter";
            }
            m_adapterInfo.name = m_adapterName;
            m_adapterInfo.vendorId = description.VendorId;
            m_adapterInfo.deviceId = description.DeviceId;
            m_adapterInfo.dedicatedVideoMemoryBytes =
                description.DedicatedVideoMemory;
            m_adapterInfo.sharedSystemMemoryBytes =
                description.SharedSystemMemory;
            LARGE_INTEGER driverVersion{};
            if (SUCCEEDED(candidate->CheckInterfaceSupport(
                    __uuidof(IDXGIDevice),
                    &driverVersion)))
            {
                const std::uint64_t raw =
                    (static_cast<std::uint64_t>(
                        static_cast<std::uint32_t>(
                            driverVersion.HighPart)) << 32u)
                    | static_cast<std::uint32_t>(
                        driverVersion.LowPart);
                m_adapterInfo.driverVersionRaw = raw;
                std::ostringstream version;
                version
                    << HIWORD(driverVersion.HighPart) << '.'
                    << LOWORD(driverVersion.HighPart) << '.'
                    << HIWORD(driverVersion.LowPart) << '.'
                    << LOWORD(driverVersion.LowPart);
                m_adapterInfo.driverVersion = version.str();
            }
            m_adapterInfo.apiVersion =
                "D3D_FEATURE_LEVEL_12_0";
            return;
        }
    }

    throw std::runtime_error("Failed to find a Direct3D 12 compatible hardware adapter.");
}

void D3D12Context::CreateDevice()
{
    Core::ThrowIfFailed(
        D3D12CreateDevice(m_adapter.Get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&m_device)),
        "Failed to create D3D12 device.");
}

void D3D12Context::CreateCommandObjects()
{
    D3D12_COMMAND_QUEUE_DESC queueDesc{};
    queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    queueDesc.Priority = D3D12_COMMAND_QUEUE_PRIORITY_NORMAL;
    queueDesc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;

    Core::ThrowIfFailed(m_device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&m_commandQueue)), "Failed to create command queue.");

    queueDesc.Type = D3D12_COMMAND_LIST_TYPE_COMPUTE;
    Core::ThrowIfFailed(
        m_device->CreateCommandQueue(
            &queueDesc,
            IID_PPV_ARGS(&m_computeCommandQueue)),
        "Failed to create a D3D12 compute command queue.");

    queueDesc.Type = D3D12_COMMAND_LIST_TYPE_COPY;
    Core::ThrowIfFailed(
        m_device->CreateCommandQueue(
            &queueDesc,
            IID_PPV_ARGS(&m_copyCommandQueue)),
        "Failed to create a D3D12 copy command queue.");

    for (FrameContext& frameContext : m_frameContexts)
    {
        Core::ThrowIfFailed(
            m_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&frameContext.commandAllocator)),
            "Failed to create command allocator.");
    }

    Core::ThrowIfFailed(
        m_device->CreateCommandList(
            0,
            D3D12_COMMAND_LIST_TYPE_DIRECT,
            m_frameContexts[0].commandAllocator.Get(),
            nullptr,
            IID_PPV_ARGS(&m_commandList)),
        "Failed to create command list.");
    Core::ThrowIfFailed(m_commandList->Close(), "Failed to close initial command list.");
}

void D3D12Context::CreateSwapChain()
{
    DXGI_SWAP_CHAIN_DESC1 swapChainDesc{};
    swapChainDesc.Width = m_width;
    swapChainDesc.Height = m_height;
    swapChainDesc.Format = m_backBufferFormat;
    swapChainDesc.Stereo = FALSE;
    swapChainDesc.SampleDesc.Count = 1;
    swapChainDesc.BufferUsage = SwapChainUsage;
    swapChainDesc.BufferCount = FrameCount;
    swapChainDesc.Scaling = DXGI_SCALING_STRETCH;
    swapChainDesc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    swapChainDesc.AlphaMode = DXGI_ALPHA_MODE_UNSPECIFIED;
    swapChainDesc.Flags = m_presentationPlan.swapChainFlags;

    ComPtr<IDXGISwapChain1> swapChain;
    Core::ThrowIfFailed(
        m_factory->CreateSwapChainForHwnd(
            m_commandQueue.Get(),
            static_cast<HWND>(m_window->GetNativeHandle()),
            &swapChainDesc,
            nullptr,
            nullptr,
            &swapChain),
        "Failed to create swap chain.");

    Core::ThrowIfFailed(m_factory->MakeWindowAssociation(static_cast<HWND>(m_window->GetNativeHandle()), DXGI_MWA_NO_ALT_ENTER),
                        "Failed to associate window with DXGI factory.");
    Core::ThrowIfFailed(swapChain.As(&m_swapChain), "Failed to query IDXGISwapChain4.");
    Core::ThrowIfFailed(
        m_swapChain->SetMaximumFrameLatency(
            m_presentationPlan.maximumFrameLatency),
        "Failed to configure DXGI maximum frame latency.");
    m_frameLatencyWaitableObject =
        m_swapChain->GetFrameLatencyWaitableObject();
    Core::Check(m_frameLatencyWaitableObject != nullptr,
        "DXGI did not provide a frame-latency waitable object.");
    m_currentFrameIndex = m_swapChain->GetCurrentBackBufferIndex();
    m_framePacingState.requested = m_framePacingConfiguration;
    m_framePacingState.effectivePresentation =
        m_presentationPlan.effectivePresentation;
    m_framePacingState.effectiveMaxQueuedFrames =
        m_presentationPlan.maximumFrameLatency;
    m_framePacingState.swapchainImageCount = FrameCount;
    m_framePacingState.frameResourceSlotCount = FrameCount;
    m_framePacingState.syncInterval = m_presentationPlan.syncInterval;
    m_framePacingState.presentFlags = m_presentationPlan.presentFlags;
    m_framePacingState.nativePresentMode = "DXGI_FLIP_DISCARD";
    m_framePacingState.fallbackReason =
        m_presentationPlan.fallbackReason;
    m_framePacingState.tearingSupported = m_tearingSupported;
    m_framePacingState.tearingEnabled =
        m_presentationPlan.tearingEnabled;
    m_framePacingState.admissionSource =
        m_presentationPlan.effectivePresentation
                == PresentationIntent::Immediate
            ? FrameAdmissionSource::FrameQueueFallback
            : FrameAdmissionSource::DxgiFrameLatencyWaitableObject;
}

void D3D12Context::CreateDescriptorHeaps()
{
    D3D12_DESCRIPTOR_HEAP_DESC rtvHeapDesc{};
    rtvHeapDesc.NumDescriptors = FrameCount;
    rtvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    Core::ThrowIfFailed(m_device->CreateDescriptorHeap(&rtvHeapDesc, IID_PPV_ARGS(&m_rtvHeap)), "Failed to create RTV heap.");

    D3D12_DESCRIPTOR_HEAP_DESC dsvHeapDesc{};
    dsvHeapDesc.NumDescriptors = 1;
    dsvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
    Core::ThrowIfFailed(m_device->CreateDescriptorHeap(&dsvHeapDesc, IID_PPV_ARGS(&m_dsvHeap)), "Failed to create DSV heap.");

    D3D12_DESCRIPTOR_HEAP_DESC srvHeapDesc{};
    srvHeapDesc.NumDescriptors = ShaderVisibleSrvCapacity;
    srvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    srvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    Core::ThrowIfFailed(m_device->CreateDescriptorHeap(&srvHeapDesc, IID_PPV_ARGS(&m_srvHeap)), "Failed to create SRV heap.");

    D3D12_DESCRIPTOR_HEAP_DESC samplerHeapDesc{};
    samplerHeapDesc.NumDescriptors = ShaderVisibleSamplerCapacity;
    samplerHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER;
    samplerHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    Core::ThrowIfFailed(
        m_device->CreateDescriptorHeap(&samplerHeapDesc, IID_PPV_ARGS(&m_samplerHeap)),
        "Failed to create sampler heap.");

    m_rtvDescriptorSize = m_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    m_srvDescriptorSize = m_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    m_samplerDescriptorSize = m_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER);

    std::scoped_lock lock(m_descriptorAllocatorMutex);
    m_freeSrvDescriptors = {
        {1, ShaderVisibleSrvCapacity - 1}};
    m_freeSamplerDescriptors = {
        {0, ShaderVisibleSamplerCapacity}};
    m_descriptorAllocatorStatistics = {};
    m_descriptorAllocatorStatistics.poolCount = 2;
    m_descriptorAllocatorStatistics
        .resourceDescriptorCapacity =
        ShaderVisibleSrvCapacity - 1;
    m_descriptorAllocatorStatistics
        .samplerDescriptorCapacity =
        ShaderVisibleSamplerCapacity;
}

void D3D12Context::CreateRenderTargets()
{
    for (std::uint32_t index = 0; index < FrameCount; ++index)
    {
        Core::ThrowIfFailed(m_swapChain->GetBuffer(index, IID_PPV_ARGS(&m_renderTargets[index])), "Failed to retrieve swap chain buffer.");
        const D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle =
            OffsetCpuHandle(
                m_rtvHeap->GetCPUDescriptorHandleForHeapStart(),
                index,
                m_rtvDescriptorSize);
        m_device->CreateRenderTargetView(
            m_renderTargets[index].Get(),
            nullptr,
            rtvHandle);

        TextureDescription textureDescription{};
        textureDescription.width = m_width;
        textureDescription.height = m_height;
        textureDescription.format = Format::Rgba8Unorm;
        textureDescription.usage =
            TextureUsage::RenderTarget
            | TextureUsage::CopySource;
        auto texture = std::make_shared<
            D3D12::D3D12Texture>(
            m_renderTargets[index].Get(),
            textureDescription);
        texture->SetDebugName(
            "SwapChain.BackBuffer."
            + std::to_string(index));

        TextureViewDescription viewDescription{};
        viewDescription.type =
            TextureViewType::RenderTarget;
        viewDescription.format = Format::Rgba8Unorm;
        auto view = std::make_shared<
            D3D12::D3D12TextureView>(
            texture,
            rtvHandle,
            viewDescription);
        m_backBufferTextures[index] =
            std::move(texture);
        m_backBufferViews[index] =
            std::move(view);
    }
}

void D3D12Context::CreateDepthStencil()
{
    D3D12_CLEAR_VALUE clearValue{};
    clearValue.Format = m_depthFormat;
    clearValue.DepthStencil.Depth = 1.0f;
    clearValue.DepthStencil.Stencil = 0;

    D3D12_HEAP_PROPERTIES heapProperties{};
    heapProperties.Type = D3D12_HEAP_TYPE_DEFAULT;

    TextureDescription depthDescription{};
    depthDescription.width = m_width;
    depthDescription.height = m_height;
    depthDescription.format = Format::D32Float;
    depthDescription.usage =
        TextureUsage::DepthStencil
        | TextureUsage::ShaderResource;
    D3D12_RESOURCE_DESC resourceDesc =
        D3D12::ToNativeTextureDescription(
            depthDescription);
    resourceDesc.Format = DXGI_FORMAT_R32_TYPELESS;

    Core::ThrowIfFailed(
        m_device->CreateCommittedResource(
            &heapProperties,
            D3D12_HEAP_FLAG_NONE,
            &resourceDesc,
            D3D12_RESOURCE_STATE_DEPTH_WRITE,
            &clearValue,
            IID_PPV_ARGS(&m_depthStencil)),
        "Failed to create depth-stencil buffer.");

    D3D12_DEPTH_STENCIL_VIEW_DESC dsvDesc{};
    dsvDesc.Format = m_depthFormat;
    dsvDesc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
    dsvDesc.Flags = D3D12_DSV_FLAG_NONE;
    m_device->CreateDepthStencilView(m_depthStencil.Get(), &dsvDesc, m_dsvHeap->GetCPUDescriptorHandleForHeapStart());

    m_depthStencilTexture = std::make_shared<
        D3D12::D3D12Texture>(
        m_depthStencil.Get(),
        depthDescription);
    TextureViewDescription viewDescription{};
    viewDescription.type =
        TextureViewType::DepthStencil;
    viewDescription.format = Format::D32Float;
    m_depthStencilView = std::make_shared<
        D3D12::D3D12TextureView>(
        std::static_pointer_cast<
            D3D12::D3D12Texture>(
            m_depthStencilTexture),
        m_dsvHeap
            ->GetCPUDescriptorHandleForHeapStart(),
        viewDescription);
}

void D3D12Context::CreateFence()
{
    Core::ThrowIfFailed(m_device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&m_fence)), "Failed to create fence.");
    m_fenceEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
    Core::Check(m_fenceEvent != nullptr, "Failed to create fence event.");
    Core::ThrowIfFailed(
        m_device->CreateFence(
            0,
            D3D12_FENCE_FLAG_NONE,
            IID_PPV_ARGS(&m_computeFence)),
        "Failed to create a D3D12 compute fence.");
    Core::ThrowIfFailed(
        m_device->CreateFence(
            0,
            D3D12_FENCE_FLAG_NONE,
            IID_PPV_ARGS(&m_copyFence)),
        "Failed to create a D3D12 copy fence.");
    Core::ThrowIfFailed(
        m_device->CreateFence(
            0,
            D3D12_FENCE_FLAG_NONE,
            IID_PPV_ARGS(&m_uploadFence)),
        "Failed to create a D3D12 upload fence.");
    m_computeFenceEvent =
        CreateEvent(nullptr, FALSE, FALSE, nullptr);
    Core::Check(
        m_computeFenceEvent != nullptr,
        "Failed to create a D3D12 compute fence event.");
    m_uploadFenceEvent =
        CreateEvent(nullptr, FALSE, FALSE, nullptr);
    Core::Check(
        m_uploadFenceEvent != nullptr,
        "Failed to create a D3D12 upload fence event.");
}

void D3D12Context::ReleaseSizeDependentResources()
{
    m_depthStencilView.reset();
    m_depthStencilTexture.reset();
    m_depthStencil.Reset();
    for (std::uint32_t index = 0;
         index < FrameCount;
         ++index)
    {
        m_backBufferViews[index].reset();
        m_backBufferTextures[index].reset();
        m_renderTargets[index].Reset();
    }
}

void D3D12Context::TransitionCurrentBackBuffer(const ResourceState before, const ResourceState after)
{
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
    barrier.Transition.pResource = m_renderTargets[m_currentFrameIndex].Get();
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = D3D12::ToNativeResourceStates(before);
    barrier.Transition.StateAfter = D3D12::ToNativeResourceStates(after);
    m_commandList->ResourceBarrier(1, &barrier);
}

void D3D12Context::WaitForFrame(const std::uint32_t frameIndex)
{
    const std::uint64_t fenceValue = m_frameContexts[frameIndex].fenceValue;
    auto* pacing = FramePacing();
    if (pacing) pacing->fenceTarget = fenceValue;
    if (fenceValue == 0)
    {
        return;
    }

    const auto completed = m_fence->GetCompletedValue();
    if (pacing) pacing->fenceCompletedBefore = completed;
    if (completed < fenceValue)
    {
        if (pacing) pacing->frameFenceWaitCalled = true;
        Core::ThrowIfFailed(m_fence->SetEventOnCompletion(fenceValue, m_fenceEvent), "Failed to set per-frame fence event.");
        const auto waitResult = WaitForSingleObject(m_fenceEvent, INFINITE);
        if (pacing) pacing->waitResult = waitResult;
    }
}
} // namespace Prism::RHI
