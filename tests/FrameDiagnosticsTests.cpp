#include "Renderer/FrameDiagnostics.h"
#include "Tools/FrameDiagnosticsComparison.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>

void TestFrameCaptureSequence(const std::filesystem::path& root);

namespace
{
using Json = nlohmann::json;
using Prism::Renderer::FrameDiagnostics;

void Expect(bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
}

template<class Callback> void ExpectThrows(Callback callback)
{
    bool threw = false;
    try { callback(); } catch (const std::exception&) { threw = true; }
    Expect(threw, "Expected rejected diagnostic operation.");
}

Json Read(const std::filesystem::path& path)
{
    std::ifstream input(path);
    return Json::parse(input);
}

Prism::Renderer::RenderViewDiagnostics View(bool producer = true)
{
    const Json counts = producer ? Json{{"SpectralOcean.Evolution", 1}} : Json::object();
    return {{{"backend", "Direct3D 12"}, {"width", 1280}, {"height", 800},
        {"simulationProducer", producer}, {"recordedPassCounts", counts},
        {"sharedSimulationPassCounts", counts},
        {"recordedPasses", producer ? Json::array({"SpectralOcean.Evolution"}) : Json::array()},
        {"passCountMeaning", "completed-command-recording-or-replay-not-gpu-completion"},
        {"graphStateMeaning", "logical-rdg-final-state-and-declared-accesses-not-driver-validation"},
        {"history", {{"taa", {{"resetCallCount", 2}}}}},
        {"graph", {{"passes", Json::array({{{"name", "SpectralOcean.Evolution"},
            {"culled", !producer}, {"sideEffect", false}, {"dependencies", Json::array()},
            {"reads", Json::array()}, {"writes", Json::array()}, {"queue", "Compute"},
            {"executionIndex", 0}, {"parallelRecordable", true}, {"cpuMilliseconds", 1.0}}})},
            {"resources", Json::array({{{"name", "Ocean"}, {"kind", "texture"},
                {"stateBits", 8}, {"imported", true}, {"transient", false}, {"history", true},
                {"currentVersion", 1}, {"active", true}, {"textureSubresourceCount", 4},
                {"bufferStateRangeCount", 0}, {"nativeAllocation", 1234}}})},
            {"queueSync", Json::array()}, {"queueBatches", Json::array()}}}}};
}

void SyncSource(Json& capture)
{
    capture["source"] = capture["frame"]["views"][capture["view"].get<std::string>()];
}

void TestComparison(const Json& reference)
{
    const auto compare = [&](const Json& candidate) {
        return Prism::Tools::CompareFrameDiagnostics(reference, candidate);
    };
    Expect(compare(reference).at("passed"), "Identical captures must pass.");
    auto changed = reference;
    changed["frame"]["views"]["game"]["history"]["taa"]["resetCallCount"] = 3;
    SyncSource(changed);
    Expect(!compare(changed).at("passed"), "History reset difference was ignored.");
    changed = reference;
    changed["recordedFrameId"] = 99;
    Expect(!compare(changed).at("passed"), "Wrong frame was accepted.");
    changed = reference;
    changed["sourceFresh"] = false;
    Expect(!compare(changed).at("passed"), "Stale capture was accepted.");
    changed = reference;
    changed["view"] = "scene";
    Expect(!compare(changed).at("passed"), "Wrong view was accepted.");
    changed = reference;
    changed["frame"]["views"]["game"]["graph"]["resources"][0]["stateBits"] = 16;
    SyncSource(changed);
    Expect(!compare(changed).at("passed"), "Resource state difference was ignored.");
    changed = reference;
    changed["frame"]["views"]["game"]["recordedPassCounts"]["SpectralOcean.Evolution"] = 2;
    SyncSource(changed);
    Expect(!compare(changed).at("passed"), "Incorrect simulation/pass counts were accepted.");
    changed = reference;
    auto& game = changed["frame"]["views"]["game"];
    game["recordedPasses"].push_back("SpectralOcean.Evolution");
    game["recordedPassCounts"]["SpectralOcean.Evolution"] = 2;
    game["sharedSimulationPassCounts"]["SpectralOcean.Evolution"] = 2;
    changed["frame"]["sharedSimulationPassCounts"]["SpectralOcean.Evolution"] = 2;
    SyncSource(changed);
    Expect(!compare(changed).at("passed"), "Actual duplicate simulation was ignored.");
    changed = reference;
    changed["frame"]["views"]["game"]["graph"]["passes"][0]["cpuMilliseconds"] = 999;
    changed["frame"]["views"]["game"]["graph"]["resources"][0]["nativeAllocation"] = 4567;
    SyncSource(changed);
    Expect(compare(changed).at("passed"), "Timing/allocation noise must not change semantics.");
    changed = reference;
    changed["frame"]["views"]["game"]["graph"]["passes"][0]["queue"] = "Graphics";
    SyncSource(changed);
    Expect(!compare(changed).at("passed"), "Same-backend queue difference was ignored.");
    for (auto& view : changed["frame"]["views"]) view["backend"] = "Vulkan";
    SyncSource(changed);
    const auto cross = compare(changed);
    Expect(cross.at("passed") && cross.at("kind") == "cross-backend", "Cross-backend semantic policy failed.");
    changed["frame"]["views"]["game"]["graph"]["resources"][0]["stateBits"] = 16;
    SyncSource(changed);
    Expect(!compare(changed).at("passed"), "Cross-backend state hazard was ignored.");
    changed = reference;
    changed["frame"].erase("views");
    Expect(!compare(changed).at("passed"), "Malformed diagnostics were accepted.");
}

void TestRecorder(const std::filesystem::path& root)
{
    const auto log = root / "frames.jsonl";
    const auto capture = root / "capture.json";
    {
        FrameDiagnostics recorder(log, capture);
        ExpectThrows([&] { recorder.RequestCapture(log); });
        ExpectThrows([&] { recorder.RequestCapture(capture); });
        ExpectThrows([&] { recorder.CompleteFrame(); });
        ExpectThrows([&] { recorder.ResolveCapture(true); });
        recorder.RequestCapture(root / "image.bmp");
        recorder.BeginFrame(1, 0, 0.0, 1, "HPWater");
        ExpectThrows([&] { recorder.BeginFrame(2, 1, 1.0 / 60, 2, "HPWater"); });
        ExpectThrows([&] { recorder.RecordView("invalid", View()); });
        recorder.RecordView("game", View());
        recorder.RecordView("scene", View(false));
        ExpectThrows([&] { recorder.RecordView("game", View()); });
        recorder.RecordCapture("game");
        recorder.CompleteFrame();
        recorder.ResolveCapture(false);
        recorder.ResolveCapture(true, "capture write failed");
        Expect(!std::filesystem::exists(capture), "Unresolved capture was published.");
        // Later frames and repeated RecordCapture must not replace the image's source.
        recorder.BeginFrame(2, 1, 1.0 / 60, 2, "OtherScene");
        recorder.RecordView("game", View());
        recorder.RecordCapture("game");
        recorder.CompleteFrame();
        recorder.ResolveCapture(true);
        recorder.ResolveCapture(true); // Vulkan exposes a persistent completion flag.
        ExpectThrows([&] { recorder.BeginFrame(2, 1, 1.0 / 60, 2, "OtherScene"); });
    }
    const auto report = Read(capture);
    Expect(report.at("recordedFrameId") == 1 && report.at("source").at("scene") == "HPWater",
        "Delayed capture lost original identity.");
    Expect(report.at("frame").at("views").size() == 2, "Second view was lost.");
    Expect(report.at("frame").at("sharedSimulationPassCounts").at("SpectralOcean.Evolution") == 1,
        "Shared simulation ran twice.");
    std::ifstream lines(log);
    std::string line;
    std::size_t count = 0;
    while (std::getline(lines, line)) { Expect(Json::parse(line).at("frameId") == ++count, "Wrong frame log sequence."); }
    Expect(count == 2, "Wrong frame log length.");
    ExpectThrows([&] { FrameDiagnostics existing(log, {}); });
    ExpectThrows([&] { FrameDiagnostics existing({}, capture); });
    ExpectThrows([&] { FrameDiagnostics collision(root / "collision", root / "collision"); });
    TestComparison(report);

    FrameDiagnostics stale({}, root / "stale.json");
    stale.BeginFrame(1, 0, 0, 1, "HPWater");
    stale.RecordView("scene", View(false));
    stale.CompleteFrame();
    stale.RequestCapture(root / "stale.bmp");
    stale.BeginFrame(2, 1, 1.0 / 60, 2, "HPWater");
    stale.RecordView("game", View());
    stale.RecordCapture("scene");
    stale.CompleteFrame();
    stale.ResolveCapture(true);
    const auto old = Read(root / "stale.json");
    Expect(old.at("sourceKnown") && !old.at("sourceFresh") && old.at("source").at("frameId") == 1,
        "Hidden Scene image was incorrectly labelled current.");
    Expect(!Prism::Tools::CompareFrameDiagnostics(old, old).at("passed"), "Stale self-comparison must fail.");

    FrameDiagnostics unknown({}, root / "unknown.json");
    unknown.RequestCapture(root / "unknown.bmp");
    unknown.BeginFrame(1, 0, 0, 1, "HPWater");
    unknown.RecordCapture("scene");
    unknown.CompleteFrame();
    unknown.ResolveCapture(true);
    Expect(!Read(root / "unknown.json").at("sourceKnown"), "Unrendered view was invented.");
}
} // namespace

int main()
{
    try
    {
        const auto root = std::filesystem::current_path() / "frame-diagnostics-tests"
            / std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
        std::filesystem::create_directories(root);
        TestRecorder(root);
        TestFrameCaptureSequence(root);
        std::cout << "Frame diagnostics recorder and comparison tests passed.\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
