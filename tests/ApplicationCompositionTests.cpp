#include "Core/Application/ApplicationComposition.h"

#include <algorithm>
#include <exception>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace
{
using Prism::Core::ApplicationCompositionAction;

void Expect(const bool condition, const char* const message)
{
    if (!condition)
    {
        throw std::runtime_error(message);
    }
}

class RecordingActions final
    : public Prism::Core::IApplicationCompositionActions
{
public:
    void Execute(const ApplicationCompositionAction action) override
    {
        trace.push_back(action);
        if (action == ApplicationCompositionAction::FrameComplete)
        {
            ++completedFrames;
        }
        if (failAt.has_value() && action == *failAt)
        {
            throw std::runtime_error("Injected composition failure.");
        }
    }

    bool ShouldClose() const override
    {
        return closeAfterFrames.has_value()
            && completedFrames >= *closeAfterFrames;
    }

    std::vector<ApplicationCompositionAction> trace;
    std::optional<ApplicationCompositionAction> failAt;
    std::optional<std::uint64_t> closeAfterFrames;
    std::uint64_t completedFrames = 0;
};

void ExpectTrace(
    const RecordingActions& actions,
    const std::vector<ApplicationCompositionAction>& expected,
    const char* const message)
{
    if (actions.trace == expected)
    {
        return;
    }

    std::string detail = message;
    detail += "\nExpected:";
    for (const ApplicationCompositionAction action : expected)
    {
        detail += " ";
        detail += Prism::Core::ToString(action);
    }
    detail += "\nActual:";
    for (const ApplicationCompositionAction action : actions.trace)
    {
        detail += " ";
        detail += Prism::Core::ToString(action);
    }
    throw std::runtime_error(detail);
}

std::vector<ApplicationCompositionAction> InitializationTrace(
    const bool editorEnabled)
{
    std::vector<ApplicationCompositionAction> result{
        ApplicationCompositionAction::InitializeBegin,
        ApplicationCompositionAction::CreateServices,
        ApplicationCompositionAction::InitializeBackend,
        ApplicationCompositionAction::LoadContent,
        ApplicationCompositionAction::InitializeRenderers};
    if (editorEnabled)
    {
        result.push_back(ApplicationCompositionAction::InitializeEditor);
    }
    result.push_back(ApplicationCompositionAction::InitializeComplete);
    return result;
}

std::vector<ApplicationCompositionAction> FrameTrace(
    const bool editorEnabled,
    const bool sceneViewEnabled)
{
    std::vector<ApplicationCompositionAction> result{
        ApplicationCompositionAction::FrameBegin,
        ApplicationCompositionAction::EventsAndInput};
    if (editorEnabled)
    {
        result.push_back(
            ApplicationCompositionAction::EditorAndTransactions);
    }
    result.insert(result.end(), {
        ApplicationCompositionAction::SceneSynchronization,
        ApplicationCompositionAction::BeginBackendFrame,
        ApplicationCompositionAction::AssetUploadAndActivation,
        ApplicationCompositionAction::PublishFrameInput,
        ApplicationCompositionAction::RenderGameView});
    if (sceneViewEnabled)
    {
        result.push_back(ApplicationCompositionAction::RenderSceneView);
    }
    result.insert(result.end(), {
        ApplicationCompositionAction::CaptureReportsAndUi,
        ApplicationCompositionAction::EndBackendFrame,
        ApplicationCompositionAction::CompletionFeedback,
        ApplicationCompositionAction::FrameComplete});
    return result;
}

std::vector<ApplicationCompositionAction> ShutdownTrace(
    const bool editorEnabled,
    const bool waitForGpu = false)
{
    std::vector<ApplicationCompositionAction> result{
        ApplicationCompositionAction::RunExitRequested,
        ApplicationCompositionAction::DrainGpuAndReports,
        ApplicationCompositionAction::ShutdownBegin};
    if (waitForGpu)
    {
        result.push_back(ApplicationCompositionAction::WaitForGpu);
    }
    if (editorEnabled)
    {
        result.push_back(ApplicationCompositionAction::ReleaseEditor);
    }
    result.insert(result.end(), {
        ApplicationCompositionAction::ReleaseSceneState,
        ApplicationCompositionAction::ReleaseAssets,
        ApplicationCompositionAction::ReleaseRenderers,
        ApplicationCompositionAction::ReleaseBackendAndWindow,
        ApplicationCompositionAction::ReleaseTiming,
        ApplicationCompositionAction::ShutdownComplete});
    return result;
}

void Append(
    std::vector<ApplicationCompositionAction>& destination,
    const std::vector<ApplicationCompositionAction>& source)
{
    destination.insert(destination.end(), source.begin(), source.end());
}

void TestNormalEditorPath()
{
    RecordingActions actions;
    actions.closeAfterFrames = 2;
    const Prism::Core::ApplicationCompositionOptions options{
        .editorEnabled = true,
        .sceneViewEnabled = true};

    const auto result =
        Prism::Core::ExecuteApplicationComposition(options, actions);
    Expect(result.completedFrameCount == 2,
        "The normal Editor path did not complete two frames.");
    Expect(result.exitReason
            == Prism::Core::ApplicationCompositionExitReason::WindowRequested,
        "The normal Editor path reported the wrong exit reason.");

    auto expected = InitializationTrace(true);
    Append(expected, FrameTrace(true, true));
    Append(expected, FrameTrace(true, true));
    Append(expected, ShutdownTrace(true));
    ExpectTrace(actions, expected,
        "The normal Editor composition order changed.");
}

void TestNoEditorPath()
{
    RecordingActions actions;
    actions.closeAfterFrames = 1;
    const Prism::Core::ApplicationCompositionOptions options{};

    const auto result =
        Prism::Core::ExecuteApplicationComposition(options, actions);
    Expect(result.completedFrameCount == 1,
        "The no-Editor path did not complete one frame.");

    auto expected = InitializationTrace(false);
    Append(expected, FrameTrace(false, false));
    Append(expected, ShutdownTrace(false));
    ExpectTrace(actions, expected,
        "The no-Editor composition order changed.");
    Expect(std::ranges::find(
               actions.trace,
               ApplicationCompositionAction::InitializeEditor)
            == actions.trace.end(),
        "The no-Editor path initialized Editor state.");
}

void TestMaximumFrameEarlyExit()
{
    RecordingActions actions;
    const Prism::Core::ApplicationCompositionOptions options{
        .editorEnabled = true,
        .sceneViewEnabled = true,
        .maximumFrameCount = 1};

    const auto result =
        Prism::Core::ExecuteApplicationComposition(options, actions);
    Expect(result.completedFrameCount == 1,
        "The finite run did not stop after its first completed frame.");
    Expect(result.exitReason
            == Prism::Core::ApplicationCompositionExitReason::MaximumFrameCount,
        "The finite run did not report the maximum-frame exit.");

    auto expected = InitializationTrace(true);
    Append(expected, FrameTrace(true, true));
    Append(expected, ShutdownTrace(true));
    ExpectTrace(actions, expected,
        "The finite-run exit moved before frame completion.");
}

void TestPartialInitializationFailure()
{
    const auto expectFailure = [](
        const ApplicationCompositionAction failAt,
        const std::vector<ApplicationCompositionAction>& expected)
    {
        RecordingActions actions;
        actions.failAt = failAt;
        bool failed = false;
        try
        {
            (void)Prism::Core::ExecuteApplicationComposition(
                {.editorEnabled = true, .sceneViewEnabled = true},
                actions);
        }
        catch (const std::runtime_error&)
        {
            failed = true;
        }
        Expect(failed,
            "An injected initialization failure was swallowed.");
        ExpectTrace(actions, expected,
            "A partial initialization cleanup trace changed.");
    };

    expectFailure(ApplicationCompositionAction::CreateServices, {
        ApplicationCompositionAction::InitializeBegin,
        ApplicationCompositionAction::CreateServices,
        ApplicationCompositionAction::InitializeFailed,
        ApplicationCompositionAction::ShutdownBegin,
        ApplicationCompositionAction::ShutdownComplete});
    expectFailure(ApplicationCompositionAction::InitializeBackend, {
        ApplicationCompositionAction::InitializeBegin,
        ApplicationCompositionAction::CreateServices,
        ApplicationCompositionAction::InitializeBackend,
        ApplicationCompositionAction::InitializeFailed,
        ApplicationCompositionAction::ShutdownBegin,
        ApplicationCompositionAction::ReleaseSceneState,
        ApplicationCompositionAction::ReleaseAssets,
        ApplicationCompositionAction::ReleaseBackendAndWindow,
        ApplicationCompositionAction::ReleaseTiming,
        ApplicationCompositionAction::ShutdownComplete});
    expectFailure(ApplicationCompositionAction::LoadContent, {
        ApplicationCompositionAction::InitializeBegin,
        ApplicationCompositionAction::CreateServices,
        ApplicationCompositionAction::InitializeBackend,
        ApplicationCompositionAction::LoadContent,
        ApplicationCompositionAction::InitializeFailed,
        ApplicationCompositionAction::ShutdownBegin,
        ApplicationCompositionAction::WaitForGpu,
        ApplicationCompositionAction::ReleaseSceneState,
        ApplicationCompositionAction::ReleaseAssets,
        ApplicationCompositionAction::ReleaseBackendAndWindow,
        ApplicationCompositionAction::ReleaseTiming,
        ApplicationCompositionAction::ShutdownComplete});
    expectFailure(ApplicationCompositionAction::InitializeRenderers, {
        ApplicationCompositionAction::InitializeBegin,
        ApplicationCompositionAction::CreateServices,
        ApplicationCompositionAction::InitializeBackend,
        ApplicationCompositionAction::LoadContent,
        ApplicationCompositionAction::InitializeRenderers,
        ApplicationCompositionAction::InitializeFailed,
        ApplicationCompositionAction::ShutdownBegin,
        ApplicationCompositionAction::WaitForGpu,
        ApplicationCompositionAction::ReleaseSceneState,
        ApplicationCompositionAction::ReleaseAssets,
        ApplicationCompositionAction::ReleaseBackendAndWindow,
        ApplicationCompositionAction::ReleaseTiming,
        ApplicationCompositionAction::ShutdownComplete});
    expectFailure(ApplicationCompositionAction::InitializeEditor, {
        ApplicationCompositionAction::InitializeBegin,
        ApplicationCompositionAction::CreateServices,
        ApplicationCompositionAction::InitializeBackend,
        ApplicationCompositionAction::LoadContent,
        ApplicationCompositionAction::InitializeRenderers,
        ApplicationCompositionAction::InitializeEditor,
        ApplicationCompositionAction::InitializeFailed,
        ApplicationCompositionAction::ShutdownBegin,
        ApplicationCompositionAction::WaitForGpu,
        ApplicationCompositionAction::ReleaseSceneState,
        ApplicationCompositionAction::ReleaseAssets,
        ApplicationCompositionAction::ReleaseRenderers,
        ApplicationCompositionAction::ReleaseBackendAndWindow,
        ApplicationCompositionAction::ReleaseTiming,
        ApplicationCompositionAction::ShutdownComplete});
}

void TestRunFailureCleanup()
{
    RecordingActions actions;
    actions.failAt = ApplicationCompositionAction::RenderGameView;
    bool failed = false;
    try
    {
        (void)Prism::Core::ExecuteApplicationComposition(
            {.editorEnabled = true, .sceneViewEnabled = true},
            actions);
    }
    catch (const std::runtime_error&)
    {
        failed = true;
    }
    Expect(failed, "The injected run failure was swallowed.");

    auto expected = InitializationTrace(true);
    Append(expected, {
        ApplicationCompositionAction::FrameBegin,
        ApplicationCompositionAction::EventsAndInput,
        ApplicationCompositionAction::EditorAndTransactions,
        ApplicationCompositionAction::SceneSynchronization,
        ApplicationCompositionAction::BeginBackendFrame,
        ApplicationCompositionAction::AssetUploadAndActivation,
        ApplicationCompositionAction::PublishFrameInput,
        ApplicationCompositionAction::RenderGameView});
    auto shutdown = ShutdownTrace(true, true);
    shutdown.erase(shutdown.begin(), shutdown.begin() + 2);
    Append(expected, shutdown);
    ExpectTrace(actions, expected,
        "A run failure did not perform exactly one GPU-draining shutdown.");
}

void TestInvalidSceneViewComposition()
{
    RecordingActions actions;
    bool rejected = false;
    try
    {
        (void)Prism::Core::ExecuteApplicationComposition(
            {.editorEnabled = false, .sceneViewEnabled = true},
            actions);
    }
    catch (const std::invalid_argument&)
    {
        rejected = true;
    }
    Expect(rejected,
        "A Scene view without Editor composition was accepted.");
    Expect(actions.trace.empty(),
        "Invalid composition executed an application action.");
}
} // namespace

int main()
{
    try
    {
        TestNormalEditorPath();
        TestNoEditorPath();
        TestMaximumFrameEarlyExit();
        TestPartialInitializationFailure();
        TestRunFailureCleanup();
        TestInvalidSceneViewComposition();
        std::cout << "Application composition tests passed.\n";
        return 0;
    }
    catch (const std::exception& exception)
    {
        std::cerr << exception.what() << '\n';
        return 1;
    }
}
