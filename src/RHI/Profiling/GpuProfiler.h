#pragma once

#include "RHI/ICommandContext.h"
#include "RHI/Vulkan/VulkanLoader.h"

#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#if defined(PRISM_RENDER_HAS_D3D12)
#include <d3d12.h>
#include <wrl/client.h>
#endif

namespace Prism::RHI
{
class D3D12Context;
class IRenderBackend;
}

namespace Prism::RHI::Vulkan
{
class VulkanContext;
}

namespace Prism::RHI
{
class GpuProfiler
{
public:
    enum class SamplingMode : std::uint8_t
    {
        Off,
        Frame,
        Detailed
    };

    struct Timing
    {
        std::string name;
        float milliseconds = 0.0f;
        RHI::CommandQueueType queue =
            RHI::CommandQueueType::Graphics;
        double startMilliseconds = 0.0;
        double endMilliseconds = 0.0;
        bool calibrated = false;
    };

    struct TimelineMetadata
    {
        bool crossQueueCalibrated = false;
        std::string calibrationMethod;
        std::uint64_t graphicsTimestampFrequency = 0;
        std::uint64_t computeTimestampFrequency = 0;
        std::uint32_t timestampValidBits = 0;
    };

    ~GpuProfiler();

    void SetSamplingMode(SamplingMode mode) noexcept;
    [[nodiscard]] SamplingMode GetSamplingMode() const noexcept;
    [[nodiscard]] SamplingMode GetActiveSamplingMode() const noexcept;

    void Initialize(RHI::IRenderBackend& backend);
    void BeginFrame(RHI::IRenderBackend& backend);
    void BeginPass(
        RHI::IRenderBackend& backend,
        std::string_view name);
    void EndPass(RHI::IRenderBackend& backend);
    void EndFrame(RHI::IRenderBackend& backend);
    void ResolveSubmittedFrame(
        RHI::IRenderBackend& backend);

#if defined(PRISM_RENDER_HAS_D3D12)
    void Initialize(RHI::D3D12Context& context);
#endif
    void Initialize(RHI::Vulkan::VulkanContext& context);
#if defined(PRISM_RENDER_HAS_D3D12)
    void BeginFrame(RHI::D3D12Context& context);
#endif
    void BeginFrame(RHI::Vulkan::VulkanContext& context);
#if defined(PRISM_RENDER_HAS_D3D12)
    void BeginPass(RHI::D3D12Context& context, std::string_view name);
#endif
    void BeginPass(RHI::Vulkan::VulkanContext& context, std::string_view name);
#if defined(PRISM_RENDER_HAS_D3D12)
    void EndPass(RHI::D3D12Context& context);
#endif
    void EndPass(RHI::Vulkan::VulkanContext& context);
#if defined(PRISM_RENDER_HAS_D3D12)
    void EndFrame(RHI::D3D12Context& context);
#endif
    void EndFrame(RHI::Vulkan::VulkanContext& context);
#if defined(PRISM_RENDER_HAS_D3D12)
    void ResolveSubmittedFrame(RHI::D3D12Context& context);
#endif
    void ResolveSubmittedFrame(RHI::Vulkan::VulkanContext& context);

    float GetMilliseconds(std::string_view name) const;
    const std::vector<Timing>& GetLatestTimings() const;
    const TimelineMetadata& GetTimelineMetadata() const;
    [[nodiscard]] std::uint64_t
        GetLatestTimingGeneration() const;
    [[nodiscard]] std::uint32_t
        GetLatestResolvedFrameIndex() const;

private:
    struct Sample
    {
        std::string name;
        std::uint32_t beginQuery = 0;
        std::uint32_t endQuery = 0;
        RHI::CommandQueueType queue =
            RHI::CommandQueueType::Graphics;
    };

    struct QueueCalibration
    {
        std::uint64_t gpuTimestamp = 0;
        std::uint64_t cpuTimestamp = 0;
        std::uint64_t gpuFrequency = 0;
        bool valid = false;
    };

    struct FrameData
    {
        std::uint64_t submissionGeneration = 0;
        std::uint32_t queryCount = 0;
        std::uint32_t frameBeginQuery = 0;
        std::vector<Sample> samples;
        std::array<QueueCalibration, 2> queueCalibrations{};
    };

    static constexpr std::uint32_t MaxProfiledPasses =
        63;
    static constexpr std::uint32_t MaxQueriesPerFrame =
        2u + MaxProfiledPasses * 2u;
#if defined(PRISM_RENDER_HAS_D3D12)
    std::uint32_t WriteTimestamp(RHI::D3D12Context& context);
#endif
    std::uint32_t WriteTimestamp(RHI::Vulkan::VulkanContext& context);
#if defined(PRISM_RENDER_HAS_D3D12)
    void ReadCompletedD3D12Frame(std::uint32_t frameIndex);
#endif
    void ReadCompletedVulkanFrame(std::uint32_t frameIndex);

#if defined(PRISM_RENDER_HAS_D3D12)
    Microsoft::WRL::ComPtr<ID3D12QueryHeap> m_queryHeap;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_readbackBuffer;
#endif
    VkDevice m_vulkanDevice = VK_NULL_HANDLE;
    std::vector<VkQueryPool> m_vulkanQueryPools;
    float m_vulkanTimestampPeriod = 0.0f;
    std::uint32_t m_vulkanTimestampValidBits = 0;
    std::vector<FrameData> m_frameData;
    std::vector<Sample> m_currentSamples;
    std::vector<Timing> m_latestTimings;
    TimelineMetadata m_timelineMetadata;
    std::string m_activePassName;
    std::uint64_t m_graphicsTimestampFrequency = 0;
    std::uint64_t m_computeTimestampFrequency = 0;
    std::uint64_t m_qpcFrequency = 0;
    std::uint64_t m_nextSubmissionGeneration = 1;
    std::uint64_t m_latestTimingGeneration = 0;
    std::uint32_t m_currentFrameIndex = 0;
    std::uint32_t m_currentQueryCount = 0;
    std::uint32_t m_frameBeginQuery = 0;
    std::uint32_t m_activePassBeginQuery = 0;
    std::uint32_t m_lastSubmittedFrameIndex = 0;
    std::uint32_t m_latestResolvedFrameIndex = 0;
    std::uint32_t m_framesInFlight = 0;
    RHI::CommandQueueType m_activePassQueue =
        RHI::CommandQueueType::Graphics;
    bool m_initialized = false;
    SamplingMode m_requestedSamplingMode = SamplingMode::Frame;
    SamplingMode m_activeSamplingMode = SamplingMode::Frame;
};
} // namespace Prism::RHI
