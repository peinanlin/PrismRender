#include "Asset/ShaderManager.h"

#include "Asset/IShaderCompiler.h"
#include "Asset/SlangShaderCompiler.h"

#include <stdexcept>

namespace Prism::Asset
{
    ShaderManager::ShaderManager()
        : ShaderManager(std::make_unique<SlangShaderCompiler>())
    {
}

ShaderManager::ShaderManager(std::unique_ptr<IShaderCompiler> compiler)
    : m_compiler(std::move(compiler))
{
    if (m_compiler == nullptr)
    {
        throw std::invalid_argument("ShaderManager requires a shader compiler.");
    }
}

ShaderManager::~ShaderManager() = default;

const RHI::ShaderBinary& ShaderManager::LoadShader(
    const std::filesystem::path& filePath,
    const std::string_view entryPoint,
    const RHI::ShaderStage stage,
    const RHI::ShaderBinaryFormat format)
{
    const std::string cacheKey = filePath.string() + "|" + std::string(entryPoint) + "|" +
                                 std::string(RHI::ToString(stage)) + "|" + std::string(RHI::ToString(format));
    const auto iterator = m_shaderCache.find(cacheKey);
    if (iterator != m_shaderCache.end())
    {
        return iterator->second;
    }

    ShaderCompileRequest request;
    request.filePath = filePath;
    request.entryPoint = entryPoint;
    request.stage = stage;
    request.format = format;
#if defined(_DEBUG)
    request.debug = true;
#endif

    RHI::ShaderBinary binary;
    try
    {
        binary = m_compiler->Compile(request);
    }
    catch (const std::exception& exception)
    {
        throw std::runtime_error(
            "Failed to compile shader '"
            + filePath.string()
            + "' entry '"
            + std::string(entryPoint)
            + "': "
            + exception.what());
    }
    catch (...)
    {
        throw std::runtime_error(
            "Failed to compile shader '"
            + filePath.string()
            + "' entry '"
            + std::string(entryPoint)
            + "' because of an unknown compiler error.");
    }
    const auto [insertedIterator, _] = m_shaderCache.emplace(cacheKey, std::move(binary));
    return insertedIterator->second;
}
} // namespace Prism::Asset
