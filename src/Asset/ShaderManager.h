#pragma once

#include "RHI/ShaderTypes.h"

#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>

namespace Prism::Asset
{
    class IShaderCompiler;

    class ShaderManager
    {
        public:
            ShaderManager();
            explicit ShaderManager(std::unique_ptr<IShaderCompiler> compiler);
            ~ShaderManager();

            const RHI::ShaderBinary& LoadShader(
                const std::filesystem::path& filePath,
                std::string_view entryPoint,
                RHI::ShaderStage stage,
                RHI::ShaderBinaryFormat format = RHI::ShaderBinaryFormat::Dxil);

        private:
            std::unique_ptr<IShaderCompiler> m_compiler;
            std::unordered_map<std::string, RHI::ShaderBinary> m_shaderCache;
    };
} // namespace Prism::Asset
