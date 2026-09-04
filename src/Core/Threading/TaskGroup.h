#pragma once

#include "Core/Threading/ITaskExecutor.h"

#include <cstddef>
#include <atomic>
#include <functional>
#include <memory>

namespace Prism::Core
{
class TaskCancellationToken
{
public:
    [[nodiscard]] bool IsCancellationRequested() const noexcept;

private:
    explicit TaskCancellationToken(
        std::shared_ptr<const std::atomic_bool> cancellationRequested);

    std::shared_ptr<const std::atomic_bool> m_cancellationRequested;

    friend class TaskGroup;
};

using CancellableTask = std::function<void(const TaskCancellationToken&)>;

class TaskGroup
{
public:
    TaskGroup();
    ~TaskGroup();

    TaskGroup(const TaskGroup&) = delete;
    TaskGroup& operator=(const TaskGroup&) = delete;
    TaskGroup(TaskGroup&& other) noexcept;
    TaskGroup& operator=(TaskGroup&& other) noexcept;

    void Submit(
        ITaskExecutor& executor,
        Task task,
        TaskExecutionHint hint = TaskExecutionHint::Queue);
    void Submit(
        ITaskExecutor& executor,
        CancellableTask task,
        TaskExecutionHint hint = TaskExecutionHint::Queue);
    void Cancel() noexcept;
    void Wait();

    [[nodiscard]] bool IsComplete() const noexcept;
    [[nodiscard]] std::size_t GetSubmittedTaskCount() const noexcept;
    [[nodiscard]] std::size_t GetPendingTaskCount() const noexcept;
    [[nodiscard]] std::size_t GetCancelledTaskCount() const noexcept;
    [[nodiscard]] bool IsCancellationRequested() const noexcept;

private:
    struct State;

    void SubmitImpl(
        ITaskExecutor& executor,
        CancellableTask task,
        TaskExecutionHint hint);
    void WaitNoThrow() noexcept;

    std::shared_ptr<State> m_state;
};
} // namespace Prism::Core
