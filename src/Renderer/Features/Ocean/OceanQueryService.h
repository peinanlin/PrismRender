#pragma once

#include "Renderer/Features/Ocean/OceanSettings.h"

#include <cstdint>
#include <deque>
#include <span>
#include <unordered_map>
#include <vector>

namespace Prism::Renderer
{
struct OceanDisplacementBounds
{
    float vertical = 0.0f;
    float horizontal = 0.0f;
    float radius = 0.0f;
    std::uint64_t settingsVersion = 0u;
};

enum class OceanQueryState : std::uint8_t
{
    Unavailable,
    Pending,
    Completed,
    Expired
};

struct OceanQueryPoint
{
    DirectX::XMFLOAT2 worldPosition{};
};

struct OceanDisplacementSample
{
    DirectX::XMFLOAT3 displacement{};
};

struct OceanQueryResult
{
    std::uint64_t sequenceId = 0u;
    double simulationTimeSeconds = 0.0;
    std::uint64_t submittedFrame = 0u;
    std::uint64_t completedFrame = 0u;
    OceanQueryState state = OceanQueryState::Unavailable;
    std::vector<OceanDisplacementSample> samples;
};

class OceanQueryService
{
public:
    void Configure(const OceanSettings& settings) noexcept;
    void Reset() noexcept;

    // Submission and completion are deliberately separate.  The renderer
    // submits a GPU batch without waiting; the backend calls CompleteGpuBatch
    // after its copy/readback fence becomes available on a later frame.
    [[nodiscard]] bool SubmitGpuBatch(
        std::uint64_t sequenceId,
        double simulationTimeSeconds,
        std::uint64_t submittedFrame,
        std::span<const OceanQueryPoint> points);
    [[nodiscard]] bool CompleteGpuBatch(
        std::uint64_t sequenceId,
        std::uint64_t completedFrame,
        std::span<const OceanDisplacementSample> samples);
    [[nodiscard]] OceanQueryState Poll(
        std::uint64_t sequenceId,
        OceanQueryResult* result = nullptr) const;
    [[nodiscard]] bool RequestCpuReadback(
        std::uint64_t sequenceId) noexcept;
    void AdvanceFrame(std::uint64_t frameSerial) noexcept;
    [[nodiscard]] OceanQueryState FindHistorical(
        double simulationTimeSeconds,
        OceanQueryResult* result = nullptr) const;
    [[nodiscard]] std::size_t PendingCount() const noexcept;
    [[nodiscard]] std::size_t CompletedCount() const noexcept;
    [[nodiscard]] std::uint64_t ExpiredCount() const noexcept
    {
        return m_expiredCount;
    }
    [[nodiscard]] float LastReadbackLatencyFrames() const noexcept
    {
        return m_lastReadbackLatencyFrames;
    }
    [[nodiscard]] std::uint64_t ReadbackCopyRequestCount() const noexcept
    {
        return m_readbackCopyRequestCount;
    }
    [[nodiscard]] std::uint64_t GpuBatchCount() const noexcept
    {
        return m_gpuBatchCount;
    }
    [[nodiscard]] bool HasAllocatedQueryResource() const noexcept
    {
        return !m_records.empty();
    }

    [[nodiscard]] const OceanDisplacementBounds& Bounds() const noexcept
    {
        return m_bounds;
    }
    [[nodiscard]] std::uint64_t SettingsVersion() const noexcept
    {
        return m_settingsVersion;
    }

    [[nodiscard]] static OceanDisplacementBounds EstimateBounds(
        const OceanSettings& settings) noexcept;

private:
    struct QueryRecord
    {
        OceanQueryResult result;
        std::vector<OceanQueryPoint> points;
        bool cpuReadbackRequested = false;
    };

    void TrimHistory() noexcept;

    OceanDisplacementBounds m_bounds{};
    std::uint64_t m_settingsVersion = 0u;
    bool m_gpuQueriesEnabled = true;
    bool m_cpuReadbackEnabled = true;
    std::uint32_t m_historyCapacity = 30u;
    std::uint64_t m_expiredCount = 0u;
    std::uint64_t m_gpuBatchCount = 0u;
    std::uint64_t m_readbackCopyRequestCount = 0u;
    float m_lastReadbackLatencyFrames = 0.0f;
    std::unordered_map<std::uint64_t, QueryRecord> m_records;
    std::deque<std::uint64_t> m_completedHistory;
};
} // namespace Prism::Renderer
