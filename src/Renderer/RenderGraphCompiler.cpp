#include "Renderer/RenderGraph.h"
#include "Renderer/Graph/GraphCompiler.h"
#include "Renderer/Graph/QueueScheduler.h"
#include "Renderer/Graph/ResourceLifetimePlanner.h"
#include "Renderer/Graph/TransientAliasPlanner.h"

#include <iomanip>
#include <algorithm>
#include <ranges>
#include <sstream>

namespace Prism::Renderer
{
namespace
{
class FingerprintBuilder
{
public:
    void Append(const std::string_view value)
    {
        for (const unsigned char character : value)
        {
            m_hash ^= character;
            m_hash *= Prime;
        }
        m_hash ^= 0xffu;
        m_hash *= Prime;
    }

    template <typename Value>
    void AppendValue(const Value value)
    {
        Append(std::to_string(
            static_cast<std::uint64_t>(value)));
    }

    [[nodiscard]] std::uint64_t Get() const
    {
        return m_hash;
    }

private:
    static constexpr std::uint64_t Offset =
        14695981039346656037ull;
    static constexpr std::uint64_t Prime =
        1099511628211ull;
    std::uint64_t m_hash = Offset;
};

template <typename Set>
void AppendSortedStrings(
    FingerprintBuilder& builder,
    const Set& values)
{
    std::vector<std::string_view> sorted;
    sorted.reserve(values.size());
    for (const auto& value : values)
    {
        sorted.emplace_back(value);
    }
    std::ranges::sort(sorted);
    for (const std::string_view value : sorted)
    {
        builder.Append(value);
    }
}
} // namespace

void RenderGraph::InvalidateCompilation()
{
    m_compiledGraph.Clear();
    m_executionState.InvalidateCompiledPlan();
}

std::uint64_t RenderGraph::BuildDeclarationFingerprint() const
{
    FingerprintBuilder fingerprint;
    fingerprint.AppendValue(m_passCullingEnabled);
    fingerprint.AppendValue(m_queueExecutionMode);
    AppendSortedStrings(fingerprint, m_importedResources);
    AppendSortedStrings(fingerprint, m_outputResources);

    const auto appendTextureDescription =
        [&fingerprint](const RHI::TextureDescription& description)
        {
            fingerprint.AppendValue(description.dimension);
            fingerprint.AppendValue(description.width);
            fingerprint.AppendValue(description.height);
            fingerprint.AppendValue(description.arrayLayers);
            fingerprint.AppendValue(description.mipLevels);
            fingerprint.AppendValue(description.sampleCount);
            fingerprint.AppendValue(description.format);
            fingerprint.AppendValue(description.usage);
            fingerprint.AppendValue(description.memoryAccess);
        };
    const auto appendBufferDescription =
        [&fingerprint](const RHI::BufferDescription& description)
        {
            fingerprint.AppendValue(description.size);
            fingerprint.AppendValue(description.stride);
            fingerprint.AppendValue(description.usage);
            fingerprint.AppendValue(description.memoryAccess);
        };

    for (const RegisteredTexture& texture :
         m_registeredTextures)
    {
        fingerprint.Append("texture");
        fingerprint.Append(texture.name);
        fingerprint.AppendValue(texture.currentVersion);
        fingerprint.AppendValue(texture.lifetime);
        appendTextureDescription(texture.description);
        if (texture.texture != nullptr)
        {
            if (const auto* allocation =
                    texture.texture
                        ->GetTransientAllocationInfo();
                allocation != nullptr)
            {
                fingerprint.AppendValue(allocation->poolId);
                fingerprint.AppendValue(
                    allocation->allocationIndex);
                fingerprint.AppendValue(
                    allocation->allocationBytes);
            }
        }
    }
    for (const RegisteredBuffer& buffer :
         m_registeredBuffers)
    {
        fingerprint.Append("buffer");
        fingerprint.Append(buffer.name);
        fingerprint.AppendValue(buffer.currentVersion);
        fingerprint.AppendValue(buffer.lifetime);
        appendBufferDescription(buffer.description);
        if (buffer.buffer != nullptr)
        {
            if (const auto* allocation =
                    buffer.buffer
                        ->GetTransientAllocationInfo();
                allocation != nullptr)
            {
                fingerprint.AppendValue(allocation->poolId);
                fingerprint.AppendValue(
                    allocation->allocationIndex);
                fingerprint.AppendValue(
                    allocation->allocationBytes);
            }
        }
    }
    for (const RegisteredTextureView& view :
         m_registeredTextureViews)
    {
        fingerprint.Append("view");
        fingerprint.Append(view.name);
        fingerprint.AppendValue(view.texture.index);
        fingerprint.AppendValue(view.texture.version);
        if (view.view != nullptr)
        {
            const RHI::TextureViewDescription& description =
                view.view->GetDescription();
            fingerprint.AppendValue(description.type);
            fingerprint.AppendValue(description.format);
            fingerprint.AppendValue(description.baseMipLevel);
            fingerprint.AppendValue(description.mipLevelCount);
            fingerprint.AppendValue(description.baseArrayLayer);
            fingerprint.AppendValue(description.arrayLayerCount);
        }
    }

    std::vector<std::string_view> textureResourceNames;
    textureResourceNames.reserve(m_textureResources.size());
    for (const auto& [name, resource] : m_textureResources)
    {
        (void)resource;
        textureResourceNames.emplace_back(name);
    }
    std::ranges::sort(textureResourceNames);
    for (const std::string_view name : textureResourceNames)
    {
        const TextureResource& resource =
            m_textureResources.at(std::string(name));
        fingerprint.Append(name);
        fingerprint.AppendValue(resource.state);
        for (const RHI::ResourceState state :
             resource.subresourceStates)
        {
            fingerprint.AppendValue(state);
        }
    }

    std::vector<std::string_view> bufferResourceNames;
    bufferResourceNames.reserve(m_bufferResources.size());
    for (const auto& [name, resource] : m_bufferResources)
    {
        (void)resource;
        bufferResourceNames.emplace_back(name);
    }
    std::ranges::sort(bufferResourceNames);
    for (const std::string_view name : bufferResourceNames)
    {
        const BufferResource& resource =
            m_bufferResources.at(std::string(name));
        fingerprint.Append(name);
        appendBufferDescription(resource.description);
        for (const BufferStateRange& range : resource.states)
        {
            fingerprint.AppendValue(range.offset);
            fingerprint.AppendValue(range.size);
            fingerprint.AppendValue(range.state);
        }
    }

    const auto appendAccess =
        [&fingerprint](const TrackedResourceAccess& access)
        {
            fingerprint.Append(access.name);
            fingerprint.AppendValue(access.state);
            fingerprint.AppendValue(access.texture);
            fingerprint.AppendValue(access.buffer);
            fingerprint.AppendValue(access.version);
            fingerprint.AppendValue(
                access.textureRange.baseMipLevel);
            fingerprint.AppendValue(
                access.textureRange.mipLevelCount);
            fingerprint.AppendValue(
                access.textureRange.baseArrayLayer);
            fingerprint.AppendValue(
                access.textureRange.arrayLayerCount);
            fingerprint.AppendValue(access.bufferRange.offset);
            fingerprint.AppendValue(access.bufferRange.size);
        };
    for (const Pass& pass : m_passes)
    {
        fingerprint.Append("pass");
        fingerprint.Append(pass.info.name);
        fingerprint.AppendValue(pass.options.queue);
        fingerprint.AppendValue(pass.options.sideEffect);
        fingerprint.AppendValue(pass.options.allowCulling);
        fingerprint.AppendValue(
            pass.options.parallelRecordable);
        for (const std::string& read : pass.reads)
        {
            fingerprint.Append("read");
            fingerprint.Append(read);
        }
        for (const std::string& write : pass.writes)
        {
            fingerprint.Append("write");
            fingerprint.Append(write);
        }
        for (const TrackedResourceAccess& read :
             pass.resourceReads)
        {
            fingerprint.Append("tracked-read");
            appendAccess(read);
        }
        for (const TrackedResourceAccess& write :
             pass.resourceWrites)
        {
            fingerprint.Append("tracked-write");
            appendAccess(write);
        }
    }
    return fingerprint.Get();
}

bool RenderGraph::TryRestoreCompilationCache(
    const std::uint64_t declarationFingerprint)
{
    if (!m_compilationCache.has_value()
        || m_compilationCache->declarationFingerprint
            != declarationFingerprint
        || m_compilationCache->passes.size()
            != m_passes.size())
    {
        return false;
    }

    const CompilationCacheEntry& cache =
        *m_compilationCache;
    m_compiledGraph.declarationFingerprint =
        cache.declarationFingerprint;
    m_compiledGraph.passes = cache.passes;
    for (std::size_t index = 0;
         index < m_passes.size();
         ++index)
    {
        Pass& pass = m_passes[index];
        const CachedPassCompilation& cached =
            cache.passes[index];
        pass.executionIndex = cached.executionIndex;
        pass.active = cached.active;
        pass.cullReason = cached.cullReason;
        pass.livenessDependencies =
            cached.livenessDependencies;
        pass.executionDependencies =
            cached.executionDependencies;
    }
    m_compiledGraph.compiledResources =
        cache.compiledResources;
    m_compiledGraph.queueSyncDescriptions =
        cache.queueSyncDescriptions;
    m_compiledGraph.queueBatchDescriptions =
        cache.queueBatchDescriptions;
    m_compiledGraph.queueBatchSubmissionOrder =
        cache.queueBatchSubmissionOrder;
    m_compiledGraph.passToQueueBatch =
        cache.passToQueueBatch;
    m_compiledGraph.nativeAliasGroups =
        cache.nativeAliasGroups;
    m_compiledGraph.summary = cache.summary;
    m_compiledGraph.summary.compiled = true;
    m_compiledGraph.summary.compilationCacheHit = true;
    m_compiledGraph.graphSignature =
        cache.graphSignature;
    m_compiledGraph.valid = true;
    return true;
}

void RenderGraph::StoreCompilationCache(
    const std::uint64_t declarationFingerprint)
{
    CompilationCacheEntry cache{};
    cache.declarationFingerprint =
        declarationFingerprint;
    GraphCompiler::WriteCompiledPassPlans(
        m_description,
        cache);
    cache.compiledResources =
        m_compiledGraph.compiledResources;
    cache.queueSyncDescriptions =
        m_compiledGraph.queueSyncDescriptions;
    cache.queueBatchDescriptions =
        m_compiledGraph.queueBatchDescriptions;
    cache.queueBatchSubmissionOrder =
        m_compiledGraph.queueBatchSubmissionOrder;
    cache.passToQueueBatch =
        m_compiledGraph.passToQueueBatch;
    cache.nativeAliasGroups =
        m_compiledGraph.nativeAliasGroups;
    cache.summary = m_compiledGraph.summary;
    cache.summary.compilationCacheHit = false;
    cache.graphSignature =
        m_compiledGraph.graphSignature;
    m_compiledGraph.declarationFingerprint =
        declarationFingerprint;
    m_compiledGraph.passes = cache.passes;
    m_compilationCache = std::move(cache);
}

void RenderGraph::BuildGraphSignature()
{
    constexpr std::uint64_t offset =
        14695981039346656037ull;
    constexpr std::uint64_t prime =
        1099511628211ull;
    std::uint64_t hash = offset;
    const auto append =
        [&](const std::string_view value)
        {
            for (const unsigned char character : value)
            {
                hash ^= character;
                hash *= prime;
            }
            hash ^= 0xffu;
            hash *= prime;
        };
    for (const Pass& pass : m_passes)
    {
        if (!pass.active)
        {
            continue;
        }
        append(pass.info.name);
        append(
            pass.options.queue == QueueClass::Compute
            ? "compute"
            : "graphics");
        for (const std::size_t dependency :
             pass.executionDependencies)
        {
            append(std::to_string(dependency));
        }
        for (const TrackedResourceAccess& read :
             pass.resourceReads)
        {
            append("r:" + read.name + ':'
                + std::to_string(
                    static_cast<std::uint32_t>(
                        read.state))
                + ":v"
                + std::to_string(read.version)
                + ":m"
                + std::to_string(
                    read.textureRange.baseMipLevel)
                + ':'
                + std::to_string(
                    read.textureRange.mipLevelCount)
                + ":l"
                + std::to_string(
                    read.textureRange.baseArrayLayer)
                + ':'
                + std::to_string(
                    read.textureRange.arrayLayerCount)
                + ":b"
                + std::to_string(
                    read.bufferRange.offset)
                + ':'
                + std::to_string(
                    read.bufferRange.size));
        }
        for (const TrackedResourceAccess& write :
             pass.resourceWrites)
        {
            append("w:" + write.name + ':'
                + std::to_string(
                    static_cast<std::uint32_t>(
                        write.state))
                + ":v"
                + std::to_string(write.version)
                + ":m"
                + std::to_string(
                    write.textureRange.baseMipLevel)
                + ':'
                + std::to_string(
                    write.textureRange.mipLevelCount)
                + ":l"
                + std::to_string(
                    write.textureRange.baseArrayLayer)
                + ':'
                + std::to_string(
                    write.textureRange.arrayLayerCount)
                + ":b"
                + std::to_string(
                    write.bufferRange.offset)
                + ':'
                + std::to_string(
                    write.bufferRange.size));
        }
    }
    std::ostringstream signature;
    signature << std::hex
              << std::setfill('0')
              << std::setw(16)
              << hash;
    m_compiledGraph.graphSignature =
        signature.str();
}

void RenderGraph::Compile()
{
    if (m_compiledGraph.valid)
    {
        return;
    }
    const std::uint64_t declarationFingerprint =
        BuildDeclarationFingerprint();
    if (TryRestoreCompilationCache(
            declarationFingerprint))
    {
        return;
    }
    m_compiledGraph.summary = {};
    m_compiledGraph.summary.declaredPassCount =
        m_passes.size();
    GraphCompiler::BuildLivenessDependencies(m_description);
    GraphCompiler::CullPasses(m_description);

    const std::size_t executionIndex =
        GraphCompiler::AssignExecutionIndices(m_description);
    m_compiledGraph.summary.activePassCount =
        executionIndex;
    m_compiledGraph.summary.culledPassCount =
        m_passes.size() - executionIndex;
    m_compiledGraph.summary.passCullingEnabled =
        m_passCullingEnabled
        && (!m_outputResources.empty()
            || std::ranges::any_of(
                m_passes,
                [](const Pass& pass)
                {
                    return pass.options.sideEffect
                        || !pass.options.allowCulling;
                }));
    m_compiledGraph.summary.serialQueueExecution = true;

    GraphCompiler::BuildExecutionDependencies(m_description);
    ResourceLifetimePlanner::Build(
        m_description,
        m_executionState,
        m_compiledGraph);
    m_compiledGraph.summary.registeredTextureCount =
        m_registeredTextures.size();
    m_compiledGraph.summary.registeredBufferCount =
        m_registeredBuffers.size();
    m_compiledGraph.summary.registeredTextureViewCount =
        m_registeredTextureViews.size();
    m_compiledGraph.summary.historyResourceCount =
        std::ranges::count_if(
            m_compiledGraph.compiledResources,
            [](const auto& entry)
            {
                return entry.second.history;
            });
    m_compiledGraph.summary.versionedResourceCount =
        std::ranges::count_if(
            m_compiledGraph.compiledResources,
            [](const auto& entry)
            {
                return entry.second.currentVersion > 0;
            });
    m_compiledGraph.summary.parameterPassCount =
        std::ranges::count_if(
            m_passes,
            [](const Pass& pass)
            {
                return static_cast<bool>(
                    pass.parameterExecute);
            });
    for (const auto& [name, texture] :
         m_textureResources)
    {
        (void)name;
        m_compiledGraph.summary.textureSubresourceCount +=
            texture.subresourceStates.size();
    }
    for (const auto& [name, buffer] :
         m_bufferResources)
    {
        (void)name;
        m_compiledGraph.summary.bufferStateRangeCount +=
            buffer.states.size();
    }
    TransientAliasPlanner::Build(
        m_description,
        m_executionState,
        m_compiledGraph);
    GraphCompiler::ReduceTransitiveDependencies(m_description);
    QueueScheduler::Build(
        m_description,
        m_compiledGraph);
    BuildGraphSignature();
    m_compiledGraph.summary.compiled = true;
    m_compiledGraph.valid = true;
    StoreCompilationCache(declarationFingerprint);
}
} // namespace Prism::Renderer
