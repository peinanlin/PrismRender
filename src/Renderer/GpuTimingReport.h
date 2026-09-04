#pragma once

#include "RHI/Profiling/GpuProfiler.h"

#include <filesystem>
#include <span>
#include <string>

#include <json.hpp>

namespace Prism::RHI
{
enum class GraphicsApi;
}

namespace Prism::Renderer
{
[[nodiscard]] nlohmann::json BuildGpuTimingReport(
    std::span<const RHI::GpuProfiler::Timing> timings,
    RHI::GraphicsApi graphicsApi,
    const RHI::GpuProfiler::TimelineMetadata& timelineMetadata = {},
    const nlohmann::json& captureMetadata = nlohmann::json::object());

bool WriteGpuTimingReport(
    const std::filesystem::path& path,
    std::span<const RHI::GpuProfiler::Timing> timings,
    RHI::GraphicsApi graphicsApi,
    std::string* outError = nullptr,
    const RHI::GpuProfiler::TimelineMetadata& timelineMetadata = {},
    const nlohmann::json& captureMetadata = nlohmann::json::object());
} // namespace Prism::Renderer
