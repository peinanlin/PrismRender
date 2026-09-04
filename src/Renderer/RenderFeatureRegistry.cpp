#include "Renderer/RenderFeatureRegistry.h"

#include "Core/Assert.h"
#include "Renderer/RenderGraph.h"

#include <algorithm>
#include <exception>
#include <limits>
#include <sstream>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace Prism::Renderer
{
void RenderFeatureRegistry::Register(
    RenderFeatureRegistration registration)
{
    Core::Check(
        !registration.id.empty(),
        "Render feature IDs must not be empty.");
    Core::Check(
        m_initializedLifecycleOrder.empty(),
        "Cannot register render features while the lifecycle is initialized.");
    m_lifecycleRegistrations.push_back(std::move(registration));
    m_lifecycleOrder.clear();
    m_resolvedLifecycleIds.clear();
    m_lifecycleOrderResolved = false;
}

const std::vector<RenderFeatureId>&
RenderFeatureRegistry::ResolveLifecycleOrder()
{
    if (m_lifecycleOrderResolved)
    {
        return m_resolvedLifecycleIds;
    }

    const std::size_t featureCount = m_lifecycleRegistrations.size();
    std::unordered_map<RenderFeatureId, std::size_t> featureIndices;
    featureIndices.reserve(featureCount);

    for (std::size_t index = 0; index < featureCount; ++index)
    {
        const RenderFeatureRegistration& registration =
            m_lifecycleRegistrations[index];
        const bool inserted =
            featureIndices.emplace(registration.id, index).second;
        Core::Check(
            inserted,
            "Duplicate render feature ID: " + registration.id);

        for (const RenderFeatureOperation operation :
             registration.requiredOperations)
        {
            Core::Check(
                registration.operations.Has(operation),
                "Render feature '" + registration.id
                    + "' is missing a required lifecycle operation.");
        }
    }

    std::vector<std::vector<std::size_t>> dependents(featureCount);
    std::vector<std::size_t> dependencyCounts(featureCount, 0);
    std::unordered_set<std::string> edges;
    for (std::size_t index = 0; index < featureCount; ++index)
    {
        const RenderFeatureRegistration& registration =
            m_lifecycleRegistrations[index];
        for (const RenderFeatureDependency& dependency :
             registration.dependencies)
        {
            const auto dependencyIt = featureIndices.find(dependency.featureId);
            if (dependencyIt == featureIndices.end())
            {
                Core::Check(
                    !dependency.required,
                    "Render feature '" + registration.id
                        + "' is missing required dependency '"
                        + dependency.featureId + "'.");
                continue;
            }

            const std::size_t dependencyIndex = dependencyIt->second;
            const std::string edgeKey = std::to_string(dependencyIndex)
                + ":" + std::to_string(index);
            if (edges.insert(edgeKey).second)
            {
                dependents[dependencyIndex].push_back(index);
                ++dependencyCounts[index];
            }
        }
    }

    const auto stageKey = [this](const std::size_t index)
    {
        const auto& stages = m_lifecycleRegistrations[index].stages;
        if (stages.empty())
        {
            return std::numeric_limits<std::uint8_t>::max();
        }
        return static_cast<std::uint8_t>(
            *std::min_element(stages.begin(), stages.end()));
    };

    std::vector<bool> emitted(featureCount, false);
    std::vector<std::size_t> resolvedOrder;
    resolvedOrder.reserve(featureCount);
    while (resolvedOrder.size() != featureCount)
    {
        std::size_t selected = featureCount;
        for (std::size_t index = 0; index < featureCount; ++index)
        {
            if (emitted[index] || dependencyCounts[index] != 0)
            {
                continue;
            }
            if (selected == featureCount
                || stageKey(index) < stageKey(selected)
                || (stageKey(index) == stageKey(selected)
                    && index < selected))
            {
                selected = index;
            }
        }

        if (selected == featureCount)
        {
            std::ostringstream message;
            message << "Render feature dependency cycle:";
            for (std::size_t index = 0; index < featureCount; ++index)
            {
                if (!emitted[index])
                {
                    message << ' ' << m_lifecycleRegistrations[index].id;
                }
            }
            throw std::runtime_error(message.str());
        }

        emitted[selected] = true;
        resolvedOrder.push_back(selected);
        for (const std::size_t dependent : dependents[selected])
        {
            --dependencyCounts[dependent];
        }
    }

    std::vector<RenderFeatureId> resolvedIds;
    resolvedIds.reserve(featureCount);
    for (const std::size_t index : resolvedOrder)
    {
        resolvedIds.push_back(m_lifecycleRegistrations[index].id);
    }
    m_lifecycleOrder = std::move(resolvedOrder);
    m_resolvedLifecycleIds = std::move(resolvedIds);
    m_lifecycleOrderResolved = true;
    return m_resolvedLifecycleIds;
}

void RenderFeatureRegistry::InitializeLifecycleFeatures(
    const RenderFeatureInitializationContext& context)
{
    Core::Check(
        m_initializedLifecycleOrder.empty(),
        "Render feature lifecycle is already initialized.");
    static_cast<void>(ResolveLifecycleOrder());

    for (const std::size_t index : m_lifecycleOrder)
    {
        RenderFeatureRegistration& registration =
            m_lifecycleRegistrations[index];
        try
        {
            if (registration.operations.initialize)
            {
                registration.operations.initialize(context);
            }
            m_initializedLifecycleOrder.push_back(index);
        }
        catch (...)
        {
            const std::exception_ptr initializationFailure =
                std::current_exception();
            if (registration.operations.shutdown)
            {
                try
                {
                    registration.operations.shutdown();
                }
                catch (...)
                {
                    // Preserve the initialization error while continuing to
                    // release already initialized dependencies.
                }
            }
            try
            {
                ShutdownLifecycleFeatures();
            }
            catch (...)
            {
                // Preserve the original initialization failure.
            }
            std::rethrow_exception(initializationFailure);
        }
    }
}

void RenderFeatureRegistry::BuildLifecycleStage(
    const RenderFeatureStage stage,
    RenderFeatureGraphContext& context) const
{
    Core::Check(
        m_initializedLifecycleOrder.size() == m_lifecycleOrder.size(),
        "Render feature lifecycle must be initialized before graph building.");
    for (const std::size_t index : m_initializedLifecycleOrder)
    {
        const RenderFeatureRegistration& registration =
            m_lifecycleRegistrations[index];
        if (!registration.operations.buildGraph
            || std::find(
                   registration.stages.begin(),
                   registration.stages.end(),
                   stage)
                == registration.stages.end())
        {
            continue;
        }
        registration.operations.buildGraph(context);
    }
}

void RenderFeatureRegistry::PrepareLifecycleFeatures(
    const RenderFeatureFrameContext& context) const
{
    Core::Check(
        m_initializedLifecycleOrder.size() == m_lifecycleOrder.size(),
        "Render feature lifecycle must be initialized before frame preparation.");
    for (const std::size_t index : m_initializedLifecycleOrder)
    {
        const auto& prepareFrame =
            m_lifecycleRegistrations[index].operations.prepareFrame;
        if (prepareFrame)
        {
            prepareFrame(context);
        }
    }
}

void RenderFeatureRegistry::PrepareLifecycleFeatures(
    const RenderFeatureFrameContext& context,
    const RenderFeatureScope scope) const
{
    Core::Check(
        m_initializedLifecycleOrder.size() == m_lifecycleOrder.size(),
        "Render feature lifecycle must be initialized before scoped frame preparation.");
    for (const std::size_t index : m_initializedLifecycleOrder)
    {
        const RenderFeatureRegistration& registration =
            m_lifecycleRegistrations[index];
        if (registration.scope == scope
            && registration.operations.prepareFrame)
        {
            registration.operations.prepareFrame(context);
        }
    }
}

void RenderFeatureRegistry::ResizeLifecycleFeatures(
    const RenderFeatureResizeContext& context) const
{
    Core::Check(
        m_initializedLifecycleOrder.size() == m_lifecycleOrder.size(),
        "Render feature lifecycle must be initialized before resize.");
    for (const std::size_t index : m_initializedLifecycleOrder)
    {
        const auto& resize =
            m_lifecycleRegistrations[index].operations.resize;
        if (resize)
        {
            resize(context);
        }
    }
}

void RenderFeatureRegistry::ResizeLifecycleFeatures(
    const RenderFeatureResizeContext& context,
    const RenderFeatureScope scope) const
{
    Core::Check(
        m_initializedLifecycleOrder.size() == m_lifecycleOrder.size(),
        "Render feature lifecycle must be initialized before scoped resize.");
    for (const std::size_t index : m_initializedLifecycleOrder)
    {
        const RenderFeatureRegistration& registration =
            m_lifecycleRegistrations[index];
        if (registration.scope == scope
            && registration.operations.resize)
        {
            registration.operations.resize(context);
        }
    }
}

void RenderFeatureRegistry::NotifyLifecycleSceneChanged(
    const RenderFeatureSceneContext& context) const
{
    Core::Check(
        m_initializedLifecycleOrder.size() == m_lifecycleOrder.size(),
        "Render feature lifecycle must be initialized before scene notification.");
    for (const std::size_t index : m_initializedLifecycleOrder)
    {
        const auto& sceneChanged =
            m_lifecycleRegistrations[index].operations.sceneChanged;
        if (sceneChanged)
        {
            sceneChanged(context);
        }
    }
}

void RenderFeatureRegistry::NotifyLifecycleSceneChanged(
    const RenderFeatureSceneContext& context,
    const RenderFeatureScope scope) const
{
    Core::Check(
        m_initializedLifecycleOrder.size() == m_lifecycleOrder.size(),
        "Render feature lifecycle must be initialized before scoped scene notification.");
    for (const std::size_t index : m_initializedLifecycleOrder)
    {
        const RenderFeatureRegistration& registration =
            m_lifecycleRegistrations[index];
        if (registration.scope == scope
            && registration.operations.sceneChanged)
        {
            registration.operations.sceneChanged(context);
        }
    }
}

void RenderFeatureRegistry::ShutdownLifecycleFeatures()
{
    std::vector<std::size_t> initialized =
        std::move(m_initializedLifecycleOrder);
    m_initializedLifecycleOrder.clear();

    std::exception_ptr firstFailure;
    for (auto it = initialized.rbegin(); it != initialized.rend(); ++it)
    {
        const auto& shutdown =
            m_lifecycleRegistrations[*it].operations.shutdown;
        if (!shutdown)
        {
            continue;
        }
        try
        {
            shutdown();
        }
        catch (...)
        {
            if (!firstFailure)
            {
                firstFailure = std::current_exception();
            }
        }
    }
    if (firstFailure)
    {
        std::rethrow_exception(firstFailure);
    }
}

const std::vector<RenderFeatureRegistration>&
RenderFeatureRegistry::GetLifecycleRegistrations() const noexcept
{
    return m_lifecycleRegistrations;
}

bool RenderFeatureRegistry::AreLifecycleFeaturesInitialized() const noexcept
{
    return !m_lifecycleRegistrations.empty()
        && m_initializedLifecycleOrder.size()
            == m_lifecycleRegistrations.size();
}
} // namespace Prism::Renderer
