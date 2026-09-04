#include "Core/Profiling/SceneViewRefreshController.h"

#include <exception>
#include <iostream>
#include <stdexcept>

namespace
{
void Expect(const bool condition, const char* const message)
{
    if (!condition)
    {
        throw std::runtime_error(message);
    }
}

template <typename Callback>
void Reject(Callback&& callback)
{
    bool rejected = false;
    try
    {
        callback();
    }
    catch (const std::exception&)
    {
        rejected = true;
    }
    Expect(rejected, "Invalid Scene View refresh input was accepted.");
}
} // namespace

int main()
{
    using namespace Prism::Core;
    try
    {
        Expect(ParseSceneViewRefreshPolicy("catalog-default")
                    == SceneViewRefreshPolicy::CatalogDefault
                && ParseSceneViewRefreshPolicy("on-interaction")
                    == SceneViewRefreshPolicy::OnInteraction
                && ParseSceneViewRefreshPolicy("30hz")
                    == SceneViewRefreshPolicy::ThirtyHertz,
            "Scene View refresh policies were not parsed exactly.");
        Reject([]
        {
            (void)ParseSceneViewRefreshPolicy("throttled");
        });

        SceneViewRefreshController controller;
        SceneViewRefreshController::Inputs inputs{};
        inputs.visible = true;
        inputs.sessionTimeSeconds = 0.0;
        auto decision = controller.Evaluate(inputs);
        Expect(decision.render
                && decision.reason == SceneViewRefreshReason::Live,
            "Catalog continuous behavior was not preserved.");

        controller.Reset();
        inputs.catalogOnInteraction = true;
        decision = controller.Evaluate(inputs);
        Expect(!decision.render
                && decision.reason
                    == SceneViewRefreshReason::CatalogDefault,
            "Catalog on-interaction behavior was not preserved.");
        inputs.requiresRefresh = true;
        decision = controller.Evaluate(inputs);
        Expect(decision.render,
            "A dirty on-interaction Scene View did not refresh.");

        controller.SetPolicy(SceneViewRefreshPolicy::Live);
        inputs.requiresRefresh = false;
        decision = controller.Evaluate(inputs);
        Expect(decision.render
                && decision.reason == SceneViewRefreshReason::Live,
            "Live policy did not override the catalog policy.");

        controller.SetPolicy(SceneViewRefreshPolicy::Paused);
        inputs.interacting = true;
        decision = controller.Evaluate(inputs);
        Expect(!decision.render
                && decision.reason == SceneViewRefreshReason::Paused,
            "Paused policy refreshed from interaction.");
        inputs.visible = false;
        inputs.captureRequested = true;
        decision = controller.Evaluate(inputs);
        Expect(decision.render
                && decision.reason == SceneViewRefreshReason::Capture,
            "Explicit Scene capture did not override hidden/paused state.");

        controller.SetPolicy(SceneViewRefreshPolicy::ThirtyHertz);
        inputs = {};
        inputs.visible = true;
        inputs.sessionTimeSeconds = 1.0;
        Expect(controller.Evaluate(inputs).render,
            "30 Hz policy did not render its first eligible frame.");
        inputs.sessionTimeSeconds = 1.01;
        Expect(!controller.Evaluate(inputs).render,
            "30 Hz policy rendered before its next interval.");
        inputs.sessionTimeSeconds = 1.034;
        Expect(controller.Evaluate(inputs).render,
            "30 Hz policy did not render after its interval.");

        inputs.sessionTimeSeconds = -1.0;
        Reject([&] { (void)controller.Evaluate(inputs); });

        std::cout << "Scene View refresh policy tests passed.\n";
        return 0;
    }
    catch (const std::exception& exception)
    {
        std::cerr << exception.what() << '\n';
        return 1;
    }
}
