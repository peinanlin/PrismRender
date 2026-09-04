#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace Prism::Core
{
struct FrameCaptureRequest
{
    std::uint64_t frameId;
    std::filesystem::path imagePath;
    std::filesystem::path diagnosticsPath;
};

enum class FrameCaptureActionType : std::uint8_t
{
    SetTemporalAntiAliasing,
    SetShadows,
    RefreshSceneView,
    ActivateStreamingScene,
    ActivateDemoScene
};

struct FrameCaptureAction
{
    std::uint64_t frameId = 0;
    FrameCaptureActionType type = FrameCaptureActionType::RefreshSceneView;
    bool enabled = false;
    std::string demoScene;
};

// Opt-in validation only. Reuses the backend's single outstanding readback;
// never skips a simulation frame or changes the lifetime of render resources.
class FrameCaptureSequence
{
public:
    FrameCaptureSequence(const std::filesystem::path& inputPath,
        const std::filesystem::path& outputDirectory);
    std::vector<FrameCaptureAction> ConsumeActions(std::uint64_t frameId);
    std::optional<FrameCaptureRequest> BeginFrame(std::uint64_t frameId);
    void ResolveCapture(bool complete, std::string_view error);
    void RequireComplete() const;
    bool IsComplete() const;
    bool HasAction(FrameCaptureActionType type) const;
    std::uint64_t LastSampleFrame() const { return m_frames.back(); }

private:
    std::vector<std::uint64_t> m_frames;
    std::vector<FrameCaptureAction> m_actions;
    std::filesystem::path m_outputDirectory;
    std::size_t m_nextSample = 0;
    std::size_t m_nextAction = 0;
    std::uint64_t m_lastFrame = 0;
    std::uint64_t m_lastActionFrame = 0;
    bool m_pending = false;
};
} // namespace Prism::Core
