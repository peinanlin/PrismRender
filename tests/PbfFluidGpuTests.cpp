#include "Asset/ShaderManager.h"
#include "Platform/Window.h"
#include "RHI/ICommandContext.h"
#include "RHI/IFrameContext.h"
#include "RHI/IGraphicsDevice.h"
#include "RHI/IRenderBackend.h"
#include "RHI/RenderBackendFactory.h"
#include "RHI/ShaderLayoutBuilder.h"
#include "Renderer/DemoSceneSettings.h"
#include "Renderer/Features/Fluid/PbfFluidSimulation.h"
#include "Renderer/Pipeline/PipelineCache.h"
#include "Renderer/RenderSettings.h"
#include "Scene/DemoSceneCatalog.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string_view>
#include <vector>

namespace
{
using namespace Prism;

// Destroy this before any simulation/binding objects, including on failure.
struct WaitBeforeResourcesDie
{
    RHI::IFrameContext& frame;
    ~WaitBeforeResourcesDie() { frame.WaitForGpu(); }
};

void PreparePresent(RHI::IRenderBackend& backend)
{
    auto& frame = backend.GetFrameContext();
    RHI::RenderingInfo rendering{};
    rendering.width = frame.GetFrameWidth();
    rendering.height = frame.GetFrameHeight();
    RHI::RenderingAttachment color{};
    color.view = &frame.GetCurrentBackBufferView();
    color.loadOperation = RHI::LoadOperation::Clear;
    color.storeOperation = RHI::StoreOperation::Store;
    color.stateBefore = frame.GetGraphicsApi() == RHI::GraphicsApi::Vulkan
        ? RHI::ResourceState::Present : RHI::ResourceState::RenderTarget;
    color.stateAfter = RHI::ResourceState::RenderTarget;
    rendering.colorAttachments.push_back(color);
    backend.GetCommandContext().BeginRendering(rendering);
    backend.GetCommandContext().EndRendering();
}
} // namespace

int main(int argc, char** argv)
{
    std::cout << std::unitbuf;
    try
    {
        if (argc != 2 || (std::string_view(argv[1]) != "d3d12"
            && std::string_view(argv[1]) != "vulkan"))
            throw std::runtime_error("Usage: PrismPbfFluidGpuTests d3d12|vulkan");
        const auto api = std::string_view(argv[1]) == "vulkan"
            ? RHI::GraphicsApi::Vulkan : RHI::GraphicsApi::Direct3D12;
        Platform::Window window("PBF GPU Repeatability", 64u, 64u);
        auto backend = RHI::CreateRenderBackend(api);
        backend->Initialize(window);
        auto& device = backend->GetGraphicsDevice();
        auto& frame = backend->GetFrameContext();
        Asset::ShaderManager shaders;
        Renderer::PipelineCache cache;
        Renderer::RenderSettings settings{};
        Renderer::ApplyDemoSceneSettings(Scene::DemoSceneId::PbfLab, settings);
        Renderer::PbfFluidSimulation simulation;
        simulation.Initialize(device, shaders, cache, std::filesystem::path(PRISM_RENDER_SHADER_DIR) / "Fluid",
            device.GetPreferredShaderBinaryFormat(), frame.GetFramesInFlight(), settings.fluid);

        const auto& shader = shaders.LoadShader(
            std::filesystem::path(PRISM_RENDER_TEST_SHADER_DIR) / "PbfSnapshot.hlsl",
            "SnapshotCS", RHI::ShaderStage::Compute, device.GetPreferredShaderBinaryFormat());
        const std::array stages{RHI::ShaderLayoutStage{&shader.reflection, RHI::ShaderStage::Compute}};
        auto layout = device.CreateDescriptorSetLayout(RHI::BuildDescriptorSetLayout(stages, {}));
        RHI::ComputePipelineDescription pipelineDescription{};
        pipelineDescription.computeShader = shader;
        pipelineDescription.descriptorSetLayout = layout;
        auto pipeline = device.CreateComputePipeline(pipelineDescription);
        const std::size_t wordCount = static_cast<std::size_t>(simulation.GetParticleCount()) * 5u;
        RHI::BufferDescription description{};
        description.size = wordCount * sizeof(std::uint32_t);
        description.stride = sizeof(std::uint32_t);
        description.usage = RHI::BufferUsage::Storage | RHI::BufferUsage::CopySource
            | RHI::BufferUsage::CopyDestination;
        description.memoryAccess = RHI::MemoryAccess::GpuOnly;
        auto output = device.CreateBuffer(description);
        description.usage = RHI::BufferUsage::CopyDestination;
        description.memoryAccess = RHI::MemoryAccess::GpuToCpu;
        auto readback = device.CreateBuffer(description);
        auto set = device.CreateDescriptorSet(layout);
        set->WriteBuffer(16u, simulation.GetParticlePositionBufferShared());
        set->WriteBuffer(17u, simulation.GetParticleDensityBufferShared());
        set->WriteBuffer(32u, output);
        WaitBeforeResourcesDie wait{frame};

        constexpr std::array sampleFrames{0u, 1u, 30u};
        std::array<std::vector<std::uint32_t>, sampleFrames.size()> reference;
        bool identical = true;
        std::uint32_t dispatches = 0u;
        for (std::uint32_t replay = 0u; replay < 3u; ++replay)
        {
            simulation.RequestReset();
            for (std::uint32_t logicalFrame = 0u; logicalFrame <= sampleFrames.back(); ++logicalFrame)
            {
                if (frame.BeginFrame() != RHI::FrameResult::Ready)
                    throw std::runtime_error("Cannot begin PBF test frame.");
                const auto slot = frame.GetCurrentFrameIndex();
                simulation.Update(slot, logicalFrame == 0u ? 0.0f : 1.0f / 60.0f, settings.fluid);
                Renderer::RenderGraph graph;
                auto resources = simulation.RegisterRenderGraph(graph, slot);
                Renderer::PbfFluidSimulation::AddPasses(graph, resources,
                    [&](auto& command, const auto&) { simulation.Execute(command, slot); });
                auto& command = backend->GetCommandContext();
                graph.Execute(command);
                const auto sample = std::ranges::find(sampleFrames, logicalFrame);
                const bool capture = sample != sampleFrames.end();
                if (capture)
                {
                    for (const auto& buffer : {simulation.GetParticlePositionBufferShared(),
                        simulation.GetParticleDensityBufferShared()})
                        command.BufferBarrier({buffer.get(), RHI::ResourceState::UnorderedAccess,
                            RHI::ResourceState::ShaderResource});
                    command.BindComputePipeline(*pipeline);
                    command.BindDescriptorSet(*set);
                    command.Dispatch((simulation.GetParticleCount() + 63u) / 64u, 1u, 1u);
                    command.BufferBarrier({output.get(), RHI::ResourceState::UnorderedAccess,
                        RHI::ResourceState::CopySource});
                    command.CopyBuffer(*output, *readback, description.size);
                    command.BufferBarrier({output.get(), RHI::ResourceState::CopySource,
                        RHI::ResourceState::UnorderedAccess});
                }
                simulation.EndFrame(true, capture ? RHI::ResourceState::ShaderResource
                    : RHI::ResourceState::UnorderedAccess, capture ? RHI::ResourceState::ShaderResource
                    : RHI::ResourceState::UnorderedAccess);
                PreparePresent(*backend);
                if (frame.EndFrame() != RHI::FrameResult::Ready)
                    throw std::runtime_error("Cannot end PBF test frame.");
                dispatches += simulation.GetStatistics().dispatchCount;
                if (capture)
                {
                    frame.WaitForGpu();
                    std::vector<std::uint32_t> values(wordCount);
                    readback->Read(values.data(), description.size);
                    for (const auto value : values)
                        if (!std::isfinite(std::bit_cast<float>(value)))
                            throw std::runtime_error("PBF snapshot contains a non-finite value.");
                    for (std::size_t i = 3u; i < values.size(); i += 5u)
                        if (std::bit_cast<float>(values[i]) != 1.0f)
                            throw std::runtime_error("PBF snapshot did not contain initialized particles.");
                    auto& expected = reference[static_cast<std::size_t>(sample - sampleFrames.begin())];
                    if (replay == 0u) expected = values;
                    std::size_t differences = 0u;
                    float maximumDifference = 0.0f;
                    for (std::size_t i = 0u; i < values.size(); ++i)
                    {
                        differences += values[i] != expected[i];
                        maximumDifference = std::max(maximumDifference,
                            std::abs(std::bit_cast<float>(values[i]) - std::bit_cast<float>(expected[i])));
                    }
                    const auto& statistics = simulation.GetStatistics();
                    std::cout << "replay=" << replay << " frame=" << logicalFrame
                        << " changedWords=" << differences << " maxAbs=" << maximumDifference
                        << " gridOverflow=" << statistics.gridOverflowCount
                        << " neighborOverflow=" << statistics.neighborOverflowCount
                        << " maxCell=" << statistics.maximumCellOccupancy << '\n';
                    identical = identical && differences == 0u;
                }
            }
            // The last diagnostic readback belongs to an earlier frame slot.
            // All submitted work is complete at the final sample; resolve each
            // slot without advancing time before accepting this replay.
            for (std::uint32_t slot = 0u; slot < frame.GetFramesInFlight(); ++slot)
                simulation.Update(slot, 0.0f, settings.fluid);
            const auto& diagnostics = simulation.GetStatistics();
            std::cout << "replay=" << replay << " resolvedGridOverflow=" << diagnostics.gridOverflowCount
                << " resolvedNeighborOverflow=" << diagnostics.neighborOverflowCount
                << " invalidParticles=" << diagnostics.invalidParticleCount << '\n';
            if (!diagnostics.diagnosticsValid || diagnostics.gridOverflowCount != 0u
                || diagnostics.neighborOverflowCount != 0u || diagnostics.invalidParticleCount != 0u)
                throw std::runtime_error("PBF replay needs valid, non-overflowing neighbor data.");
        }
        if (reference.front() == reference.back())
            throw std::runtime_error("PBF replay did not advance the simulation.");
        std::cout << "dispatches=" << dispatches << " particles=" << simulation.GetParticleCount() << '\n';
        if (!identical) throw std::runtime_error("PBF fixed-input GPU replay is not bitwise repeatable.");
        std::cout << "PBF GPU repeatability passed.\n";
        return EXIT_SUCCESS;
    }
    catch (const std::exception& error)
    {
        std::cerr << "PBF GPU repeatability failed: " << error.what() << '\n';
        return EXIT_FAILURE;
    }
}
