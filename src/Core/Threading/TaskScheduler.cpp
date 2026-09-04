#include "Core/Threading/TaskScheduler.h"

#include "Core/Assert.h"

#include <algorithm>
#include <exception>
#include <stdexcept>
#include <utility>

namespace Prism::Core
{
namespace
{
thread_local const TaskScheduler* CurrentWorkerScheduler = nullptr;

std::thread StartWorker(TaskScheduler::WorkerEntry entry)
{
    return std::thread(std::move(entry));
}
} // namespace

TaskScheduler::TaskScheduler(
    const TaskSchedulerConfig config,
    WorkerFactory workerFactory)
    : m_config(config)
{
    Check(m_config.workerCount > 0,
        "TaskScheduler requires at least one worker.");
    Check(m_config.workerCount <= MaximumWorkerCount,
        "TaskScheduler worker count exceeds its fixed limit.");
    Check(m_config.queueCapacity > 0,
        "TaskScheduler requires a non-zero queue capacity.");
    Check(m_config.queueCapacity <= MaximumQueueCapacity,
        "TaskScheduler queue capacity exceeds its fixed limit.");

    if (!workerFactory)
    {
        workerFactory = StartWorker;
    }

    m_workers.reserve(m_config.workerCount);
    try
    {
        for (std::size_t workerIndex = 0;
             workerIndex < m_config.workerCount;
             ++workerIndex)
        {
            m_workers.emplace_back(
                workerFactory([this] { WorkerLoop(); }));
            Check(m_workers.back().joinable(),
                "TaskScheduler worker factory returned a non-joinable thread.");
        }
    }
    catch (...)
    {
        StopAfterStartupFailure();
        throw;
    }
}

TaskScheduler::~TaskScheduler()
{
    Shutdown();
}

void TaskScheduler::Submit(
    Task task,
    const TaskExecutionHint hint,
    Task completion)
{
    Check(static_cast<bool>(task),
        "TaskScheduler cannot submit an empty task.");

    if (hint == TaskExecutionHint::RunInline)
    {
        const auto executionStart =
            std::chrono::steady_clock::now();
        {
            std::lock_guard lock(m_mutex);
            if (!m_acceptingTasks)
            {
                throw std::runtime_error(
                    "TaskScheduler is no longer accepting tasks.");
            }
            ++m_activeInlineTaskCount;
            ++m_submittedTaskCount;
        }
        std::exception_ptr failure;
        try
        {
            task();
        }
        catch (...)
        {
            failure = std::current_exception();
        }
        {
            std::lock_guard lock(m_mutex);
            --m_activeInlineTaskCount;
            ++m_completedTaskCount;
            m_cumulativeExecutionNanoseconds +=
                static_cast<std::uint64_t>(
                    std::chrono::duration_cast<
                        std::chrono::nanoseconds>(
                            std::chrono::steady_clock::now()
                            - executionStart).count());
        }
        m_inlineTasksCompleted.notify_all();
        try
        {
            if (completion)
            {
                completion();
            }
        }
        catch (...)
        {
            if (!failure)
            {
                failure = std::current_exception();
            }
        }
        if (failure)
        {
            std::rethrow_exception(failure);
        }
        return;
    }

    std::unique_lock lock(m_mutex);
    if (m_acceptingTasks
        && m_tasks.size() >= m_config.queueCapacity)
    {
        ++m_waitingSubmitterCount;
        m_queueSpaceAvailable.wait(lock, [this]
        {
            return !m_acceptingTasks
                || m_tasks.size() < m_config.queueCapacity;
        });
        --m_waitingSubmitterCount;
    }
    if (!m_acceptingTasks)
    {
        throw std::runtime_error(
            "TaskScheduler is no longer accepting tasks.");
    }
    m_tasks.push_back({
        std::move(task),
        std::move(completion),
        std::chrono::steady_clock::now()});
    ++m_submittedTaskCount;
    m_peakQueueDepth = (std::max)(m_peakQueueDepth, m_tasks.size());
    lock.unlock();
    m_taskAvailable.notify_one();
}

void TaskScheduler::Shutdown() noexcept
{
    if (IsCurrentThreadWorker())
    {
        {
            std::lock_guard lock(m_mutex);
            m_acceptingTasks = false;
            m_stopping = true;
        }
        m_taskAvailable.notify_all();
        m_queueSpaceAvailable.notify_all();
        return;
    }

    std::vector<std::thread> workers;
    {
        std::unique_lock lock(m_mutex);
        if (m_shutdownComplete)
        {
            return;
        }
        if (m_shutdownInProgress)
        {
            m_shutdownCompleted.wait(lock, [this]
            {
                return m_shutdownComplete;
            });
            return;
        }
        m_shutdownInProgress = true;
        m_acceptingTasks = false;
        m_stopping = true;
        workers.swap(m_workers);
    }

    m_taskAvailable.notify_all();
    m_queueSpaceAvailable.notify_all();
    for (std::thread& worker : workers)
    {
        if (worker.joinable())
        {
            worker.join();
        }
    }
    std::unique_lock lock(m_mutex);
    m_inlineTasksCompleted.wait(lock, [this]
    {
        return m_activeInlineTaskCount == 0;
    });
    m_shutdownComplete = true;
    m_shutdownInProgress = false;
    lock.unlock();
    m_shutdownCompleted.notify_all();
}

bool TaskScheduler::IsAcceptingTasks() const noexcept
{
    std::lock_guard lock(m_mutex);
    return m_acceptingTasks;
}

bool TaskScheduler::IsCurrentThreadWorker() const noexcept
{
    return CurrentWorkerScheduler == this;
}

std::string_view TaskScheduler::GetName() const noexcept
{
    return "pool";
}

TaskExecutorStatistics TaskScheduler::GetStatistics() const noexcept
{
    std::lock_guard lock(m_mutex);
    return {
        "pool",
        m_submittedTaskCount,
        m_completedTaskCount,
        m_cumulativeQueueWaitNanoseconds,
        m_cumulativeExecutionNanoseconds,
        m_cumulativeJoinNanoseconds,
        m_config.workerCount,
        m_config.queueCapacity,
        m_activeWorkerCount,
        m_peakActiveWorkerCount,
        m_tasks.size(),
        m_peakQueueDepth};
}

void TaskScheduler::RecordJoinDuration(
    const std::chrono::nanoseconds duration) noexcept
{
    std::lock_guard lock(m_mutex);
    m_cumulativeJoinNanoseconds +=
        static_cast<std::uint64_t>(duration.count());
}

std::size_t TaskScheduler::GetWorkerCount() const noexcept
{
    return m_config.workerCount;
}

std::size_t TaskScheduler::GetQueueCapacity() const noexcept
{
    return m_config.queueCapacity;
}

std::size_t TaskScheduler::GetQueuedTaskCount() const noexcept
{
    std::lock_guard lock(m_mutex);
    return m_tasks.size();
}

std::size_t TaskScheduler::GetWaitingSubmitterCount() const noexcept
{
    std::lock_guard lock(m_mutex);
    return m_waitingSubmitterCount;
}

std::size_t TaskScheduler::GetPeakQueueDepth() const noexcept
{
    std::lock_guard lock(m_mutex);
    return m_peakQueueDepth;
}

std::size_t TaskScheduler::GetPeakActiveWorkerCount() const noexcept
{
    std::lock_guard lock(m_mutex);
    return m_peakActiveWorkerCount;
}

std::size_t TaskScheduler::GetUnhandledTaskFailureCount() const noexcept
{
    std::lock_guard lock(m_mutex);
    return m_unhandledTaskFailureCount;
}

void TaskScheduler::RethrowUnhandledTaskFailure() const
{
    std::exception_ptr failure;
    {
        std::lock_guard lock(m_mutex);
        failure = m_firstUnhandledTaskFailure;
    }
    if (failure)
    {
        std::rethrow_exception(failure);
    }
}

void TaskScheduler::WorkerLoop() noexcept
{
    CurrentWorkerScheduler = this;
    for (;;)
    {
        Task task;
        Task completion;
        std::chrono::steady_clock::time_point queuedAt;
        {
            std::unique_lock lock(m_mutex);
            m_taskAvailable.wait(lock, [this]
            {
                return m_stopping || !m_tasks.empty();
            });
            if (m_tasks.empty())
            {
                if (m_stopping)
                {
                    CurrentWorkerScheduler = nullptr;
                    return;
                }
                continue;
            }

            task = std::move(m_tasks.front().task);
            completion = std::move(
                m_tasks.front().completion);
            queuedAt = m_tasks.front().queuedAt;
            m_tasks.pop_front();
            m_cumulativeQueueWaitNanoseconds +=
                static_cast<std::uint64_t>(
                    std::chrono::duration_cast<
                        std::chrono::nanoseconds>(
                            std::chrono::steady_clock::now()
                            - queuedAt).count());
            ++m_activeWorkerCount;
            m_peakActiveWorkerCount = (std::max)(
                m_peakActiveWorkerCount,
                m_activeWorkerCount);
        }
        m_queueSpaceAvailable.notify_one();

        const auto executionStart =
            std::chrono::steady_clock::now();
        std::exception_ptr failure;
        try
        {
            task();
        }
        catch (...)
        {
            failure = std::current_exception();
        }
        {
            std::lock_guard lock(m_mutex);
            if (failure)
            {
                ++m_unhandledTaskFailureCount;
                if (!m_firstUnhandledTaskFailure)
                {
                    m_firstUnhandledTaskFailure = failure;
                }
            }
            --m_activeWorkerCount;
            ++m_completedTaskCount;
            m_cumulativeExecutionNanoseconds +=
                static_cast<std::uint64_t>(
                    std::chrono::duration_cast<
                        std::chrono::nanoseconds>(
                            std::chrono::steady_clock::now()
                            - executionStart).count());
        }
        try
        {
            if (completion)
            {
                completion();
            }
        }
        catch (...)
        {
            std::lock_guard lock(m_mutex);
            ++m_unhandledTaskFailureCount;
            if (!m_firstUnhandledTaskFailure)
            {
                m_firstUnhandledTaskFailure =
                    std::current_exception();
            }
        }
    }
}

void TaskScheduler::StopAfterStartupFailure() noexcept
{
    {
        std::lock_guard lock(m_mutex);
        m_acceptingTasks = false;
        m_stopping = true;
    }
    m_taskAvailable.notify_all();
    m_queueSpaceAvailable.notify_all();
    for (std::thread& worker : m_workers)
    {
        if (worker.joinable())
        {
            worker.join();
        }
    }
    m_workers.clear();
}
} // namespace Prism::Core
