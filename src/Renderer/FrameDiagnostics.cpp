#include "Renderer/FrameDiagnostics.h"

#include <cmath>
#include <stdexcept>
#include <string>
#include <utility>

namespace Prism::Renderer
{
namespace
{
void PrepareOutput(const std::filesystem::path& path)
{
    if (std::filesystem::exists(path))
        throw std::runtime_error("Refusing to overwrite frame diagnostics: " + path.string());
    if (path.has_parent_path())
        std::filesystem::create_directories(path.parent_path());
}

std::string ViewKey(const std::string_view view)
{
    if (view != "game" && view != "scene")
        throw std::invalid_argument("Frame diagnostics view must be game or scene.");
    return std::string(view);
}
} // namespace

FrameDiagnostics::FrameDiagnostics(const std::filesystem::path& frameLogPath,
    const std::filesystem::path& captureReportPath)
    : m_frameLogPath(frameLogPath), m_captureReportPath(captureReportPath)
{
    if (!captureReportPath.empty()) PrepareOutput(captureReportPath);
    if (!frameLogPath.empty())
    {
        if (!captureReportPath.empty()
            && std::filesystem::absolute(frameLogPath).lexically_normal()
                == std::filesystem::absolute(captureReportPath).lexically_normal())
            throw std::invalid_argument("Frame log and capture diagnostics paths must differ.");
        PrepareOutput(frameLogPath);
        m_frameLog.open(frameLogPath);
        if (!m_frameLog) throw std::runtime_error("Cannot open frame diagnostics log.");
    }
}

void FrameDiagnostics::BeginFrame(const std::uint64_t frameId,
    const std::uint32_t frameSlot, const double simulationTime,
    const std::uint64_t sceneGeneration, const std::string_view scene)
{
    if (m_frameOpen || frameId <= m_lastFrameId || !std::isfinite(simulationTime))
        throw std::logic_error("Invalid frame diagnostics sequence.");
    m_frame = {{"format", "PrismFrameDiagnostics"}, {"version", 1},
        {"frameId", frameId}, {"frameSlot", frameSlot},
        {"simulationTimeSeconds", simulationTime}, {"sceneGeneration", sceneGeneration},
        {"scene", scene}, {"views", nlohmann::json::object()},
        {"sharedSimulationPassCounts", nlohmann::json::object()}};
    m_frameOpen = true;
    m_lastFrameId = frameId;
}

void FrameDiagnostics::RecordView(const std::string_view view, RenderViewDiagnostics report)
{
    auto& diagnostics = report.value;
    const std::string key = ViewKey(view);
    if (!m_frameOpen || m_frame["views"].contains(key))
        throw std::logic_error("View diagnostics recorded outside a frame or twice.");
    for (const auto& [name, count] : diagnostics.at("sharedSimulationPassCounts").items())
    {
        auto& total = m_frame["sharedSimulationPassCounts"][name];
        total = (total.is_null() ? 0u : total.get<std::uint64_t>()) + count.get<std::uint64_t>();
    }
    diagnostics["frameId"] = m_frame["frameId"];
    diagnostics["sceneGeneration"] = m_frame["sceneGeneration"];
    diagnostics["scene"] = m_frame["scene"];
    diagnostics["simulationTimeSeconds"] = m_frame["simulationTimeSeconds"];
    diagnostics["view"] = key;
    m_latestViews[key] = diagnostics;
    m_frame["views"][key] = std::move(diagnostics);
}

void FrameDiagnostics::RequestCapture(const std::filesystem::path& imagePath)
{
    if (!m_requestedImagePath.empty())
        throw std::logic_error("A diagnostic capture is already pending.");
    if (imagePath.empty()) throw std::invalid_argument("Capture image path is empty.");
    for (const auto& outputPath : {m_frameLogPath, m_captureReportPath})
    {
        if (outputPath.empty()) continue;
        std::error_code pathError;
        if (std::filesystem::absolute(outputPath).lexically_normal()
                == std::filesystem::absolute(imagePath).lexically_normal()
            || std::filesystem::equivalent(outputPath, imagePath, pathError))
            throw std::invalid_argument("Capture image and diagnostics paths must differ.");
    }
    m_requestedImagePath = imagePath;
    m_pendingCapture.reset();
    m_captureResolved = false;
}

void FrameDiagnostics::RequestCapture(const std::filesystem::path& imagePath,
    const std::filesystem::path& captureReportPath)
{
    if (!m_requestedImagePath.empty())
        throw std::logic_error("A diagnostic capture is already pending.");
    if (captureReportPath.empty()) throw std::invalid_argument("Sequence capture report path is empty.");
    PrepareOutput(captureReportPath);
    if (!m_frameLogPath.empty()
        && std::filesystem::absolute(captureReportPath).lexically_normal()
            == std::filesystem::absolute(m_frameLogPath).lexically_normal())
        throw std::invalid_argument("Frame log and capture diagnostics paths must differ.");
    const auto previousPath = m_captureReportPath;
    m_captureReportPath = captureReportPath;
    try { RequestCapture(imagePath); }
    catch (...) { m_captureReportPath = previousPath; throw; }
}

void FrameDiagnostics::RecordCapture(const std::string_view view)
{
    if (m_requestedImagePath.empty() || m_pendingCapture) return;
    const std::string key = ViewKey(view);
    if (!m_frameOpen) throw std::logic_error("Capture recorded outside a frame.");
    // A hidden/on-demand Scene viewport may expose an older image. Preserve
    // that fact; diagnostics must never force a render merely to look fresh.
    const bool known = m_latestViews.contains(key);
    const nlohmann::json source = known ? m_latestViews.at(key) : nlohmann::json(nullptr);
    m_pendingCapture = {{"format", "PrismCaptureDiagnostics"}, {"version", 1},
        {"imagePath", std::filesystem::absolute(m_requestedImagePath).generic_string()},
        {"view", key}, {"recordedFrameId", m_frame["frameId"]},
        {"sourceKnown", known},
        {"sourceFresh", known && source.at("frameId") == m_frame["frameId"]},
        {"source", source}, {"frame", m_frame}};
}

void FrameDiagnostics::CompleteFrame()
{
    if (!m_frameOpen) throw std::logic_error("No diagnostics frame to complete.");
    if (m_frameLog.is_open())
    {
        m_frameLog << m_frame.dump() << '\n';
        m_frameLog.flush();
        if (!m_frameLog) throw std::runtime_error("Cannot write frame diagnostics log.");
    }
    m_frameOpen = false;
}

void FrameDiagnostics::ResolveCapture(const bool complete, const std::string_view error)
{
    // Vulkan's completion flag is persistent and also set on save failure;
    // D3D12 exposes a one-shot success. Do not change either backend contract.
    if (!complete || !error.empty() || m_captureResolved) return;
    if (!m_pendingCapture)
        throw std::logic_error("Capture resolved without recorded diagnostics.");
    if (m_frameOpen) throw std::logic_error("Capture resolved before frame submission.");
    if (!m_captureReportPath.empty())
    {
        PrepareOutput(m_captureReportPath);
        std::ofstream output(m_captureReportPath);
        output << m_pendingCapture->dump(2) << '\n';
        output.close();
        if (!output) throw std::runtime_error("Cannot write capture diagnostics report.");
    }
    m_requestedImagePath.clear();
    m_pendingCapture.reset();
    m_captureResolved = true;
}
} // namespace Prism::Renderer
