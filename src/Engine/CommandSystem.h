#pragma once

#include "Engine/Reflection.h"
#include "Engine/SceneChangeTracker.h"
#include "Engine/World.h"

#include <filesystem>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include <json.hpp>

namespace Prism::Engine
{
class CommandProcessor
{
public:
    explicit CommandProcessor(std::filesystem::path projectRoot = std::filesystem::current_path());

    [[nodiscard]] nlohmann::json Execute(const nlohmann::json& request);
    [[nodiscard]] nlohmann::json CaptureState() const;
    void RestoreState(const nlohmann::json& state);
    void InitializeWorld(World world);
    [[nodiscard]] std::string ComputeStateHash() const;
    bool SaveSnapshot(
        const std::filesystem::path& path,
        std::string* outError = nullptr) const;
    bool LoadSnapshotFile(
        const std::filesystem::path& path,
        std::string* outError = nullptr);

    [[nodiscard]] nlohmann::json ExportJournal() const;
    bool SaveJournal(const std::filesystem::path& path, std::string* outError = nullptr) const;
    bool ReplayJournal(const nlohmann::json& journal, std::string* outError = nullptr);
    bool ReplayJournalFile(const std::filesystem::path& path, std::string* outError = nullptr);

    [[nodiscard]] World& GetWorld();
    [[nodiscard]] const World& GetWorld() const;
    [[nodiscard]] const ReflectionRegistry& GetReflection() const;
    [[nodiscard]] SceneChangeTracker& GetSceneChangeTracker() noexcept;
    [[nodiscard]] const SceneChangeTracker& GetSceneChangeTracker() const noexcept;
    [[nodiscard]] nlohmann::json GetComponentProperties(
        EntityId entity,
        std::string_view component) const;

private:
    struct JournalEntry
    {
        std::uint64_t transactionId = 0;
        std::vector<nlohmann::json> requests;
        nlohmann::json beforeState;
        nlohmann::json afterState;
        SceneChangeCategory sceneChanges =
            SceneChangeCategory::None;
    };

    struct ActiveTransaction
    {
        std::uint64_t id = 0;
        nlohmann::json beforeState;
        std::vector<nlohmann::json> requests;
        SceneChangeCategory sceneChanges =
            SceneChangeCategory::None;
    };

    [[nodiscard]] nlohmann::json ExecuteUncached(
        const std::string& requestId,
        const std::string& command,
        const nlohmann::json& arguments);
    [[nodiscard]] nlohmann::json ApplyCommand(
        const std::string& command,
        const nlohmann::json& arguments);
    [[nodiscard]] nlohmann::json DescribeEngine() const;
    [[nodiscard]] nlohmann::json SerializeEntity(const EntityRecord& entity) const;
    [[nodiscard]] nlohmann::json SerializeComponent(
        const EntityRecord& entity,
        std::string_view component) const;
    void SetComponentProperties(
        EntityRecord& entity,
        std::string_view component,
        const nlohmann::json& properties);

    [[nodiscard]] EntityId ReadEntityArgument(
        const nlohmann::json& arguments,
        std::string_view field = "entity") const;
    [[nodiscard]] std::filesystem::path ResolveProjectPath(const std::filesystem::path& path) const;
    [[nodiscard]] bool IsMutatingCommand(std::string_view command) const;
    [[nodiscard]] SceneChangeCategory ClassifySceneChange(
        std::string_view command,
        const nlohmann::json& arguments) const;
    [[nodiscard]] nlohmann::json MakeSuccess(
        const std::string& requestId,
        const std::string& command,
        std::uint64_t transactionId,
        nlohmann::json data = {}) const;
    [[nodiscard]] nlohmann::json MakeFailure(
        const std::string& requestId,
        const std::string& command,
        std::string code,
        std::string message) const;

    void AppendJournalEntry(JournalEntry entry);
    void ResetJournal(const nlohmann::json& initialState);

    std::filesystem::path m_projectRoot;
    World m_world;
    ReflectionRegistry m_reflection;
    double m_fixedDeltaSeconds = 1.0 / 60.0;
    std::uint64_t m_simulationTick = 0;
    std::uint64_t m_randomSeed = 1;
    std::uint64_t m_nextTransactionId = 1;
    nlohmann::json m_initialState;
    std::vector<JournalEntry> m_journal;
    std::size_t m_journalCursor = 0;
    std::optional<ActiveTransaction> m_activeTransaction;
    std::unordered_map<std::string, nlohmann::json> m_requestCache;
    SceneChangeTracker m_sceneChangeTracker;
};
} // namespace Prism::Engine
