#include "Core/Threading/TaskExecutorFactory.h"

#include "Core/Threading/TaskScheduler.h"

#include <algorithm>
#include <stdexcept>
#include <thread>

namespace Prism::Core
{
namespace
{
constexpr std::size_t DefaultRenderQueueCapacity = 256;

std::size_t DefaultRenderWorkerCount() noexcept
{
    const std::size_t hardwareThreads =
        std::thread::hardware_concurrency();
    const std::size_t availableWorkers =
        hardwareThreads > 1 ? hardwareThreads - 1 : 1;
    return (std::min)(availableWorkers,
        TaskScheduler::MaximumWorkerCount);
}
} // namespace

TaskExecutorKind ParseTaskExecutorKind(const std::string_view value)
{
    if (value.empty() || value == "pool")
    {
        return TaskExecutorKind::Pool;
    }
    if (value == "inline")
    {
        return TaskExecutorKind::Inline;
    }
    throw std::invalid_argument(
        "Render task executor must be inline or pool.");
}

std::string_view ToString(const TaskExecutorKind kind) noexcept
{
    return kind == TaskExecutorKind::Pool ? "pool" : "inline";
}

std::unique_ptr<ITaskExecutor> CreateTaskExecutor(
    const TaskExecutorKind kind)
{
    if (kind == TaskExecutorKind::Inline)
    {
        return std::make_unique<InlineTaskExecutor>();
    }
    return std::make_unique<TaskScheduler>(
        TaskSchedulerConfig{
            DefaultRenderWorkerCount(),
            DefaultRenderQueueCapacity});
}
} // namespace Prism::Core
