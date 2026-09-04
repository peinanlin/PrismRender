#include "Core/Application/CaptureAutomationController.h"

#include "Core/Application/AssetRuntimeCoordinator.h"
#include "Core/Application/RenderExecutionService.h"
#include "Core/Application/WaterValidationSequence.h"
#include "Core/Environment.h"
#include "RHI/IGraphicsDevice.h"
#include "RHI/IFrameContext.h"
#include "RHI/IRenderBackend.h"
#include "Renderer/Features/Ocean/WaterBenchmarkReport.h"
#include "Renderer/FrameDiagnostics.h"
#include "Renderer/GpuTimingReport.h"
#include "Renderer/RenderCapture.h"
#include "Renderer/RenderGraphDiagnostics.h"
#include "Renderer/SceneRenderer.h"
#include "Scene/AssetStreamingSceneBridge.h"
#include "Scene/DemoSceneCatalog.h"
#include "Scene/RenderScene.h"
#include "Scene/SceneFraming.h"
#include "Scene/SceneSession.h"

#include <json.hpp>

#include <limits>
#include <stdexcept>
#include <utility>

namespace Prism::Core
{
CaptureAutomationController::CaptureAutomationController() = default;
CaptureAutomationController::~CaptureAutomationController() = default;

void CaptureAutomationController::Initialize(
    const Scene::DemoSceneId activeScene,
    const bool deterministicCaptureEnabled,
    const bool assetStreamingRequested)
{
    m_waterValidationSequence =
        std::make_unique<WaterValidationSequence>(
            IsEnvironmentVariableEnabled(
                "PRISM_RENDER_WATER_VALIDATION_SEQUENCE"));

    std::string frameDiagnosticsPath = ReadEnvironmentVariableValue(
        "PRISM_RENDER_FRAME_DIAGNOSTICS_PATH");
    const std::string captureDiagnosticsPath =
        ReadEnvironmentVariableValue(
            "PRISM_RENDER_CAPTURE_DIAGNOSTICS_PATH");
    const std::string sequencePath = ReadEnvironmentVariableValue(
        "PRISM_RENDER_CAPTURE_SEQUENCE_PATH");
    const std::string sequenceOutput = ReadEnvironmentVariableValue(
        "PRISM_RENDER_CAPTURE_SEQUENCE_OUTPUT");
    if (!sequencePath.empty() || !sequenceOutput.empty())
    {
        if (sequencePath.empty() || sequenceOutput.empty()
            || !deterministicCaptureEnabled)
        {
            throw std::invalid_argument(
                "Capture sequence requires input, output and "
                "PRISM_RENDER_DETERMINISTIC=1.");
        }
        for (const char* incompatible : {
                 "PRISM_RENDER_CAPTURE_PATH",
                 "PRISM_RENDER_CAPTURE_DELAY_FRAMES",
                 "PRISM_RENDER_CAPTURE_DIAGNOSTICS_PATH",
                 "PRISM_RENDER_FRAME_DIAGNOSTICS_PATH"})
        {
            if (!ReadEnvironmentVariableValue(incompatible).empty())
            {
                throw std::invalid_argument(
                    std::string("Capture sequence conflicts with ")
                    + incompatible);
            }
        }
        m_captureSequence = std::make_unique<FrameCaptureSequence>(
            sequencePath,
            sequenceOutput);
        frameDiagnosticsPath =
            (std::filesystem::path(sequenceOutput) / "frames.jsonl")
                .string();
    }
    if (!frameDiagnosticsPath.empty()
        || !captureDiagnosticsPath.empty())
    {
        m_frameDiagnostics =
            std::make_unique<Renderer::FrameDiagnostics>(
                frameDiagnosticsPath,
                captureDiagnosticsPath);
    }

    const std::string capturePath = ReadEnvironmentVariableValue(
        "PRISM_RENDER_CAPTURE_PATH");
    if (!capturePath.empty())
    {
        const std::string captureDelay = ReadEnvironmentVariableValue(
            "PRISM_RENDER_CAPTURE_DELAY_FRAMES");
        if (!captureDelay.empty())
        {
            try
            {
                std::size_t parsedLength = 0;
                const unsigned long requestedDelay = std::stoul(
                    captureDelay,
                    &parsedLength);
                if (parsedLength != captureDelay.size()
                    || requestedDelay
                        > std::numeric_limits<std::uint32_t>::max())
                {
                    throw std::out_of_range("capture delay");
                }
                m_captureDelayFramesRemaining =
                    static_cast<std::uint32_t>(requestedDelay);
            }
            catch (const std::exception&)
            {
                throw std::runtime_error(
                    "PRISM_RENDER_CAPTURE_DELAY_FRAMES must be an "
                    "unsigned integer.");
            }
        }
        if (m_captureDelayFramesRemaining > 0)
        {
            m_delayedCapturePath = capturePath;
        }
        else if (assetStreamingRequested
                 || activeScene
                     == Scene::DemoSceneId::AssetStreamingLab)
        {
            m_deferredStreamingCapturePath = capturePath;
        }
        else
        {
            m_pendingCapturePath = capturePath;
        }
    }

    m_exitAfterCapture = ReadEnvironmentVariableValue(
        "PRISM_RENDER_EXIT_AFTER_CAPTURE") == "1";
    m_renderGraphReportPath = ReadEnvironmentVariableValue(
        "PRISM_RENDER_RDG_REPORT_PATH");
    m_gpuTimingReportPath = ReadEnvironmentVariableValue(
        "PRISM_RENDER_GPU_TIMING_REPORT_PATH");
    m_exitAfterRenderGraphReport = ReadEnvironmentVariableValue(
        "PRISM_RENDER_EXIT_AFTER_RDG_REPORT") == "1";
    m_streamingReportPath = ReadEnvironmentVariableValue(
        "PRISM_RENDER_ASSET_STREAMING_REPORT_PATH");
    m_streamingSceneReportPath = ReadEnvironmentVariableValue(
        "PRISM_RENDER_ASSET_STREAMING_SCENE_REPORT_PATH");
    const std::string maximumFrames = ReadEnvironmentVariableValue(
        "PRISM_RENDER_MAX_FRAMES");
    if (!maximumFrames.empty())
    {
        try
        {
            // Preserve the established CLI contract: stoull accepts the
            // same prefix forms that ApplicationHost accepted before the
            // ownership migration.
            m_maximumFrameCount = std::stoull(maximumFrames);
        }
        catch (const std::exception&)
        {
            throw std::runtime_error(
                "PRISM_RENDER_MAX_FRAMES must be an unsigned integer.");
        }
    }
}

bool CaptureAutomationController::IsDiagnosticsEnabled() const noexcept
{
    return m_frameDiagnostics != nullptr;
}

bool CaptureAutomationController::RequiresCaptureProfiling() const noexcept
{
    return m_frameDiagnostics != nullptr
        || m_captureSequence != nullptr
        || !m_renderGraphReportPath.empty()
        || !m_gpuTimingReportPath.empty();
}

std::uint64_t
CaptureAutomationController::GetMaximumFrameCount() const noexcept
{
    return m_maximumFrameCount;
}

bool CaptureAutomationController::HasSequenceAction(
    const FrameCaptureActionType type) const noexcept
{
    return m_captureSequence != nullptr
        && m_captureSequence->HasAction(type);
}

std::vector<FrameCaptureAction>
CaptureAutomationController::ConsumeActions(const std::uint64_t frameId)
{
    return m_captureSequence != nullptr
        ? m_captureSequence->ConsumeActions(frameId)
        : std::vector<FrameCaptureAction>{};
}

std::optional<RenderCaptureRequest>
CaptureAutomationController::PrepareFrameCapture(
    const std::uint64_t logicalFrameId)
{
    if (logicalFrameId == 0
        || logicalFrameId != m_acceptedFrameCount + 1)
    {
        throw std::logic_error(
            "Capture automation received a skipped or duplicate accepted frame.");
    }
    ++m_acceptedFrameCount;
    m_captureComplete = false;

    std::optional<FrameCaptureRequest> sequenceRequest;
    if (m_captureSequence != nullptr)
    {
        sequenceRequest = m_captureSequence->BeginFrame(
            logicalFrameId);
    }

    if (!m_delayedCapturePath.empty()
        && m_captureDelayFramesRemaining > 0
        && --m_captureDelayFramesRemaining == 0)
    {
        m_pendingCapturePath = std::move(m_delayedCapturePath);
        m_delayedCapturePath.clear();
    }
    if (m_streamingSceneActivationSucceeded
        && !m_deferredStreamingCapturePath.empty()
        && m_streamingCaptureFramesRemaining > 0
        && --m_streamingCaptureFramesRemaining == 0)
    {
        m_pendingCapturePath =
            std::move(m_deferredStreamingCapturePath);
        m_deferredStreamingCapturePath.clear();
    }

    std::filesystem::path outputPath;
    std::filesystem::path diagnosticsPath;
    if (sequenceRequest.has_value())
    {
        outputPath = sequenceRequest->imagePath;
        diagnosticsPath = sequenceRequest->diagnosticsPath;
    }
    else if (!m_pendingCapturePath.empty())
    {
        outputPath = std::move(m_pendingCapturePath);
        m_pendingCapturePath.clear();
    }
    if (outputPath.empty())
    {
        return std::nullopt;
    }
    if (m_activeCaptureRequestId != 0)
    {
        throw std::logic_error(
            "A completed-frame capture request is already pending.");
    }

    if (m_frameDiagnostics != nullptr)
    {
        if (diagnosticsPath.empty())
        {
            m_frameDiagnostics->RequestCapture(outputPath);
        }
        else
        {
            m_frameDiagnostics->RequestCapture(
                outputPath,
                diagnosticsPath);
        }
    }
    m_activeCaptureRequestId = m_nextCaptureRequestId++;
    m_activeCaptureTargetFrameId = logicalFrameId;
    return RenderCaptureRequest{
        m_activeCaptureRequestId,
        logicalFrameId,
        std::move(outputPath)};
}

CaptureResolveResult
CaptureAutomationController::ConsumeCompletedFrame(
    const RenderFrameCompletion& completion)
{
    if (completion.acceptanceId == 0
        || !completion.logicalFrameId
        || completion.acceptanceId <= m_lastCompletedAcceptanceId
        || completion.logicalFrameId.value
            <= m_lastCompletedLogicalFrameId
        || completion.logicalFrameId.value > m_acceptedFrameCount)
    {
        throw std::logic_error(
            "Capture automation received an invalid completed-frame identity.");
    }

    m_lastCompletedAcceptanceId = completion.acceptanceId;
    m_lastCompletedLogicalFrameId = completion.logicalFrameId.value;
    m_lastCompletedSceneGeneration = completion.sceneGeneration.value;
    m_lastCompletedDataRevision = completion.dataRevision.value;
    m_lastCompletedSceneEpoch = completion.sceneEpoch.value;
    m_lastCompletedViewEpoch = completion.viewEpoch.value;
    m_lastCompletedSettingsRevision = completion.settingsRevision;
    ++m_completedFrameCount;

    if (m_frameDiagnostics != nullptr)
    {
        m_frameDiagnostics->CompleteFrame();
    }

    CaptureResolveResult result{};
    if (completion.captureRequestId == 0)
    {
        if (m_activeCaptureRequestId != 0
            && completion.logicalFrameId.value
                >= m_activeCaptureTargetFrameId)
        {
            throw std::logic_error(
                "A completed frame omitted its active capture identity.");
        }
        return result;
    }
    if (completion.captureRequestId != m_activeCaptureRequestId
        || completion.logicalFrameId.value
            < m_activeCaptureTargetFrameId)
    {
        throw std::logic_error(
            "Capture completion does not match its accepted request.");
    }

    result.complete = completion.captureResolved;
    result.error = completion.captureError;
    if (m_frameDiagnostics != nullptr)
    {
        m_frameDiagnostics->ResolveCapture(
            result.complete,
            result.error);
    }
    if (m_captureSequence != nullptr)
    {
        m_captureSequence->ResolveCapture(
            result.complete,
            result.error);
    }
    if (result.complete || !result.error.empty())
    {
        m_activeCaptureRequestId = 0;
        m_activeCaptureTargetFrameId = 0;
    }
    m_captureComplete = result.complete;
    return result;
}

std::uint64_t
CaptureAutomationController::GetAcceptedFrameCount() const noexcept
{
    return m_acceptedFrameCount;
}

std::uint64_t
CaptureAutomationController::GetCompletedFrameCount() const noexcept
{
    return m_completedFrameCount;
}

Renderer::FrameDiagnostics*
CaptureAutomationController::GetFrameDiagnostics() noexcept
{
    return m_frameDiagnostics.get();
}

const Renderer::FrameDiagnostics*
CaptureAutomationController::GetFrameDiagnostics() const noexcept
{
    return m_frameDiagnostics.get();
}

void CaptureAutomationController::NotifyStreamingSceneActivated(
    Scene::RenderScene& scene)
{
    if (!m_deferredStreamingCapturePath.empty())
    {
        (void)Scene::FrameCameraToRenderObjects(scene);
        m_streamingCaptureFramesRemaining = 2;
    }
}

void CaptureAutomationController::RecordStreamingActivationFailure(
    std::string errorCode,
    std::string errorMessage)
{
    m_streamingSceneActivated = true;
    m_streamingActivationErrorCode = std::move(errorCode);
    m_streamingActivationErrorMessage = std::move(errorMessage);
}

void CaptureAutomationController::RecordStreamingActivationSuccess(
    const std::size_t renderObjectCount) noexcept
{
    m_streamingSceneActivated = true;
    m_streamingSceneActivationSucceeded = true;
    m_streamingActivatedObjectCount = renderObjectCount;
}

bool CaptureAutomationController::ShouldAttemptStreamingActivation()
    const noexcept
{
    return !m_streamingSceneActivated
        && !m_streamingSceneAssetId.empty();
}

void CaptureAutomationController::SetStreamingSceneAssetId(
    std::string assetId)
{
    m_streamingSceneAssetId = std::move(assetId);
}

const std::string&
CaptureAutomationController::GetStreamingSceneAssetId() const noexcept
{
    return m_streamingSceneAssetId;
}

WaterValidationSequence&
CaptureAutomationController::GetWaterValidationSequence() noexcept
{
    return *m_waterValidationSequence;
}

const WaterValidationSequence&
CaptureAutomationController::GetWaterValidationSequence() const noexcept
{
    return *m_waterValidationSequence;
}

bool CaptureAutomationController::ShouldCollectRenderGraphReport()
    const noexcept
{
    return !m_renderGraphReportPath.empty()
        && !m_renderGraphReportWritten;
}

bool CaptureAutomationController::ShouldCollectGpuTimingReport()
    const noexcept
{
    return !m_gpuTimingReportPath.empty()
        && !m_gpuTimingReportWritten
        && (m_activeCaptureRequestId != 0
            || (!m_exitAfterCapture
                && m_completedFrameCount + 1u >= 30u)
            || (m_maximumFrameCount > 0u
                && m_completedFrameCount + 1u
                    >= m_maximumFrameCount));
}

void CaptureAutomationController::WriteRenderGraphReportIfRequested(
    const RenderFrameFeedback& feedback)
{
    if (m_renderGraphReportPath.empty()
        || m_renderGraphReportWritten)
    {
        return;
    }
    if (!feedback.renderGraphReport.has_value())
    {
        throw std::logic_error(
            "RenderGraph report was not frozen on the execution lane.");
    }
    std::string error;
    if (!Renderer::WriteRenderGraphReport(
            m_renderGraphReportPath,
            *feedback.renderGraphReport,
            &error))
    {
        throw std::runtime_error(
            "RenderGraph report failed: " + error);
    }
    m_renderGraphReportWritten = true;
}

void CaptureAutomationController::WriteGpuTimingReportIfReady(
    const RenderFrameFeedback& feedback,
    const std::string_view sceneLabel,
    const std::string_view adapterName)
{
    if (m_gpuTimingReportPath.empty() || m_gpuTimingReportWritten
        || !(m_captureComplete
             || (!m_exitAfterCapture && m_completedFrameCount >= 30u)
             || (m_maximumFrameCount > 0u
                 && m_completedFrameCount >= m_maximumFrameCount)))
    {
        return;
    }
    const RenderViewRuntimeFeedback* const gameFeedback =
        feedback.FindView(Scene::GameRenderViewId);
    if (gameFeedback == nullptr
        || !feedback.gpuTimingCaptureMetadata.has_value())
    {
        throw std::logic_error(
            "GPU timing report was not frozen on the execution lane.");
    }
    nlohmann::json metadata =
        *feedback.gpuTimingCaptureMetadata;
    metadata["completedFrames"] = m_completedFrameCount;
    metadata["completedAcceptanceId"] =
        m_lastCompletedAcceptanceId;
    metadata["completedLogicalFrameId"] =
        m_lastCompletedLogicalFrameId;
    metadata["sceneGeneration"] =
        m_lastCompletedSceneGeneration;
    metadata["sceneDataRevision"] =
        m_lastCompletedDataRevision;
    metadata["sceneEpoch"] = m_lastCompletedSceneEpoch;
    metadata["viewEpoch"] = m_lastCompletedViewEpoch;
    metadata["settingsRevision"] =
        m_lastCompletedSettingsRevision;
    metadata["validationSequence"] =
        m_waterValidationSequence->IsEnabled();
    metadata["scene"] = sceneLabel;
    metadata["cameraPreset"] = ReadEnvironmentVariableValue(
        "PRISM_RENDER_WATER_CAMERA");
    metadata["surfacePreset"] = ReadEnvironmentVariableValue(
        "PRISM_RENDER_OCEAN_PRESET");
    metadata["adapter"] = adapterName;
    metadata["deterministic"] =
        Renderer::IsDeterministicRenderCaptureEnabled();
    std::string error;
    if (!Renderer::WriteGpuTimingReport(
            m_gpuTimingReportPath,
            gameFeedback->gpuTimings,
            feedback.graphicsApi,
            &error,
            gameFeedback->gpuTimelineMetadata,
            metadata))
    {
        throw std::runtime_error(
            "GPU timing report failed: " + error);
    }
    m_gpuTimingReportWritten = true;
}

void CaptureAutomationController::HandleCaptureError(
    const std::string& error,
    Scene::SceneSession& sceneSession) const
{
    if (error.empty())
    {
        return;
    }
    if (m_exitAfterCapture)
    {
        throw std::runtime_error(
            "Render capture failed: " + error);
    }
    sceneSession.SetLoadMessage(
        "Scene capture failed: " + error);
}

bool CaptureAutomationController::ShouldExit() const noexcept
{
    if (m_exitAfterCapture
        && (m_captureSequence != nullptr
                ? m_captureSequence->IsComplete()
                : m_captureComplete))
    {
        return true;
    }
    if (m_renderGraphReportWritten && m_exitAfterRenderGraphReport)
    {
        return true;
    }
    return m_maximumFrameCount > 0
        && m_completedFrameCount >= m_maximumFrameCount;
}

void CaptureAutomationController::RequireSequenceComplete() const
{
    if (m_captureSequence != nullptr)
    {
        m_captureSequence->RequireComplete();
    }
}

std::optional<std::filesystem::path>
CaptureAutomationController::GetAssetStreamingReportPath(
    const bool streamingEnabled) const
{
    if (!streamingEnabled)
    {
        if (!m_streamingReportPath.empty()
            || !m_streamingSceneReportPath.empty())
        {
            throw std::runtime_error(
                "Asset streaming report requested, but asset streaming "
                "is not enabled.");
        }
        return std::nullopt;
    }
    return m_streamingReportPath.empty()
        ? std::nullopt
        : std::optional<std::filesystem::path>{
              m_streamingReportPath};
}

void CaptureAutomationController::WriteStreamingSceneReport(
    AssetRuntimeCoordinator& assets,
    const std::string_view graphicsApiName,
    const Scene::RenderScene& scene) const
{
    if (!m_streamingSceneReportPath.empty())
    {
        std::string error;
        if (!Scene::AssetStreamingSceneBridge::WriteReport(
                m_streamingSceneReportPath,
                graphicsApiName,
                m_streamingSceneAssetId,
                m_streamingSceneActivated,
                m_streamingSceneActivationSucceeded,
                m_streamingActivatedObjectCount,
                m_streamingActivationErrorCode,
                m_streamingActivationErrorMessage,
                assets.GetRegistry(),
                scene,
                &error))
        {
            throw std::runtime_error(
                "Asset streaming Scene report failed: " + error);
        }
    }
}

} // namespace Prism::Core
