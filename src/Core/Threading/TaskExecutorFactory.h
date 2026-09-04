#pragma once

#include "Core/Threading/ITaskExecutor.h"

#include <memory>
#include <string_view>

namespace Prism::Core
{
enum class TaskExecutorKind
{
    Inline,
    Pool
};

[[nodiscard]] TaskExecutorKind ParseTaskExecutorKind(
    std::string_view value);
[[nodiscard]] std::string_view ToString(
    TaskExecutorKind kind) noexcept;
[[nodiscard]] std::unique_ptr<ITaskExecutor> CreateTaskExecutor(
    TaskExecutorKind kind);
} // namespace Prism::Core
