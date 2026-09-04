#include "Renderer/Features/Ocean/OceanQueryService.h"

#include <algorithm>
#include <cmath>
#include <utility>

namespace Prism::Renderer
{
namespace
{
float FiniteNonNegative(const float value) noexcept
{
    return std::isfinite(value) ? std::max(value, 0.0f) : 0.0f;
}
}

OceanDisplacementBounds OceanQueryService::EstimateBounds(
    const OceanSettings& settings) noexcept
{
    // This is deliberately an upper bound, not a prediction of the current
    // crest.  Wind/swell energy is converted to a conservative height range,
    // then choppiness and local interaction are added before culling runs.
    const float windEnergy =
        0.50f * FiniteNonNegative(settings.baseWind.speed)
        * std::sqrt(std::max(
            FiniteNonNegative(settings.baseWind.fetchKilometers), 0.001f))
        * FiniteNonNegative(settings.baseWind.amplitudeMultiplier);
    const float swellEnergy =
        0.16f * FiniteNonNegative(settings.swell.speed)
        * std::sqrt(std::max(
            FiniteNonNegative(settings.swell.fetchKilometers), 0.001f))
        * FiniteNonNegative(settings.swell.amplitudeMultiplier);
    const float vertical = std::clamp(
        0.25f + windEnergy + swellEnergy,
        0.25f,
        512.0f);
    const float horizontal = std::clamp(
        vertical
            * (0.75f + 1.25f * FiniteNonNegative(settings.lateralMultiplier)),
        0.25f,
        1024.0f);
    const float local = settings.local.enabled
        ? std::clamp(
            0.20f * FiniteNonNegative(settings.local.domainSizeMeters)
                * FiniteNonNegative(settings.local.amplitudeMultiplier),
            0.0f,
            256.0f)
        : 0.0f;
    OceanDisplacementBounds bounds{};
    bounds.vertical = vertical + local;
    bounds.horizontal = horizontal + local;
    bounds.radius = std::sqrt(
        bounds.vertical * bounds.vertical
        + bounds.horizontal * bounds.horizontal);
    return bounds;
}

void OceanQueryService::Configure(const OceanSettings& settings) noexcept
{
    m_gpuQueriesEnabled = settings.query.enableGpuQueries;
    m_cpuReadbackEnabled = settings.query.enableCpuReadback;
    m_historyCapacity = std::clamp(settings.query.readbackFifoEntries, 1u, 1024u);
    TrimHistory();
    const OceanDisplacementBounds next = EstimateBounds(settings);
    if (next.vertical == m_bounds.vertical
        && next.horizontal == m_bounds.horizontal
        && next.radius == m_bounds.radius)
    {
        return;
    }
    m_bounds = next;
    ++m_settingsVersion;
    m_bounds.settingsVersion = m_settingsVersion;
}

void OceanQueryService::Reset() noexcept
{
    const bool alreadyReset = m_bounds.radius == 0.0f
        && m_bounds.settingsVersion == m_settingsVersion
        && m_records.empty();
    if (alreadyReset)
    {
        return;
    }
    m_bounds = {};
    ++m_settingsVersion;
    m_bounds.settingsVersion = m_settingsVersion;
    m_records.clear();
    m_completedHistory.clear();
    m_expiredCount = 0u;
    m_gpuBatchCount = 0u;
    m_readbackCopyRequestCount = 0u;
    m_lastReadbackLatencyFrames = 0.0f;
}

bool OceanQueryService::SubmitGpuBatch(
    const std::uint64_t sequenceId,
    const double simulationTimeSeconds,
    const std::uint64_t submittedFrame,
    const std::span<const OceanQueryPoint> points)
{
    if (!m_gpuQueriesEnabled || sequenceId == 0u || points.empty()
        || m_records.contains(sequenceId))
    {
        return false;
    }
    QueryRecord record{};
    record.result.sequenceId = sequenceId;
    record.result.simulationTimeSeconds = simulationTimeSeconds;
    record.result.submittedFrame = submittedFrame;
    record.result.state = OceanQueryState::Pending;
    record.points.assign(points.begin(), points.end());
    m_records.emplace(sequenceId, std::move(record));
    ++m_gpuBatchCount;
    return true;
}

bool OceanQueryService::CompleteGpuBatch(
    const std::uint64_t sequenceId,
    const std::uint64_t completedFrame,
    const std::span<const OceanDisplacementSample> samples)
{
    const auto iterator = m_records.find(sequenceId);
    if (iterator == m_records.end()
        || iterator->second.result.state != OceanQueryState::Pending
        || samples.size() != iterator->second.points.size())
    {
        return false;
    }
    QueryRecord& record = iterator->second;
    record.result.completedFrame = completedFrame;
    m_lastReadbackLatencyFrames = static_cast<float>(
        completedFrame >= record.result.submittedFrame
            ? completedFrame - record.result.submittedFrame
            : 0u);
    record.result.samples.assign(samples.begin(), samples.end());
    record.result.state = OceanQueryState::Completed;
    m_completedHistory.push_back(sequenceId);
    TrimHistory();
    return true;
}

OceanQueryState OceanQueryService::Poll(
    const std::uint64_t sequenceId,
    OceanQueryResult* result) const
{
    const auto iterator = m_records.find(sequenceId);
    if (iterator == m_records.end())
    {
        return OceanQueryState::Unavailable;
    }
    if (result != nullptr)
    {
        *result = iterator->second.result;
        if (!iterator->second.cpuReadbackRequested
            && result->state == OceanQueryState::Completed)
        {
            result->samples.clear();
        }
    }
    return iterator->second.result.state;
}

bool OceanQueryService::RequestCpuReadback(
    const std::uint64_t sequenceId) noexcept
{
    if (!m_cpuReadbackEnabled)
    {
        return false;
    }
    const auto iterator = m_records.find(sequenceId);
    if (iterator == m_records.end()
        || iterator->second.result.state == OceanQueryState::Expired)
    {
        return false;
    }
    if (!iterator->second.cpuReadbackRequested)
    {
        iterator->second.cpuReadbackRequested = true;
        ++m_readbackCopyRequestCount;
    }
    return true;
}

void OceanQueryService::AdvanceFrame(
    const std::uint64_t frameSerial) noexcept
{
    for (auto& [sequenceId, record] : m_records)
    {
        (void)sequenceId;
        if (record.result.state == OceanQueryState::Pending
            && frameSerial > record.result.submittedFrame + 120u)
        {
            record.result.state = OceanQueryState::Expired;
            ++m_expiredCount;
        }
    }
}

OceanQueryState OceanQueryService::FindHistorical(
    const double simulationTimeSeconds,
    OceanQueryResult* result) const
{
    for (auto iterator = m_completedHistory.rbegin();
         iterator != m_completedHistory.rend();
         ++iterator)
    {
        const auto recordIterator = m_records.find(*iterator);
        if (recordIterator == m_records.end())
        {
            continue;
        }
        const QueryRecord& record = recordIterator->second;
        if (record.result.simulationTimeSeconds == simulationTimeSeconds)
        {
            if (result != nullptr)
            {
                *result = record.result;
                if (!record.cpuReadbackRequested)
                {
                    result->samples.clear();
                }
            }
            return record.result.state;
        }
    }
    return OceanQueryState::Expired;
}

std::size_t OceanQueryService::PendingCount() const noexcept
{
    std::size_t count = 0u;
    for (const auto& [sequenceId, record] : m_records)
    {
        (void)sequenceId;
        count += record.result.state == OceanQueryState::Pending ? 1u : 0u;
    }
    return count;
}

std::size_t OceanQueryService::CompletedCount() const noexcept
{
    return m_completedHistory.size();
}

void OceanQueryService::TrimHistory() noexcept
{
    while (m_completedHistory.size() > m_historyCapacity)
    {
        const std::uint64_t sequenceId = m_completedHistory.front();
        m_completedHistory.pop_front();
        const auto iterator = m_records.find(sequenceId);
        if (iterator != m_records.end()
            && iterator->second.result.state == OceanQueryState::Completed)
        {
            iterator->second.result.state = OceanQueryState::Expired;
            iterator->second.result.samples.clear();
            ++m_expiredCount;
        }
    }
}
} // namespace Prism::Renderer
