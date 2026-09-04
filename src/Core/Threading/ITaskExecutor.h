#pragma once

#include <cstddef>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <exception>
#include <functional>
#include <stdexcept>
#include <string_view>

namespace Prism::Core
{
using Task = std::function<void()>;

enum class TaskExecutionHint
{
    Queue,
    RunInline
};

struct TaskExecutorStatistics
{
    std::string_view executorName = "unknown";
    std::uint64_t submittedTaskCount = 0;
    std::uint64_t completedTaskCount = 0;
    std::uint64_t cumulativeQueueWaitNanoseconds = 0;
    std::uint64_t cumulativeExecutionNanoseconds = 0;
    std::uint64_t cumulativeJoinNanoseconds = 0;
    std::size_t workerCount = 0;
    std::size_t queueCapacity = 0;
    std::size_t activeWorkerCount = 0;
    std::size_t peakActiveWorkerCount = 0;
    std::size_t queuedTaskCount = 0;
    std::size_t peakQueuedTaskCount = 0;
};

class ITaskExecutor
{
public:
    virtual ~ITaskExecutor() = default;

    virtual void Submit(
        Task task,
        TaskExecutionHint hint = TaskExecutionHint::Queue,
        Task completion = {}) = 0;
    virtual void Shutdown() noexcept = 0;
    [[nodiscard]] virtual bool IsAcceptingTasks() const noexcept = 0;
    [[nodiscard]] virtual bool IsCurrentThreadWorker() const noexcept = 0;
    [[nodiscard]] virtual std::string_view GetName() const noexcept = 0;
    [[nodiscard]] virtual TaskExecutorStatistics
        GetStatistics() const noexcept = 0;
    virtual void RecordJoinDuration(
        std::chrono::nanoseconds duration) noexcept = 0;
};

class InlineTaskExecutor final : public ITaskExecutor
{
public:
    void Submit(
        Task task,
        const TaskExecutionHint hint = TaskExecutionHint::Queue,
        Task completion = {}) override
    {
        (void)hint;
        if (!task)
        {
            throw std::runtime_error(
                "InlineTaskExecutor cannot submit an empty task.");
        }
        if (!m_acceptingTasks.load(std::memory_order_acquire))
        {
            throw std::runtime_error(
                "InlineTaskExecutor is no longer accepting tasks.");
        }
        m_submittedTaskCount.fetch_add(1, std::memory_order_relaxed);
        const auto start = std::chrono::steady_clock::now();
        std::exception_ptr failure;
        try
        {
            task();
        }
        catch (...)
        {
            failure = std::current_exception();
        }
        CompleteTask(start);
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
    }

    void Shutdown() noexcept override
    {
        m_acceptingTasks.store(false, std::memory_order_release);
    }

    [[nodiscard]] bool IsAcceptingTasks() const noexcept override
    {
        return m_acceptingTasks.load(std::memory_order_acquire);
    }

    [[nodiscard]] bool IsCurrentThreadWorker() const noexcept override
    {
        return false;
    }

    [[nodiscard]] std::string_view GetName() const noexcept override
    {
        return "inline";
    }

    [[nodiscard]] TaskExecutorStatistics
        GetStatistics() const noexcept override
    {
        return {
            "inline",
            m_submittedTaskCount.load(std::memory_order_relaxed),
            m_completedTaskCount.load(std::memory_order_relaxed),
            0,
            m_executionNanoseconds.load(std::memory_order_relaxed),
            m_joinNanoseconds.load(std::memory_order_relaxed),
            0,
            0,
            0,
            0,
            0,
            0};
    }

    void RecordJoinDuration(
        const std::chrono::nanoseconds duration) noexcept override
    {
        m_joinNanoseconds.fetch_add(
            static_cast<std::uint64_t>(duration.count()),
            std::memory_order_relaxed);
    }

private:
    void CompleteTask(
        const std::chrono::steady_clock::time_point start) noexcept
    {
        const auto elapsed = std::chrono::duration_cast<
            std::chrono::nanoseconds>(
                std::chrono::steady_clock::now() - start);
        m_executionNanoseconds.fetch_add(
            static_cast<std::uint64_t>(elapsed.count()),
            std::memory_order_relaxed);
        m_completedTaskCount.fetch_add(1, std::memory_order_relaxed);
    }

    std::atomic_bool m_acceptingTasks = true;
    std::atomic_uint64_t m_submittedTaskCount = 0;
    std::atomic_uint64_t m_completedTaskCount = 0;
    std::atomic_uint64_t m_executionNanoseconds = 0;
    std::atomic_uint64_t m_joinNanoseconds = 0;
};
} // namespace Prism::Core
