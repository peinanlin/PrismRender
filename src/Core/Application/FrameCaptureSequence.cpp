#include "Core/Application/FrameCaptureSequence.h"

#include <fstream>
#include <limits>
#include <stdexcept>
#include <string>

#include <json.hpp>

namespace Prism::Core
{
namespace
{
FrameCaptureActionType ParseActionType(const std::string& type)
{
    if (type == "set-taa") return FrameCaptureActionType::SetTemporalAntiAliasing;
    if (type == "set-shadows") return FrameCaptureActionType::SetShadows;
    if (type == "refresh-scene") return FrameCaptureActionType::RefreshSceneView;
    if (type == "activate-streaming") return FrameCaptureActionType::ActivateStreamingScene;
    if (type == "activate-demo-scene") return FrameCaptureActionType::ActivateDemoScene;
    throw std::invalid_argument("Unknown capture sequence action: " + type);
}
} // namespace

FrameCaptureSequence::FrameCaptureSequence(const std::filesystem::path& inputPath,
    const std::filesystem::path& outputDirectory)
    : m_outputDirectory(outputDirectory)
{
    constexpr std::size_t maximumSamples = 4096;
    std::ifstream input(inputPath);
    if (!input) throw std::runtime_error("Cannot open capture sequence input.");
    const auto config = nlohmann::json::parse(input);
    if (!config.is_object() || !config.contains("version")
        || !config["version"].is_number_unsigned()
        || (config["version"] != 1 && config["version"] != 2)
        || !config.contains("frames") || !config["frames"].is_array()
        || config["frames"].empty() || config["frames"].size() > maximumSamples)
        throw std::invalid_argument("Capture sequence requires version 1 or 2 and 1..4096 ordered frames.");
    const bool hasActions = config["version"] == 2;
    if ((!hasActions && config.size() != 2)
        || (hasActions && (config.size() != 3 || !config.contains("actions")
            || !config["actions"].is_array()
            || config["actions"].size() > maximumSamples)))
        throw std::invalid_argument("Capture sequence version 2 requires an actions array.");
    for (const auto& value : config["frames"])
    {
        if (!value.is_number_unsigned())
            throw std::invalid_argument("Capture sequence frames must be positive integers.");
        const auto frame = value.get<std::uint64_t>();
        if (frame == 0 || frame > std::numeric_limits<std::uint32_t>::max()
            || (!m_frames.empty() && frame <= m_frames.back()))
            throw std::invalid_argument("Capture sequence frames must strictly increase within uint32 range.");
        m_frames.push_back(frame);
    }
    if (hasActions)
    {
        std::uint64_t previousFrame = 0;
        std::string previousType;
        for (const auto& value : config["actions"])
        {
            if (!value.is_object() || !value.contains("frame")
                || !value["frame"].is_number_unsigned()
                || !value.contains("type") || !value["type"].is_string())
                throw std::invalid_argument("Capture sequence action requires frame and type.");
            const auto frame = value["frame"].get<std::uint64_t>();
            const std::string typeName = value["type"].get<std::string>();
            const auto type = ParseActionType(typeName);
            const bool settingAction = type == FrameCaptureActionType::SetTemporalAntiAliasing
                || type == FrameCaptureActionType::SetShadows;
            const bool sceneAction =
                type == FrameCaptureActionType::ActivateDemoScene;
            if ((settingAction && (value.size() != 3 || !value.contains("enabled")
                    || !value["enabled"].is_boolean()))
                || (sceneAction && (value.size() != 3
                    || !value.contains("scene")
                    || !value["scene"].is_string()))
                || (!settingAction && !sceneAction && value.size() != 2)
                || frame == 0 || frame > m_frames.back()
                || frame < previousFrame || (frame == previousFrame && typeName <= previousType))
                throw std::invalid_argument("Capture sequence actions must be ordered, unique, in range, and type-correct.");
            FrameCaptureAction action{
                frame,
                type,
                settingAction ? value["enabled"].get<bool>() : false};
            if (sceneAction)
            {
                action.demoScene = value["scene"].get<std::string>();
                if (action.demoScene.empty())
                    throw std::invalid_argument(
                        "Capture sequence demo-scene action requires a scene name.");
            }
            m_actions.push_back(std::move(action));
            previousFrame = frame;
            previousType = typeName;
        }
    }
    if (outputDirectory.empty() || std::filesystem::exists(outputDirectory))
        throw std::invalid_argument("Capture sequence requires a new output directory.");
    if (!std::filesystem::create_directories(outputDirectory))
        throw std::runtime_error("Cannot reserve capture sequence output directory.");
}

std::vector<FrameCaptureAction> FrameCaptureSequence::ConsumeActions(const std::uint64_t frameId)
{
    if (frameId != m_lastActionFrame + 1)
        throw std::logic_error("Capture sequence actions have a skipped or duplicate logical frame.");
    m_lastActionFrame = frameId;
    if (m_nextAction < m_actions.size() && m_actions[m_nextAction].frameId < frameId)
        throw std::runtime_error("Capture sequence missed a scheduled action.");
    std::vector<FrameCaptureAction> result;
    while (m_nextAction < m_actions.size() && m_actions[m_nextAction].frameId == frameId)
        result.push_back(m_actions[m_nextAction++]);
    return result;
}

std::optional<FrameCaptureRequest> FrameCaptureSequence::BeginFrame(const std::uint64_t frameId)
{
    if (frameId != m_lastFrame + 1)
        throw std::logic_error("Capture sequence has a skipped or duplicate logical frame.");
    m_lastFrame = frameId;
    if (IsComplete() || frameId < m_frames[m_nextSample]) return std::nullopt;
    if (m_pending || frameId != m_frames[m_nextSample])
        throw std::runtime_error("Capture sequence missed a sample or still has a pending readback.");
    const auto stem = m_outputDirectory / ("frame-" + std::to_string(frameId));
    FrameCaptureRequest request{frameId, stem.string() + ".bmp", stem.string() + ".capture.json"};
    if (std::filesystem::exists(request.imagePath) || std::filesystem::exists(request.diagnosticsPath))
        throw std::runtime_error("Refusing to overwrite capture sequence evidence.");
    m_pending = true;
    return request;
}

void FrameCaptureSequence::ResolveCapture(const bool complete, const std::string_view error)
{
    if (!error.empty()) throw std::runtime_error("Capture sequence failed: " + std::string(error));
    // Vulkan retains success until the next request. Only acknowledge a request
    // actually issued by this sequence, not every later presentation.
    if (!m_pending || !complete) return;
    m_pending = false;
    ++m_nextSample;
}

bool FrameCaptureSequence::IsComplete() const
{
    return !m_pending && m_nextSample == m_frames.size()
        && m_nextAction == m_actions.size();
}

bool FrameCaptureSequence::HasAction(const FrameCaptureActionType type) const
{
    for (const auto& action : m_actions)
        if (action.type == type) return true;
    return false;
}

void FrameCaptureSequence::RequireComplete() const
{
    if (!IsComplete()) throw std::runtime_error("Application exited before all capture sequence samples completed.");
}
} // namespace Prism::Core
