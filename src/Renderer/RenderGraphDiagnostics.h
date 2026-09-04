#pragma once

#include <filesystem>
#include <string>

#include <json.hpp>

namespace Prism::RHI
{
struct DescriptorAllocatorStatistics;
struct GraphicsDeviceCapabilities;
struct UploadQueueStatistics;
struct ResourceRetirementStatistics;
enum class GraphicsApi;
}

namespace Prism::Renderer
{
class RenderGraph;

[[nodiscard]] nlohmann::json BuildRenderGraphReport(
    const RenderGraph& graph,
    RHI::GraphicsApi graphicsApi,
    const RHI::GraphicsDeviceCapabilities*
        deviceCapabilities = nullptr,
    const RHI::DescriptorAllocatorStatistics*
        descriptorStatistics = nullptr,
    const RHI::UploadQueueStatistics*
        uploadStatistics = nullptr,
    const RHI::ResourceRetirementStatistics*
        retirementStatistics = nullptr);

bool WriteRenderGraphReport(
    const std::filesystem::path& path,
    const RenderGraph& graph,
    RHI::GraphicsApi graphicsApi,
    const RHI::GraphicsDeviceCapabilities*
        deviceCapabilities = nullptr,
    const RHI::DescriptorAllocatorStatistics*
        descriptorStatistics = nullptr,
    const RHI::UploadQueueStatistics*
        uploadStatistics = nullptr,
    const RHI::ResourceRetirementStatistics*
        retirementStatistics = nullptr,
    std::string* outError = nullptr);

bool WriteRenderGraphReport(
    const std::filesystem::path& path,
    const nlohmann::json& report,
    std::string* outError = nullptr);
} // namespace Prism::Renderer
