#include "Core/Application/ApplicationComposition.h"

#include <exception>
#include <stdexcept>

namespace Prism::Core
{
namespace
{
struct InitializationProgress
{
    bool servicesCreated = false;
    bool backendInitialized = false;
    bool renderersInitialized = false;
    bool editorInitialized = false;
};

void ExecuteInitialization(
    const ApplicationCompositionOptions& options,
    IApplicationCompositionActions& actions,
    InitializationProgress& progress)
{
    actions.Execute(ApplicationCompositionAction::InitializeBegin);
    actions.Execute(ApplicationCompositionAction::CreateServices);
    progress.servicesCreated = true;
    actions.Execute(ApplicationCompositionAction::InitializeBackend);
    progress.backendInitialized = true;
    actions.Execute(ApplicationCompositionAction::LoadContent);
    actions.Execute(ApplicationCompositionAction::InitializeRenderers);
    progress.renderersInitialized = true;
    if (options.editorEnabled)
    {
        actions.Execute(ApplicationCompositionAction::InitializeEditor);
        progress.editorInitialized = true;
    }
    actions.Execute(ApplicationCompositionAction::InitializeComplete);
}

void ExecuteFrame(
    const ApplicationCompositionOptions& options,
    IApplicationCompositionActions& actions)
{
    actions.Execute(ApplicationCompositionAction::FrameBegin);
    actions.Execute(ApplicationCompositionAction::EventsAndInput);
    if (options.editorEnabled)
    {
        actions.Execute(
            ApplicationCompositionAction::EditorAndTransactions);
    }
    actions.Execute(ApplicationCompositionAction::SceneSynchronization);
    actions.Execute(ApplicationCompositionAction::BeginBackendFrame);
    actions.Execute(
        ApplicationCompositionAction::AssetUploadAndActivation);
    actions.Execute(ApplicationCompositionAction::PublishFrameInput);
    actions.Execute(ApplicationCompositionAction::RenderGameView);
    if (options.sceneViewEnabled)
    {
        actions.Execute(ApplicationCompositionAction::RenderSceneView);
    }
    actions.Execute(ApplicationCompositionAction::CaptureReportsAndUi);
    actions.Execute(ApplicationCompositionAction::EndBackendFrame);
    actions.Execute(ApplicationCompositionAction::CompletionFeedback);
    actions.Execute(ApplicationCompositionAction::FrameComplete);
}

void ExecuteShutdown(
    const ApplicationCompositionOptions& options,
    IApplicationCompositionActions& actions,
    const InitializationProgress& progress,
    const bool gpuAlreadyDrained)
{
    actions.Execute(ApplicationCompositionAction::ShutdownBegin);
    if (progress.backendInitialized && !gpuAlreadyDrained)
    {
        actions.Execute(ApplicationCompositionAction::WaitForGpu);
    }
    if (options.editorEnabled && progress.editorInitialized)
    {
        actions.Execute(ApplicationCompositionAction::ReleaseEditor);
    }
    if (progress.servicesCreated)
    {
        actions.Execute(ApplicationCompositionAction::ReleaseSceneState);
        actions.Execute(ApplicationCompositionAction::ReleaseAssets);
    }
    if (progress.renderersInitialized)
    {
        actions.Execute(ApplicationCompositionAction::ReleaseRenderers);
    }
    if (progress.servicesCreated)
    {
        actions.Execute(
            ApplicationCompositionAction::ReleaseBackendAndWindow);
        actions.Execute(ApplicationCompositionAction::ReleaseTiming);
    }
    actions.Execute(ApplicationCompositionAction::ShutdownComplete);
}
} // namespace

std::string_view ToString(
    const ApplicationCompositionAction action) noexcept
{
    switch (action)
    {
    case ApplicationCompositionAction::InitializeBegin:
        return "initialize-begin";
    case ApplicationCompositionAction::CreateServices:
        return "create-services";
    case ApplicationCompositionAction::InitializeBackend:
        return "initialize-backend";
    case ApplicationCompositionAction::LoadContent:
        return "load-content";
    case ApplicationCompositionAction::InitializeRenderers:
        return "initialize-renderers";
    case ApplicationCompositionAction::InitializeEditor:
        return "initialize-editor";
    case ApplicationCompositionAction::InitializeComplete:
        return "initialize-complete";
    case ApplicationCompositionAction::FrameBegin:
        return "frame-begin";
    case ApplicationCompositionAction::EventsAndInput:
        return "events-and-input";
    case ApplicationCompositionAction::EditorAndTransactions:
        return "editor-and-transactions";
    case ApplicationCompositionAction::SceneSynchronization:
        return "scene-synchronization";
    case ApplicationCompositionAction::BeginBackendFrame:
        return "begin-backend-frame";
    case ApplicationCompositionAction::AssetUploadAndActivation:
        return "asset-upload-and-activation";
    case ApplicationCompositionAction::PublishFrameInput:
        return "publish-frame-input";
    case ApplicationCompositionAction::RenderGameView:
        return "render-game-view";
    case ApplicationCompositionAction::RenderSceneView:
        return "render-scene-view";
    case ApplicationCompositionAction::CaptureReportsAndUi:
        return "capture-reports-and-ui";
    case ApplicationCompositionAction::EndBackendFrame:
        return "end-backend-frame";
    case ApplicationCompositionAction::CompletionFeedback:
        return "completion-feedback";
    case ApplicationCompositionAction::FrameComplete:
        return "frame-complete";
    case ApplicationCompositionAction::RunExitRequested:
        return "run-exit-requested";
    case ApplicationCompositionAction::DrainGpuAndReports:
        return "drain-gpu-and-reports";
    case ApplicationCompositionAction::ShutdownBegin:
        return "shutdown-begin";
    case ApplicationCompositionAction::WaitForGpu:
        return "wait-for-gpu";
    case ApplicationCompositionAction::ReleaseEditor:
        return "release-editor";
    case ApplicationCompositionAction::ReleaseSceneState:
        return "release-scene-state";
    case ApplicationCompositionAction::ReleaseAssets:
        return "release-assets";
    case ApplicationCompositionAction::ReleaseRenderers:
        return "release-renderers";
    case ApplicationCompositionAction::ReleaseBackendAndWindow:
        return "release-backend-and-window";
    case ApplicationCompositionAction::ReleaseTiming:
        return "release-timing";
    case ApplicationCompositionAction::ShutdownComplete:
        return "shutdown-complete";
    case ApplicationCompositionAction::InitializeFailed:
        return "initialize-failed";
    }
    return "unknown";
}

ApplicationCompositionResult ExecuteApplicationComposition(
    const ApplicationCompositionOptions& options,
    IApplicationCompositionActions& actions)
{
    if (options.sceneViewEnabled && !options.editorEnabled)
    {
        throw std::invalid_argument(
            "A Scene view requires the Editor composition path.");
    }

    InitializationProgress progress{};
    try
    {
        ExecuteInitialization(options, actions, progress);
    }
    catch (...)
    {
        const std::exception_ptr failure = std::current_exception();
        actions.Execute(ApplicationCompositionAction::InitializeFailed);
        ExecuteShutdown(options, actions, progress, false);
        std::rethrow_exception(failure);
    }

    ApplicationCompositionResult result{};
    try
    {
        while (!actions.ShouldClose())
        {
            ExecuteFrame(options, actions);
            ++result.completedFrameCount;
            if (options.maximumFrameCount != 0
                && result.completedFrameCount
                    >= options.maximumFrameCount)
            {
                result.exitReason =
                    ApplicationCompositionExitReason::MaximumFrameCount;
                break;
            }
        }

        actions.Execute(ApplicationCompositionAction::RunExitRequested);
        actions.Execute(ApplicationCompositionAction::DrainGpuAndReports);
    }
    catch (...)
    {
        const std::exception_ptr failure = std::current_exception();
        ExecuteShutdown(options, actions, progress, false);
        std::rethrow_exception(failure);
    }

    ExecuteShutdown(options, actions, progress, true);
    return result;
}
} // namespace Prism::Core
