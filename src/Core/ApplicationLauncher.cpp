#include "Core/ApplicationLauncher.h"

#include "Core/ApplicationCommandLine.h"
#include "Core/ApplicationHost.h"
#include "Core/CpuTrace.h"
#include "Core/ProcessDiagnostics.h"
#include "RHI/GraphicsApi.h"

#include <iostream>
#include <stdexcept>

namespace Prism::Core
{
    int RunApplication(const int argumentCount, char** arguments)
    {
        CpuTraceSpan applicationSpan("Application", "process");
        const ApplicationCommandLineOptions options =
            ParseApplicationCommandLine(
                argumentCount,
                arguments);
        if (options.helpRequested)
        {
            std::cout << BuildApplicationHelpText();
            return 0;
        }
        CpuTraceSpan apiSelectionSpan(
            "GraphicsApiSelection",
            "initialization");
        const RHI::GraphicsApi graphicsApi = options.graphicsApi;
        apiSelectionSpan.End();
        ProcessDiagnostics::SetGraphicsApi(RHI::ToString(graphicsApi));
        ProcessDiagnostics::Log(
            "info",
            "renderer.api_selected",
            "The renderer graphics API was selected.",
            {{"graphicsApi", RHI::ToString(graphicsApi)}});
        if (graphicsApi == RHI::GraphicsApi::Direct3D11)
        {
            throw std::runtime_error(
                "The D3D11 translator exists, but a runnable D3D11 backend has not been implemented.");
        }
    #if defined(PRISM_RENDER_HAS_D3D12)
    #else
        if (graphicsApi == RHI::GraphicsApi::Direct3D12)
        {
            throw std::runtime_error(
                "Direct3D 12 is only available on Windows. Use --api=vulkan.");
        }
    #endif

        ProcessDiagnostics::Log(
            "info",
            "renderer.initialize.begin",
            "ApplicationHost initialization started.");
        ApplicationHost application(
            graphicsApi,
            options.demoScene);
        {
            CpuTraceSpan initializeSpan(
                "RendererInitialize",
                "initialization");
            application.Initialize();
        }
        ProcessDiagnostics::Log(
            "info",
            "renderer.initialize.completed",
            "ApplicationHost initialization completed.");
        CpuTraceSpan runSpan("RendererRun", "process");
        return application.Run();
    }
} // namespace Prism::Core
