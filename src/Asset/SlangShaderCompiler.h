#pragma once

#include "Asset/IShaderCompiler.h"

#include <memory>

namespace Prism::Asset
{
    class SlangShaderCompiler final : public IShaderCompiler
    {
    public:
        SlangShaderCompiler();
        ~SlangShaderCompiler() override;

        SlangShaderCompiler(const SlangShaderCompiler&) = delete;
        SlangShaderCompiler& operator=(const SlangShaderCompiler&) = delete;

        RHI::ShaderBinary Compile(const ShaderCompileRequest& request) override;

    private:
        struct Impl;
        std::unique_ptr<Impl> m_impl;
    };
} // namespace Prism::Asset
