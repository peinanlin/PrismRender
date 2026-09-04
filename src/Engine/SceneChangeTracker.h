#pragma once

#include <cstdint>

namespace Prism::Engine
{
enum class SceneChangeCategory : std::uint32_t
{
    None = 0,
    EntityTopology = 1u << 0u,
    Hierarchy = 1u << 1u,
    Transform = 1u << 2u,
    Visibility = 1u << 3u,
    Material = 1u << 4u,
    Camera = 1u << 5u,
    Lighting = 1u << 6u,
    FullRebuild = 1u << 31u
};

constexpr SceneChangeCategory operator|(
    const SceneChangeCategory left,
    const SceneChangeCategory right)
{
    return static_cast<SceneChangeCategory>(
        static_cast<std::uint32_t>(left)
        | static_cast<std::uint32_t>(right));
}

constexpr SceneChangeCategory& operator|=(
    SceneChangeCategory& left,
    const SceneChangeCategory right)
{
    left = left | right;
    return left;
}

constexpr bool HasSceneChange(
    const SceneChangeCategory value,
    const SceneChangeCategory category)
{
    return (static_cast<std::uint32_t>(value)
        & static_cast<std::uint32_t>(category)) != 0;
}

struct SceneChangeSet
{
    std::uint64_t revision = 0;
    SceneChangeCategory categories = SceneChangeCategory::None;
    std::uint32_t mutationCount = 0;

    [[nodiscard]] bool HasChanges() const
    {
        return categories != SceneChangeCategory::None;
    }

    [[nodiscard]] bool RequiresFullRebuild() const
    {
        return HasSceneChange(
            categories,
            SceneChangeCategory::FullRebuild);
    }
};

// Collects mutable World edits and publishes only committed change sets.
// A scope corresponds to one externally visible transaction.
class SceneChangeTracker final
{
public:
    void BeginCommitScope();
    void Record(SceneChangeCategory categories);
    SceneChangeSet CommitScope();
    void RollbackScope() noexcept;

    [[nodiscard]] bool IsScopeActive() const noexcept;
    [[nodiscard]] bool HasPendingChanges() const noexcept;
    [[nodiscard]] const SceneChangeSet& PeekPendingChanges() const noexcept;
    SceneChangeSet ConsumePendingChanges() noexcept;
    void Reset() noexcept;

private:
    SceneChangeSet Publish(
        SceneChangeCategory categories,
        std::uint32_t mutationCount);

    std::uint64_t m_nextRevision = 1;
    bool m_scopeActive = false;
    SceneChangeCategory m_scopeCategories =
        SceneChangeCategory::None;
    std::uint32_t m_scopeMutationCount = 0;
    SceneChangeSet m_pending;
};
} // namespace Prism::Engine
