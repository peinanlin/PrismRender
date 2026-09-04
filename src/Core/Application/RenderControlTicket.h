#pragma once

#include "Core/Application/RenderControlCommand.h"

#include <condition_variable>
#include <cstdint>
#include <exception>
#include <memory>
#include <mutex>
#include <optional>

namespace Prism::Core
{
class RenderControlCompletionState final
{
public:
    void Complete(RenderControlAcknowledgement acknowledgement);
    void Fail(std::exception_ptr failure) noexcept;
    [[nodiscard]] RenderControlAcknowledgement Wait() const;

private:
    mutable std::mutex m_mutex;
    mutable std::condition_variable m_condition;
    std::optional<RenderControlAcknowledgement> m_acknowledgement;
    std::exception_ptr m_failure;
    bool m_completed = false;
};

class RenderControlTicket final
{
public:
    RenderControlTicket() = default;
    RenderControlTicket(
        std::uint64_t acceptanceId,
        RenderControlCommandId commandId,
        std::shared_ptr<RenderControlCompletionState> completion);

    [[nodiscard]] explicit operator bool() const noexcept;
    [[nodiscard]] std::uint64_t GetAcceptanceId() const noexcept;
    [[nodiscard]] RenderControlCommandId GetCommandId() const noexcept;
    [[nodiscard]] RenderControlAcknowledgement Wait() const;

private:
    std::uint64_t m_acceptanceId = 0;
    RenderControlCommandId m_commandId;
    std::shared_ptr<RenderControlCompletionState> m_completion;
};
} // namespace Prism::Core
