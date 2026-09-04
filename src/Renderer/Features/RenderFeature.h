#pragma once

#include "Renderer/Features/RenderFeatureContext.h"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace Prism::Renderer
{
using RenderFeatureId = std::string;

enum class RenderFeatureScope : std::uint8_t
{
    DeviceShared,
    ViewLocal
};

enum class RenderFeatureStage : std::uint8_t
{
    FramePreparation,
    Shadows,
    Opaque,
    Lighting,
    WaterOptics,
    Transparent,
    Temporal,
    PostProcess
};

enum class RenderFeatureOperation : std::uint8_t
{
    Initialize,
    PrepareFrame,
    BuildGraph,
    Resize,
    SceneChanged,
    Shutdown
};

enum class RenderFeatureInitializationPrerequisite : std::uint8_t
{
    GraphicsDevice,
    ShaderManager,
    PipelineCache,
    SharedResources
};

struct RenderFeatureDependency
{
    RenderFeatureId featureId;
    bool required = true;
};

struct RenderFeatureOperations
{
    std::function<void(const RenderFeatureInitializationContext&)> initialize;
    std::function<void(const RenderFeatureFrameContext&)> prepareFrame;
    std::function<void(RenderFeatureGraphContext&)> buildGraph;
    std::function<void(const RenderFeatureResizeContext&)> resize;
    std::function<void(const RenderFeatureSceneContext&)> sceneChanged;
    std::function<void()> shutdown;

    [[nodiscard]] bool Has(RenderFeatureOperation operation) const noexcept
    {
        switch (operation)
        {
        case RenderFeatureOperation::Initialize:
            return static_cast<bool>(initialize);
        case RenderFeatureOperation::PrepareFrame:
            return static_cast<bool>(prepareFrame);
        case RenderFeatureOperation::BuildGraph:
            return static_cast<bool>(buildGraph);
        case RenderFeatureOperation::Resize:
            return static_cast<bool>(resize);
        case RenderFeatureOperation::SceneChanged:
            return static_cast<bool>(sceneChanged);
        case RenderFeatureOperation::Shutdown:
            return static_cast<bool>(shutdown);
        }
        return false;
    }
};

// Concrete features bind existing class methods into this descriptor. They do
// not inherit from a common runtime base class.
struct RenderFeatureRegistration
{
    RenderFeatureId id;
    RenderFeatureScope scope = RenderFeatureScope::ViewLocal;
    std::vector<RenderFeatureStage> stages;
    std::vector<RenderFeatureDependency> dependencies;
    std::vector<RenderFeatureOperation> requiredOperations;
    std::vector<RenderFeatureInitializationPrerequisite>
        initializationPrerequisites;
    RenderFeatureOperations operations;
};
} // namespace Prism::Renderer
