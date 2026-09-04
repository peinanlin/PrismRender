#pragma once

#include "RHI/ShaderTypes.h"

#include <filesystem>
#include <string>

namespace Prism::Asset
{
    struct ShaderCompileRequest
    {
        std::filesystem::path filePath;
        std::string entryPoint;
        RHI::ShaderStage stage = RHI::ShaderStage::Vertex;
        RHI::ShaderBinaryFormat format = RHI::ShaderBinaryFormat::Dxil;
        bool debug = false;
        std::string targetProfile;
    };

class IShaderCompiler
{
    public:
        virtual ~IShaderCompiler() = default;
        virtual RHI::ShaderBinary Compile(const ShaderCompileRequest& request) = 0;
    };
} // namespace Prism::Asset
