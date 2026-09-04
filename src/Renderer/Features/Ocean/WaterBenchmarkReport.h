#pragma once

#include <json.hpp>

namespace Prism::Renderer
{
struct OceanSettings;
struct OceanStatistics;
class RenderGraph;

[[nodiscard]] nlohmann::json BuildWaterBenchmarkMetadata(const OceanSettings& settings,
    const OceanStatistics& statistics, const RenderGraph& graph);
}
