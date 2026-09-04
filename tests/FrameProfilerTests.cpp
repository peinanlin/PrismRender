#include "Core/Profiling/FrameProfiler.h"
#include "Core/Profiling/FrameProfilerReport.h"
#include "Core/Profiling/ProfilingLevelSchedule.h"

#include <chrono>
#include <cmath>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>

#include <json.hpp>

namespace
{
void Expect(const bool condition, const char* const message)
{
    if (!condition)
    {
        throw std::runtime_error(message);
    }
}

template <typename Callback>
void Reject(Callback&& callback)
{
    bool rejected = false;
    try
    {
        callback();
    }
    catch (const std::exception&)
    {
        rejected = true;
    }
    Expect(rejected, "Invalid frame profiler input was accepted.");
}

Prism::Core::FrameProfilerSnapshot MakeSnapshot(
    const std::uint64_t frameId)
{
    using namespace Prism::Core;
    FrameProfilerSnapshot snapshot{};
    snapshot.frameId = frameId;
    snapshot.editorLoop = {4.0, true};
    snapshot.cpuLanes[static_cast<std::size_t>(FrameCpuLane::Main)] = {
        {3.0, true}, {0.25, true}, FrameProfiler::CurrentThreadId()};
    snapshot.views[static_cast<std::size_t>(ProfiledView::Game)].active = true;
    snapshot.views[static_cast<std::size_t>(ProfiledView::Game)].rendered = true;
    snapshot.views[static_cast<std::size_t>(ProfiledView::Game)].cpuRender = {
        1.5, true};
    return snapshot;
}
} // namespace

int main()
{
    using namespace Prism::Core;
    try
    {
        Expect(ToString(ProfilingLevel::Detailed) == "detailed",
            "Profiling level names are unstable.");
        Expect(ToString(FrameCpuLane::Worker) == "worker",
            "CPU lane names are unstable.");
        Expect(ToString(ProfiledView::Scene) == "scene",
            "View names are unstable.");
        Expect(ToString(FrameCpuPhase::SubmitAndPresent)
                == "submit-and-present"
                && ToString(FrameWaitPhase::Prepare) == "prepare",
            "Frame phase names are unstable.");
        Expect(ParseProfilingLevel("off") == ProfilingLevel::Off
                && ParseProfilingLevel("capture")
                    == ProfilingLevel::Capture,
            "Profiling levels were not parsed exactly.");
        Reject([] { (void)ParseProfilingLevel("verbose"); });

        const ProfilingLevelSchedule schedule =
            ProfilingLevelSchedule::Parse(
                "off:2,basic:3,detailed:2,capture:1");
        Expect(schedule.GetSegmentCount() == 4
                && schedule.GetTotalFrameCount() == 8,
            "Profiling level sequence did not retain its bounded segments.");
        Expect(schedule.GetLevel(1) == ProfilingLevel::Off
                && schedule.GetLevel(2) == ProfilingLevel::Off
                && schedule.GetLevel(3) == ProfilingLevel::Basic
                && schedule.GetLevel(6) == ProfilingLevel::Detailed
                && schedule.GetLevel(8) == ProfilingLevel::Capture,
            "Profiling level sequence switched at the wrong frame boundary.");
        Reject([]
        {
            (void)ProfilingLevelSchedule::Parse("basic:0");
        });
        Reject([]
        {
            (void)ProfilingLevelSchedule::Parse("basic:2,");
        });
        Reject([&] { (void)schedule.GetLevel(9); });

        auto levelProfiler = std::make_unique<FrameProfiler>();
        Expect(levelProfiler->GetRequestedLevel()
                == ProfilingLevel::Basic,
            "Basic is not the default profiling level.");
        levelProfiler->SetRequestedLevel(ProfilingLevel::Detailed);
        Expect(levelProfiler->GetRequestedLevel()
                == ProfilingLevel::Detailed,
            "Runtime profiling level did not affect subsequent frames.");

        auto associationProfiler =
            std::make_unique<FrameProfiler>();
        Expect(!associationProfiler->AssociateViewGpuTiming(
                    ProfiledView::Game, 1, 0, 0, 0, 0.0)
                    .has_value(),
            "GPU timing was reported before a completed generation existed.");
        const auto associated =
            associationProfiler->AssociateViewGpuTiming(
                ProfiledView::Game, 2, 1, 1, 0, 0.75);
        Expect(associated.has_value()
                && associated->frameId == 1
                && associated->generation == 1
                && associated->totalMilliseconds == 0.75,
            "GPU timing was not associated with its submitted CPU frame.");
        Expect(!associationProfiler->AssociateViewGpuTiming(
                    ProfiledView::Game, 3, 2, 1, 0, 0.75)
                    .has_value(),
            "A stale GPU generation was consumed twice.");
        Reject([&]
        {
            (void)associationProfiler->AssociateViewGpuTiming(
                ProfiledView::Game, 4, 3, 0, 0, 0.75);
        });

        FrameProfiler profiler;
        FrameProfilerSnapshot first = MakeSnapshot(1);
        profiler.SubmitCompleted(first);
        const auto latest = profiler.GetLatest();
        Expect(latest.has_value() && latest->frameId == 1
                && latest->completed,
            "Completed frame identity was not retained.");
        Expect(latest->activeViewMask == 1,
            "Active view mask was not derived from view state.");
        Expect(!latest->cpuLanes[
                    static_cast<std::size_t>(FrameCpuLane::Render)]
                    .active.available,
            "A missing render lane was reported as zero milliseconds.");

        auto workerProfiler = std::make_unique<FrameProfiler>();
        FrameProfilerSnapshot workerSnapshot = MakeSnapshot(1);
        auto& aggregateWorkerLane = workerSnapshot.cpuLanes[
            static_cast<std::size_t>(FrameCpuLane::Worker)];
        aggregateWorkerLane.active = {0.5, true};
        aggregateWorkerLane.waiting = {0.1, true};
        workerSnapshot.workers.available = true;
        workerSnapshot.workers.executing = aggregateWorkerLane.active;
        workerSnapshot.workers.queued = aggregateWorkerLane.waiting;
        workerProfiler->SubmitCompleted(std::move(workerSnapshot));
        Expect(workerProfiler->GetLatest()->workers.available
                && workerProfiler->GetLatest()->cpuLanes[
                    static_cast<std::size_t>(FrameCpuLane::Worker)]
                    .threadId == 0,
            "Aggregated worker timing required a fabricated thread ID.");
        Reject([]
        {
            auto missingThreadProfiler =
                std::make_unique<FrameProfiler>();
            FrameProfilerSnapshot invalid = MakeSnapshot(1);
            invalid.cpuLanes[
                static_cast<std::size_t>(FrameCpuLane::Render)]
                .active = {0.5, true};
            missingThreadProfiler->SubmitCompleted(std::move(invalid));
        });

        Reject([&] { profiler.SubmitCompleted(first); });
        Reject([&]
        {
            FrameProfilerSnapshot invalid = MakeSnapshot(2);
            invalid.editorLoop = {1.0, false};
            profiler.SubmitCompleted(invalid);
        });

        FrameProfiler generationProfiler;
        FrameProfilerSnapshot gpuFirst = MakeSnapshot(1);
        auto& gameFirst = gpuFirst.views[
            static_cast<std::size_t>(ProfiledView::Game)];
        gameFirst.gpuTotal = {0.8, true};
        gameFirst.gpuGeneration = 10;
        gameFirst.gpuResolvedFrameId = 1;
        generationProfiler.SubmitCompleted(gpuFirst);
        Reject([&]
        {
            FrameProfilerSnapshot stale = MakeSnapshot(2);
            auto& game = stale.views[
                static_cast<std::size_t>(ProfiledView::Game)];
            game.gpuTotal = {0.9, true};
            game.gpuGeneration = 9;
            game.gpuResolvedFrameId = 1;
            generationProfiler.SubmitCompleted(stale);
        });
        Reject([]
        {
            FrameProfiler futureProfiler;
            FrameProfilerSnapshot future = MakeSnapshot(1);
            auto& game = future.views[
                static_cast<std::size_t>(ProfiledView::Game)];
            game.gpuTotal = {0.9, true};
            game.gpuGeneration = 1;
            game.gpuResolvedFrameId = 2;
            futureProfiler.SubmitCompleted(future);
        });

        FrameProfiler crossThreadProfiler;
        std::uint64_t submittingThread = 0;
        std::thread producer([&]
        {
            FrameProfilerSnapshot snapshot = MakeSnapshot(1);
            submittingThread = FrameProfiler::CurrentThreadId();
            snapshot.cpuLanes[
                static_cast<std::size_t>(FrameCpuLane::Main)]
                .threadId = submittingThread;
            crossThreadProfiler.SubmitCompleted(std::move(snapshot));
        });
        producer.join();
        Expect(crossThreadProfiler.GetLatest()
                    ->cpuLanes[static_cast<std::size_t>(FrameCpuLane::Main)]
                    .threadId == submittingThread,
            "Cross-thread completed submission lost its thread identity.");

        auto ringProfiler = std::make_unique<FrameProfiler>();
        for (std::uint64_t frame = 1;
             frame <= FrameProfiler::MaximumHistoryFrames + 5;
             ++frame)
        {
            ringProfiler->SubmitCompleted(MakeSnapshot(frame));
        }
        const auto history = ringProfiler->GetHistory();
        Expect(history.size() == FrameProfiler::MaximumHistoryFrames,
            "Frame history is not bounded.");
        Expect(history.front().frameId == 6
                && history.back().frameId
                    == FrameProfiler::MaximumHistoryFrames + 5,
            "Frame history ring order is incorrect.");

        ringProfiler->Shutdown();
        ringProfiler->Shutdown();
        Expect(ringProfiler->IsShutdown(),
            "Frame profiler shutdown is not idempotent.");
        Reject([&]
        {
            ringProfiler->SubmitCompleted(
                MakeSnapshot(FrameProfiler::MaximumHistoryFrames + 6));
        });

        const std::filesystem::path reportRoot =
            std::filesystem::temp_directory_path()
            / ("prism-frame-profiler-"
                + std::to_string(
                    std::chrono::steady_clock::now()
                        .time_since_epoch().count()));
        const std::filesystem::path reportPath =
            reportRoot / "frames.jsonl";
        std::filesystem::create_directories(reportRoot);
        {
            FrameProfiler reportProfiler;
            FrameProfilerReport report(
                reportPath,
                nlohmann::json{{"test", true}});
            for (std::uint64_t frameId = 1; frameId <= 4; ++frameId)
            {
                FrameProfilerSnapshot snapshot = MakeSnapshot(frameId);
                snapshot.requestedLevel = schedule.GetLevel(frameId);
                snapshot.actualLevel = snapshot.requestedLevel;
                snapshot.profilerOverhead = {0.01, true};
                reportProfiler.SubmitCompleted(std::move(snapshot));
                report.WriteCompleted(*reportProfiler.GetLatest());
            }
            report.Finish(4);
            Reject([&] { report.Finish(4); });
        }
        std::ifstream reportInput(reportPath);
        std::string line;
        std::size_t lineCount = 0;
        nlohmann::json firstFrame;
        nlohmann::json footer;
        while (std::getline(reportInput, line))
        {
            const nlohmann::json row = nlohmann::json::parse(line);
            if (row.at("type") == "frame" && firstFrame.is_null())
            {
                firstFrame = row;
            }
            footer = row;
            ++lineCount;
        }
        Expect(lineCount == 6
                && firstFrame.at("actualLevel") == "off"
                && firstFrame.at("lanes").at("worker")
                    .at("activeMs").is_null()
                && !firstFrame.at("workers").at("available")
                && firstFrame.at("workers").at("queuedMs").is_null()
                && footer.at("type") == "footer"
                && footer.at("completedFrames") == 4,
            "Completed profiler snapshots were not exported faithfully.");
        reportInput.close();
        std::filesystem::remove_all(reportRoot);

        std::cout << "Frame profiler tests passed.\n";
        return 0;
    }
    catch (const std::exception& exception)
    {
        std::cerr << exception.what() << '\n';
        return 1;
    }
}
