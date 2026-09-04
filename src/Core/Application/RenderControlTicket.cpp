#include "Core/Application/RenderControlTicket.h"

#include <stdexcept>
#include <utility>

namespace Prism::Core
{
void RenderControlCompletionState::Complete(
    RenderControlAcknowledgement acknowledgement)
{
    {
        std::lock_guard lock(m_mutex);
        if (m_completed)
        {
            throw std::logic_error(
                "Render control completion was published twice.");
        }
        m_acknowledgement = std::move(acknowledgement);
        m_completed = true;
    }
    m_condition.notify_all();
}

void RenderControlCompletionState::Fail(
    std::exception_ptr failure) noexcept
{
    {
        std::lock_guard lock(m_mutex);
        if (m_completed)
        {
            return;
        }
        if (!failure)
        {
            failure = std::make_exception_ptr(std::runtime_error(
                "Render control failed without an exception."));
        }
        m_failure = std::move(failure);
        m_completed = true;
    }
    m_condition.notify_all();
}

RenderControlAcknowledgement
RenderControlCompletionState::Wait() const
{
    std::unique_lock lock(m_mutex);
    m_condition.wait(lock, [this] { return m_completed; });
    if (m_failure)
    {
        std::rethrow_exception(m_failure);
    }
    if (!m_acknowledgement)
    {
        throw std::logic_error(
            "Render control completed without an acknowledgement.");
    }
    return *m_acknowledgement;
}

RenderControlTicket::RenderControlTicket(
    const std::uint64_t acceptanceId,
    const RenderControlCommandId commandId,
    std::shared_ptr<RenderControlCompletionState> completion)
    : m_acceptanceId(acceptanceId),
      m_commandId(commandId),
      m_completion(std::move(completion))
{
    if (m_acceptanceId == 0 || !m_commandId || !m_completion)
    {
        throw std::invalid_argument(
            "Render control ticket requires accepted command identity.");
    }
}

RenderControlTicket::operator bool() const noexcept
{
    return m_acceptanceId != 0 && m_commandId && m_completion != nullptr;
}

std::uint64_t RenderControlTicket::GetAcceptanceId() const noexcept
{
    return m_acceptanceId;
}

RenderControlCommandId RenderControlTicket::GetCommandId() const noexcept
{
    return m_commandId;
}

RenderControlAcknowledgement RenderControlTicket::Wait() const
{
    if (!*this)
    {
        throw std::logic_error(
            "Cannot wait on an empty render control ticket.");
    }
    return m_completion->Wait();
}
} // namespace Prism::Core
