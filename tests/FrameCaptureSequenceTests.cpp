#include "Core/Application/FrameCaptureSequence.h"
#include "Renderer/FrameDiagnostics.h"

#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>

namespace
{
void Expect(bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
}
template<class Callback> void Reject(Callback callback)
{
    bool rejected = false;
    try { callback(); } catch (const std::exception&) { rejected = true; }
    Expect(rejected, "Expected invalid capture sequence to fail.");
}
} // namespace

void TestFrameCaptureSequence(const std::filesystem::path& root)
{
    using Prism::Core::FrameCaptureSequence;
    using Json = nlohmann::json;
    const auto config = root / "sequence-input.json";
    const auto write = [&](const Json& value) { std::ofstream(config) << value.dump(); };
    for (const auto& invalid : {Json{{"version", 2}, {"frames", {1}}},
             Json{{"version", 1}, {"frames", Json::array()}},
             Json{{"version", 1}, {"frames", {0, 1}}},
             Json{{"version", 1}, {"frames", {2, 1}}},
             Json{{"version", 1}, {"frames", {1, 1}}},
             Json{{"version", 1}, {"frames", {-1}}},
             Json{{"version", 1}, {"frames", {1.5}}},
             Json{{"version", 1}, {"frames", {4294967296ULL}}},
             Json{{"version", 1}, {"frames", {1}}, {"typo", true}},
             Json{{"version", 2}, {"frames", {2}}, {"actions", {{{"frame", 1}, {"type", "unknown"}}}}},
             Json{{"version", 2}, {"frames", {2}}, {"actions", {{{"frame", 1}, {"type", "set-taa"}}}}},
             Json{{"version", 2}, {"frames", {2}}, {"actions", {{{"frame", 1}, {"type", "refresh-scene"}, {"enabled", true}}}}},
             Json{{"version", 2}, {"frames", {2}}, {"actions", {{{"frame", 3}, {"type", "refresh-scene"}}}}},
             Json{{"version", 2}, {"frames", {2}}, {"actions", {{{"frame", 1}, {"type", "set-taa"}, {"enabled", true}}, {{"frame", 1}, {"type", "set-taa"}, {"enabled", false}}}}}})
    {
        write(invalid);
        Reject([&] { FrameCaptureSequence sequence(config, root / "invalid"); });
        Expect(!std::filesystem::exists(root / "invalid"), "Invalid input reserved output.");
    }
    write({{"version", 1}, {"frames", {2, 3, 5}}});
    FrameCaptureSequence sequence(config, root / "sequence");
    Reject([&] { FrameCaptureSequence existing(config, root / "sequence"); });
    Reject([&] { sequence.RequireComplete(); });
    Reject([&] { (void)sequence.BeginFrame(2); });
    Expect(!sequence.BeginFrame(1), "Captured a non-sample frame.");
    Prism::Renderer::FrameDiagnostics recorder(root / "sequence" / "frames.jsonl", {});
    for (std::uint64_t frame = 2; frame <= 6; ++frame)
    {
        const auto request = sequence.BeginFrame(frame);
        recorder.BeginFrame(frame, static_cast<std::uint32_t>(frame % 3), (frame - 1) / 60.0, 1, "test");
        if (request)
        {
            Expect(request->frameId == frame, "Incorrect sample identity.");
            recorder.RequestCapture(request->imagePath, request->diagnosticsPath);
            Reject([&] { recorder.RequestCapture(root / "pending.bmp"); });
            Reject([&] { recorder.RequestCapture(root / "pending.bmp", root / "pending.json"); });
            recorder.RecordView("game", {{{"sharedSimulationPassCounts", Json::object()}}});
            recorder.RecordCapture("game");
        }
        recorder.CompleteFrame();
        if (request)
        {
            recorder.ResolveCapture(false);
            sequence.ResolveCapture(false, {});
            Reject([&] { sequence.RequireComplete(); });
            recorder.ResolveCapture(true);
            sequence.ResolveCapture(true, {});
            std::ifstream report(request->diagnosticsPath);
            Expect(Json::parse(report).at("recordedFrameId") == frame, "Capture sidecar was replaced by a later frame.");
        }
        // Persistent Vulkan-style completion must not acknowledge future samples.
        recorder.ResolveCapture(true);
        sequence.ResolveCapture(true, {});
    }
    sequence.RequireComplete();
    Expect(sequence.IsComplete() && sequence.LastSampleFrame() == 5, "Final sequence completion failed.");
    Reject([&] { (void)sequence.BeginFrame(6); });

    write({{"version", 1}, {"frames", {1, 2}}});
    FrameCaptureSequence busy(config, root / "busy");
    (void)busy.BeginFrame(1);
    Reject([&] { (void)busy.BeginFrame(2); });
    Reject([&] { busy.RequireComplete(); });
    Reject([&] { busy.ResolveCapture(true, "save failed"); });
    Expect(!busy.IsComplete(), "Failed image acknowledged success.");
    FrameCaptureSequence collision(config, root / "collision-sequence");
    std::ofstream(root / "collision-sequence" / "frame-1.bmp") << "keep";
    Reject([&] { (void)collision.BeginFrame(1); });

    write({{"version", 2}, {"frames", {2, 4}}, {"actions", {
        {{"frame", 1}, {"type", "set-taa"}, {"enabled", false}},
        {{"frame", 2}, {"type", "refresh-scene"}},
        {{"frame", 3}, {"type", "set-shadows"}, {"enabled", false}},
        {{"frame", 4}, {"type", "activate-streaming"}}}}});
    FrameCaptureSequence controlled(config, root / "controlled-sequence");
    using ActionType = Prism::Core::FrameCaptureActionType;
    Expect(controlled.HasAction(ActionType::SetTemporalAntiAliasing)
        && controlled.HasAction(ActionType::ActivateStreamingScene),
        "Capture sequence action inventory was lost.");
    for (std::uint64_t frame = 1; frame <= 4; ++frame)
    {
        const auto actions = controlled.ConsumeActions(frame);
        Expect(actions.size() == 1 && actions.front().frameId == frame,
            "Capture sequence action ran on the wrong frame.");
        const auto request = controlled.BeginFrame(frame);
        if (request) controlled.ResolveCapture(true, {});
    }
    controlled.RequireComplete();
    Reject([&] { (void)controlled.ConsumeActions(4); });

    write({{"version", 2}, {"frames", {1, 3, 5}}, {"actions", {
        {{"frame", 2}, {"type", "activate-demo-scene"}, {"scene", "terrain-vt"}},
        {{"frame", 4}, {"type", "activate-demo-scene"}, {"scene", "preview"}}}}});
    FrameCaptureSequence sceneTransitions(config, root / "scene-transition-sequence");
    Expect(sceneTransitions.HasAction(ActionType::ActivateDemoScene),
        "Capture sequence lost demo-scene transition actions.");
    for (std::uint64_t frame = 1; frame <= 5; ++frame)
    {
        const auto actions = sceneTransitions.ConsumeActions(frame);
        if (frame == 2 || frame == 4)
            Expect(actions.size() == 1 && !actions.front().demoScene.empty(),
                "Demo-scene action did not retain its parsed scene id.");
        const auto request = sceneTransitions.BeginFrame(frame);
        if (request) sceneTransitions.ResolveCapture(true, {});
    }
    sceneTransitions.RequireComplete();
}
