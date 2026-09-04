#include "Renderer/Features/ClusteredLighting.h"
#include "Renderer/Pipeline/PipelineCache.h"

#include "Core/Assert.h"
#include "RHI/IGraphicsDevice.h"
#include "RHI/ShaderLayoutBuilder.h"
#include "Scene/RenderScene.h"
#include "Scene/RenderSceneView.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <utility>

namespace Prism::Renderer
{
void ClusteredLighting::Initialize(
    RHI::IGraphicsDevice& device,
    Asset::ShaderManager& shaderManager,
    PipelineCache& pipelineCache,
    const std::filesystem::path& shaderPath,
    const RHI::ShaderBinaryFormat shaderFormat,
    const std::uint32_t framesInFlight)
{
    Core::Check(
        framesInFlight > 0,
        "Clustered lighting requires at least one frame in flight.");
    m_device = &device;
    const RHI::ShaderBinary& shader =
        shaderManager.LoadShader(
            shaderPath,
            "BuildLightClustersCS",
            RHI::ShaderStage::Compute,
            shaderFormat);
    const std::array stages = {
        RHI::ShaderLayoutStage{
            &shader.reflection,
            RHI::ShaderStage::Compute}};
    m_descriptorSetLayout =
        device.CreateDescriptorSetLayout(
            RHI::BuildDescriptorSetLayout(
                stages,
                {}));
    RHI::ComputePipelineDescription pipeline{};
    pipeline.computeShader = shader;
    pipeline.descriptorSetLayout =
        m_descriptorSetLayout;
    m_pipeline =
        pipelineCache.GetOrCreateCompute(
            device,
            "Feature.ClusteredLighting",
            pipeline);

    m_frames.resize(framesInFlight);
    for (FrameResources& frame : m_frames)
    {
        RHI::BufferDescription constants{};
        constants.size = sizeof(Constants);
        constants.stride = sizeof(Constants);
        constants.usage =
            RHI::BufferUsage::Constant;
        constants.memoryAccess =
            RHI::MemoryAccess::CpuToGpu;
        frame.constants =
            device.CreateBuffer(constants);

        RHI::BufferDescription lights{};
        lights.size =
            sizeof(GpuPointLight)
            * MaxLightCount;
        lights.stride = sizeof(GpuPointLight);
        lights.usage =
            RHI::BufferUsage::ShaderResource;
        lights.memoryAccess =
            RHI::MemoryAccess::CpuToGpu;
        frame.lights =
            device.CreateBuffer(lights);
    }
}

void ClusteredLighting::Resize(
    const std::uint32_t width,
    const std::uint32_t height)
{
    Core::Check(
        m_device != nullptr
            && width > 0
            && height > 0,
        "Clustered lighting resize requires an initialized device and extent.");
    m_width = width;
    m_height = height;
    m_tileCountX =
        (width + TileSize - 1u) / TileSize;
    m_tileCountY =
        (height + TileSize - 1u) / TileSize;
    m_clusterCount =
        m_tileCountX
        * m_tileCountY
        * DepthSliceCount;
    RebuildFrameResources();
}

void ClusteredLighting::Update(
    const Scene::RenderSceneView& scene,
    const std::uint32_t frameIndex,
    const bool enabled)
{
    Core::Check(
        frameIndex < m_frames.size()
            && m_clusterCount > 0,
        "Clustered lighting update uses an invalid frame.");
    const std::uint32_t lightCount =
        std::min<std::uint32_t>(
            scene.GetActivePointLightCount(),
            MaxLightCount);
    std::array<GpuPointLight, MaxLightCount>
        lights{};
    for (std::uint32_t index = 0;
         index < lightCount;
         ++index)
    {
        const Scene::PointLight& source =
            scene.GetPointLights()[index];
        lights[index] = {
            source.position,
            source.range,
            source.color,
            source.intensity};
    }
    FrameResources& frame = m_frames[frameIndex];
    frame.lights->Update(
        lights.data(),
        sizeof(lights));

    const Scene::Camera& camera =
        scene.GetCamera();
    Constants constants{};
    DirectX::XMStoreFloat4x4(
        &constants.worldToView,
        DirectX::XMMatrixTranspose(
            camera.GetViewMatrix()));
    const float projectionY =
        1.0f
        / std::tan(
            camera.GetFieldOfViewYRadians()
            * 0.5f);
    const float projectionX =
        projectionY
        / camera.GetAspectRatio();
    constants.viewportProjection = {
        static_cast<float>(m_width),
        static_cast<float>(m_height),
        projectionX,
        projectionY};
    const float nearPlane =
        std::max(camera.GetNearPlane(), 0.001f);
    const float farPlane =
        std::max(
            camera.GetFarPlane(),
            nearPlane + 0.001f);
    constants.depthParameters = {
        nearPlane,
        farPlane,
        std::log2(farPlane / nearPlane),
        enabled ? 1.0f : 0.0f};
    constants.tileCountX = m_tileCountX;
    constants.tileCountY = m_tileCountY;
    constants.lightCount =
        enabled ? lightCount : 0u;
    frame.constants->Update(
        &constants,
        sizeof(constants));
}

void ClusteredLighting::Execute(
    RHI::ICommandContext& commandContext,
    const std::uint32_t frameIndex) const
{
    Core::Check(
        frameIndex < m_frames.size()
            && m_clusterCount > 0,
        "Clustered lighting execution uses an invalid frame.");
    commandContext.BindComputePipeline(
        *m_pipeline);
    commandContext.BindDescriptorSet(
        *m_frames[frameIndex].descriptorSet);
    commandContext.Dispatch(
        (m_clusterCount + 63u) / 64u,
        1,
        1);
}

void ClusteredLighting::EndFrame(
    const std::uint32_t frameIndex,
    const bool executed)
{
    Core::Check(
        frameIndex < m_frames.size(),
        "Clustered lighting end-frame uses an invalid frame.");
    if (executed)
    {
        m_frames[frameIndex].countState =
            RHI::ResourceState::ShaderResource;
        m_frames[frameIndex].indexState =
            RHI::ResourceState::ShaderResource;
    }
}

RHI::IBuffer&
ClusteredLighting::GetConstantsBuffer(
    const std::uint32_t frameIndex) const
{
    return *m_frames.at(frameIndex).constants;
}

std::shared_ptr<RHI::IBuffer>
ClusteredLighting::GetConstantsBufferShared(
    const std::uint32_t frameIndex) const
{
    return m_frames.at(frameIndex).constants;
}

RHI::IBuffer&
ClusteredLighting::GetLightBuffer(
    const std::uint32_t frameIndex) const
{
    return *m_frames.at(frameIndex).lights;
}

std::shared_ptr<RHI::IBuffer>
ClusteredLighting::GetLightBufferShared(
    const std::uint32_t frameIndex) const
{
    return m_frames.at(frameIndex).lights;
}

RHI::IBuffer&
ClusteredLighting::GetClusterCountBuffer(
    const std::uint32_t frameIndex) const
{
    return *m_frames.at(frameIndex).clusterCounts;
}

std::shared_ptr<RHI::IBuffer>
ClusteredLighting::GetClusterCountBufferShared(
    const std::uint32_t frameIndex) const
{
    return m_frames.at(frameIndex).clusterCounts;
}

RHI::IBuffer&
ClusteredLighting::GetClusterIndexBuffer(
    const std::uint32_t frameIndex) const
{
    return *m_frames.at(frameIndex).clusterIndices;
}

std::shared_ptr<RHI::IBuffer>
ClusteredLighting::GetClusterIndexBufferShared(
    const std::uint32_t frameIndex) const
{
    return m_frames.at(frameIndex).clusterIndices;
}

RHI::ResourceState
ClusteredLighting::GetClusterCountInitialState(
    const std::uint32_t frameIndex) const
{
    return m_frames.at(frameIndex).countState;
}

RHI::ResourceState
ClusteredLighting::GetClusterIndexInitialState(
    const std::uint32_t frameIndex) const
{
    return m_frames.at(frameIndex).indexState;
}

std::uint32_t ClusteredLighting::GetClusterCount() const
{
    return m_clusterCount;
}

void ClusteredLighting::RebuildFrameResources()
{
    for (FrameResources& frame : m_frames)
    {
        RHI::BufferDescription counts{};
        counts.size =
            static_cast<std::size_t>(
                m_clusterCount)
            * sizeof(std::uint32_t);
        counts.stride = sizeof(std::uint32_t);
        counts.usage =
            RHI::BufferUsage::Storage
            | RHI::BufferUsage::CopyDestination;
        counts.memoryAccess =
            RHI::MemoryAccess::GpuOnly;
        frame.clusterCounts =
            m_device->CreateBuffer(counts);

        RHI::BufferDescription indices{};
        indices.size =
            static_cast<std::size_t>(
                m_clusterCount)
            * MaxLightsPerCluster
            * sizeof(std::uint32_t);
        indices.stride = sizeof(std::uint32_t);
        indices.usage =
            RHI::BufferUsage::Storage
            | RHI::BufferUsage::CopyDestination;
        indices.memoryAccess =
            RHI::MemoryAccess::GpuOnly;
        frame.clusterIndices =
            m_device->CreateBuffer(indices);

        frame.descriptorSet =
            m_device->CreateDescriptorSet(
                m_descriptorSetLayout);
        frame.descriptorSet->WriteBuffer(
            0,
            frame.constants);
        frame.descriptorSet->WriteBuffer(
            16,
            frame.lights);
        frame.descriptorSet->WriteBuffer(
            32,
            frame.clusterCounts);
        frame.descriptorSet->WriteBuffer(
            33,
            frame.clusterIndices);
        frame.countState =
            RHI::ResourceState::UnorderedAccess;
        frame.indexState =
            RHI::ResourceState::UnorderedAccess;
    }
}
ClusteredLightingGraphContribution
ClusteredLighting::CreateRenderGraphContribution(
    const std::uint32_t frameIndex)
{
    ClusteredLightingGraphContribution contribution{};
    contribution.clusterConstants =
        &GetConstantsBuffer(frameIndex);
    contribution.pointLights =
        &GetLightBuffer(frameIndex);
    contribution.clusterLightCounts =
        &GetClusterCountBuffer(frameIndex);
    contribution.clusterLightIndices =
        &GetClusterIndexBuffer(frameIndex);
    contribution.clusterCountInitialState =
        GetClusterCountInitialState(frameIndex);
    contribution.clusterIndexInitialState =
        GetClusterIndexInitialState(frameIndex);
    contribution.execute =
        [this, frameIndex](
            RHI::ICommandContext& commandContext,
            const RenderGraphPassResources&)
        {
            Execute(commandContext, frameIndex);
        };
    return contribution;
}

void ClusteredLighting::AddPasses(
    RenderGraph& graph,
    const BufferHandle clusterConstants,
    const BufferHandle pointLights,
    BufferHandle& clusterLightCounts,
    BufferHandle& clusterLightIndices,
    RenderGraph::ParameterExecuteCallback execute)
{
    Core::Check(
        static_cast<bool>(execute),
        "Clustered lighting requires an execute callback.");
    auto parameters = graph.CreatePassParameters();
    parameters.ReadBuffer(
        clusterConstants,
        RHI::ResourceState::ConstantBuffer);
    parameters.ReadBuffer(
        pointLights,
        RHI::ResourceState::ShaderResource);
    clusterLightCounts = parameters.WriteBuffer(
        clusterLightCounts,
        RHI::ResourceState::UnorderedAccess);
    clusterLightIndices = parameters.WriteBuffer(
        clusterLightIndices,
        RHI::ResourceState::UnorderedAccess);
    graph.AddParameterPass(
        "ClusteredLightBuild",
        std::move(parameters),
        std::move(execute),
        RenderGraph::PassOptions{
            RenderGraph::QueueClass::Compute,
            true,
            false,
            true,
            RenderGraph::ParallelRecordingContract::AuditedIndependent()});
}
} // namespace Prism::Renderer
