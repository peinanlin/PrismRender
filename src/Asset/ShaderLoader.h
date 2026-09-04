#pragma once

#include <filesystem>
#include <string>

namespace Prism::Asset
{
class ShaderLoader
{
public:
    static std::string LoadSourceText(const std::filesystem::path& filePath);
};
} // namespace Prism::Asset
