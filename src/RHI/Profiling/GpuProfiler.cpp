#include "RHI/Profiling/GpuProfiler.h"

#include "Core/Assert.h"
#if defined(PRISM_RENDER_HAS_D3D12)
#include "RHI/D3D12/D3D12Context.h"
#endif
#include "RHI/IFrameContext.h"
#include "RHI/IRenderBackend.h"
#include "RHI/Vulkan/VulkanContext.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>

namespace Prism::RHI
{
void GpuProfiler::SetSamplingMode(
    const SamplingMode mode) noexcept
{
    m_requestedSamplingMode = mode;
}

GpuProfiler::SamplingMode
GpuProfiler::GetSamplingMode() const noexcept
{
    return m_requestedSamplingMode;
}

GpuProfiler::SamplingMode
GpuProfiler::GetActiveSamplingMode() const noexcept
{
    return m_activeSamplingMode;
}

namespace
{
std::size_t QueueIndex(const RHI::CommandQueueType queue)
{
    return queue == RHI::CommandQueueType::Compute ? 1u : 0u;
}

#if defined(PRISM_RENDER_HAS_D3D12)
template <typename Calibration>
double D3D12TimestampToQpcMilliseconds(
    const std::uint64_t timestamp,
    const Calibration& calibration,
    const std::uint64_t qpcFrequency)
{
    if (!calibration.valid
        || calibration.gpuFrequency == 0
        || qpcFrequency == 0)
    {
        return 0.0;
    }

    const long double gpuDelta =
        static_cast<long double>(timestamp)
        - static_cast<long double>(calibration.gpuTimestamp);
    const long double qpcTimestamp =
        static_cast<long double>(calibration.cpuTimestamp)
        + gpuDelta * static_cast<long double>(qpcFrequency)
            / static_cast<long double>(calibration.gpuFrequency);
    return static_cast<double>(
        qpcTimestamp * 1000.0L
        / static_cast<long double>(qpcFrequency));
}
#endif

std::uint64_t TimestampMask(const std::uint32_t validBits)
{
    if (validBits == 0 || validBits >= 64)
    {
        return std::numeric_limits<std::uint64_t>::max();
    }
    return (std::uint64_t{1} << validBits) - 1;
}

std::uint64_t VulkanTimestampDelta(
    const std::uint64_t begin,
    const std::uint64_t end,
    const std::uint64_t mask)
{
    return (end - begin) & mask;
}
} // namespace

GpuProfiler::~GpuProfiler()
{
    if (m_vulkanDevice != VK_NULL_HANDLE)
    {
        for (const VkQueryPool queryPool : m_vulkanQueryPools)
        {
            if (queryPool != VK_NULL_HANDLE)
            {
                vkDestroyQueryPool(m_vulkanDevice, queryPool, nullptr);
            }
        }
    }
}

void GpuProfiler::Initialize(
    RHI::IRenderBackend& backend)
{
    RHI::IFrameContext& frame =
        backend.GetFrameContext();
#if defined(PRISM_RENDER_HAS_D3D12)
    if (frame.GetGraphicsApi()
        == RHI::GraphicsApi::Direct3D12)
    {
        auto* context = dynamic_cast<
            RHI::D3D12Context*>(&frame);
        Core::Check(
            context != nullptr,
            "The D3D12 GPU profiler requires a D3D12 frame context.");
        Initialize(*context);
        return;
    }
#endif
    auto* context = dynamic_cast<
        RHI::Vulkan::VulkanContext*>(&frame);
    Core::Check(
        context != nullptr,
        "The Vulkan GPU profiler requires a Vulkan frame context.");
    Initialize(*context);
}

void GpuProfiler::BeginFrame(
    RHI::IRenderBackend& backend)
{
    RHI::IFrameContext& frame =
        backend.GetFrameContext();
#if defined(PRISM_RENDER_HAS_D3D12)
    if (auto* context = dynamic_cast<
            RHI::D3D12Context*>(&frame))
    {
        BeginFrame(*context);
        return;
    }
#endif
    auto* context = dynamic_cast<
        RHI::Vulkan::VulkanContext*>(&frame);
    Core::Check(context != nullptr, "Unsupported GPU profiler frame backend.");
    BeginFrame(*context);
}

void GpuProfiler::BeginPass(
    RHI::IRenderBackend& backend,
    const std::string_view name)
{
    RHI::IFrameContext& frame =
        backend.GetFrameContext();
#if defined(PRISM_RENDER_HAS_D3D12)
    if (auto* context = dynamic_cast<
            RHI::D3D12Context*>(&frame))
    {
        BeginPass(*context, name);
        return;
    }
#endif
    auto* context = dynamic_cast<
        RHI::Vulkan::VulkanContext*>(&frame);
    Core::Check(context != nullptr, "Unsupported GPU profiler frame backend.");
    BeginPass(*context, name);
}

void GpuProfiler::EndPass(
    RHI::IRenderBackend& backend)
{
    RHI::IFrameContext& frame =
        backend.GetFrameContext();
#if defined(PRISM_RENDER_HAS_D3D12)
    if (auto* context = dynamic_cast<
            RHI::D3D12Context*>(&frame))
    {
        EndPass(*context);
        return;
    }
#endif
    auto* context = dynamic_cast<
        RHI::Vulkan::VulkanContext*>(&frame);
    Core::Check(context != nullptr, "Unsupported GPU profiler frame backend.");
    EndPass(*context);
}

void GpuProfiler::EndFrame(
    RHI::IRenderBackend& backend)
{
    RHI::IFrameContext& frame =
        backend.GetFrameContext();
#if defined(PRISM_RENDER_HAS_D3D12)
    if (auto* context = dynamic_cast<
            RHI::D3D12Context*>(&frame))
    {
        EndFrame(*context);
        return;
    }
#endif
    auto* context = dynamic_cast<
        RHI::Vulkan::VulkanContext*>(&frame);
    Core::Check(context != nullptr, "Unsupported GPU profiler frame backend.");
    EndFrame(*context);
}

void GpuProfiler::ResolveSubmittedFrame(
    RHI::IRenderBackend& backend)
{
    RHI::IFrameContext& frame =
        backend.GetFrameContext();
#if defined(PRISM_RENDER_HAS_D3D12)
    if (auto* context = dynamic_cast<
            RHI::D3D12Context*>(&frame))
    {
        ResolveSubmittedFrame(*context);
        return;
    }
#endif
    auto* context = dynamic_cast<
        RHI::Vulkan::VulkanContext*>(&frame);
    Core::Check(context != nullptr, "Unsupported GPU profiler frame backend.");
    ResolveSubmittedFrame(*context);
}

#if defined(PRISM_RENDER_HAS_D3D12)
void GpuProfiler::Initialize(RHI::D3D12Context& context)
{
    if (m_initialized)
    {
        return;
    }

    m_framesInFlight = context.GetFramesInFlight();
    Core::Check(
        m_framesInFlight > 0,
        "The D3D12 GPU profiler requires at least one frame in flight.");
    m_frameData.resize(m_framesInFlight);

    D3D12_QUERY_HEAP_DESC queryHeapDesc{};
    queryHeapDesc.Count =
        MaxQueriesPerFrame * m_framesInFlight;
    queryHeapDesc.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
    Core::ThrowIfFailed(
        context.GetDevice()->CreateQueryHeap(&queryHeapDesc, IID_PPV_ARGS(&m_queryHeap)),
        "Failed to create GPU timestamp query heap.");

    D3D12_HEAP_PROPERTIES readbackHeap{};
    readbackHeap.Type = D3D12_HEAP_TYPE_READBACK;
    D3D12_RESOURCE_DESC readbackDesc{};
    readbackDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    readbackDesc.Width = static_cast<UINT64>(queryHeapDesc.Count) * sizeof(std::uint64_t);
    readbackDesc.Height = 1;
    readbackDesc.DepthOrArraySize = 1;
    readbackDesc.MipLevels = 1;
    readbackDesc.SampleDesc.Count = 1;
    readbackDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    Core::ThrowIfFailed(
        context.GetDevice()->CreateCommittedResource(
            &readbackHeap,
            D3D12_HEAP_FLAG_NONE,
            &readbackDesc,
            D3D12_RESOURCE_STATE_COPY_DEST,
            nullptr,
            IID_PPV_ARGS(&m_readbackBuffer)),
        "Failed to create GPU timestamp readback buffer.");

    Core::ThrowIfFailed(
        context.GetCommandQueue()->GetTimestampFrequency(
            &m_graphicsTimestampFrequency),
        "Failed to query the graphics queue timestamp frequency.");
    if (context.GetComputeCommandQueue() != nullptr)
    {
        Core::ThrowIfFailed(
            context.GetComputeCommandQueue()->GetTimestampFrequency(
                &m_computeTimestampFrequency),
            "Failed to query the compute queue timestamp frequency.");
    }
    else
    {
        m_computeTimestampFrequency =
            m_graphicsTimestampFrequency;
    }

    LARGE_INTEGER qpcFrequency{};
    Core::Check(
        QueryPerformanceFrequency(&qpcFrequency) != FALSE,
        "Failed to query the QPC frequency.");
    m_qpcFrequency =
        static_cast<std::uint64_t>(qpcFrequency.QuadPart);
    m_timelineMetadata.calibrationMethod =
        "d3d12_get_clock_calibration_qpc";
    m_timelineMetadata.graphicsTimestampFrequency =
        m_graphicsTimestampFrequency;
    m_timelineMetadata.computeTimestampFrequency =
        m_computeTimestampFrequency;
    m_initialized = true;
}
#endif

void GpuProfiler::Initialize(RHI::Vulkan::VulkanContext& context)
{
    if (m_initialized)
    {
        return;
    }
    m_framesInFlight = context.GetFramesInFlight();
    Core::Check(
        m_framesInFlight > 0,
        "The Vulkan GPU profiler requires at least one frame in flight.");
    m_frameData.resize(m_framesInFlight);
    m_vulkanQueryPools.resize(
        m_framesInFlight,
        VK_NULL_HANDLE);
    m_vulkanDevice = context.GetDevice();
    VkPhysicalDeviceProperties properties{};
    vkGetPhysicalDeviceProperties(context.GetPhysicalDevice(), &properties);
    m_vulkanTimestampPeriod = properties.limits.timestampPeriod;
    std::uint32_t queueFamilyCount = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(
        context.GetPhysicalDevice(),
        &queueFamilyCount,
        nullptr);
    std::vector<VkQueueFamilyProperties> queueFamilyProperties(
        queueFamilyCount);
    vkGetPhysicalDeviceQueueFamilyProperties(
        context.GetPhysicalDevice(),
        &queueFamilyCount,
        queueFamilyProperties.data());
    const std::uint32_t graphicsFamily =
        context.GetGraphicsQueueFamilyIndex();
    const std::uint32_t computeFamily =
        context.GetComputeQueueFamilyIndex();
    if (graphicsFamily < queueFamilyProperties.size()
        && computeFamily < queueFamilyProperties.size())
    {
        const std::uint32_t graphicsBits =
            queueFamilyProperties[graphicsFamily].timestampValidBits;
        const std::uint32_t computeBits =
            queueFamilyProperties[computeFamily].timestampValidBits;
        if (graphicsBits != 0 && computeBits != 0)
        {
            m_vulkanTimestampValidBits =
                std::min(graphicsBits, computeBits);
        }
    }
    m_timelineMetadata.crossQueueCalibrated =
        m_vulkanTimestampValidBits != 0;
    m_timelineMetadata.calibrationMethod =
        "vulkan_device_timestamp";
    m_timelineMetadata.timestampValidBits =
        m_vulkanTimestampValidBits;
    if (m_vulkanTimestampPeriod > 0.0f)
    {
        const auto frequency = static_cast<std::uint64_t>(
            std::llround(
                1'000'000'000.0
                / static_cast<double>(m_vulkanTimestampPeriod)));
        m_timelineMetadata.graphicsTimestampFrequency = frequency;
        m_timelineMetadata.computeTimestampFrequency = frequency;
    }
    for (VkQueryPool& queryPool : m_vulkanQueryPools)
    {
        VkQueryPoolCreateInfo createInfo{
            VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
        createInfo.queryType = VK_QUERY_TYPE_TIMESTAMP;
        createInfo.queryCount = MaxQueriesPerFrame;
        Core::Check(
            vkCreateQueryPool(
                m_vulkanDevice,
                &createInfo,
                nullptr,
                &queryPool) == VK_SUCCESS,
            "Failed to create Vulkan timestamp query pool.");
    }
    m_initialized = true;
}

#if defined(PRISM_RENDER_HAS_D3D12)
void GpuProfiler::BeginFrame(RHI::D3D12Context& context)
{
    Core::Check(m_initialized, "GPU profiler must be initialized before use.");
    m_currentFrameIndex = context.GetCurrentFrameIndex();
    Core::Check(
        m_currentFrameIndex < m_frameData.size(),
        "D3D12 frame index exceeds the GPU profiler frame count.");
    ReadCompletedD3D12Frame(m_currentFrameIndex);
    m_activeSamplingMode = m_requestedSamplingMode;
    m_currentQueryCount = 0;
    m_currentSamples.clear();
    m_activePassName.clear();
    FrameData& frameData = m_frameData[m_currentFrameIndex];
    frameData.queueCalibrations = {};
    frameData.queueCalibrations[QueueIndex(
        RHI::CommandQueueType::Graphics)].gpuFrequency =
            m_graphicsTimestampFrequency;
    frameData.queueCalibrations[QueueIndex(
        RHI::CommandQueueType::Compute)].gpuFrequency =
            m_computeTimestampFrequency;
    if (m_activeSamplingMode == SamplingMode::Off)
    {
        m_timelineMetadata.crossQueueCalibrated = false;
        return;
    }
    const auto calibrateQueue =
        [this, &frameData](
            ID3D12CommandQueue* queue,
            const RHI::CommandQueueType queueType,
            const std::uint64_t frequency)
        {
            if (queue == nullptr || frequency == 0)
            {
                return;
            }
            QueueCalibration& calibration =
                frameData.queueCalibrations[QueueIndex(queueType)];
            calibration.gpuFrequency = frequency;
            calibration.valid = SUCCEEDED(
                queue->GetClockCalibration(
                    &calibration.gpuTimestamp,
                    &calibration.cpuTimestamp));
        };
    if (m_activeSamplingMode == SamplingMode::Detailed)
    {
        calibrateQueue(
            context.GetCommandQueue(),
            RHI::CommandQueueType::Graphics,
            m_graphicsTimestampFrequency);
        calibrateQueue(
            context.GetComputeCommandQueue(),
            RHI::CommandQueueType::Compute,
            m_computeTimestampFrequency);
    }
    m_timelineMetadata.crossQueueCalibrated =
        frameData.queueCalibrations[0].valid
        && frameData.queueCalibrations[1].valid;
    m_frameBeginQuery = WriteTimestamp(context);
}
#endif

void GpuProfiler::BeginFrame(RHI::Vulkan::VulkanContext& context)
{
    Core::Check(m_initialized, "GPU profiler must be initialized before use.");
    m_currentFrameIndex = context.GetCurrentFrameIndex();
    Core::Check(
        m_currentFrameIndex < m_frameData.size(),
        "Vulkan frame index exceeds the GPU profiler frame count.");
    ReadCompletedVulkanFrame(m_currentFrameIndex);
    m_activeSamplingMode = m_requestedSamplingMode;
    m_currentQueryCount = 0;
    m_currentSamples.clear();
    m_activePassName.clear();
    if (m_activeSamplingMode == SamplingMode::Off)
    {
        return;
    }
    vkCmdResetQueryPool(
        context.GetCommandBuffer(),
        m_vulkanQueryPools[m_currentFrameIndex],
        0,
        MaxQueriesPerFrame);
    m_frameBeginQuery = WriteTimestamp(context);
}

#if defined(PRISM_RENDER_HAS_D3D12)
void GpuProfiler::BeginPass(RHI::D3D12Context& context, const std::string_view name)
{
    if (m_activeSamplingMode != SamplingMode::Detailed)
    {
        return;
    }
    Core::Check(m_activePassName.empty(), "GPU profiler pass markers must not overlap.");
    m_activePassName = name;
    m_activePassQueue = context.GetActiveCommandQueue();
    m_activePassBeginQuery = WriteTimestamp(context);
}
#endif

void GpuProfiler::BeginPass(
    RHI::Vulkan::VulkanContext& context,
    const std::string_view name)
{
    if (m_activeSamplingMode != SamplingMode::Detailed)
    {
        return;
    }
    Core::Check(
        m_activePassName.empty(),
        "GPU profiler pass markers must not overlap.");
    m_activePassName = name;
    m_activePassQueue = context.GetActiveCommandQueue();
    m_activePassBeginQuery = WriteTimestamp(context);
}

#if defined(PRISM_RENDER_HAS_D3D12)
void GpuProfiler::EndPass(RHI::D3D12Context& context)
{
    if (m_activeSamplingMode != SamplingMode::Detailed)
    {
        return;
    }
    Core::Check(!m_activePassName.empty(), "GPU profiler pass marker is not active.");
    Sample sample{};
    sample.name = std::move(m_activePassName);
    sample.beginQuery = m_activePassBeginQuery;
    sample.endQuery = WriteTimestamp(context);
    sample.queue = m_activePassQueue;
    m_currentSamples.push_back(std::move(sample));
    m_activePassName.clear();
}
#endif

void GpuProfiler::EndPass(RHI::Vulkan::VulkanContext& context)
{
    if (m_activeSamplingMode != SamplingMode::Detailed)
    {
        return;
    }
    Core::Check(
        !m_activePassName.empty(),
        "GPU profiler pass marker is not active.");
    Sample sample{};
    sample.name = std::move(m_activePassName);
    sample.beginQuery = m_activePassBeginQuery;
    sample.endQuery = WriteTimestamp(context);
    sample.queue = m_activePassQueue;
    m_currentSamples.push_back(std::move(sample));
    m_activePassName.clear();
}

#if defined(PRISM_RENDER_HAS_D3D12)
void GpuProfiler::EndFrame(RHI::D3D12Context& context)
{
    if (m_activeSamplingMode == SamplingMode::Off)
    {
        return;
    }
    Core::Check(m_activePassName.empty(), "GPU profiler frame ended with an active pass marker.");
    Sample total{};
    total.name = "Renderer";
    total.beginQuery = m_frameBeginQuery;
    total.endQuery = WriteTimestamp(context);
    total.queue = RHI::CommandQueueType::Graphics;
    m_currentSamples.push_back(std::move(total));

    const std::uint32_t frameBase = m_currentFrameIndex * MaxQueriesPerFrame;
    context.GetCommandList()->ResolveQueryData(
        m_queryHeap.Get(),
        D3D12_QUERY_TYPE_TIMESTAMP,
        frameBase,
        m_currentQueryCount,
        m_readbackBuffer.Get(),
        static_cast<UINT64>(frameBase) * sizeof(std::uint64_t));

    FrameData& frameData = m_frameData[m_currentFrameIndex];
    frameData.submissionGeneration =
        m_nextSubmissionGeneration++;
    frameData.queryCount = m_currentQueryCount;
    frameData.frameBeginQuery = m_frameBeginQuery;
    frameData.samples = m_currentSamples;
    m_lastSubmittedFrameIndex = m_currentFrameIndex;
}
#endif

void GpuProfiler::EndFrame(RHI::Vulkan::VulkanContext& context)
{
    if (m_activeSamplingMode == SamplingMode::Off)
    {
        return;
    }
    Core::Check(
        m_activePassName.empty(),
        "GPU profiler frame ended with an active pass marker.");
    Sample total{};
    total.name = "Renderer";
    total.beginQuery = m_frameBeginQuery;
    total.endQuery = WriteTimestamp(context);
    total.queue = RHI::CommandQueueType::Graphics;
    m_currentSamples.push_back(std::move(total));

    FrameData& frameData = m_frameData[m_currentFrameIndex];
    frameData.submissionGeneration =
        m_nextSubmissionGeneration++;
    frameData.queryCount = m_currentQueryCount;
    frameData.frameBeginQuery = m_frameBeginQuery;
    frameData.samples = m_currentSamples;
    m_lastSubmittedFrameIndex = m_currentFrameIndex;
}

#if defined(PRISM_RENDER_HAS_D3D12)
void GpuProfiler::ResolveSubmittedFrame(RHI::D3D12Context& context)
{
    context.WaitForGpu();
    ReadCompletedD3D12Frame(m_lastSubmittedFrameIndex);
}
#endif

void GpuProfiler::ResolveSubmittedFrame(
    RHI::Vulkan::VulkanContext& context)
{
    context.WaitForGpu();
    ReadCompletedVulkanFrame(m_lastSubmittedFrameIndex);
}

float GpuProfiler::GetMilliseconds(const std::string_view name) const
{
    for (const Timing& timing : m_latestTimings)
    {
        if (timing.name == name)
        {
            return timing.milliseconds;
        }
    }
    return 0.0f;
}

const std::vector<GpuProfiler::Timing>& GpuProfiler::GetLatestTimings() const
{
    return m_latestTimings;
}

const GpuProfiler::TimelineMetadata&
GpuProfiler::GetTimelineMetadata() const
{
    return m_timelineMetadata;
}

std::uint64_t
GpuProfiler::GetLatestTimingGeneration() const
{
    return m_latestTimingGeneration;
}

std::uint32_t
GpuProfiler::GetLatestResolvedFrameIndex() const
{
    return m_latestResolvedFrameIndex;
}

#if defined(PRISM_RENDER_HAS_D3D12)
std::uint32_t GpuProfiler::WriteTimestamp(RHI::D3D12Context& context)
{
    Core::Check(m_currentQueryCount < MaxQueriesPerFrame, "GPU profiler query budget is exhausted.");
    const std::uint32_t relativeQuery = m_currentQueryCount++;
    const std::uint32_t absoluteQuery = m_currentFrameIndex * MaxQueriesPerFrame + relativeQuery;
    context.GetCommandList()->EndQuery(m_queryHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, absoluteQuery);
    return relativeQuery;
}
#endif

std::uint32_t GpuProfiler::WriteTimestamp(
    RHI::Vulkan::VulkanContext& context)
{
    Core::Check(
        m_currentQueryCount < MaxQueriesPerFrame,
        "GPU profiler query budget is exhausted.");
    const std::uint32_t query = m_currentQueryCount++;
    vkCmdWriteTimestamp(
        context.GetCommandBuffer(),
        VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
        m_vulkanQueryPools[m_currentFrameIndex],
        query);
    return query;
}

#if defined(PRISM_RENDER_HAS_D3D12)
void GpuProfiler::ReadCompletedD3D12Frame(const std::uint32_t frameIndex)
{
    const FrameData& frameData = m_frameData[frameIndex];
    if (frameData.queryCount == 0
        || frameData.submissionGeneration == 0
        || frameData.submissionGeneration
            <= m_latestTimingGeneration
        || m_graphicsTimestampFrequency == 0)
    {
        return;
    }

    const std::uint32_t frameBase = frameIndex * MaxQueriesPerFrame;
    const D3D12_RANGE readRange{
        static_cast<SIZE_T>(frameBase) * sizeof(std::uint64_t),
        static_cast<SIZE_T>(frameBase + frameData.queryCount) * sizeof(std::uint64_t)};
    void* mappedData = nullptr;
    Core::ThrowIfFailed(m_readbackBuffer->Map(0, &readRange, &mappedData), "Failed to map GPU timestamp results.");
    const auto* timestamps = static_cast<const std::uint64_t*>(mappedData) + frameBase;

    m_latestTimings.clear();
    m_latestTimings.reserve(frameData.samples.size());
    const QueueCalibration& graphicsCalibration =
        frameData.queueCalibrations[QueueIndex(
            RHI::CommandQueueType::Graphics)];
    const double frameStartMilliseconds =
        D3D12TimestampToQpcMilliseconds(
            timestamps[frameData.frameBeginQuery],
            graphicsCalibration,
            m_qpcFrequency);
    for (const Sample& sample : frameData.samples)
    {
        const std::uint64_t begin = timestamps[sample.beginQuery];
        const std::uint64_t end = timestamps[sample.endQuery];
        const QueueCalibration& calibration =
            frameData.queueCalibrations[QueueIndex(sample.queue)];
        const bool calibrated =
            calibration.valid
            && graphicsCalibration.valid
            && m_qpcFrequency != 0;
        const double startMilliseconds = calibrated
            ? D3D12TimestampToQpcMilliseconds(
                begin,
                calibration,
                m_qpcFrequency)
                - frameStartMilliseconds
            : 0.0;
        const double endMilliseconds = calibrated
            ? D3D12TimestampToQpcMilliseconds(
                end,
                calibration,
                m_qpcFrequency)
                - frameStartMilliseconds
            : 0.0;
        const double milliseconds = end >= begin
            && calibration.gpuFrequency != 0
            ? static_cast<double>(end - begin) * 1000.0
                / static_cast<double>(calibration.gpuFrequency)
            : 0.0;
        m_latestTimings.push_back({
            sample.name,
            static_cast<float>(milliseconds),
            sample.queue,
            startMilliseconds,
            endMilliseconds,
            calibrated});
    }

    const D3D12_RANGE writtenRange{0, 0};
    m_readbackBuffer->Unmap(0, &writtenRange);
    m_latestTimingGeneration =
        frameData.submissionGeneration;
    m_latestResolvedFrameIndex = frameIndex;
}
#endif

void GpuProfiler::ReadCompletedVulkanFrame(
    const std::uint32_t frameIndex)
{
    const FrameData& frameData = m_frameData[frameIndex];
    if (frameData.queryCount == 0
        || frameData.submissionGeneration == 0
        || frameData.submissionGeneration
            <= m_latestTimingGeneration
        || m_vulkanDevice == VK_NULL_HANDLE
        || m_vulkanTimestampPeriod <= 0.0f)
    {
        return;
    }
    std::array<std::uint64_t, MaxQueriesPerFrame> timestamps{};
    const VkResult result = vkGetQueryPoolResults(
        m_vulkanDevice,
        m_vulkanQueryPools[frameIndex],
        0,
        frameData.queryCount,
        sizeof(std::uint64_t) * frameData.queryCount,
        timestamps.data(),
        sizeof(std::uint64_t),
        VK_QUERY_RESULT_64_BIT);
    if (result == VK_NOT_READY)
    {
        return;
    }
    Core::Check(
        result == VK_SUCCESS,
        "Failed to read Vulkan timestamp query results.");

    m_latestTimings.clear();
    m_latestTimings.reserve(frameData.samples.size());
    const std::uint64_t timestampMask =
        TimestampMask(m_vulkanTimestampValidBits);
    const std::uint64_t frameBegin =
        timestamps[frameData.frameBeginQuery];
    const bool calibrated =
        m_vulkanTimestampValidBits != 0;
    for (const Sample& sample : frameData.samples)
    {
        const std::uint64_t begin = timestamps[sample.beginQuery];
        const std::uint64_t end = timestamps[sample.endQuery];
        const double startMilliseconds = calibrated
            ? static_cast<double>(
                VulkanTimestampDelta(
                    frameBegin,
                    begin,
                    timestampMask))
                * static_cast<double>(m_vulkanTimestampPeriod)
                / 1'000'000.0
            : 0.0;
        const double endMilliseconds = calibrated
            ? static_cast<double>(
                VulkanTimestampDelta(
                    frameBegin,
                    end,
                    timestampMask))
                * static_cast<double>(m_vulkanTimestampPeriod)
                / 1'000'000.0
            : 0.0;
        const double milliseconds =
            static_cast<double>(
                VulkanTimestampDelta(
                    begin,
                    end,
                    timestampMask))
            * static_cast<double>(m_vulkanTimestampPeriod)
            / 1'000'000.0;
        m_latestTimings.push_back({
            sample.name,
            static_cast<float>(milliseconds),
            sample.queue,
            startMilliseconds,
            endMilliseconds,
            calibrated});
    }
    m_latestTimingGeneration =
        frameData.submissionGeneration;
    m_latestResolvedFrameIndex = frameIndex;
}
} // namespace Prism::RHI
