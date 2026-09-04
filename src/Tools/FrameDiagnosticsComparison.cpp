#include "Tools/FrameDiagnosticsComparison.h"

#include <stdexcept>
#include <string>

namespace Prism::Tools
{
namespace
{
using Json = nlohmann::json;

Json Select(const Json& input, std::initializer_list<const char*> keys)
{
    Json result = Json::object();
    for (const char* key : keys) result[key] = input.at(key);
    return result;
}

void Require(const bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
}

Json ProjectView(const Json& view, const bool sameBackend)
{
    Json counts = Json::object();
    Json simulation = Json::object();
    for (const auto& value : view.at("recordedPasses"))
    {
        const std::string name = value.get<std::string>();
        counts[name] = counts.value(name, 0u) + 1u;
        if (name.starts_with("SpectralOcean.") || name.starts_with("FftOcean.")
            || name.starts_with("Ocean.LocalWave.") || name == "PbfFluid.Simulate"
            || name == "InteractiveTerrain.BrushAndErosion")
            simulation[name] = simulation.value(name, 0u) + 1u;
    }
    Require(counts == view.at("recordedPassCounts"), "Pass counts disagree with recorded passes.");
    Require(simulation == view.at("sharedSimulationPassCounts"), "Simulation counts disagree with recorded passes.");
    Json result = Select(view, {"view", "width", "height", "frameId", "scene", "sceneGeneration",
        "simulationTimeSeconds", "simulationProducer", "history", "recordedPassCounts",
        "sharedSimulationPassCounts", "passCountMeaning", "graphStateMeaning"});
    result["outputTarget"] = view.value("outputTarget", "view-texture");
    const auto& graph = view.at("graph");
    result["passes"] = Json::object();
    for (const auto& pass : graph.at("passes"))
    {
        const auto name = pass.at("name").get<std::string>();
        Require(!result["passes"].contains(name), "Duplicate graph pass name.");
        auto selected = Select(pass, {"culled", "sideEffect", "dependencies", "reads", "writes"});
        if (sameBackend)
        {
            selected["queue"] = pass.at("queue");
            selected["executionIndex"] = pass.at("executionIndex");
            selected["parallelRecordable"] = pass.at("parallelRecordable");
        }
        result["passes"][name] = std::move(selected);
    }
    result["resources"] = Json::object();
    for (const auto& resource : graph.at("resources"))
    {
        // Unused imports can legitimately retain API-specific initial states.
        if (!sameBackend && !resource.at("active").get<bool>()) continue;
        const auto name = resource.at("name").get<std::string>();
        Require(!result["resources"].contains(name), "Duplicate graph resource name.");
        result["resources"][name] = Select(resource, {"kind", "stateBits", "imported", "transient",
            "history", "currentVersion", "active", "textureSubresourceCount", "bufferStateRangeCount"});
    }
    if (sameBackend)
    {
        result["recordedPasses"] = view.at("recordedPasses");
        result["queueSync"] = graph.at("queueSync");
        result["queueBatches"] = graph.at("queueBatches");
    }
    return result;
}

Json ProjectCapture(const Json& capture, const bool sameBackend)
{
    Require(capture.at("format") == "PrismCaptureDiagnostics" && capture.at("version") == 1,
        "Unsupported capture diagnostics format.");
    Require(capture.at("sourceKnown").get<bool>() && capture.at("sourceFresh").get<bool>(),
        "Capture source is unknown or stale.");
    const auto& frame = capture.at("frame");
    Require(frame.at("format") == "PrismFrameDiagnostics" && frame.at("version") == 1,
        "Unsupported frame diagnostics format.");
    Require(capture.at("recordedFrameId") == frame.at("frameId"), "Capture/frame ID mismatch.");
    const auto viewName = capture.at("view").get<std::string>();
    Require(viewName == "game" || viewName == "scene", "Invalid capture view.");
    Require(capture.at("source") == frame.at("views").at(viewName), "Capture/source view mismatch.");
    Json result = Select(frame, {"frameId", "simulationTimeSeconds", "sceneGeneration", "scene",
        "sharedSimulationPassCounts"});
    result["captureView"] = viewName;
    if (sameBackend) result["frameSlot"] = frame.at("frameSlot");
    result["views"] = Json::object();
    Json totals = Json::object();
    for (const auto& [name, view] : frame.at("views").items())
    {
        Require((name == "game" || name == "scene") && view.at("view") == name, "Invalid view identity.");
        for (const char* key : {"frameId", "simulationTimeSeconds", "sceneGeneration", "scene"})
            Require(view.at(key) == frame.at(key), "View/frame identity mismatch.");
        Require(view.at("backend") == capture.at("source").at("backend"), "Mixed backends within frame.");
        result["views"][name] = ProjectView(view, sameBackend);
        for (const auto& [pass, count] : view.at("sharedSimulationPassCounts").items())
            totals[pass] = totals.value(pass, 0u) + count.get<std::uint64_t>();
    }
    Require(totals == frame.at("sharedSimulationPassCounts"), "Per-view simulation totals disagree with frame.");
    return result;
}
} // namespace

nlohmann::json CompareFrameDiagnostics(const nlohmann::json& reference, const nlohmann::json& candidate)
{
    Json report = {{"format", "PrismFrameDiagnosticsComparison"}, {"version", 1}, {"passed", false}};
    try
    {
        const auto referenceApi = reference.at("source").at("backend").get<std::string>();
        const auto candidateApi = candidate.at("source").at("backend").get<std::string>();
        for (const auto& api : {referenceApi, candidateApi})
            Require(api == "Direct3D 12" || api == "Vulkan", "Unsupported diagnostics backend.");
        const bool sameBackend = referenceApi == candidateApi;
        report["kind"] = sameBackend ? "same-backend" : "cross-backend";
        report["differences"] = Json::diff(ProjectCapture(reference, sameBackend), ProjectCapture(candidate, sameBackend));
        report["passed"] = report["differences"].empty();
    }
    catch (const std::exception& error)
    {
        report["error"] = error.what();
    }
    return report;
}
} // namespace Prism::Tools
