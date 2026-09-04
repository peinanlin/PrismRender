#pragma once

#include "Core/Threading/ITaskExecutor.h"

#include <condition_variable>
#include <cstddef>
#include <deque>
#include <exception>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

namespace Prism::Core
{
struct TaskSchedulerConfig
{
    std::size_t workerCount = 1;
    std::size_t queueCapacity = 64;
};

class TaskScheduler final : public ITaskExecutor
{
public:
    static constexpr std::size_t MaximumWorkerCount = 64;
    static constexpr std::size_t MaximumQueueCapacity = 65536;

    using WorkerEntry = std::function<void()>;
    using WorkerFactory = std::function<std::thread(WorkerEntry)>;

    explicit TaskScheduler(
        TaskSchedulerConfig config,
        WorkerFactory workerFactory = {});
    ~TaskScheduler() override;

    TaskScheduler(const TaskScheduler&) = delete;
    TaskScheduler& operator=(const TaskScheduler&) = delete;
    TaskScheduler(TaskScheduler&&) = delete;
    TaskScheduler& operator=(TaskScheduler&&) = delete;

    void Submit(
        Task task,
        TaskExecutionHint hint = TaskExecutionHint::Queue,
        Task completion = {}) override;
    void Shutdown() noexcept override;
    [[nodiscard]] bool IsAcceptingTasks() const noexcept override;
    [[nodiscard]] bool IsCurrentThreadWorker() const noexcept override;
    [[nodiscard]] std::string_view GetName() const noexcept override;
    [[nodiscard]] TaskExecutorStatistics
        GetStatistics() const noexcept override;
    void RecordJoinDuration(
        std::chrono::nanoseconds duration) noexcept override;

    [[nodiscard]] std::size_t GetWorkerCount() const noexcept;
    [[nodiscard]] std::size_t GetQueueCapacity() const noexcept;
    [[nodiscard]] std::size_t GetQueuedTaskCount() const noexcept;
    [[nodiscard]] std::size_t GetWaitingSubmitterCount() const noexcept;
    [[nodiscard]] std::size_t GetPeakQueueDepth() const noexcept;
    [[nodiscard]] std::size_t GetPeakActiveWorkerCount() const noexcept;
    [[nodiscard]] std::size_t GetUnhandledTaskFailureCount() const noexcept;
    void RethrowUnhandledTaskFailure() const;

private:
    struct ScheduledTask
    {
        Task task;
        Task completion;
        std::chrono::steady_clock::time_point queuedAt;
    };

    void WorkerLoop() noexcept;
    void StopAfterStartupFailure() noexcept;

    TaskSchedulerConfig m_config;
    mutable std::mutex m_mutex;
    std::condition_variable m_taskAvailable;
    std::condition_variable m_queueSpaceAvailable;
    std::condition_variable m_inlineTasksCompleted;
    std::condition_variable m_shutdownCompleted;
    std::deque<ScheduledTask> m_tasks;
    std::vector<std::thread> m_workers;
    bool m_acceptingTasks = true;
    bool m_stopping = false;
    bool m_shutdownInProgress = false;
    bool m_shutdownComplete = false;
    std::size_t m_waitingSubmitterCount = 0;
    std::size_t m_activeInlineTaskCount = 0;
    std::size_t m_activeWorkerCount = 0;
    std::size_t m_peakQueueDepth = 0;
    std::size_t m_peakActiveWorkerCount = 0;
    std::size_t m_unhandledTaskFailureCount = 0;
    std::exception_ptr m_firstUnhandledTaskFailure;
    std::uint64_t m_submittedTaskCount = 0;
    std::uint64_t m_completedTaskCount = 0;
    std::uint64_t m_cumulativeQueueWaitNanoseconds = 0;
    std::uint64_t m_cumulativeExecutionNanoseconds = 0;
    std::uint64_t m_cumulativeJoinNanoseconds = 0;
};
} // namespace Prism::Core
