#pragma once

#include "Core/Application/FrameCaptureSequence.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace Prism::Core
{
class AssetRuntimeCoordinator;
struct RenderFrameCompletion;
struct RenderFrameFeedback;
class WaterValidationSequence;
}

namespace Prism::RHI
{
class IRenderBackend;
}

namespace Prism::Renderer
{
class FrameDiagnostics;
class SceneRenderer;
}

namespace Prism::Scene
{
enum class DemoSceneId : std::uint32_t;
class RenderScene;
class SceneSession;
}

namespace Prism::Core
{
struct CaptureResolveResult
{
    bool complete = false;
    std::string error;
};

struct RenderCaptureRequest
{
    std::uint64_t requestId = 0;
    std::uint64_t targetLogicalFrameId = 0;
    std::filesystem::path outputPath;
};

// Owns opt-in capture/report state. Requests are bound to accepted logical
// frames, while progress, reports and exit conditions advance only from
// render-lane completion feedback. It stores no ApplicationHost pointer.
class CaptureAutomationController final
{
public:
    CaptureAutomationController();
    ~CaptureAutomationController();

    CaptureAutomationController(
        const CaptureAutomationController&) = delete;
    CaptureAutomationController& operator=(
        const CaptureAutomationController&) = delete;

    void Initialize(
        Scene::DemoSceneId activeScene,
        bool deterministicCaptureEnabled,
        bool assetStreamingRequested);

    [[nodiscard]] bool IsDiagnosticsEnabled() const noexcept;
    [[nodiscard]] bool RequiresCaptureProfiling() const noexcept;
    [[nodiscard]] std::uint64_t GetMaximumFrameCount() const noexcept;
    [[nodiscard]] bool HasSequenceAction(
        FrameCaptureActionType type) const noexcept;
    [[nodiscard]] std::vector<FrameCaptureAction> ConsumeActions(
        std::uint64_t frameId);
    [[nodiscard]] std::optional<RenderCaptureRequest>
        PrepareFrameCapture(std::uint64_t logicalFrameId);
    [[nodiscard]] CaptureResolveResult ConsumeCompletedFrame(
        const RenderFrameCompletion& completion);
    [[nodiscard]] std::uint64_t GetAcceptedFrameCount() const noexcept;
    [[nodiscard]] std::uint64_t GetCompletedFrameCount() const noexcept;

    [[nodiscard]] Renderer::FrameDiagnostics*
        GetFrameDiagnostics() noexcept;
    [[nodiscard]] const Renderer::FrameDiagnostics*
        GetFrameDiagnostics() const noexcept;

    void NotifyStreamingSceneActivated(Scene::RenderScene& scene);
    void RecordStreamingActivationFailure(
        std::string errorCode,
        std::string errorMessage);
    void RecordStreamingActivationSuccess(
        std::size_t renderObjectCount) noexcept;
    [[nodiscard]] bool ShouldAttemptStreamingActivation() const noexcept;
    void SetStreamingSceneAssetId(std::string assetId);
    [[nodiscard]] const std::string&
        GetStreamingSceneAssetId() const noexcept;
    [[nodiscard]] WaterValidationSequence&
        GetWaterValidationSequence() noexcept;
    [[nodiscard]] const WaterValidationSequence&
        GetWaterValidationSequence() const noexcept;

    [[nodiscard]] bool ShouldCollectRenderGraphReport() const noexcept;
    [[nodiscard]] bool ShouldCollectGpuTimingReport() const noexcept;
    void WriteRenderGraphReportIfRequested(
        const RenderFrameFeedback& feedback);
    void WriteGpuTimingReportIfReady(
        const RenderFrameFeedback& feedback,
        std::string_view sceneLabel,
        std::string_view adapterName);
    void HandleCaptureError(
        const std::string& error,
        Scene::SceneSession& sceneSession) const;
    [[nodiscard]] bool ShouldExit() const noexcept;
    void RequireSequenceComplete() const;
    [[nodiscard]] std::optional<std::filesystem::path>
        GetAssetStreamingReportPath(bool streamingEnabled) const;
    void WriteStreamingSceneReport(
        AssetRuntimeCoordinator& assets,
        std::string_view graphicsApiName,
        const Scene::RenderScene& scene) const;

private:
    std::unique_ptr<Renderer::FrameDiagnostics> m_frameDiagnostics;
    std::unique_ptr<FrameCaptureSequence> m_captureSequence;
    std::unique_ptr<WaterValidationSequence> m_waterValidationSequence;
    std::filesystem::path m_renderGraphReportPath;
    std::filesystem::path m_gpuTimingReportPath;
    std::filesystem::path m_streamingReportPath;
    std::filesystem::path m_streamingSceneReportPath;
    std::filesystem::path m_deferredStreamingCapturePath;
    std::filesystem::path m_delayedCapturePath;
    std::filesystem::path m_pendingCapturePath;
    std::string m_streamingSceneAssetId;
    std::string m_streamingActivationErrorCode;
    std::string m_streamingActivationErrorMessage;
    std::uint64_t m_maximumFrameCount = 0;
    std::uint64_t m_nextCaptureRequestId = 1;
    std::uint64_t m_activeCaptureRequestId = 0;
    std::uint64_t m_activeCaptureTargetFrameId = 0;
    std::uint64_t m_acceptedFrameCount = 0;
    std::uint64_t m_completedFrameCount = 0;
    std::uint64_t m_lastCompletedAcceptanceId = 0;
    std::uint64_t m_lastCompletedLogicalFrameId = 0;
    std::uint64_t m_lastCompletedSceneGeneration = 0;
    std::uint64_t m_lastCompletedDataRevision = 0;
    std::uint64_t m_lastCompletedSceneEpoch = 0;
    std::uint64_t m_lastCompletedViewEpoch = 0;
    std::uint64_t m_lastCompletedSettingsRevision = 0;
    std::size_t m_streamingActivatedObjectCount = 0;
    std::uint32_t m_streamingCaptureFramesRemaining = 0;
    std::uint32_t m_captureDelayFramesRemaining = 0;
    bool m_exitAfterCapture = false;
    bool m_exitAfterRenderGraphReport = false;
    bool m_renderGraphReportWritten = false;
    bool m_gpuTimingReportWritten = false;
    bool m_captureComplete = false;
    bool m_streamingSceneActivated = false;
    bool m_streamingSceneActivationSucceeded = false;
};
} // namespace Prism::Core
