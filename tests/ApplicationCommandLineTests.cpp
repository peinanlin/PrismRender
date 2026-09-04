#include "Core/ApplicationCommandLine.h"

#include <cstdlib>
#include <initializer_list>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace
{
using Prism::Core::ApplicationCommandLineOptions;
using Prism::Scene::DemoSceneId;

void Expect(const bool condition, const char* message)
{
    if (!condition)
    {
        throw std::runtime_error(message);
    }
}

ApplicationCommandLineOptions Parse(
    std::initializer_list<const char*> arguments)
{
    std::vector<std::string> storage;
    storage.reserve(arguments.size());
    for (const char* argument : arguments)
    {
        storage.emplace_back(argument);
    }
    std::vector<char*> pointers;
    pointers.reserve(storage.size());
    for (std::string& argument : storage)
    {
        pointers.push_back(argument.data());
    }
    return Prism::Core::ParseApplicationCommandLine(
        static_cast<int>(pointers.size()),
        pointers.data());
}

void ExpectInvalid(
    std::initializer_list<const char*> arguments)
{
    bool threw = false;
    try
    {
        (void)Parse(arguments);
    }
    catch (const std::invalid_argument&)
    {
        threw = true;
    }
    Expect(threw, "Invalid command line was accepted.");
}
} // namespace

int main()
{
    try
    {
        Expect(
            Parse({"PrismRender", "--scene=hpwater-ocean"}).demoScene
                == DemoSceneId::HpWaterOceanLab,
            "The hpwater-ocean scene was not parsed.");
        Expect(
            Parse({"PrismRender", "--scene", "hp-water"}).demoScene
                == DemoSceneId::HpWaterOceanLab,
            "The hp-water scene alias was not parsed.");
        Expect(
            Parse({"PrismRender", "--scene=pbf"}).demoScene
                == DemoSceneId::PbfLab,
            "The pbf scene was not parsed.");
        Expect(
            Parse({"PrismRender", "--scene", "fluid-render"})
                    .demoScene
                == DemoSceneId::FluidRenderLab,
            "The fluid-render scene was not parsed.");
        Expect(
            Parse({"PrismRender", "--scene=fluid-caustics"})
                    .demoScene
                == DemoSceneId::FluidCausticsLab,
            "The fluid-caustics scene was not parsed.");
        Expect(
            Parse({"PrismRender", "--scene=fluid-toon"})
                    .demoScene
                == DemoSceneId::FluidToonLab,
            "The fluid-toon scene was not parsed.");
        Expect(
            Parse({"PrismRender", "--scene=fluid"}).demoScene
                == DemoSceneId::FluidRenderLab,
            "The legacy fluid alias must select fluid-render.");
        Expect(
            Parse({"PrismRender", "--api=vulkan", "--scene=pbf"})
                    .graphicsApi
                == Prism::RHI::GraphicsApi::Vulkan,
            "Combined API and scene parsing failed.");
        Expect(
            Parse({"PrismRender", "-h"}).helpRequested,
            "The short help switch was not parsed.");
        ExpectInvalid({"PrismRender", "--scene"});
        ExpectInvalid({"PrismRender", "--scene=unknown-fluid"});

        const std::string help =
            Prism::Core::BuildApplicationHelpText();
        for (const char* scene : {
                 "pbf",
                 "fluid-render",
                 "fluid-caustics",
                 "fluid-toon",
                 "hpwater-ocean",
                 "fluid -> fluid-render"})
        {
            Expect(
                help.find(scene) != std::string::npos,
                "Application help omitted a Fluid demo scene.");
        }
        std::cout << "Application command-line tests passed.\n";
        return EXIT_SUCCESS;
    }
    catch (const std::exception& exception)
    {
        std::cerr << "Application command-line test failed: "
                  << exception.what() << '\n';
        return EXIT_FAILURE;
    }
}
