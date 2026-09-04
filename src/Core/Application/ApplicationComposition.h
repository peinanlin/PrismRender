#pragma once

#include <cstdint>
#include <string_view>

namespace Prism::Core
{
    // Coarse application actions intentionally describe ownership boundaries,
    // not renderer algorithms. P1 coordinators can replace individual actions
    // without changing their externally observable order.
    enum class ApplicationCompositionAction
    {
        InitializeBegin,
        CreateServices,
        InitializeBackend,
        LoadContent,
        InitializeRenderers,
        InitializeEditor,
        InitializeComplete,
        FrameBegin,
        EventsAndInput,
        EditorAndTransactions,
        SceneSynchronization,
        BeginBackendFrame,
        AssetUploadAndActivation,
        PublishFrameInput,
        RenderGameView,
        RenderSceneView,
        CaptureReportsAndUi,
        EndBackendFrame,
        CompletionFeedback,
        FrameComplete,
        RunExitRequested,
        DrainGpuAndReports,
        ShutdownBegin,
        WaitForGpu,
        ReleaseEditor,
        ReleaseSceneState,
        ReleaseAssets,
        ReleaseRenderers,
        ReleaseBackendAndWindow,
        ReleaseTiming,
        ShutdownComplete,
        InitializeFailed
    };

    std::string_view ToString(ApplicationCompositionAction action) noexcept;

    struct ApplicationCompositionOptions
    {
        bool editorEnabled = false;
        bool sceneViewEnabled = false;

        // Zero preserves the interactive behavior: the window/host decides
        // when to close. A non-zero value models PRISM_RENDER_MAX_FRAMES.
        std::uint64_t maximumFrameCount = 0;
    };

    enum class ApplicationCompositionExitReason
    {
        WindowRequested,
        MaximumFrameCount
    };

    struct ApplicationCompositionResult
    {
        ApplicationCompositionExitReason exitReason =
            ApplicationCompositionExitReason::WindowRequested;
        std::uint64_t completedFrameCount = 0;
    };

    // The interface is deliberately narrow: it owns no application state and
    // only executes a named action or reports the existing close condition.
    // Tests use a recorder; P1 services will provide the production actions.
    class IApplicationCompositionActions
    {
    public:
        virtual ~IApplicationCompositionActions() = default;

        virtual void Execute(ApplicationCompositionAction action) = 0;
        virtual bool ShouldClose() const = 0;
    };

    // Executes the ApplicationHost lifecycle contract. Initialization and run
    // failures clean up only successfully created stages; a successful run is
    // already GPU-drained and therefore does not request a duplicate wait.
    ApplicationCompositionResult ExecuteApplicationComposition(
        const ApplicationCompositionOptions& options,
        IApplicationCompositionActions& actions);
} // namespace Prism::Core
