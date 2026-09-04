#pragma once

#include "Scene/RenderSceneView.h"

#include <cstdint>
#include <memory>

namespace Prism::RHI
{
class IGraphicsDevice;
}

namespace Prism::Renderer
{
class RenderGraph;

using LogicalFrameId = std::uint64_t;
using RenderViewId = std::uint64_t;

// Operation-specific contexts keep initialization, per-frame, and per-view
// inputs explicit without exposing one large nullable service bag.
struct RenderFeatureInitializationContext
{
    // Present when GraphicsDevice is declared as an initialization
    // prerequisite. CPU-only fixtures and features may leave it null.
    RHI::IGraphicsDevice* graphicsDevice = nullptr;
};

struct RenderFeatureFrameContext
{
    LogicalFrameId logicalFrameId = 0;
    RenderViewId viewId = 0;
    std::uint32_t frameIndex = 0;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    Scene::RenderSceneView scene;
};

struct RenderFeatureGraphContext
{
    RenderGraph& graph;
    LogicalFrameId logicalFrameId = 0;
    RenderViewId viewId = 0;
    std::uint32_t frameIndex = 0;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    Scene::RenderSceneView scene;
    bool gpuDrivenEnabled = false;
    // Retained by graph callbacks/parallel recording tasks. Legacy fixtures
    // may leave this empty because they execute against a caller-owned scene.
    std::shared_ptr<const Scene::RenderSceneView> sceneLifetime;
};

struct RenderFeatureResizeContext
{
    LogicalFrameId logicalFrameId = 0;
    RenderViewId viewId = 0;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    // True only at a caller-owned GPU-idle resize safe point.
    bool gpuIdle = false;
};

struct RenderFeatureSceneContext
{
    Scene::RenderSceneView scene;
    std::uint64_t sceneRevision = 0;
};
} // namespace Prism::Renderer
