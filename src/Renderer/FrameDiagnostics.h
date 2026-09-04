#pragma once

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string_view>

#include <json.hpp>

namespace Prism::Renderer
{
struct RenderViewDiagnostics
{
    nlohmann::json value;
};

// Opt-in, render-lane-only recorder. Retains one frame, two latest views and
// one pending capture; never holds render resources or changes scheduling.
class FrameDiagnostics
{
public:
    FrameDiagnostics(const std::filesystem::path& frameLogPath,
        const std::filesystem::path& captureReportPath);
    void BeginFrame(std::uint64_t frameId, std::uint32_t frameSlot,
        double simulationTime, std::uint64_t sceneGeneration,
        std::string_view scene);
    void RecordView(std::string_view view, RenderViewDiagnostics diagnostics);
    void RequestCapture(const std::filesystem::path& imagePath);
    void RequestCapture(const std::filesystem::path& imagePath,
        const std::filesystem::path& captureReportPath);
    void RecordCapture(std::string_view view);
    void CompleteFrame();
    void ResolveCapture(bool complete, std::string_view error = {});

private:
    std::ofstream m_frameLog;
    std::filesystem::path m_frameLogPath;
    std::filesystem::path m_captureReportPath;
    std::filesystem::path m_requestedImagePath;
    nlohmann::json m_frame;
    nlohmann::json m_latestViews = nlohmann::json::object();
    std::optional<nlohmann::json> m_pendingCapture;
    std::uint64_t m_lastFrameId = 0;
    bool m_frameOpen = false;
    bool m_captureResolved = false;
};
} // namespace Prism::Renderer
