#pragma once

#include "Core/Application/RenderControlCommand.h"
#include "Core/Application/RenderControlTicket.h"
#include "Core/Application/RenderFrameExecutionState.h"

#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <exception>
#include <memory>
#include <mutex>
#include <optional>
#include <variant>

namespace Prism::Core
{
struct QueuedRenderControl
{
    RenderControlCommand command;
    std::shared_ptr<RenderControlCompletionState> completion;
};

using RenderExecutionPayload = std::variant<
    FrameEnvelope,
    QueuedRenderControl>;

struct RenderFrameQueueEntry
{
    std::uint64_t acceptanceId = 0;
    std::uint64_t acceptedTimestampNanoseconds = 0;
    std::size_t waitingDepthAtAcceptance = 0;
    RenderExecutionPayload payload;
};

enum class RenderFrameQueueState : std::uint8_t
{
    Open,
    Draining,
    Closed,
    Failed
};

struct RenderFrameQueueStatistics
{
    std::uint64_t acceptedCount = 0;
    std::uint64_t consumedCount = 0;
    std::uint64_t cancelledCount = 0;
    std::size_t waitingCount = 0;
    std::size_t peakWaitingCount = 0;
    std::size_t waitingProducerCount = 0;
    std::size_t capacity = 0;
    RenderFrameQueueState state = RenderFrameQueueState::Open;
};

class RenderFrameQueue final
{
public:
    static constexpr std::size_t DefaultCapacity = 1;
    static constexpr std::size_t MaximumCapacity = 64;

    explicit RenderFrameQueue(
        std::size_t capacity = DefaultCapacity);

    RenderFrameQueue(const RenderFrameQueue&) = delete;
    RenderFrameQueue& operator=(const RenderFrameQueue&) = delete;

    [[nodiscard]] std::uint64_t Push(FrameEnvelope frame);
    [[nodiscard]] std::uint64_t Push(RenderControlCommand command);
    [[nodiscard]] std::uint64_t Push(QueuedRenderControl command);
    [[nodiscard]] std::optional<std::uint64_t>
        TryPush(FrameEnvelope frame);
    [[nodiscard]] std::optional<std::uint64_t>
        TryPush(RenderControlCommand command);
    [[nodiscard]] std::optional<std::uint64_t>
        TryPush(QueuedRenderControl command);
    [[nodiscard]] std::optional<RenderFrameQueueEntry> WaitPop();

    void Close() noexcept;
    void Fail(std::exception_ptr failure) noexcept;
    void RethrowFailure() const;

    [[nodiscard]] RenderFrameQueueStatistics
        GetStatistics() const noexcept;

private:
    [[nodiscard]] std::uint64_t PushPayload(
        RenderExecutionPayload payload);
    [[nodiscard]] std::optional<std::uint64_t> TryPushPayload(
        RenderExecutionPayload payload);
    void ValidatePayload(const RenderExecutionPayload& payload) const;
    [[nodiscard]] std::uint64_t AcceptPayload(
        RenderExecutionPayload payload);

    const std::size_t m_capacity;
    mutable std::mutex m_mutex;
    std::condition_variable m_workAvailable;
    std::condition_variable m_spaceAvailable;
    std::deque<RenderFrameQueueEntry> m_entries;
    RenderFrameQueueState m_state = RenderFrameQueueState::Open;
    std::exception_ptr m_failure;
    std::uint64_t m_nextAcceptanceId = 1;
    std::uint64_t m_acceptedCount = 0;
    std::uint64_t m_consumedCount = 0;
    std::uint64_t m_cancelledCount = 0;
    std::uint64_t m_lastLogicalFrameId = 0;
    std::uint64_t m_pendingBeforeFrameId = 0;
    std::uint64_t m_lastControlCommandId = 0;
    std::uint64_t m_lastSceneEpoch = 0;
    std::uint64_t m_lastViewEpoch = 0;
    std::uint64_t m_lastSettingsRevision = 0;
    std::size_t m_peakWaitingCount = 0;
    std::size_t m_waitingProducerCount = 0;
};
} // namespace Prism::Core
