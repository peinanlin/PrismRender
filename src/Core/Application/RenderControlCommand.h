#pragma once

#include "Asset/AssetDatabase.h"
#include "Scene/RenderFramePacket.h"
#include "Scene/RenderView.h"
#include "Scene/DemoSceneCatalog.h"
#include "RHI/FramePacing.h"

#include <compare>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <variant>

namespace Prism::Core
{
struct RenderControlCommandId
{
    std::uint64_t value = 0;
    [[nodiscard]] explicit operator bool() const noexcept
    {
        return value != 0;
    }
    auto operator<=>(const RenderControlCommandId&) const = default;
};

struct RenderEpoch
{
    std::uint64_t value = 0;
    [[nodiscard]] explicit operator bool() const noexcept
    {
        return value != 0;
    }
    auto operator<=>(const RenderEpoch&) const = default;
};

enum class RenderControlBoundary : std::uint8_t
{
    BeforeFrame,
    AfterFrame
};

struct ResizeRenderViewCommand
{
    Scene::RenderViewId viewId;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
};

struct ResetRenderHistoryCommand
{
    Scene::RenderViewId viewId;
};

struct ChangeRenderQualityCommand
{
    std::uint64_t settingsRevision = 0;
};

struct SwitchRenderSceneCommand
{
    RenderEpoch nextSceneEpoch;
    std::optional<Scene::DemoSceneId> demoScene;
    float aspectRatio = 1.0f;
};

struct CaptureRenderFrameCommand
{
    std::uint64_t requestId = 0;
    std::filesystem::path outputPath;
};

struct DrainRenderExecutionCommand
{
};

struct AdmitRenderFrameCommand
{
};

struct ChangeFramePacingCommand
{
    RHI::FramePacingConfiguration configuration;
    std::uint64_t generation = 0;
};

struct ProcessAssetRenderWorkCommand
{
    std::uint64_t logicalFrameId = 0;
};

struct WriteAssetStreamingReportCommand
{
    std::filesystem::path outputPath;
};

struct ImportRenderAssetCommand
{
    std::filesystem::path sourcePath;
    bool reimport = false;
};

struct StopRenderExecutionCommand
{
};

using RenderControlPayload = std::variant<
    ResizeRenderViewCommand,
    ResetRenderHistoryCommand,
    ChangeRenderQualityCommand,
    SwitchRenderSceneCommand,
    CaptureRenderFrameCommand,
    AdmitRenderFrameCommand,
    ChangeFramePacingCommand,
    DrainRenderExecutionCommand,
    ProcessAssetRenderWorkCommand,
    WriteAssetStreamingReportCommand,
    ImportRenderAssetCommand,
    StopRenderExecutionCommand>;

struct RenderControlCommand
{
    RenderControlCommandId id;
    Scene::LogicalFrameId targetFrame;
    RenderEpoch sceneEpoch;
    RenderEpoch viewEpoch;
    RenderControlBoundary boundary =
        RenderControlBoundary::BeforeFrame;
    RenderControlPayload payload = StopRenderExecutionCommand{};
};

struct RenderControlAcknowledgement
{
    RenderControlCommandId commandId;
    Scene::LogicalFrameId completedFrame;
    RenderEpoch sceneEpoch;
    RenderEpoch viewEpoch;
    bool succeeded = false;
    bool gpuDrained = false;
    std::size_t uploadedAssetCount = 0;
    std::size_t evictedAssetCount = 0;
    std::size_t bindingUpdateCount = 0;
    bool assetStreamingReportWritten = false;
    bool sceneChanged = false;
    std::optional<Asset::AssetImportResult> assetImportResult;
    std::optional<RHI::FrameAdmissionResult> frameAdmission;
    std::optional<RHI::FramePacingState> framePacingState;
    std::string framePacingError;
};
} // namespace Prism::Core
