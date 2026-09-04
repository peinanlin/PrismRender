#pragma once

#include "RHI/GraphicsTypes.h"

#include <any>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <typeindex>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

namespace Prism::RHI
{
class IBuffer;
class ITexture;
class ITextureView;
}

namespace Prism::Renderer
{
class RenderGraph;

inline constexpr std::uint32_t InvalidRenderGraphIndex =
    std::numeric_limits<std::uint32_t>::max();

struct TextureHandle
{
    std::uint32_t index = InvalidRenderGraphIndex;
    std::uint32_t generation = 0;
    std::uint32_t version = 0;

    [[nodiscard]] bool IsValid() const
    {
        return index != InvalidRenderGraphIndex
            && generation != 0;
    }

    bool operator==(const TextureHandle&) const = default;
};

struct BufferHandle
{
    std::uint32_t index = InvalidRenderGraphIndex;
    std::uint32_t generation = 0;
    std::uint32_t version = 0;

    [[nodiscard]] bool IsValid() const
    {
        return index != InvalidRenderGraphIndex
            && generation != 0;
    }

    bool operator==(const BufferHandle&) const = default;
};

struct TextureViewHandle
{
    std::uint32_t index = InvalidRenderGraphIndex;
    std::uint32_t generation = 0;
    TextureHandle texture;

    [[nodiscard]] bool IsValid() const
    {
        return index != InvalidRenderGraphIndex
            && generation != 0
            && texture.IsValid();
    }

    bool operator==(const TextureViewHandle&) const =
        default;
};

struct TextureSubresourceRange
{
    std::uint32_t baseMipLevel = 0;
    std::uint32_t mipLevelCount = 0;
    std::uint32_t baseArrayLayer = 0;
    std::uint32_t arrayLayerCount = 0;
};

struct BufferRange
{
    std::size_t offset = 0;
    std::size_t size = 0;
};

enum class RenderGraphResourceLifetime
{
    External,
    Persistent,
    Transient,
    History
};

enum class RenderGraphAccessMode
{
    Read,
    Write
};

struct TextureParameterAccess
{
    TextureHandle handle;
    RHI::ResourceState state =
        RHI::ResourceState::Undefined;
    TextureSubresourceRange range;
    RenderGraphAccessMode mode =
        RenderGraphAccessMode::Read;
};

struct BufferParameterAccess
{
    BufferHandle handle;
    RHI::ResourceState state =
        RHI::ResourceState::Undefined;
    BufferRange range;
    RenderGraphAccessMode mode =
        RenderGraphAccessMode::Read;
};

struct TextureViewParameterAccess
{
    TextureViewHandle handle;
    RHI::ResourceState state =
        RHI::ResourceState::Undefined;
    RenderGraphAccessMode mode =
        RenderGraphAccessMode::Read;
};

class RenderGraphPassParameters
{
public:
    TextureHandle ReadTexture(
        TextureHandle handle,
        RHI::ResourceState state,
        TextureSubresourceRange range = {});
    TextureHandle WriteTexture(
        TextureHandle handle,
        RHI::ResourceState state,
        TextureSubresourceRange range = {});
    BufferHandle ReadBuffer(
        BufferHandle handle,
        RHI::ResourceState state,
        BufferRange range = {});
    BufferHandle WriteBuffer(
        BufferHandle handle,
        RHI::ResourceState state,
        BufferRange range = {});
    TextureViewHandle ReadTextureView(
        TextureViewHandle handle,
        RHI::ResourceState state);
    TextureHandle WriteTextureView(
        TextureViewHandle handle,
        RHI::ResourceState state);

    [[nodiscard]] const std::vector<
        TextureParameterAccess>&
        GetTextureAccesses() const;
    [[nodiscard]] const std::vector<
        BufferParameterAccess>&
        GetBufferAccesses() const;
    [[nodiscard]] const std::vector<
        TextureViewParameterAccess>&
        GetTextureViewAccesses() const;

private:
    std::vector<TextureParameterAccess>
        m_textureAccesses;
    std::vector<BufferParameterAccess>
        m_bufferAccesses;
    std::vector<TextureViewParameterAccess>
        m_textureViewAccesses;
};

class RenderGraphPassResources
{
public:
    [[nodiscard]] RHI::ITexture& GetTexture(
        TextureHandle handle) const;
    [[nodiscard]] RHI::IBuffer& GetBuffer(
        BufferHandle handle) const;
    [[nodiscard]] RHI::ITextureView& GetTextureView(
        TextureViewHandle handle) const;

private:
    explicit RenderGraphPassResources(
        const RenderGraph& graph,
        const RenderGraphPassParameters& parameters)
        : m_graph(&graph),
          m_parameters(&parameters)
    {
    }

    const RenderGraph* m_graph = nullptr;
    const RenderGraphPassParameters* m_parameters =
        nullptr;

    friend class RenderGraph;
};

struct TextureHistoryHandle
{
    TextureHandle previous;
    TextureHandle current;
};

enum class RenderGraphBlackboardValueScope : std::uint8_t
{
    DeviceShared,
    ViewLocal
};

struct RenderGraphBlackboardPublication
{
    std::string producerId;
    RenderGraphBlackboardValueScope scope =
        RenderGraphBlackboardValueScope::ViewLocal;
    std::uint64_t viewId = 0;
    std::uint32_t graphGeneration = 0;
    std::uint32_t valueVersion = 0;
};

struct RenderGraphBlackboardRequest
{
    std::string consumerId;
    std::uint64_t viewId = 0;
    std::uint32_t graphGeneration = 0;
    std::optional<std::uint32_t> expectedVersion;
    // Human-readable contract name used to attribute validation failures.
    // Slot/value types remain the authoritative runtime type identity.
    std::string resourceName;
};

class RenderGraphBlackboard
{
public:
    template <typename T>
    void Set(T value)
    {
        m_values.insert_or_assign(
            std::type_index(typeid(T)),
            std::any(std::move(value)));
    }

    template <typename T>
    [[nodiscard]] bool Contains() const
    {
        return m_values.contains(
            std::type_index(typeid(T)));
    }

    template <typename T>
    [[nodiscard]] T& Get()
    {
        const auto found = m_values.find(
            std::type_index(typeid(T)));
        if (found == m_values.end())
        {
            throw std::out_of_range(
                "The RenderGraph blackboard value is missing.");
        }
        return std::any_cast<T&>(found->second);
    }

    template <typename T>
    [[nodiscard]] const T& Get() const
    {
        const auto found = m_values.find(
            std::type_index(typeid(T)));
        if (found == m_values.end())
        {
            throw std::out_of_range(
                "The RenderGraph blackboard value is missing.");
        }
        return std::any_cast<const T&>(
            found->second);
    }

    template <typename Slot, typename T>
    void Publish(
        T value,
        RenderGraphBlackboardPublication publication)
    {
        if (publication.producerId.empty())
        {
            throw std::invalid_argument(
                "A feature graph publication requires a producer ID.");
        }
        if (publication.graphGeneration == 0)
        {
            throw std::invalid_argument(
                "A feature graph publication requires a graph generation.");
        }

        const std::type_index slotType(typeid(Slot));
        const auto existing = m_featureValues.find(slotType);
        if (existing != m_featureValues.end())
        {
            throw std::logic_error(
                "Feature graph slot already has producer '"
                + existing->second.publication.producerId
                + "'; conflicting producer '" + publication.producerId
                + "' must not overwrite it.");
        }
        FeatureValue entry;
        entry.value = std::any(std::move(value));
        entry.valueType = std::type_index(typeid(std::decay_t<T>));
        entry.publication = std::move(publication);
        m_featureValues.emplace(slotType, std::move(entry));
    }

    template <typename Slot, typename T>
    void PublishNext(
        T value,
        RenderGraphBlackboardPublication publication)
    {
        const std::type_index slotType(typeid(Slot));
        const auto found = m_featureValues.find(slotType);
        if (found == m_featureValues.end())
        {
            throw std::out_of_range(
                "Cannot publish a new feature graph version before its initial publication.");
        }

        FeatureValue& existing = found->second;
        const RenderGraphBlackboardPublication& previous =
            existing.publication;
        if (publication.producerId != previous.producerId)
        {
            throw std::logic_error(
                "A different producer cannot replace a feature graph publication.");
        }
        if (publication.scope != previous.scope
            || publication.viewId != previous.viewId
            || publication.graphGeneration != previous.graphGeneration)
        {
            throw std::logic_error(
                "A feature graph publication cannot change scope, view, or graph generation.");
        }
        if (publication.valueVersion <= previous.valueVersion)
        {
            throw std::logic_error(
                "A feature graph publication version must advance explicitly.");
        }
        if (existing.valueType
            != std::type_index(typeid(std::decay_t<T>)))
        {
            throw std::bad_any_cast();
        }

        existing.value = std::any(std::move(value));
        existing.publication = std::move(publication);
    }

    template <typename Slot, typename T>
    [[nodiscard]] const T& Require(
        const RenderGraphBlackboardRequest& request) const
    {
        const auto found = m_featureValues.find(
            std::type_index(typeid(Slot)));
        if (found == m_featureValues.end())
        {
            throw std::out_of_range(
                "Required feature graph input '"
                + RequestResourceName(request)
                + "' is missing for consumer '"
                + request.consumerId + "'.");
        }

        const FeatureValue& entry = found->second;
        if (entry.valueType != std::type_index(typeid(T)))
        {
            throw std::runtime_error(
                "Feature graph input '"
                + RequestResourceName(request)
                + "' has the wrong value type for consumer '"
                + request.consumerId + "'.");
        }
        ValidateRequest(entry.publication, request);
        return std::any_cast<const T&>(entry.value);
    }

    template <typename Slot, typename T>
    [[nodiscard]] T GetOptionalOr(
        const RenderGraphBlackboardRequest& request,
        T fallback) const
    {
        if (!m_featureValues.contains(std::type_index(typeid(Slot))))
        {
            return fallback;
        }
        return Require<Slot, T>(request);
    }

    template <typename Slot>
    [[nodiscard]] bool ContainsFeature() const
    {
        return m_featureValues.contains(
            std::type_index(typeid(Slot)));
    }

    void Clear()
    {
        m_values.clear();
        m_featureValues.clear();
    }

private:
    struct FeatureValue
    {
        std::any value;
        std::type_index valueType{typeid(void)};
        RenderGraphBlackboardPublication publication;
    };

    static void ValidateRequest(
        const RenderGraphBlackboardPublication& publication,
        const RenderGraphBlackboardRequest& request)
    {
        if (request.graphGeneration != publication.graphGeneration)
        {
            throw std::logic_error(
                "Feature graph input '" + RequestResourceName(request)
                + "' for consumer '" + request.consumerId
                + "' belongs to a stale graph generation.");
        }
        if (request.expectedVersion.has_value()
            && *request.expectedVersion != publication.valueVersion)
        {
            throw std::logic_error(
                "Feature graph input '" + RequestResourceName(request)
                + "' for consumer '" + request.consumerId
                + "' does not match the expected version.");
        }
        if (publication.scope
                == RenderGraphBlackboardValueScope::ViewLocal
            && request.viewId != publication.viewId)
        {
            throw std::logic_error(
                "View-local feature graph input '"
                + RequestResourceName(request) + "' for consumer '"
                + request.consumerId + "' cannot cross views.");
        }
    }

    static std::string RequestResourceName(
        const RenderGraphBlackboardRequest& request)
    {
        return request.resourceName.empty()
            ? "<unnamed>"
            : request.resourceName;
    }

    std::unordered_map<std::type_index, std::any>
        m_values;
    std::unordered_map<std::type_index, FeatureValue>
        m_featureValues;
};
} // namespace Prism::Renderer
