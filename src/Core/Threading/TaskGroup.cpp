#include "Core/Threading/TaskGroup.h"

#include "Core/Assert.h"

#include <condition_variable>
#include <exception>
#include <mutex>
#include <stdexcept>
#include <utility>

namespace Prism::Core
{
struct TaskGroup::State
{
    mutable std::mutex mutex;
    std::condition_variable completed;
    std::size_t submittedTaskCount = 0;
    std::size_t pendingTaskCount = 0;
    std::size_t cancelledTaskCount = 0;
    std::exception_ptr firstFailure;
    std::shared_ptr<std::atomic_bool> cancellationRequested =
        std::make_shared<std::atomic_bool>(false);
    ITaskExecutor* executor = nullptr;
    bool sealed = false;
};

TaskCancellationToken::TaskCancellationToken(
    std::shared_ptr<const std::atomic_bool> cancellationRequested)
    : m_cancellationRequested(std::move(cancellationRequested))
{
}

bool TaskCancellationToken::IsCancellationRequested() const noexcept
{
    return m_cancellationRequested
        && m_cancellationRequested->load(std::memory_order_acquire);
}

TaskGroup::TaskGroup()
    : m_state(std::make_shared<State>())
{
}

TaskGroup::~TaskGroup()
{
    WaitNoThrow();
}

TaskGroup::TaskGroup(TaskGroup&& other) noexcept
    : m_state(std::move(other.m_state))
{
}

TaskGroup& TaskGroup::operator=(TaskGroup&& other) noexcept
{
    if (this != &other)
    {
        WaitNoThrow();
        m_state = std::move(other.m_state);
    }
    return *this;
}

void TaskGroup::Submit(
    ITaskExecutor& executor,
    Task task,
    const TaskExecutionHint hint)
{
    Check(static_cast<bool>(task),
        "TaskGroup cannot submit an empty task.");
    Check(static_cast<bool>(m_state),
        "A moved-from TaskGroup cannot accept tasks.");

    SubmitImpl(
        executor,
        [task = std::move(task)](
            const TaskCancellationToken&) mutable
        {
            task();
        },
        hint);
}

void TaskGroup::Submit(
    ITaskExecutor& executor,
    CancellableTask task,
    const TaskExecutionHint hint)
{
    SubmitImpl(executor, std::move(task), hint);
}

void TaskGroup::SubmitImpl(
    ITaskExecutor& executor,
    CancellableTask task,
    const TaskExecutionHint hint)
{
    Check(static_cast<bool>(task),
        "TaskGroup cannot submit an empty task.");
    Check(static_cast<bool>(m_state),
        "A moved-from TaskGroup cannot accept tasks.");

    const std::shared_ptr<State> state = m_state;
    {
        std::lock_guard lock(state->mutex);
        if (state->sealed)
        {
            throw std::runtime_error(
                "TaskGroup cannot accept tasks after waiting has begun.");
        }
        if (state->executor && state->executor != &executor)
        {
            throw std::runtime_error(
                "TaskGroup tasks must use one executor.");
        }
        state->executor = &executor;
        ++state->submittedTaskCount;
        ++state->pendingTaskCount;
    }

    const TaskCancellationToken cancellationToken(
        state->cancellationRequested);
    try
    {
        executor.Submit(
            [state,
             task = std::move(task),
             cancellationToken]() mutable noexcept
            {
                std::exception_ptr failure;
                const bool cancelledBeforeStart =
                    cancellationToken.IsCancellationRequested();
                if (!cancelledBeforeStart)
                {
                    try
                    {
                        task(cancellationToken);
                    }
                    catch (...)
                    {
                        failure = std::current_exception();
                    }
                }

                {
                    std::lock_guard lock(state->mutex);
                    if (cancelledBeforeStart)
                    {
                        ++state->cancelledTaskCount;
                    }
                    if (failure && !state->firstFailure)
                    {
                        state->firstFailure = failure;
                        state->cancellationRequested->store(
                            true, std::memory_order_release);
                    }
                }
            },
            hint,
            [state]() noexcept
            {
                {
                    std::lock_guard lock(state->mutex);
                    --state->pendingTaskCount;
                }
                state->completed.notify_all();
            });
    }
    catch (...)
    {
        {
            std::lock_guard lock(state->mutex);
            --state->submittedTaskCount;
            --state->pendingTaskCount;
            if (state->submittedTaskCount == 0)
            {
                state->executor = nullptr;
            }
        }
        state->completed.notify_all();
        throw;
    }
}

void TaskGroup::Cancel() noexcept
{
    if (m_state)
    {
        m_state->cancellationRequested->store(
            true, std::memory_order_release);
    }
}

void TaskGroup::Wait()
{
    Check(static_cast<bool>(m_state),
        "A moved-from TaskGroup cannot be waited.");

    std::exception_ptr failure;
    {
        std::unique_lock lock(m_state->mutex);
        m_state->sealed = true;
        if (m_state->pendingTaskCount > 0
            && m_state->executor
            && m_state->executor->IsCurrentThreadWorker())
        {
            throw std::runtime_error(
                "TaskGroup cannot block a worker on work from the same "
                "executor.");
        }
        m_state->completed.wait(lock, [this]
        {
            return m_state->pendingTaskCount == 0;
        });
        failure = m_state->firstFailure;
    }
    if (failure)
    {
        std::rethrow_exception(failure);
    }
}

std::size_t TaskGroup::GetCancelledTaskCount() const noexcept
{
    if (!m_state)
    {
        return 0;
    }
    std::lock_guard lock(m_state->mutex);
    return m_state->cancelledTaskCount;
}

bool TaskGroup::IsCancellationRequested() const noexcept
{
    return m_state
        && m_state->cancellationRequested->load(
            std::memory_order_acquire);
}

bool TaskGroup::IsComplete() const noexcept
{
    if (!m_state)
    {
        return true;
    }
    std::lock_guard lock(m_state->mutex);
    return m_state->pendingTaskCount == 0;
}

std::size_t TaskGroup::GetSubmittedTaskCount() const noexcept
{
    if (!m_state)
    {
        return 0;
    }
    std::lock_guard lock(m_state->mutex);
    return m_state->submittedTaskCount;
}

std::size_t TaskGroup::GetPendingTaskCount() const noexcept
{
    if (!m_state)
    {
        return 0;
    }
    std::lock_guard lock(m_state->mutex);
    return m_state->pendingTaskCount;
}

void TaskGroup::WaitNoThrow() noexcept
{
    if (!m_state)
    {
        return;
    }
    try
    {
        Wait();
    }
    catch (...)
    {
    }
}
} // namespace Prism::Core
