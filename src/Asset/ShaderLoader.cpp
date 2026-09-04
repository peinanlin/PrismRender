#include "Asset/ShaderLoader.h"

#include <fstream>
#include <iterator>
#include <stdexcept>

namespace Prism::Asset
{
std::string ShaderLoader::LoadSourceText(const std::filesystem::path& filePath)
{
    std::ifstream stream(filePath, std::ios::binary);
    if (!stream)
    {
        throw std::runtime_error("Failed to open shader source file '" + filePath.string() + "'.");
    }
    return std::string(std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>());
}
} // namespace Prism::Asset
