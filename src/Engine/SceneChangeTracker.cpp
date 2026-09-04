#include "Engine/SceneChangeTracker.h"

#include <limits>
#include <stdexcept>

namespace Prism::Engine
{
void SceneChangeTracker::BeginCommitScope()
{
    if (m_scopeActive)
    {
        throw std::logic_error(
            "A SceneChangeTracker commit scope is already active.");
    }
    m_scopeActive = true;
    m_scopeCategories = SceneChangeCategory::None;
    m_scopeMutationCount = 0;
}

void SceneChangeTracker::Record(
    const SceneChangeCategory categories)
{
    if (categories == SceneChangeCategory::None)
    {
        return;
    }
    if (m_scopeActive)
    {
        m_scopeCategories |= categories;
        ++m_scopeMutationCount;
        return;
    }
    (void)Publish(categories, 1);
}

SceneChangeSet SceneChangeTracker::CommitScope()
{
    if (!m_scopeActive)
    {
        throw std::logic_error(
            "SceneChangeTracker has no active commit scope.");
    }
    const SceneChangeCategory categories = m_scopeCategories;
    const std::uint32_t mutationCount = m_scopeMutationCount;
    m_scopeActive = false;
    m_scopeCategories = SceneChangeCategory::None;
    m_scopeMutationCount = 0;
    return Publish(categories, mutationCount);
}

void SceneChangeTracker::RollbackScope() noexcept
{
    m_scopeActive = false;
    m_scopeCategories = SceneChangeCategory::None;
    m_scopeMutationCount = 0;
}

bool SceneChangeTracker::IsScopeActive() const noexcept
{
    return m_scopeActive;
}

bool SceneChangeTracker::HasPendingChanges() const noexcept
{
    return m_pending.HasChanges();
}

const SceneChangeSet&
SceneChangeTracker::PeekPendingChanges() const noexcept
{
    return m_pending;
}

SceneChangeSet SceneChangeTracker::ConsumePendingChanges() noexcept
{
    SceneChangeSet result = m_pending;
    m_pending = {};
    return result;
}

void SceneChangeTracker::Reset() noexcept
{
    RollbackScope();
    m_pending = {};
}

SceneChangeSet SceneChangeTracker::Publish(
    const SceneChangeCategory categories,
    const std::uint32_t mutationCount)
{
    if (categories == SceneChangeCategory::None)
    {
        return {};
    }
    if (m_nextRevision == std::numeric_limits<std::uint64_t>::max())
    {
        throw std::overflow_error(
            "Scene change revision capacity exhausted.");
    }
    const std::uint64_t revision = m_nextRevision++;
    m_pending.revision = revision;
    m_pending.categories |= categories;
    m_pending.mutationCount += mutationCount;
    return {revision, categories, mutationCount};
}
} // namespace Prism::Engine
