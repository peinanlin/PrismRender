#include "Asset/SlangShaderCompiler.h"

#include "Asset/ShaderLoader.h"
#include "Core/CpuTrace.h"

#include <algorithm>
#include <cctype>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <string>

#include <slang-com-ptr.h>
#include <slang.h>

namespace Prism::Asset
{
    namespace
    {
        struct SlangTarget
        {
            SlangCompileTarget format = SLANG_TARGET_UNKNOWN;
            const char* profile = nullptr;
        };

        bool IsRayTracingStage(
            const RHI::ShaderStage stage)
        {
            return stage == RHI::ShaderStage::RayGeneration
                || stage == RHI::ShaderStage::AnyHit
                || stage == RHI::ShaderStage::ClosestHit
                || stage == RHI::ShaderStage::Miss
                || stage == RHI::ShaderStage::Intersection
                || stage == RHI::ShaderStage::Callable;
        }

        SlangTarget GetSlangTarget(
            const RHI::ShaderBinaryFormat format,
            const RHI::ShaderStage stage)
        {
            switch (format)
            {
            case RHI::ShaderBinaryFormat::Dxil:
                return {
                    SLANG_DXIL,
                    IsRayTracingStage(stage)
                        ? "sm_6_3"
                        : "sm_6_0"};
            case RHI::ShaderBinaryFormat::SpirV: return {SLANG_SPIRV, "spirv_1_5"};
            case RHI::ShaderBinaryFormat::Dxbc: return {SLANG_DXBC, "sm_5_1"};
            case RHI::ShaderBinaryFormat::MetalSource: return {SLANG_METAL, "metal_2_4"};
            }
            throw std::invalid_argument("Unsupported Slang shader target.");
        }

        SlangStage GetSlangStage(const RHI::ShaderStage stage)
        {
            switch (stage)
            {
            case RHI::ShaderStage::Vertex: return SLANG_STAGE_VERTEX;
            case RHI::ShaderStage::Hull: return SLANG_STAGE_HULL;
            case RHI::ShaderStage::Domain: return SLANG_STAGE_DOMAIN;
            case RHI::ShaderStage::Pixel: return SLANG_STAGE_FRAGMENT;
            case RHI::ShaderStage::Compute: return SLANG_STAGE_COMPUTE;
            case RHI::ShaderStage::RayGeneration: return SLANG_STAGE_RAY_GENERATION;
            case RHI::ShaderStage::AnyHit: return SLANG_STAGE_ANY_HIT;
            case RHI::ShaderStage::ClosestHit: return SLANG_STAGE_CLOSEST_HIT;
            case RHI::ShaderStage::Miss: return SLANG_STAGE_MISS;
            case RHI::ShaderStage::Intersection: return SLANG_STAGE_INTERSECTION;
            case RHI::ShaderStage::Callable: return SLANG_STAGE_CALLABLE;
            }
            return SLANG_STAGE_NONE;
        }

        RHI::ShaderResourceKind GetResourceKind(const slang::ParameterCategory category)
        {
            switch (category)
            {
            case slang::ParameterCategory::ConstantBuffer: return RHI::ShaderResourceKind::ConstantBuffer;
            case slang::ParameterCategory::ShaderResource: return RHI::ShaderResourceKind::ShaderResource;
            case slang::ParameterCategory::UnorderedAccess: return RHI::ShaderResourceKind::UnorderedAccess;
            case slang::ParameterCategory::SamplerState: return RHI::ShaderResourceKind::Sampler;
            case slang::ParameterCategory::PushConstantBuffer: return RHI::ShaderResourceKind::PushConstant;
            default: return RHI::ShaderResourceKind::Unknown;
            }
        }   

        RHI::ShaderResourceKind GetResourceKind(slang::VariableLayoutReflection* parameter)
        {
            const RHI::ShaderResourceKind categoryKind = GetResourceKind(parameter->getCategory());
            if (categoryKind != RHI::ShaderResourceKind::Unknown)
            {
                return categoryKind;
            }

            slang::TypeLayoutReflection* typeLayout = parameter->getTypeLayout();
            switch (typeLayout->getKind())
            {
            case slang::TypeReflection::Kind::ConstantBuffer:
            case slang::TypeReflection::Kind::ParameterBlock:
                return RHI::ShaderResourceKind::ConstantBuffer;
            case slang::TypeReflection::Kind::SamplerState:
                return RHI::ShaderResourceKind::Sampler;
            case slang::TypeReflection::Kind::Resource:
            case slang::TypeReflection::Kind::TextureBuffer:
                return typeLayout->getResourceAccess() == SLANG_RESOURCE_ACCESS_READ
                    ? RHI::ShaderResourceKind::ShaderResource
                    : RHI::ShaderResourceKind::UnorderedAccess;
            case slang::TypeReflection::Kind::ShaderStorageBuffer:
                return RHI::ShaderResourceKind::UnorderedAccess;
            default:
                return RHI::ShaderResourceKind::Unknown;
            }
        }

        RHI::ShaderResourceShape GetResourceShape(
            slang::VariableLayoutReflection* parameter)
        {
            slang::TypeLayoutReflection* typeLayout =
                parameter->getTypeLayout();
            switch (
                typeLayout->getResourceShape()
                & SLANG_RESOURCE_BASE_SHAPE_MASK)
            {
            case SLANG_STRUCTURED_BUFFER:
            case SLANG_BYTE_ADDRESS_BUFFER:
            case SLANG_TEXTURE_BUFFER:
                return RHI::ShaderResourceShape::Buffer;
            case SLANG_TEXTURE_1D:
            case SLANG_TEXTURE_2D:
            case SLANG_TEXTURE_3D:
            case SLANG_TEXTURE_CUBE:
            case SLANG_TEXTURE_SUBPASS:
                return RHI::ShaderResourceShape::Texture;
            case SLANG_ACCELERATION_STRUCTURE:
                return RHI::ShaderResourceShape::
                    AccelerationStructure;
            default:
                break;
            }
            return typeLayout->getKind()
                       == slang::TypeReflection::Kind::
                           ShaderStorageBuffer
                ? RHI::ShaderResourceShape::Buffer
                : RHI::ShaderResourceShape::Unknown;
        }

        std::string GetDiagnostics(slang::IBlob* diagnostics)
        {
            if (diagnostics == nullptr || diagnostics->getBufferPointer() == nullptr)
            {
                return {};
            }   
            return std::string(
                static_cast<const char*>(diagnostics->getBufferPointer()),
                diagnostics->getBufferSize());
        }

        void ThrowOnFailure(const SlangResult result, const std::string& action, slang::IBlob* diagnostics = nullptr)
        {
            if (SLANG_SUCCEEDED(result))
            {
                return;
            }

            const std::string details = GetDiagnostics(diagnostics);
            throw std::runtime_error(action + (details.empty() ? "." : ":\n" + details));
        }

        std::string MakeModuleName(const ShaderCompileRequest& request)
        {
            std::string name = request.filePath.stem().string() + "_" + request.entryPoint + "_" +
                               std::string(RHI::ToString(request.format)) + "_"
                               + request.targetProfile;
            std::ranges::transform(name, name.begin(), [](const unsigned char character) {
                return std::isalnum(character) != 0 ? static_cast<char>(character) : '_';
            });
            return name;
        }

        RHI::ShaderReflection ReflectProgram(
            slang::IComponentType* program,
            const RHI::ShaderBinaryFormat format)
        {
            RHI::ShaderReflection reflection;
            Slang::ComPtr<slang::IBlob> diagnostics;
            slang::ProgramLayout* layout = program->getLayout(0, diagnostics.writeRef());
            if (layout == nullptr)
            {
                throw std::runtime_error("Slang failed to reflect the linked shader program: " + GetDiagnostics(diagnostics));
            }

            reflection.resources.reserve(layout->getParameterCount());
            for (unsigned index = 0; index < layout->getParameterCount(); ++index)
            {
                slang::VariableLayoutReflection* parameter = layout->getParameterByIndex(index);
                if (parameter == nullptr)
                {
                    continue;
                }

                RHI::ShaderResourceBinding binding;
                binding.name = parameter->getName() != nullptr ? parameter->getName() : "<unnamed>";
                binding.kind = GetResourceKind(parameter);
                binding.shape = GetResourceShape(parameter);
                binding.bindingIndex = parameter->getBindingIndex();
                binding.bindingSpace = parameter->getBindingSpace();
                // DXIL reflection reports the index within a D3D register class
                // (b/t/u/s). Normalize it to the RHI's single descriptor namespace;
                // SPIR-V already reports the explicit vk::binding value.
                if (format == RHI::ShaderBinaryFormat::Dxil)
                {
                    switch (binding.kind)
                    {
                    case RHI::ShaderResourceKind::ShaderResource:
                        binding.bindingIndex += 16u;
                        break;
                    case RHI::ShaderResourceKind::UnorderedAccess:
                        binding.bindingIndex += 32u;
                        break;
                    case RHI::ShaderResourceKind::Sampler:
                        binding.bindingIndex += 48u;
                        break;
                    default:
                        break;
                    }
                }

                slang::TypeLayoutReflection* reflectedType =
                    parameter->getTypeLayout();
                std::size_t reflectedSize =
                    reflectedType->getSize(slang::ParameterCategory::Uniform);
                // A ConstantBuffer parameter is a descriptor container. Slang reports
                // its payload size on the element layout, while the outer layout may
                // report zero uniform bytes. Preserve the actual CBV contract in the
                // backend-neutral reflection record.
                if (binding.kind == RHI::ShaderResourceKind::ConstantBuffer
                    && (reflectedSize == 0u
                        || reflectedSize == SLANG_UNKNOWN_SIZE
                        || reflectedSize == SLANG_UNBOUNDED_SIZE))
                {
                    if (slang::TypeLayoutReflection* elementType =
                            reflectedType->getElementTypeLayout())
                    {
                        reflectedSize = elementType->getSize(
                            slang::ParameterCategory::Uniform);
                    }
                }
                binding.byteSize = reflectedSize == SLANG_UNKNOWN_SIZE || reflectedSize == SLANG_UNBOUNDED_SIZE ? 0 : reflectedSize;
                reflection.resources.push_back(std::move(binding));
            }
            return reflection;
        }
    } // namespace

    struct SlangShaderCompiler::Impl
    {
        Slang::ComPtr<slang::IGlobalSession> globalSession;
        std::mutex compileMutex;
    };

    SlangShaderCompiler::SlangShaderCompiler()
        : m_impl(std::make_unique<Impl>())
    {
        ThrowOnFailure(slang::createGlobalSession(m_impl->globalSession.writeRef()), "Failed to create Slang global session");
    }

    SlangShaderCompiler::~SlangShaderCompiler() = default;

    RHI::ShaderBinary SlangShaderCompiler::Compile(const ShaderCompileRequest& request)
    {
        Core::CpuTraceSpan traceSpan(
            "ShaderCompile",
            "shader");
        if (request.filePath.empty() || request.entryPoint.empty())
        {
            throw std::invalid_argument("Shader compilation requires a file path and entry point.");
        }

        std::scoped_lock lock(m_impl->compileMutex);
        const std::string source = ShaderLoader::LoadSourceText(request.filePath);
        const SlangTarget slangTarget =
            GetSlangTarget(
                request.format,
                request.stage);

        slang::TargetDesc targetDesc{};
        targetDesc.format = slangTarget.format;
        const char* targetProfile =
            request.targetProfile.empty()
            ? slangTarget.profile
            : request.targetProfile.c_str();
        targetDesc.profile =
            m_impl->globalSession->findProfile(targetProfile);
        if (targetDesc.profile == SLANG_PROFILE_UNKNOWN)
        {
            throw std::invalid_argument(
                "Unknown Slang target profile: "
                + std::string(targetProfile));
        }

        const std::string searchPath = request.filePath.parent_path().string();
        const char* searchPaths[] = {searchPath.c_str()};
        const slang::PreprocessorMacroDesc macros[] = {{"COMPILER_SLANG", "1"}};

        slang::SessionDesc sessionDesc{};
        sessionDesc.targets = &targetDesc;
        sessionDesc.targetCount = 1;
        sessionDesc.defaultMatrixLayoutMode = SLANG_MATRIX_LAYOUT_COLUMN_MAJOR;
        sessionDesc.searchPaths = searchPaths;
        sessionDesc.searchPathCount = 1;
        sessionDesc.preprocessorMacros = macros;
        sessionDesc.preprocessorMacroCount = 1;

        Slang::ComPtr<slang::ISession> session;
        ThrowOnFailure(m_impl->globalSession->createSession(sessionDesc, session.writeRef()), "Failed to create Slang session");

        Slang::ComPtr<slang::IBlob> diagnostics;
        const std::string moduleName = MakeModuleName(request);
        const std::string sourcePath = request.filePath.string();
        Slang::ComPtr<slang::IModule> module(
            session->loadModuleFromSourceString(moduleName.c_str(), sourcePath.c_str(), source.c_str(), diagnostics.writeRef()));
        if (module == nullptr)
        {
            throw std::runtime_error("Failed to load Slang module '" + sourcePath + "':\n" + GetDiagnostics(diagnostics));
        }
        std::string allDiagnostics = GetDiagnostics(diagnostics);

        diagnostics.setNull();
        Slang::ComPtr<slang::IEntryPoint> entryPoint;
        ThrowOnFailure(
            module->findAndCheckEntryPoint(
                request.entryPoint.c_str(), GetSlangStage(request.stage), entryPoint.writeRef(), diagnostics.writeRef()),
            "Failed to validate Slang entry point '" + request.entryPoint + "'",
            diagnostics);
        allDiagnostics += GetDiagnostics(diagnostics);

        slang::IComponentType* components[] = {module.get(), entryPoint.get()};
        Slang::ComPtr<slang::IComponentType> program;
        diagnostics.setNull();
        ThrowOnFailure(
            session->createCompositeComponentType(components, 2, program.writeRef(), diagnostics.writeRef()),
            "Failed to compose Slang shader program",
            diagnostics);
        allDiagnostics += GetDiagnostics(diagnostics);

        Slang::ComPtr<slang::IComponentType> linkedProgram;
        diagnostics.setNull();
        ThrowOnFailure(program->link(linkedProgram.writeRef(), diagnostics.writeRef()), "Failed to link Slang shader program", diagnostics);
        allDiagnostics += GetDiagnostics(diagnostics);

        Slang::ComPtr<slang::IBlob> code;
        diagnostics.setNull();
        const std::string codeGenerationAction =
            "Failed to generate " + std::string(RHI::ToString(request.format))
            + " shader code for " + request.filePath.string() + ":"
            + request.entryPoint;
        ThrowOnFailure(
            linkedProgram->getEntryPointCode(0, 0, code.writeRef(), diagnostics.writeRef()),
            codeGenerationAction,
            diagnostics);
        allDiagnostics += GetDiagnostics(diagnostics);

        RHI::ShaderBinary binary;
        binary.stage = request.stage;
        binary.format = request.format;
        binary.entryPoint = request.entryPoint;
        binary.emittedEntryPoint = request.format == RHI::ShaderBinaryFormat::SpirV ? "main" : request.entryPoint;
        binary.diagnostics = std::move(allDiagnostics);
        binary.reflection = ReflectProgram(
            linkedProgram,
            request.format);

        const auto* codeBegin = static_cast<const std::uint8_t*>(code->getBufferPointer());
        binary.bytecode.assign(codeBegin, codeBegin + code->getBufferSize());
        return binary;
    }
} // namespace Prism::Asset
