#include "RHI/RayTracingValidation.h"

#include "RHI/IGraphicsDevice.h"

#include <fstream>
#include <json.hpp>

namespace Prism::RHI
{
namespace
{
nlohmann::json SerializeBuildSizes(
    const AccelerationStructureBuildSizes& sizes)
{
    return {
        {"accelerationStructureBytes",
         sizes.accelerationStructureBytes},
        {"buildScratchBytes",
         sizes.buildScratchBytes},
        {"updateScratchBytes",
         sizes.updateScratchBytes}};
}
} // namespace

RayTracingValidationResult
RunRayTracingValidation(
    IGraphicsDevice& device)
{
    RayTracingValidationResult result{};
    const GraphicsDeviceCapabilities& capabilities =
        device.GetCapabilities();
    result.tier = capabilities.rayTracingTier;
    result.supported =
        capabilities.features
            .rayTracingAccelerationStructure;
    if (!result.supported)
    {
        result.passed = true;
        result.message =
            "Acceleration structures are not supported; the probe was skipped.";
        return result;
    }

    try
    {
        AccelerationStructureBuildDescription
            bottomLevel{};
        bottomLevel.geometries = {
            {3,
             32,
             3,
             IndexFormat::UInt32,
             RayTracingGeometryFlags::Opaque}};
        bottomLevel.flags =
            AccelerationStructureBuildFlags::PreferFastTrace
            | AccelerationStructureBuildFlags::AllowUpdate;
        result.bottomLevelSizes =
            device.QueryAccelerationStructureBuildSizes(
                bottomLevel);

        AccelerationStructureBuildDescription
            topLevel{};
        topLevel.type =
            AccelerationStructureType::TopLevel;
        topLevel.instanceCount = 1;
        topLevel.flags =
            AccelerationStructureBuildFlags::PreferFastTrace
            | AccelerationStructureBuildFlags::AllowUpdate;
        result.topLevelSizes =
            device.QueryAccelerationStructureBuildSizes(
                topLevel);

        struct ProbeVertex
        {
            float position[3];
            float padding[5];
        };
        const ProbeVertex vertices[3] = {
            {{-1.0f, -1.0f, 0.0f}, {}},
            {{0.0f, 1.0f, 0.0f}, {}},
            {{1.0f, -1.0f, 0.0f}, {}}};
        const std::uint32_t indices[3] = {
            0,
            1,
            2};
        BufferDescription vertexBufferDescription{};
        vertexBufferDescription.size =
            sizeof(vertices);
        vertexBufferDescription.stride =
            sizeof(ProbeVertex);
        vertexBufferDescription.usage =
            BufferUsage::Vertex
            | BufferUsage::
                  AccelerationStructureBuildInput;
        vertexBufferDescription.memoryAccess =
            MemoryAccess::CpuToGpu;
        const std::shared_ptr<IBuffer> vertexBuffer =
            device.CreateBuffer(
                vertexBufferDescription,
                vertices);
        BufferDescription indexBufferDescription{};
        indexBufferDescription.size =
            sizeof(indices);
        indexBufferDescription.stride =
            sizeof(std::uint32_t);
        indexBufferDescription.usage =
            BufferUsage::Index
            | BufferUsage::
                  AccelerationStructureBuildInput;
        indexBufferDescription.memoryAccess =
            MemoryAccess::CpuToGpu;
        const std::shared_ptr<IBuffer> indexBuffer =
            device.CreateBuffer(
                indexBufferDescription,
                indices);

        AccelerationStructureBuildRequest
            bottomLevelRequest{};
        bottomLevelRequest.description =
            bottomLevel;
        bottomLevelRequest.geometries = {
            {bottomLevel.geometries.front(),
             vertexBuffer,
             0,
             indexBuffer,
             0}};
        const std::shared_ptr<
            IRayTracingAccelerationStructure>
            builtBottomLevel =
                device.CreateAccelerationStructure(
                    bottomLevelRequest);
        result.bottomLevelBuilt =
            builtBottomLevel != nullptr;
        result.bottomLevelDeviceAddress =
            builtBottomLevel != nullptr
            ? builtBottomLevel->GetDeviceAddress()
            : 0;

        AccelerationStructureBuildRequest
            topLevelRequest{};
        topLevelRequest.description = topLevel;
        RayTracingInstanceDescription instance{};
        instance.instanceId = 7;
        instance.bottomLevel = builtBottomLevel;
        topLevelRequest.instances.push_back(
            std::move(instance));
        const std::shared_ptr<
            IRayTracingAccelerationStructure>
            builtTopLevel =
                device.CreateAccelerationStructure(
                    topLevelRequest);
        result.topLevelBuilt =
            builtTopLevel != nullptr;
        result.topLevelDeviceAddress =
            builtTopLevel != nullptr
            ? builtTopLevel->GetDeviceAddress()
            : 0;
        result.passed =
            result.bottomLevelSizes
                    .accelerationStructureBytes
                > 0
            && result.bottomLevelSizes
                    .buildScratchBytes
                > 0
            && result.topLevelSizes
                    .accelerationStructureBytes
                > 0
            && result.topLevelSizes
                    .buildScratchBytes
                > 0
            && result.bottomLevelBuilt
            && result.topLevelBuilt
            && result.bottomLevelDeviceAddress != 0
            && result.topLevelDeviceAddress != 0;
        result.message = result.passed
            ? "BLAS and TLAS prebuild queries and native builds completed on the active driver."
            : "The driver returned an empty size or failed to build an acceleration structure.";
    }
    catch (const std::exception& exception)
    {
        result.passed = false;
        result.message = exception.what();
    }
    return result;
}

bool WriteRayTracingValidationReport(
    const std::filesystem::path& outputPath,
    IGraphicsDevice& device,
    std::string* errorMessage)
{
    try
    {
        const RayTracingValidationResult result =
            RunRayTracingValidation(device);
        const GraphicsDeviceCapabilities& capabilities =
            device.GetCapabilities();
        const DeviceLimits& limits =
            capabilities.limits;
        const DeviceFeatures& features =
            capabilities.features;
        nlohmann::json report = {
            {"schemaVersion", 1},
            {"graphicsApi",
             std::string(ToString(
                 capabilities.graphicsApi))},
            {"adapterName", capabilities.adapterName},
            {"supported", result.supported},
            {"passed", result.passed},
            {"tier", std::string(ToString(result.tier))},
            {"features", {
                {"accelerationStructure",
                 features.rayTracingAccelerationStructure},
                {"rayTracingPipeline",
                 features.rayTracingPipeline},
                {"rayQuery", features.rayQuery},
                {"bufferDeviceAddress",
                 features.bufferDeviceAddress}}},
            {"limits", {
                {"maxRayRecursionDepth",
                 limits.maxRayRecursionDepth},
                {"shaderGroupHandleSize",
                 limits.rayTracingShaderGroupHandleSize},
                {"shaderGroupBaseAlignment",
                 limits.rayTracingShaderGroupBaseAlignment},
                {"accelerationStructureScratchAlignment",
                 limits.accelerationStructureScratchAlignment}}},
            {"bottomLevel",
             {
                 {"sizes",
                  SerializeBuildSizes(
                      result.bottomLevelSizes)},
                 {"built",
                  result.bottomLevelBuilt},
                 {"deviceAddress",
                  result.bottomLevelDeviceAddress}}},
            {"topLevel",
             {
                 {"sizes",
                  SerializeBuildSizes(
                      result.topLevelSizes)},
                 {"built",
                  result.topLevelBuilt},
                 {"deviceAddress",
                  result.topLevelDeviceAddress}}},
            {"message", result.message}};

        if (!outputPath.parent_path().empty())
        {
            std::filesystem::create_directories(
                outputPath.parent_path());
        }
        std::ofstream output(outputPath);
        if (!output)
        {
            throw std::runtime_error(
                "Failed to open the ray-tracing validation report.");
        }
        output << report.dump(2) << '\n';
        if (!output)
        {
            throw std::runtime_error(
                "Failed to write the ray-tracing validation report.");
        }
        if (errorMessage != nullptr
            && result.passed)
        {
            errorMessage->clear();
        }
        else if (errorMessage != nullptr)
        {
            *errorMessage = result.message;
        }
        return result.passed;
    }
    catch (const std::exception& exception)
    {
        if (errorMessage != nullptr)
        {
            *errorMessage = exception.what();
        }
        return false;
    }
}
} // namespace Prism::RHI
