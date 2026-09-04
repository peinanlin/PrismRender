#pragma once

#include "RHI/GraphicsApi.h"
#include "Scene/DemoSceneCatalog.h"

#include <optional>
#include <string>

namespace Prism::Core
{
struct ApplicationCommandLineOptions
{
    RHI::GraphicsApi graphicsApi = RHI::GraphicsApi::Vulkan;
    std::optional<Scene::DemoSceneId> demoScene;
    bool helpRequested = false;
};

[[nodiscard]] ApplicationCommandLineOptions
    ParseApplicationCommandLine(
        int argumentCount,
        char** arguments);
[[nodiscard]] std::string BuildApplicationHelpText();
} // namespace Prism::Core
