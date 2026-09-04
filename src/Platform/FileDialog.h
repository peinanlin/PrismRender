#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace Prism::Platform
{
struct FileDialogFilter
{
    std::wstring name;
    std::wstring pattern;
};

[[nodiscard]] std::optional<std::filesystem::path> OpenFileDialog(
    const std::wstring& title,
    const std::vector<FileDialogFilter>& filters);
}
