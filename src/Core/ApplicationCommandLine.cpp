#include "Core/ApplicationCommandLine.h"

#include <sstream>
#include <stdexcept>
#include <string_view>

namespace Prism::Core
{
namespace
{
RHI::GraphicsApi DefaultGraphicsApi()
{
#if defined(PRISM_RENDER_HAS_D3D12)
    return RHI::GraphicsApi::Direct3D12;
#else
    return RHI::GraphicsApi::Vulkan;
#endif
}

std::string_view RequireValue(
    const std::string_view option,
    int& index,
    const int argumentCount,
    char** arguments)
{
    if (index + 1 >= argumentCount)
    {
        throw std::invalid_argument(
            "Missing value for " + std::string(option) + '.');
    }
    return arguments[++index];
}
} // namespace

ApplicationCommandLineOptions ParseApplicationCommandLine(
    const int argumentCount,
    char** arguments)
{
    ApplicationCommandLineOptions options{};
    options.graphicsApi = DefaultGraphicsApi();
    for (int index = 1; index < argumentCount; ++index)
    {
        const std::string_view argument(arguments[index]);
        if (argument == "--help" || argument == "-h")
        {
            options.helpRequested = true;
            continue;
        }

        std::string_view apiName;
        if (argument.starts_with("--api="))
        {
            apiName = argument.substr(6);
        }
        else if (argument == "--api")
        {
            apiName = RequireValue(
                argument,
                index,
                argumentCount,
                arguments);
        }
        if (!apiName.empty())
        {
            const auto parsedApi =
                RHI::TryParseGraphicsApi(apiName);
            if (!parsedApi.has_value())
            {
                throw std::invalid_argument(
                    "Unknown graphics API: "
                    + std::string(apiName));
            }
            options.graphicsApi = *parsedApi;
            continue;
        }
        if (argument == "--api"
            || argument.starts_with("--api="))
        {
            throw std::invalid_argument(
                "Missing value for --api.");
        }

        std::string_view sceneName;
        if (argument.starts_with("--scene="))
        {
            sceneName = argument.substr(8);
        }
        else if (argument == "--scene")
        {
            sceneName = RequireValue(
                argument,
                index,
                argumentCount,
                arguments);
        }
        if (!sceneName.empty())
        {
            options.demoScene =
                Scene::DemoSceneCatalog::TryParse(sceneName);
            if (!options.demoScene.has_value())
            {
                throw std::invalid_argument(
                    "Unknown demo scene: "
                    + std::string(sceneName)
                    + ". Use --help to list scene names.");
            }
        }
        else if (argument == "--scene"
                 || argument.starts_with("--scene="))
        {
            throw std::invalid_argument(
                "Missing value for --scene.");
        }
    }
    return options;
}

std::string BuildApplicationHelpText()
{
    std::ostringstream output;
    output
        << "PrismRender\n"
        << "Usage: PrismRender [--api d3d12|vulkan] "
           "[--scene <name>]\n\n"
        << "Demo scenes:\n";
    for (const Scene::DemoSceneDescription& description :
         Scene::DemoSceneCatalog::GetDescriptions())
    {
        output << "  " << description.key << " - "
               << description.displayName << ": "
               << description.purpose << '\n';
    }
    output
        << "\nAliases:\n"
        << "  hpwater, hp-water -> hpwater-ocean\n"
        << "  fluid -> fluid-render (legacy screen-space Fluid Lab)\n"
        << "  position-based-fluids -> pbf\n\n"
        << "When --scene is omitted, PRISM_RENDER_DEMO_SCENE remains the "
           "environment fallback.\n";
    return output.str();
}
} // namespace Prism::Core
