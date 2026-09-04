#pragma once

#include <cstdint>
#include <string_view>

namespace Prism::Renderer { struct RenderSettings; }
namespace Prism::RHI { enum class GraphicsApi; }
namespace Prism::Platform { class Window; }
namespace Prism::Core
{
class FramePerformanceRecorder;
struct RenderFrameFeedback;
struct RenderViewRuntimeFeedback;
void RecordPerformanceView(
    FramePerformanceRecorder& recorder,
    const RenderViewRuntimeFeedback& feedback,
    const Renderer::RenderSettings& settings,
    RHI::GraphicsApi graphicsApi,
    std::uint32_t frameContextIndex,
    std::uint32_t width,
    std::uint32_t height,
    std::string_view view,
    double simulationTime,
    std::uint64_t sceneGeneration);
void CompletePerformanceFrame(
    FramePerformanceRecorder& recorder,
    const RenderFrameFeedback& feedback,
    const Platform::Window& window);
} // namespace Prism::Core
