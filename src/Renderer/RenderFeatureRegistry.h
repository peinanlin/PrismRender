#pragma once

#include "Renderer/Features/RenderFeature.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace Prism::Renderer
{
class RenderGraph;

// Keeps feature contribution order explicit while decoupling SceneRenderer's
// graph setup from every concrete feature implementation.
class RenderFeatureRegistry
{
public:
    void Register(RenderFeatureRegistration registration);
    [[nodiscard]] const std::vector<RenderFeatureId>&
        ResolveLifecycleOrder();
    void InitializeLifecycleFeatures(
        const RenderFeatureInitializationContext& context);
    void PrepareLifecycleFeatures(
        const RenderFeatureFrameContext& context) const;
    void PrepareLifecycleFeatures(
        const RenderFeatureFrameContext& context,
        RenderFeatureScope scope) const;
    void BuildLifecycleStage(
        RenderFeatureStage stage,
        RenderFeatureGraphContext& context) const;
    void ResizeLifecycleFeatures(
        const RenderFeatureResizeContext& context) const;
    void ResizeLifecycleFeatures(
        const RenderFeatureResizeContext& context,
        RenderFeatureScope scope) const;
    void NotifyLifecycleSceneChanged(
        const RenderFeatureSceneContext& context) const;
    void NotifyLifecycleSceneChanged(
        const RenderFeatureSceneContext& context,
        RenderFeatureScope scope) const;
    void ShutdownLifecycleFeatures();
    [[nodiscard]] const std::vector<RenderFeatureRegistration>&
        GetLifecycleRegistrations() const noexcept;
    [[nodiscard]] bool AreLifecycleFeaturesInitialized() const noexcept;

private:
    std::vector<RenderFeatureRegistration> m_lifecycleRegistrations;
    std::vector<std::size_t> m_lifecycleOrder;
    std::vector<RenderFeatureId> m_resolvedLifecycleIds;
    std::vector<std::size_t> m_initializedLifecycleOrder;
    bool m_lifecycleOrderResolved = false;
};
} // namespace Prism::Renderer
