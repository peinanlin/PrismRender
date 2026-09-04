#pragma once

#include "RHI/ICommandContext.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace Prism::Renderer
{
struct QueueTimingSample
{
    std::string name;
    float milliseconds = 0.0f;
    RHI::CommandQueueType queue =
        RHI::CommandQueueType::Graphics;
    double startMilliseconds = 0.0;
    double endMilliseconds = 0.0;
    bool calibrated = false;
};

struct QueueSchedulingPassProfile
{
    std::string name;
    RHI::CommandQueueType queue =
        RHI::CommandQueueType::Graphics;
    std::vector<std::size_t> dependencies;
};

struct QueueSchedulingGraphProfile
{
    std::string graphSignature;
    std::size_t overlapOpportunityCount = 0;
    std::vector<QueueSchedulingPassProfile> passes;
};

struct QueueSchedulingDecision
{
    bool historyAvailable = false;
    bool exactHistoryAvailable = false;
    bool predictionAvailable = false;
    bool predictedFromPassHistory = false;
    bool controlledExploration = false;
    bool historyExpired = false;
    bool selectNative = false;
    std::string reason = "history_unavailable";
    std::string predictionSource = "none";
    std::size_t serialSamples = 0;
    std::size_t nativeSamples = 0;
    double estimatedSerialMilliseconds = 0.0;
    double estimatedNativeMilliseconds = 0.0;
    double serialP50Milliseconds = 0.0;
    double serialP95Milliseconds = 0.0;
    double nativeP50Milliseconds = 0.0;
    double nativeP95Milliseconds = 0.0;
    double estimatedNetBenefitMilliseconds = 0.0;
    double estimatedSubmissionCostMilliseconds = 0.0;
    double estimatedOverlapMilliseconds = 0.0;
    double requiredBenefitMilliseconds = 0.0;
    double confidence = 0.0;
};

class QueueSchedulingCostModel
{
public:
    struct RuntimeIdentity
    {
        std::string graphicsApi;
        std::string adapterName;
        std::uint32_t width = 0;
        std::uint32_t height = 0;
    };

    QueueSchedulingCostModel() = default;
    ~QueueSchedulingCostModel();

    QueueSchedulingCostModel(
        const QueueSchedulingCostModel&) = delete;
    QueueSchedulingCostModel& operator=(
        const QueueSchedulingCostModel&) = delete;

    void Configure(
        RuntimeIdentity identity,
        std::filesystem::path storagePath);
    void UpdateDimensions(
        std::uint32_t width,
        std::uint32_t height);

    void Observe(
        std::string_view graphSignature,
        bool nativeMultiQueue,
        std::span<const QueueTimingSample> timings);
    void Observe(
        const QueueSchedulingGraphProfile& profile,
        bool nativeMultiQueue,
        std::span<const QueueTimingSample> timings);
    [[nodiscard]] QueueSchedulingDecision Evaluate(
        std::string_view graphSignature,
        std::size_t overlapOpportunityCount);
    [[nodiscard]] QueueSchedulingDecision Evaluate(
        const QueueSchedulingGraphProfile& profile);

    [[nodiscard]] bool Load();
    [[nodiscard]] bool Save() const;
    [[nodiscard]] const std::filesystem::path&
        GetStoragePath() const;

private:
    struct RunningStatistics
    {
        std::size_t samples = 0;
        double exponentialMilliseconds = 0.0;
        std::int64_t lastUpdatedUnixMilliseconds = 0;
        std::vector<double> recentMilliseconds;
    };

    struct ModeStatistics
    {
        RunningStatistics renderer;
        RunningStatistics computeBusy;
        RunningStatistics overlap;
        std::unordered_map<std::string, RunningStatistics>
            passes;
    };

    struct Entry
    {
        ModeStatistics serial;
        ModeStatistics native;
        QueueSchedulingGraphProfile profile;
        bool lastDecisionNative = false;
        std::uint64_t evaluationCount = 0;
    };

    [[nodiscard]] std::string BuildKey(
        std::string_view graphSignature) const;
    static void AddSample(
        RunningStatistics& statistics,
        double milliseconds);
    static double Percentile(
        const RunningStatistics& statistics,
        double percentile);
    static bool IsFresh(
        const RunningStatistics& statistics,
        std::int64_t nowUnixMilliseconds);
    [[nodiscard]] bool LoadUnlocked();
    [[nodiscard]] bool SaveUnlocked() const;
    void MarkPersistenceDirty();
    void PersistIfDue();
    [[nodiscard]] QueueSchedulingDecision EvaluateUnlocked(
        const QueueSchedulingGraphProfile& profile);
    [[nodiscard]] bool PredictFromPassHistory(
        const Entry& entry,
        QueueSchedulingDecision& decision) const;

    RuntimeIdentity m_identity;
    std::filesystem::path m_storagePath;
    std::unordered_map<std::string, Entry> m_entries;
    mutable bool m_persistenceDirty = false;
    mutable std::chrono::steady_clock::time_point
        m_lastPersistenceAttempt =
            std::chrono::steady_clock::now();
};
} // namespace Prism::Renderer
