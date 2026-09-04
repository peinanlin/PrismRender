#include "Core/Threading/TaskGroup.h"
#include "Core/Threading/TaskExecutorFactory.h"
#include "Core/Threading/TaskScheduler.h"

#include <atomic>
#include <array>
#include <condition_variable>
#include <exception>
#include <iostream>
#include <memory>
#include <mutex>
#include <set>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace
{
class TestEvent
{
public:
    void Signal()
    {
        {
            std::lock_guard lock(m_mutex);
            m_signaled = true;
        }
        m_condition.notify_all();
    }

    void Wait()
    {
        std::unique_lock lock(m_mutex);
        m_condition.wait(lock, [this] { return m_signaled; });
    }

private:
    std::mutex m_mutex;
    std::condition_variable m_condition;
    bool m_signaled = false;
};

void Expect(const bool condition, const char* const message)
{
    if (!condition)
    {
        throw std::runtime_error(message);
    }
}

template <typename Callback>
void Reject(Callback&& callback)
{
    bool rejected = false;
    try
    {
        callback();
    }
    catch (const std::exception&)
    {
        rejected = true;
    }
    Expect(rejected, "Invalid task scheduler input was accepted.");
}

class ManualTaskExecutor final : public Prism::Core::ITaskExecutor
{
public:
    void Submit(
        Prism::Core::Task task,
        const Prism::Core::TaskExecutionHint hint =
            Prism::Core::TaskExecutionHint::Queue,
        Prism::Core::Task completion = {}) override
    {
        if (!m_acceptingTasks)
        {
            throw std::runtime_error(
                "Manual executor is no longer accepting tasks.");
        }
        submissionOrder.push_back(tasks.size());
        submissionHints.push_back(hint);
        tasks.emplace_back(std::move(task));
        completions.emplace_back(std::move(completion));
    }

    void Shutdown() noexcept override
    {
        m_acceptingTasks = false;
    }

    [[nodiscard]] bool IsAcceptingTasks() const noexcept override
    {
        return m_acceptingTasks;
    }

    [[nodiscard]] bool IsCurrentThreadWorker() const noexcept override
    {
        return false;
    }

    [[nodiscard]] std::string_view GetName() const noexcept override
    {
        return "manual";
    }

    [[nodiscard]] Prism::Core::TaskExecutorStatistics
        GetStatistics() const noexcept override
    {
        return {
            "manual",
            tasks.size(),
            completedTaskCount};
    }

    void RecordJoinDuration(
        std::chrono::nanoseconds duration) noexcept override
    {
        joinNanoseconds +=
            static_cast<std::uint64_t>(duration.count());
    }

    void Execute(const std::size_t index)
    {
        Prism::Core::Task task = std::move(tasks.at(index));
        task();
        ++completedTaskCount;
        Prism::Core::Task completion =
            std::move(completions.at(index));
        if (completion)
        {
            completion();
        }
    }

    std::vector<std::size_t> submissionOrder;
    std::vector<Prism::Core::TaskExecutionHint> submissionHints;
    std::vector<Prism::Core::Task> tasks;
    std::vector<Prism::Core::Task> completions;
    std::uint64_t completedTaskCount = 0;
    std::uint64_t joinNanoseconds = 0;

private:
    bool m_acceptingTasks = true;
};

void VerifyExecutorContract(Prism::Core::ITaskExecutor& executor)
{
    using namespace Prism::Core;

    std::array<int, 4> results{};
    TaskGroup resultsGroup;
    for (std::size_t index = 0; index < results.size(); ++index)
    {
        resultsGroup.Submit(executor, [&results, index]
        {
            results[index] = static_cast<int>((index + 1) * 10);
        });
    }
    resultsGroup.Wait();
    Expect(results == std::array<int, 4>{10, 20, 30, 40},
        "Executor completion did not preserve stable result slots.");

    std::atomic<int> completedAfterFailure = 0;
    TaskGroup failureGroup;
    failureGroup.Submit(executor, []
    {
        throw std::runtime_error("Expected task failure.");
    });
    failureGroup.Submit(executor, [&completedAfterFailure]
    {
        completedAfterFailure.fetch_add(1, std::memory_order_relaxed);
    });
    Reject([&failureGroup] { failureGroup.Wait(); });
    Expect(completedAfterFailure.load(std::memory_order_relaxed) <= 1
            && failureGroup.IsComplete()
            && failureGroup.IsCancellationRequested(),
        "Executor did not collect or cancel tasks after a group failure.");
}
} // namespace

int main()
{
    using namespace Prism::Core;
    try
    {
        Expect(ParseTaskExecutorKind("") == TaskExecutorKind::Pool
                && ParseTaskExecutorKind("inline")
                    == TaskExecutorKind::Inline
                && ParseTaskExecutorKind("pool")
                    == TaskExecutorKind::Pool
                && ToString(TaskExecutorKind::Inline) == "inline"
                && ToString(TaskExecutorKind::Pool) == "pool",
            "Render task executor mode parsing changed its contract.");
        Reject([] { (void)ParseTaskExecutorKind("async"); });
        const std::unique_ptr<ITaskExecutor> defaultExecutor =
            CreateTaskExecutor(TaskExecutorKind::Inline);
        Expect(defaultExecutor != nullptr
                && defaultExecutor->GetName() == "inline",
            "The explicit inline render task executor is unavailable.");

        Reject([]
        {
            TaskScheduler scheduler({0, 1});
        });
        Reject([]
        {
            TaskScheduler scheduler({1, 0});
        });
        Reject([]
        {
            TaskScheduler scheduler(
                {TaskScheduler::MaximumWorkerCount + 1, 1});
        });
        Reject([]
        {
            TaskScheduler scheduler(
                {1, TaskScheduler::MaximumQueueCapacity + 1});
        });

        {
            TaskScheduler scheduler({1, 2});
            TaskGroup empty;
            empty.Wait();
            Expect(empty.IsComplete()
                    && empty.GetSubmittedTaskCount() == 0,
                "An empty task group did not complete immediately.");
            scheduler.Shutdown();
            scheduler.Shutdown();
            Expect(!scheduler.IsAcceptingTasks(),
                "Repeated scheduler shutdown resumed task acceptance.");
            Reject([&]
            {
                scheduler.Submit([] {});
            });
        }

        {
            TaskScheduler scheduler({1, 4});
            TaskGroup group;
            std::atomic<int> value = 0;
            group.Submit(scheduler, [&value]
            {
                value.store(7, std::memory_order_release);
            });
            group.Wait();
            Expect(value.load(std::memory_order_acquire) == 7
                    && group.GetSubmittedTaskCount() == 1
                    && group.GetPendingTaskCount() == 0,
                "A single scheduled task did not complete through its group.");
        }

        {
            constexpr int TaskCount = 32;
            TaskScheduler scheduler({4, 8});
            TaskGroup group;
            std::atomic<int> completed = 0;
            for (int index = 0; index < TaskCount; ++index)
            {
                group.Submit(scheduler, [&completed]
                {
                    completed.fetch_add(1, std::memory_order_relaxed);
                });
            }
            group.Wait();
            Expect(completed.load(std::memory_order_relaxed) == TaskCount
                    && group.GetSubmittedTaskCount() == TaskCount
                    && scheduler.GetWorkerCount() == 4
                    && scheduler.GetQueueCapacity() == 8,
                "Multiple scheduled tasks were lost or bounds changed.");
        }

        {
            InlineTaskExecutor executor;
            VerifyExecutorContract(executor);
            executor.RecordJoinDuration(std::chrono::nanoseconds(7));
            const TaskExecutorStatistics statistics =
                executor.GetStatistics();
            Expect(statistics.executorName == "inline"
                    && statistics.submittedTaskCount == 6
                    && statistics.completedTaskCount == 6
                    && statistics.cumulativeJoinNanoseconds == 7
                    && statistics.peakActiveWorkerCount == 0,
                "Inline executor statistics did not use the shared contract.");
            executor.Shutdown();
            executor.Shutdown();
            Expect(!executor.IsAcceptingTasks(),
                "Inline executor shutdown was not idempotent.");
            Reject([&executor] { executor.Submit([] {}); });
        }

        {
            TaskScheduler scheduler({2, 8});
            VerifyExecutorContract(scheduler);
        }

        {
            ManualTaskExecutor executor;
            TaskGroup group;
            std::vector<int> completionOrder;
            group.Submit(executor, [&completionOrder]
            {
                completionOrder.push_back(0);
            });
            group.Submit(executor, [&completionOrder]
            {
                completionOrder.push_back(1);
            });
            Expect(executor.submissionOrder
                        == std::vector<std::size_t>{0, 1}
                    && group.GetPendingTaskCount() == 2
                    && !group.IsComplete(),
                "Injected executor did not observe stable submission order.");
            executor.Execute(1);
            Expect(group.GetPendingTaskCount() == 1,
                "TaskGroup completion did not track a fake executor.");
            executor.Execute(0);
            group.Wait();
            Expect(completionOrder == std::vector<int>{1, 0},
                "TaskGroup replaced executor completion order.");
        }

        {
            const std::thread::id callerThread =
                std::this_thread::get_id();
            std::thread::id executionThread;
            TaskScheduler scheduler({2, 2});
            TaskGroup group;
            group.Submit(
                scheduler,
                [&executionThread]
                {
                    executionThread = std::this_thread::get_id();
                },
                TaskExecutionHint::RunInline);
            group.Wait();
            Expect(executionThread == callerThread
                    && scheduler.GetQueuedTaskCount() == 0
                    && scheduler.GetPeakQueueDepth() == 0,
                "A small inline task was unexpectedly queued.");
        }

        {
            TaskScheduler scheduler({1, 1});
            TaskGroup group;
            TestEvent firstTaskStarted;
            TestEvent releaseFirstTask;
            group.Submit(scheduler, [&]
            {
                firstTaskStarted.Signal();
                releaseFirstTask.Wait();
            });
            firstTaskStarted.Wait();
            group.Submit(scheduler, [] {});

            std::atomic_bool thirdSubmitReturned = false;
            std::thread submitter([&]
            {
                group.Submit(scheduler, [] {});
                thirdSubmitReturned.store(true, std::memory_order_release);
            });
            while (scheduler.GetWaitingSubmitterCount() == 0)
            {
                std::this_thread::yield();
            }
            Expect(!thirdSubmitReturned.load(std::memory_order_acquire)
                    && scheduler.GetQueuedTaskCount() == 1,
                "A full task queue did not apply producer backpressure.");
            releaseFirstTask.Signal();
            submitter.join();
            group.Wait();
            Expect(thirdSubmitReturned.load(std::memory_order_acquire)
                    && scheduler.GetPeakQueueDepth() == 1,
                "Backpressured submission did not resume within its bound.");
        }

        {
            constexpr int SustainedTaskCount = 256;
            TaskScheduler scheduler({3, 16});
            TaskGroup group;
            std::mutex threadSetMutex;
            std::set<std::thread::id> workerThreads;
            const std::thread::id callerThread =
                std::this_thread::get_id();
            for (int index = 0; index < SustainedTaskCount; ++index)
            {
                group.Submit(scheduler, [&]
                {
                    std::lock_guard lock(threadSetMutex);
                    workerThreads.insert(std::this_thread::get_id());
                });
            }
            group.Wait();
            scheduler.RecordJoinDuration(std::chrono::nanoseconds(11));
            const TaskExecutorStatistics statistics =
                scheduler.GetStatistics();
            Expect(!workerThreads.empty()
                    && workerThreads.size() <= 3
                    && !workerThreads.contains(callerThread)
                    && scheduler.GetPeakQueueDepth() <= 16
                    && scheduler.GetPeakActiveWorkerCount() <= 3
                    && statistics.executorName == "pool"
                    && statistics.submittedTaskCount
                        == SustainedTaskCount
                    && statistics.completedTaskCount
                        == SustainedTaskCount
                    && statistics.cumulativeExecutionNanoseconds > 0
                    && statistics.cumulativeJoinNanoseconds == 11
                    && statistics.workerCount == 3
                    && statistics.queueCapacity == 16
                    && statistics.peakQueuedTaskCount <= 16,
                "Sustained work exceeded the fixed worker or queue bound.");
        }

        {
            TaskScheduler scheduler({1, 4});
            TestEvent blockerStarted;
            TestEvent releaseBlocker;
            scheduler.Submit([&]
            {
                blockerStarted.Signal();
                releaseBlocker.Wait();
            });
            blockerStarted.Wait();

            TaskGroup group;
            std::atomic_bool ranAfterFailure = false;
            group.Submit(scheduler, []
            {
                throw std::runtime_error("group recording failed");
            });
            group.Submit(scheduler, [&]
            {
                ranAfterFailure.store(true, std::memory_order_release);
            });
            releaseBlocker.Signal();

            bool propagatedOriginalFailure = false;
            try
            {
                group.Wait();
            }
            catch (const std::runtime_error& exception)
            {
                propagatedOriginalFailure =
                    std::string(exception.what()) ==
                    "group recording failed";
            }
            Expect(propagatedOriginalFailure
                    && !ranAfterFailure.load(std::memory_order_acquire)
                    && group.GetCancelledTaskCount() == 1,
                "A group failure was hidden or did not cancel queued work.");
        }

        {
            TaskScheduler scheduler({1, 4});
            TaskGroup group;
            TestEvent taskStarted;
            TestEvent inspectCancellation;
            std::atomic_bool runningTaskObservedCancellation = false;
            std::atomic_bool queuedTaskRan = false;
            group.Submit(
                scheduler,
                [&](const TaskCancellationToken& token)
                {
                    taskStarted.Signal();
                    inspectCancellation.Wait();
                    runningTaskObservedCancellation.store(
                        token.IsCancellationRequested(),
                        std::memory_order_release);
                });
            group.Submit(scheduler, [&]
            {
                queuedTaskRan.store(true, std::memory_order_release);
            });
            taskStarted.Wait();
            group.Cancel();
            inspectCancellation.Signal();
            group.Wait();
            Expect(runningTaskObservedCancellation.load(
                        std::memory_order_acquire)
                    && !queuedTaskRan.load(std::memory_order_acquire)
                    && group.GetCancelledTaskCount() == 1,
                "Group cancellation was not cooperative and bounded.");
        }

        {
            TaskScheduler scheduler({1, 4});
            TaskGroup group;
            std::atomic_bool nestedWaitRejected = false;
            group.Submit(scheduler, [&]
            {
                try
                {
                    group.Wait();
                }
                catch (const std::runtime_error&)
                {
                    nestedWaitRejected.store(
                        true, std::memory_order_release);
                }
            });
            group.Wait();
            Expect(nestedWaitRejected.load(std::memory_order_acquire),
                "A worker was allowed to block on its own task group.");
        }

        {
            TaskScheduler scheduler({1, 1});
            TestEvent firstTaskStarted;
            TestEvent releaseFirstTask;
            scheduler.Submit([&]
            {
                firstTaskStarted.Signal();
                releaseFirstTask.Wait();
            });
            firstTaskStarted.Wait();
            scheduler.Submit([] {});

            TestEvent rejectedSubmitterFinished;
            std::atomic_bool blockedSubmissionRejected = false;
            std::thread blockedSubmitter([&]
            {
                try
                {
                    scheduler.Submit([] {});
                }
                catch (const std::runtime_error&)
                {
                    blockedSubmissionRejected.store(
                        true, std::memory_order_release);
                }
                rejectedSubmitterFinished.Signal();
            });
            while (scheduler.GetWaitingSubmitterCount() == 0)
            {
                std::this_thread::yield();
            }

            std::atomic<int> shutdownReturnCount = 0;
            std::thread firstShutdown([&]
            {
                scheduler.Shutdown();
                shutdownReturnCount.fetch_add(1, std::memory_order_release);
            });
            std::thread secondShutdown([&]
            {
                scheduler.Shutdown();
                shutdownReturnCount.fetch_add(1, std::memory_order_release);
            });
            rejectedSubmitterFinished.Wait();
            Expect(blockedSubmissionRejected.load(std::memory_order_acquire)
                    && shutdownReturnCount.load(std::memory_order_acquire)
                        == 0,
                "Concurrent shutdown did not wake a blocked producer safely.");
            releaseFirstTask.Signal();
            blockedSubmitter.join();
            firstShutdown.join();
            secondShutdown.join();
            Expect(shutdownReturnCount.load(std::memory_order_acquire) == 2,
                "Concurrent scheduler shutdown did not converge.");
        }

        {
            TaskScheduler scheduler({1, 2});
            scheduler.Submit([]
            {
                throw std::runtime_error("direct task failed");
            });
            TaskGroup drain;
            drain.Submit(scheduler, [] {});
            drain.Wait();

            bool directFailureReported = false;
            try
            {
                scheduler.RethrowUnhandledTaskFailure();
            }
            catch (const std::runtime_error& exception)
            {
                directFailureReported =
                    std::string(exception.what()) == "direct task failed";
            }
            Expect(directFailureReported
                    && scheduler.GetUnhandledTaskFailureCount() == 1,
                "A direct task exception was silently discarded.");
        }

        {
            TaskScheduler scheduler({1, 2});
            TestEvent taskStarted;
            TestEvent releaseTask;
            TestEvent destructionStarted;
            auto payload = std::make_shared<int>(42);
            std::weak_ptr<int> payloadReference = payload;
            auto group = std::make_unique<TaskGroup>();
            group->Submit(scheduler, [payload, &taskStarted, &releaseTask]
            {
                taskStarted.Signal();
                releaseTask.Wait();
                Expect(*payload == 42,
                    "A task payload changed while its group was alive.");
            });
            payload.reset();
            taskStarted.Wait();

            std::atomic_bool destructionCompleted = false;
            std::thread destroyer(
                [group = std::move(group),
                 &destructionStarted,
                 &destructionCompleted]() mutable
                {
                    destructionStarted.Signal();
                    group.reset();
                    destructionCompleted.store(
                        true, std::memory_order_release);
                });
            destructionStarted.Wait();
            Expect(!payloadReference.expired()
                    && !destructionCompleted.load(std::memory_order_acquire),
                "TaskGroup destruction released a running task reference.");
            releaseTask.Signal();
            destroyer.join();
            Expect(payloadReference.expired()
                    && destructionCompleted.load(std::memory_order_acquire),
                "Task references outlived completed group destruction.");
        }

        {
            constexpr std::uint64_t StressTaskCount = 4096;
            TaskScheduler scheduler({4, 32});
            TaskGroup group;
            std::atomic_uint64_t completed = 0;
            for (std::uint64_t taskIndex = 0;
                 taskIndex < StressTaskCount;
                 ++taskIndex)
            {
                group.Submit(scheduler, [&completed]
                {
                    completed.fetch_add(1, std::memory_order_relaxed);
                });
            }
            group.Wait();
            const TaskExecutorStatistics statistics =
                scheduler.GetStatistics();
            Expect(completed.load(std::memory_order_relaxed)
                        == StressTaskCount
                    && statistics.submittedTaskCount == StressTaskCount
                    && statistics.completedTaskCount == StressTaskCount
                    && statistics.queuedTaskCount == 0
                    && statistics.activeWorkerCount == 0
                    && statistics.peakQueuedTaskCount <= 32
                    && statistics.peakActiveWorkerCount <= 4,
                "The bounded scheduler lost work or retained a stress task.");
            scheduler.Shutdown();
        }

        {
            std::atomic<int> factoryAttempts = 0;
            std::atomic<int> liveWorkers = 0;
            TaskScheduler::WorkerFactory failingFactory =
                [&factoryAttempts, &liveWorkers](
                    TaskScheduler::WorkerEntry entry)
                {
                    const int attempt = factoryAttempts.fetch_add(
                        1, std::memory_order_relaxed);
                    if (attempt == 1)
                    {
                        throw std::runtime_error(
                            "Injected worker startup failure.");
                    }
                    return std::thread(
                        [&liveWorkers, entry = std::move(entry)]() mutable
                        {
                            liveWorkers.fetch_add(
                                1, std::memory_order_relaxed);
                            entry();
                            liveWorkers.fetch_sub(
                                1, std::memory_order_relaxed);
                        });
                };

            Reject([&]
            {
                TaskScheduler scheduler(
                    {3, 4}, std::move(failingFactory));
            });
            Expect(factoryAttempts.load(std::memory_order_relaxed) == 2
                    && liveWorkers.load(std::memory_order_relaxed) == 0,
                "A partially started scheduler did not join its workers.");
        }

        std::cout << "Task scheduler tests passed.\n";
        return 0;
    }
    catch (const std::exception& exception)
    {
        std::cerr << exception.what() << '\n';
        return 1;
    }
}
