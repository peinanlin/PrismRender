#include "Renderer/QueueSchedulingCostModel.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <fstream>
#include <limits>
#include <thread>
#include <utility>

#include <json.hpp>

namespace Prism::Renderer
{
namespace
{
using json = nlohmann::json;

constexpr double EmaAlpha = 0.25;
constexpr std::size_t MinimumSamplesPerMode = 2;
constexpr std::size_t RecentSampleCapacity = 64;
constexpr std::uint64_t ExplorationEvaluationInterval = 120;
constexpr auto HistoryMaximumAge = std::chrono::hours(24 * 14);
constexpr auto LockMaximumAge = std::chrono::minutes(5);
constexpr auto PersistenceInterval = std::chrono::seconds(60);
constexpr std::size_t LockAttemptCount = 100;

struct Interval
{
    double begin = 0.0;
    double end = 0.0;
};

std::int64_t CurrentUnixMilliseconds()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

class StorageLock final
{
public:
    explicit StorageLock(const std::filesystem::path& storagePath)
    {
        if (storagePath.empty())
        {
            m_acquired = true;
            return;
        }
        m_path = storagePath;
        m_path += ".lock";
        std::error_code error;
        if (!m_path.parent_path().empty())
        {
            std::filesystem::create_directories(
                m_path.parent_path(),
                error);
        }
        for (std::size_t attempt = 0;
             attempt < LockAttemptCount;
             ++attempt)
        {
            error.clear();
            if (std::filesystem::create_directory(
                    m_path,
                    error))
            {
                m_acquired = true;
                return;
            }
            error.clear();
            const auto writeTime =
                std::filesystem::last_write_time(
                    m_path,
                    error);
            if (!error
                && std::filesystem::file_time_type::clock::now()
                       - writeTime
                    > LockMaximumAge)
            {
                std::filesystem::remove(m_path, error);
                continue;
            }
            std::this_thread::sleep_for(
                std::chrono::milliseconds(5));
        }
    }

    ~StorageLock()
    {
        if (!m_acquired || m_path.empty())
        {
            return;
        }
        std::error_code error;
        std::filesystem::remove(m_path, error);
    }

    [[nodiscard]] bool IsAcquired() const
    {
        return m_acquired;
    }

private:
    std::filesystem::path m_path;
    bool m_acquired = false;
};

double ComputeOverlap(
    std::vector<Interval> graphics,
    std::vector<Interval> compute)
{
    std::ranges::sort(graphics, {}, &Interval::begin);
    std::ranges::sort(compute, {}, &Interval::begin);
    double overlap = 0.0;
    std::size_t graphicsIndex = 0;
    std::size_t computeIndex = 0;
    while (graphicsIndex < graphics.size()
           && computeIndex < compute.size())
    {
        overlap += std::max(
            0.0,
            std::min(
                graphics[graphicsIndex].end,
                compute[computeIndex].end)
                - std::max(
                    graphics[graphicsIndex].begin,
                    compute[computeIndex].begin));
        if (graphics[graphicsIndex].end
            < compute[computeIndex].end)
        {
            ++graphicsIndex;
        }
        else
        {
            ++computeIndex;
        }
    }
    return overlap;
}

template <typename Statistics>
json SerializeStatistics(const Statistics& statistics)
{
    return {
        {"samples", statistics.samples},
        {"emaMilliseconds",
         statistics.exponentialMilliseconds},
        {"lastUpdatedUnixMilliseconds",
         statistics.lastUpdatedUnixMilliseconds},
        {"recentMilliseconds",
         statistics.recentMilliseconds}};
}

json SerializeProfile(
    const QueueSchedulingGraphProfile& profile)
{
    json passes = json::array();
    for (const QueueSchedulingPassProfile& pass :
         profile.passes)
    {
        passes.push_back({
            {"name", pass.name},
            {"queue",
             pass.queue
                     == RHI::CommandQueueType::Compute
                 ? "compute"
                 : "graphics"},
            {"dependencies", pass.dependencies}});
    }
    return {
        {"graphSignature", profile.graphSignature},
        {"overlapOpportunityCount",
         profile.overlapOpportunityCount},
        {"passes", std::move(passes)}};
}

QueueSchedulingGraphProfile ReadProfile(
    const json& source)
{
    QueueSchedulingGraphProfile profile{};
    profile.graphSignature = source.value(
        "graphSignature",
        std::string{});
    profile.overlapOpportunityCount = source.value(
        "overlapOpportunityCount",
        0u);
    for (const json& pass :
         source.value("passes", json::array()))
    {
        QueueSchedulingPassProfile item{};
        item.name = pass.value("name", std::string{});
        item.queue =
            pass.value("queue", std::string("graphics"))
                    == "compute"
                ? RHI::CommandQueueType::Compute
                : RHI::CommandQueueType::Graphics;
        item.dependencies = pass.value(
            "dependencies",
            std::vector<std::size_t>{});
        if (!item.name.empty())
        {
            profile.passes.push_back(
                std::move(item));
        }
    }
    return profile;
}
} // namespace

QueueSchedulingCostModel::~QueueSchedulingCostModel()
{
    if (m_persistenceDirty)
    {
        (void)Save();
    }
}

void QueueSchedulingCostModel::Configure(
    RuntimeIdentity identity,
    std::filesystem::path storagePath)
{
    if (m_persistenceDirty)
    {
        (void)Save();
    }
    m_identity = std::move(identity);
    m_storagePath = std::move(storagePath);
    m_entries.clear();
    m_persistenceDirty = false;
    m_lastPersistenceAttempt =
        std::chrono::steady_clock::now();
    (void)Load();
}

void QueueSchedulingCostModel::UpdateDimensions(
    const std::uint32_t width,
    const std::uint32_t height)
{
    m_identity.width = width;
    m_identity.height = height;
}

void QueueSchedulingCostModel::AddSample(
    RunningStatistics& statistics,
    const double milliseconds)
{
    if (milliseconds < 0.0
        || !std::isfinite(milliseconds))
    {
        return;
    }
    statistics.exponentialMilliseconds =
        statistics.samples == 0
        ? milliseconds
        : statistics.exponentialMilliseconds
              * (1.0 - EmaAlpha)
            + milliseconds * EmaAlpha;
    ++statistics.samples;
    statistics.lastUpdatedUnixMilliseconds =
        CurrentUnixMilliseconds();
    statistics.recentMilliseconds.push_back(
        milliseconds);
    if (statistics.recentMilliseconds.size()
        > RecentSampleCapacity)
    {
        statistics.recentMilliseconds.erase(
            statistics.recentMilliseconds.begin(),
            statistics.recentMilliseconds.begin()
                + static_cast<std::ptrdiff_t>(
                    statistics.recentMilliseconds.size()
                    - RecentSampleCapacity));
    }
}

double QueueSchedulingCostModel::Percentile(
    const RunningStatistics& statistics,
    const double percentile)
{
    if (statistics.recentMilliseconds.empty())
    {
        return statistics.exponentialMilliseconds;
    }
    std::vector<double> sorted =
        statistics.recentMilliseconds;
    std::ranges::sort(sorted);
    const double clamped =
        std::clamp(percentile, 0.0, 1.0);
    const std::size_t index =
        static_cast<std::size_t>(std::ceil(
            clamped
            * static_cast<double>(sorted.size())))
            - (clamped > 0.0 ? 1u : 0u);
    return sorted[std::min(
        index,
        sorted.size() - 1)];
}

bool QueueSchedulingCostModel::IsFresh(
    const RunningStatistics& statistics,
    const std::int64_t nowUnixMilliseconds)
{
    if (statistics.samples == 0
        || statistics.lastUpdatedUnixMilliseconds <= 0)
    {
        return false;
    }
    const auto age = std::chrono::milliseconds(
        std::max<std::int64_t>(
            0,
            nowUnixMilliseconds
                - statistics
                      .lastUpdatedUnixMilliseconds));
    return age <= HistoryMaximumAge;
}

void QueueSchedulingCostModel::Observe(
    const std::string_view graphSignature,
    const bool nativeMultiQueue,
    const std::span<const QueueTimingSample> timings)
{
    QueueSchedulingGraphProfile profile{};
    profile.graphSignature = graphSignature;
    Observe(profile, nativeMultiQueue, timings);
}

void QueueSchedulingCostModel::Observe(
    const QueueSchedulingGraphProfile& profile,
    const bool nativeMultiQueue,
    const std::span<const QueueTimingSample> timings)
{
    if (profile.graphSignature.empty()
        || timings.empty())
    {
        return;
    }

    Entry& entry =
        m_entries[BuildKey(profile.graphSignature)];
    if (!profile.passes.empty())
    {
        entry.profile = profile;
    }
    ModeStatistics& mode =
        nativeMultiQueue
        ? entry.native
        : entry.serial;
    std::vector<Interval> graphicsIntervals;
    std::vector<Interval> computeIntervals;
    double rendererMilliseconds = -1.0;
    double computeBusyMilliseconds = 0.0;
    for (const QueueTimingSample& timing : timings)
    {
        if (timing.name == "Renderer")
        {
            rendererMilliseconds =
                timing.milliseconds;
            continue;
        }
        AddSample(
            mode.passes[timing.name],
            timing.milliseconds);
        if (timing.queue
                == RHI::CommandQueueType::Compute)
        {
            computeBusyMilliseconds +=
                timing.milliseconds;
        }
        if (!timing.calibrated
            || timing.endMilliseconds
                < timing.startMilliseconds)
        {
            continue;
        }
        auto& intervals =
            timing.queue
                    == RHI::CommandQueueType::Compute
                ? computeIntervals
                : graphicsIntervals;
        intervals.push_back({
            timing.startMilliseconds,
            timing.endMilliseconds});
    }
    if (rendererMilliseconds < 0.0)
    {
        MarkPersistenceDirty();
        PersistIfDue();
        return;
    }
    AddSample(mode.renderer, rendererMilliseconds);
    AddSample(
        mode.computeBusy,
        computeBusyMilliseconds);
    AddSample(
        mode.overlap,
        ComputeOverlap(
            std::move(graphicsIntervals),
            std::move(computeIntervals)));
    MarkPersistenceDirty();
    PersistIfDue();
}

QueueSchedulingDecision
QueueSchedulingCostModel::Evaluate(
    const std::string_view graphSignature,
    const std::size_t overlapOpportunityCount)
{
    QueueSchedulingGraphProfile profile{};
    profile.graphSignature = graphSignature;
    profile.overlapOpportunityCount =
        overlapOpportunityCount;
    return Evaluate(profile);
}

QueueSchedulingDecision
QueueSchedulingCostModel::Evaluate(
    const QueueSchedulingGraphProfile& profile)
{
    if (profile.graphSignature.empty())
    {
        return {};
    }
    QueueSchedulingDecision decision =
        EvaluateUnlocked(profile);
    MarkPersistenceDirty();
    PersistIfDue();
    return decision;
}

void QueueSchedulingCostModel::MarkPersistenceDirty()
{
    if (!m_storagePath.empty())
    {
        m_persistenceDirty = true;
    }
}

void QueueSchedulingCostModel::PersistIfDue()
{
    if (!m_persistenceDirty)
    {
        return;
    }
    const auto now = std::chrono::steady_clock::now();
    if (now - m_lastPersistenceAttempt
        < PersistenceInterval)
    {
        return;
    }
    (void)Save();
}

QueueSchedulingDecision
QueueSchedulingCostModel::EvaluateUnlocked(
    const QueueSchedulingGraphProfile& profile)
{
    QueueSchedulingDecision decision{};
    Entry& entry =
        m_entries[BuildKey(profile.graphSignature)];
    if (!profile.passes.empty())
    {
        entry.profile = profile;
    }
    ++entry.evaluationCount;

    const std::int64_t now =
        CurrentUnixMilliseconds();
    const bool serialFresh =
        IsFresh(entry.serial.renderer, now);
    const bool nativeFresh =
        IsFresh(entry.native.renderer, now);
    decision.serialSamples =
        entry.serial.renderer.samples;
    decision.nativeSamples =
        entry.native.renderer.samples;
    decision.serialP50Milliseconds =
        Percentile(entry.serial.renderer, 0.50);
    decision.serialP95Milliseconds =
        Percentile(entry.serial.renderer, 0.95);
    decision.nativeP50Milliseconds =
        Percentile(entry.native.renderer, 0.50);
    decision.nativeP95Milliseconds =
        Percentile(entry.native.renderer, 0.95);
    decision.historyExpired =
        (decision.serialSamples > 0 && !serialFresh)
        || (decision.nativeSamples > 0 && !nativeFresh);
    decision.exactHistoryAvailable =
        serialFresh
        && nativeFresh
        && decision.serialSamples
               >= MinimumSamplesPerMode
        && decision.nativeSamples
               >= MinimumSamplesPerMode;
    decision.historyAvailable =
        decision.exactHistoryAvailable;

    if (decision.exactHistoryAvailable)
    {
        decision.predictionAvailable = true;
        decision.predictionSource =
            "exact_measurement";
        decision.estimatedSerialMilliseconds =
            decision.serialP50Milliseconds;
        decision.estimatedNativeMilliseconds =
            decision.nativeP50Milliseconds;
        decision.estimatedOverlapMilliseconds =
            Percentile(entry.native.overlap, 0.50);
        decision.estimatedNetBenefitMilliseconds =
            decision.estimatedSerialMilliseconds
            - decision.estimatedNativeMilliseconds;
        decision.estimatedSubmissionCostMilliseconds =
            std::max(
                0.0,
                decision.estimatedNativeMilliseconds
                    - decision
                          .estimatedSerialMilliseconds
                    + decision
                          .estimatedOverlapMilliseconds);
    }
    else
    {
        (void)PredictFromPassHistory(
            entry,
            decision);
    }

    decision.requiredBenefitMilliseconds =
        std::max(
            0.05,
            decision.estimatedSerialMilliseconds
                * 0.03);
    decision.confidence = std::min(
        1.0,
        static_cast<double>(std::min(
            decision.serialSamples,
            decision.nativeSamples))
            / 8.0);

    if (profile.overlapOpportunityCount == 0)
    {
        decision.reason =
            "no_static_overlap_opportunity";
        entry.lastDecisionNative = false;
    }
    else if (decision.exactHistoryAvailable
             && decision
                    .estimatedNetBenefitMilliseconds
                 <= 0.0)
    {
        decision.reason =
            "measured_negative_benefit";
        entry.lastDecisionNative = false;
    }
    else if (decision.exactHistoryAvailable
             && !entry.lastDecisionNative
             && decision
                    .estimatedNetBenefitMilliseconds
                 < decision
                       .requiredBenefitMilliseconds)
    {
        decision.reason =
            "benefit_below_enable_threshold";
        entry.lastDecisionNative = false;
    }
    else if (decision.exactHistoryAvailable)
    {
        decision.selectNative = true;
        decision.reason =
            entry.lastDecisionNative
            ? "positive_benefit_hysteresis"
            : "measured_positive_benefit";
        entry.lastDecisionNative = true;
    }
    else if (serialFresh
             && entry.evaluationCount
                    % ExplorationEvaluationInterval
                == 0)
    {
        decision.selectNative = true;
        decision.controlledExploration = true;
        decision.reason =
            decision.historyExpired
            ? "controlled_refresh_probe"
            : "controlled_native_probe";
        entry.lastDecisionNative = false;
    }
    else
    {
        decision.reason =
            decision.predictionAvailable
            ? "prediction_requires_measurement"
            : decision.historyExpired
                ? "history_expired"
                : "insufficient_history";
        entry.lastDecisionNative = false;
    }
    return decision;
}

bool QueueSchedulingCostModel::PredictFromPassHistory(
    const Entry& entry,
    QueueSchedulingDecision& decision) const
{
    if (entry.profile.passes.empty())
    {
        return false;
    }

    const auto findPassMilliseconds =
        [&](const std::string& name)
            -> double
        {
            const auto exact =
                entry.serial.passes.find(name);
            if (exact != entry.serial.passes.end()
                && exact->second.samples > 0)
            {
                return Percentile(
                    exact->second,
                    0.50);
            }
            std::vector<double> candidates;
            for (const auto& [key, candidateEntry] :
                 m_entries)
            {
                (void)key;
                const auto candidate =
                    candidateEntry.serial.passes.find(
                        name);
                if (candidate
                        != candidateEntry
                               .serial.passes.end()
                    && candidate->second.samples > 0)
                {
                    candidates.push_back(
                        Percentile(
                            candidate->second,
                            0.50));
                }
            }
            if (candidates.empty())
            {
                return -1.0;
            }
            std::ranges::sort(candidates);
            return candidates[
                candidates.size() / 2];
        };

    std::vector<double> passCosts;
    passCosts.reserve(entry.profile.passes.size());
    double passSum = 0.0;
    bool hasGraphics = false;
    bool hasCompute = false;
    for (const QueueSchedulingPassProfile& pass :
         entry.profile.passes)
    {
        const double cost =
            findPassMilliseconds(pass.name);
        if (cost < 0.0)
        {
            return false;
        }
        passCosts.push_back(cost);
        passSum += cost;
        hasCompute =
            hasCompute
            || pass.queue
                == RHI::CommandQueueType::Compute;
        hasGraphics =
            hasGraphics
            || pass.queue
                == RHI::CommandQueueType::Graphics;
    }
    if (!hasGraphics || !hasCompute || passSum <= 0.0)
    {
        return false;
    }

    std::array<double, 2> queueAvailable{};
    std::vector<double> passEnds(
        entry.profile.passes.size(),
        0.0);
    for (std::size_t passIndex = 0;
         passIndex < entry.profile.passes.size();
         ++passIndex)
    {
        const QueueSchedulingPassProfile& pass =
            entry.profile.passes[passIndex];
        const std::size_t queueIndex =
            pass.queue
                    == RHI::CommandQueueType::Compute
                ? 1u
                : 0u;
        double begin = queueAvailable[queueIndex];
        for (const std::size_t dependency :
             pass.dependencies)
        {
            if (dependency < passEnds.size())
            {
                begin = std::max(
                    begin,
                    passEnds[dependency]);
            }
        }
        passEnds[passIndex] =
            begin + passCosts[passIndex];
        queueAvailable[queueIndex] =
            passEnds[passIndex];
    }
    const double nativeCriticalMilliseconds =
        std::max(
            queueAvailable[0],
            queueAvailable[1]);
    const double predictedPassOverlap =
        std::max(
            0.0,
            passSum
                - nativeCriticalMilliseconds);

    double submissionCost = 0.0;
    std::size_t submissionSamples = 0;
    for (const auto& [key, candidate] :
         m_entries)
    {
        (void)key;
        if (candidate.serial.renderer.samples
                < MinimumSamplesPerMode
            || candidate.native.renderer.samples
                < MinimumSamplesPerMode)
        {
            continue;
        }
        const double measuredCost = std::max(
            0.0,
            Percentile(candidate.native.renderer, 0.50)
                - Percentile(
                    candidate.serial.renderer,
                    0.50)
                + Percentile(
                    candidate.native.overlap,
                    0.50));
        submissionCost += measuredCost;
        ++submissionSamples;
    }
    if (submissionSamples > 0)
    {
        submissionCost /=
            static_cast<double>(submissionSamples);
    }

    const double serialEstimate =
        entry.serial.renderer.samples > 0
        ? Percentile(entry.serial.renderer, 0.50)
        : passSum;
    const double passScale =
        serialEstimate / passSum;
    decision.predictionAvailable = true;
    decision.predictedFromPassHistory = true;
    decision.predictionSource =
        "dag_pass_history";
    decision.estimatedSerialMilliseconds =
        serialEstimate;
    decision.estimatedOverlapMilliseconds =
        predictedPassOverlap * passScale;
    decision.estimatedSubmissionCostMilliseconds =
        submissionCost;
    decision.estimatedNativeMilliseconds =
        std::max(
            0.0,
            serialEstimate
                - decision
                      .estimatedOverlapMilliseconds
                + submissionCost);
    decision.estimatedNetBenefitMilliseconds =
        decision.estimatedSerialMilliseconds
        - decision.estimatedNativeMilliseconds;
    return true;
}

bool QueueSchedulingCostModel::Load()
{
    StorageLock lock(m_storagePath);
    if (!lock.IsAcquired())
    {
        return false;
    }
    const bool loaded = LoadUnlocked();
    m_lastPersistenceAttempt =
        std::chrono::steady_clock::now();
    if (loaded)
    {
        m_persistenceDirty = false;
    }
    return loaded;
}

bool QueueSchedulingCostModel::LoadUnlocked()
{
    if (m_storagePath.empty())
    {
        return true;
    }
    m_entries.clear();
    if (!std::filesystem::exists(m_storagePath))
    {
        return true;
    }
    try
    {
        std::ifstream input(
            m_storagePath,
            std::ios::binary);
        if (!input)
        {
            return false;
        }
        json document;
        input >> document;
        const std::uint32_t version =
            document.value("version", 0u);
        if (document.value("format", std::string{})
                != "PrismGpuQueueCostModel"
            || (version != 1u && version != 2u))
        {
            return false;
        }
        const auto readStatistics =
            [version](const json& source)
            {
                RunningStatistics result{};
                result.samples =
                    source.value("samples", 0u);
                result.exponentialMilliseconds =
                    source.value(
                        "emaMilliseconds",
                        0.0);
                result.lastUpdatedUnixMilliseconds =
                    source.value(
                        "lastUpdatedUnixMilliseconds",
                        version == 1u
                            ? CurrentUnixMilliseconds()
                            : 0ll);
                result.recentMilliseconds =
                    source.value(
                        "recentMilliseconds",
                        std::vector<double>{});
                if (result.recentMilliseconds.empty()
                    && result.samples > 0)
                {
                    result.recentMilliseconds.push_back(
                        result
                            .exponentialMilliseconds);
                }
                return result;
            };
        const auto readMode =
            [&](const json& source)
            {
                ModeStatistics mode{};
                mode.renderer =
                    readStatistics(source.value(
                        "renderer",
                        json::object()));
                mode.computeBusy =
                    readStatistics(source.value(
                        "computeBusy",
                        json::object()));
                mode.overlap =
                    readStatistics(source.value(
                        "overlap",
                        json::object()));
                const json passes = source.value(
                    "passes",
                    json::object());
                for (const auto& [name, value] :
                     passes.items())
                {
                    mode.passes[name] =
                        readStatistics(value);
                }
                return mode;
            };
        const json entries = document.value(
            "entries",
            json::object());
        for (const auto& [key, value] :
             entries.items())
        {
            Entry entry{};
            entry.serial = readMode(
                value.value(
                    "serial",
                    json::object()));
            entry.native = readMode(
                value.value(
                    "native",
                    json::object()));
            entry.profile = ReadProfile(
                value.value(
                    "profile",
                    json::object()));
            entry.lastDecisionNative =
                value.value(
                    "lastDecisionNative",
                    false);
            entry.evaluationCount =
                value.value(
                    "evaluationCount",
                    0ull);
            m_entries.emplace(
                key,
                std::move(entry));
        }
        return true;
    }
    catch (...)
    {
        m_entries.clear();
        return false;
    }
}

bool QueueSchedulingCostModel::Save() const
{
    m_lastPersistenceAttempt =
        std::chrono::steady_clock::now();
    StorageLock lock(m_storagePath);
    if (!lock.IsAcquired())
    {
        return false;
    }
    const bool saved = SaveUnlocked();
    if (saved)
    {
        m_persistenceDirty = false;
    }
    return saved;
}

bool QueueSchedulingCostModel::SaveUnlocked() const
{
    if (m_storagePath.empty())
    {
        return true;
    }
    try
    {
        json entries = json::object();
        const auto serializeMode =
            [](const ModeStatistics& mode)
            {
                json passes = json::object();
                for (const auto& [name, statistics] :
                     mode.passes)
                {
                    passes[name] =
                        SerializeStatistics(statistics);
                }
                return json{
                    {"renderer",
                     SerializeStatistics(mode.renderer)},
                    {"computeBusy",
                     SerializeStatistics(
                         mode.computeBusy)},
                    {"overlap",
                     SerializeStatistics(mode.overlap)},
                    {"passes", std::move(passes)}};
            };
        for (const auto& [key, entry] :
             m_entries)
        {
            entries[key] = {
                {"serial",
                 serializeMode(entry.serial)},
                {"native",
                 serializeMode(entry.native)},
                {"profile",
                 SerializeProfile(entry.profile)},
                {"lastDecisionNative",
                 entry.lastDecisionNative},
                {"evaluationCount",
                 entry.evaluationCount}};
        }
        if (!m_storagePath.parent_path().empty())
        {
            std::filesystem::create_directories(
                m_storagePath.parent_path());
        }
        const std::filesystem::path temporary =
            m_storagePath.parent_path()
            / (m_storagePath.filename().string()
               + ".tmp");
        {
            std::ofstream output(
                temporary,
                std::ios::binary
                    | std::ios::trunc);
            if (!output)
            {
                return false;
            }
            output << json{
                {"format",
                 "PrismGpuQueueCostModel"},
                {"version", 2},
                {"entries",
                 std::move(entries)}}.dump(2)
                   << '\n';
            if (!output)
            {
                return false;
            }
        }
        std::error_code error;
        if (std::filesystem::exists(m_storagePath))
        {
            std::filesystem::remove(
                m_storagePath,
                error);
            if (error)
            {
                return false;
            }
        }
        std::filesystem::rename(
            temporary,
            m_storagePath,
            error);
        return !error;
    }
    catch (...)
    {
        return false;
    }
}

const std::filesystem::path&
QueueSchedulingCostModel::GetStoragePath() const
{
    return m_storagePath;
}

std::string QueueSchedulingCostModel::BuildKey(
    const std::string_view graphSignature) const
{
    return "stage18-v2|"
        + m_identity.graphicsApi + '|'
        + m_identity.adapterName + '|'
        + std::to_string(m_identity.width) + 'x'
        + std::to_string(m_identity.height) + '|'
        + std::string(graphSignature);
}
} // namespace Prism::Renderer
