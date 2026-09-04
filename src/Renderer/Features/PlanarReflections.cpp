#include "Renderer/Features/PlanarReflections.h"
#include "Renderer/Pipeline/PipelineCache.h"

#include "Asset/Material.h"
#include "Asset/Mesh.h"
#include "Asset/Texture.h"
#include "Core/Assert.h"
#include "Renderer/MaterialParameterResolver.h"
#include "RHI/IGraphicsDevice.h"
#include "RHI/ShaderLayoutBuilder.h"
#include "Scene/RenderScene.h"
#include "Scene/RenderSceneView.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <utility>

namespace Prism::Renderer
{
using namespace DirectX;

void PlanarReflections::Initialize(
    RHI::IGraphicsDevice& device,
    Asset::ShaderManager& shaderManager,
    PipelineCache& pipelineCache,
    const std::filesystem::path& shaderPath,
    const RHI::ShaderBinaryFormat shaderFormat,
    const std::uint32_t framesInFlight,
    const std::uint32_t maxRenderObjects)
{
    Core::Check(
        framesInFlight > 0 && maxRenderObjects > 0,
        "Planar reflections require frame and object capacity.");
    m_device = &device;
    m_maxRenderObjects = maxRenderObjects;
    m_createdInitialState =
        RHI::ResourceState::Undefined;

    const RHI::ShaderBinary& vertexShader =
        shaderManager.LoadShader(
            shaderPath,
            "PlanarReflectionVS",
            RHI::ShaderStage::Vertex,
            shaderFormat);
    const RHI::ShaderBinary& pixelShader =
        shaderManager.LoadShader(
            shaderPath,
            "PlanarReflectionPS",
            RHI::ShaderStage::Pixel,
            shaderFormat);
    const std::array stages = {
        RHI::ShaderLayoutStage{
            &vertexShader.reflection,
            RHI::ShaderStage::Vertex},
        RHI::ShaderLayoutStage{
            &pixelShader.reflection,
            RHI::ShaderStage::Pixel}};
    const std::array<std::uint32_t, 2> dynamicBindings = {1u, 2u};
    m_layout = device.CreateDescriptorSetLayout(
        RHI::BuildDescriptorSetLayout(stages, dynamicBindings));

    RHI::GraphicsPipelineDescription pipeline{};
    pipeline.vertexShader = vertexShader;
    pipeline.pixelShader = pixelShader;
    pipeline.descriptorSetLayout = m_layout;
    pipeline.vertexBindings = {{
        0,
        sizeof(Asset::MeshVertex),
        RHI::VertexInputRate::PerVertex}};
    pipeline.vertexAttributes = {
        {0, 0, RHI::VertexElementFormat::Float3,
         static_cast<std::uint32_t>(offsetof(Asset::MeshVertex, position))},
        {1, 0, RHI::VertexElementFormat::Float4,
         static_cast<std::uint32_t>(offsetof(Asset::MeshVertex, color))},
        {2, 0, RHI::VertexElementFormat::Float3,
         static_cast<std::uint32_t>(offsetof(Asset::MeshVertex, normal))},
        {3, 0, RHI::VertexElementFormat::Float2,
         static_cast<std::uint32_t>(offsetof(Asset::MeshVertex, texCoord))}};
    pipeline.rasterizer.cullMode = RHI::CullMode::None;
    pipeline.depthStencil.depthTestEnabled = true;
    pipeline.depthStencil.depthWriteEnabled = true;
    pipeline.depthStencil.depthComparison = RHI::CompareOperation::Less;
    pipeline.colorFormats = {RHI::Format::Rgba16Float};
    pipeline.depthFormat = RHI::Format::D32Float;
    m_pipeline = pipelineCache.GetOrCreateGraphics(
        device,
        "Feature.PlanarReflections",
        pipeline);

    RHI::SamplerDescription sampler{};
    sampler.filter = RHI::Filter::Linear;
    sampler.addressU = RHI::AddressMode::Repeat;
    sampler.addressV = RHI::AddressMode::Repeat;
    sampler.addressW = RHI::AddressMode::Repeat;
    m_sampler = device.CreateSampler(sampler);

    m_frames.resize(framesInFlight);
    for (FrameResources& frame : m_frames)
    {
        RHI::BufferDescription frameDescription{};
        frameDescription.size = ConstantStride;
        frameDescription.usage = RHI::BufferUsage::Constant;
        frameDescription.memoryAccess = RHI::MemoryAccess::CpuToGpu;
        frame.frameConstants = device.CreateBuffer(frameDescription);

        RHI::BufferDescription objectDescription = frameDescription;
        objectDescription.size =
            static_cast<std::size_t>(ConstantStride) * maxRenderObjects;
        frame.objectConstants = device.CreateBuffer(objectDescription);
        frame.materialConstants = device.CreateBuffer(objectDescription);
    }
}

void PlanarReflections::Resize(
    const std::uint32_t width,
    const std::uint32_t height)
{
    Core::Check(
        m_device != nullptr && width > 0 && height > 0,
        "Planar reflection resize requires a device and extent.");
    m_width = std::max(1u, width / 2u);
    m_height = std::max(1u, height / 2u);

    RHI::TextureDescription color{};
    color.width = m_width;
    color.height = m_height;
    color.format = RHI::Format::Rgba16Float;
    color.usage = RHI::TextureUsage::RenderTarget
        | RHI::TextureUsage::ShaderResource;
    m_colorTexture = m_device->CreateTexture(color);

    RHI::TextureDescription depth{};
    depth.width = m_width;
    depth.height = m_height;
    depth.format = RHI::Format::D32Float;
    depth.usage = RHI::TextureUsage::DepthStencil;
    m_depthTexture = m_device->CreateTexture(depth);

    RHI::TextureViewDescription renderTarget{};
    renderTarget.type = RHI::TextureViewType::RenderTarget;
    m_colorTargetView = m_device->CreateTextureView(
        m_colorTexture,
        renderTarget);
    RHI::TextureViewDescription sampled{};
    sampled.type = RHI::TextureViewType::Sampled;
    m_colorSampledView = m_device->CreateTextureView(
        m_colorTexture,
        sampled);
    RHI::TextureViewDescription depthView{};
    depthView.type = RHI::TextureViewType::DepthStencil;
    m_depthView = m_device->CreateTextureView(
        m_depthTexture,
        depthView);
    m_colorState = m_createdInitialState;
    m_depthState = RHI::ResourceState::Undefined;
}

void PlanarReflections::Update(
    const Scene::RenderSceneView& scene,
    const std::uint32_t frameIndex,
    const float planeHeight,
    const bool enabled,
    const XMFLOAT3& clearColor)
{
    Core::Check(
        frameIndex < m_frames.size(),
        "Planar reflection update uses an invalid frame.");
    EnsureSceneDescriptors(scene);
    m_planeHeight = planeHeight;
    m_enabled = enabled;
    m_clearColor = clearColor;

    const Scene::Camera& camera = scene.GetCamera();
    XMFLOAT3 reflectedPosition = camera.GetPosition();
    reflectedPosition.y = 2.0f * planeHeight - reflectedPosition.y;
    XMFLOAT3 reflectedForward = camera.GetForwardVector();
    reflectedForward.y = -reflectedForward.y;
    XMFLOAT3 reflectedUp = camera.GetUpVector();
    reflectedUp.y = -reflectedUp.y;
    const XMMATRIX reflectedView = XMMatrixLookToLH(
        XMLoadFloat3(&reflectedPosition),
        XMVector3Normalize(XMLoadFloat3(&reflectedForward)),
        XMVector3Normalize(XMLoadFloat3(&reflectedUp)));
    m_reflectedViewProjection =
        reflectedView * camera.GetProjectionMatrix();

    FrameConstants frameConstants{};
    XMStoreFloat4x4(
        &frameConstants.reflectedViewProjection,
        XMMatrixTranspose(m_reflectedViewProjection));
    frameConstants.cameraPosition = reflectedPosition;
    frameConstants.planeHeight = planeHeight;
    frameConstants.lightDirection =
        scene.GetDirectionalLight().direction;
    frameConstants.lightIntensity =
        scene.GetDirectionalLight().intensity;
    frameConstants.lightColor =
        scene.GetDirectionalLight().color;
    frameConstants.enabled = enabled ? 1.0f : 0.0f;
    m_frames[frameIndex].frameConstants->Update(
        &frameConstants,
        sizeof(frameConstants));

    const auto& objects = scene.GetRenderObjects();
    const std::uint32_t objectCount = std::min<std::uint32_t>(
        static_cast<std::uint32_t>(objects.size()),
        m_maxRenderObjects);
    for (std::uint32_t objectIndex = 0;
         objectIndex < objectCount;
         ++objectIndex)
    {
        ObjectConstants objectConstants{};
        XMStoreFloat4x4(
            &objectConstants.world,
            XMMatrixTranspose(
                objects[objectIndex].transform.GetWorldMatrix()));
        m_frames[frameIndex].objectConstants->Update(
            &objectConstants,
            sizeof(objectConstants),
            static_cast<std::size_t>(objectIndex) * ConstantStride);
        const SharedMaterialConstants materialConstants =
            ResolveMaterialConstants(scene, objectIndex);
        m_frames[frameIndex].materialConstants->Update(
            &materialConstants,
            sizeof(materialConstants),
            static_cast<std::size_t>(objectIndex) * ConstantStride);
    }
}

void PlanarReflections::Execute(
    RHI::ICommandContext& commandContext,
    const std::uint32_t frameIndex,
    const Scene::RenderSceneView& scene) const
{
    Core::Check(
        frameIndex < m_frames.size()
            && m_colorTargetView != nullptr
            && m_depthView != nullptr,
        "Planar reflection execution uses invalid resources.");
    RHI::RenderingInfo rendering{};
    rendering.width = m_width;
    rendering.height = m_height;
    RHI::RenderingAttachment color{};
    color.view = m_colorTargetView.get();
    color.loadOperation = RHI::LoadOperation::Clear;
    color.storeOperation = RHI::StoreOperation::Store;
    color.clearColor = {
        m_clearColor.x,
        m_clearColor.y,
        m_clearColor.z,
        1.0f};
    color.stateBefore = RHI::ResourceState::RenderTarget;
    color.stateAfter = RHI::ResourceState::RenderTarget;
    rendering.colorAttachments.push_back(color);
    RHI::RenderingAttachment depth{};
    depth.view = m_depthView.get();
    depth.loadOperation = RHI::LoadOperation::Clear;
    depth.storeOperation = RHI::StoreOperation::Store;
    depth.clearDepthStencil.depth = 1.0f;
    depth.stateBefore = RHI::ResourceState::DepthWrite;
    depth.stateAfter = RHI::ResourceState::DepthWrite;
    rendering.depthAttachment = depth;
    commandContext.BeginRendering(rendering);
    commandContext.BindGraphicsPipeline(*m_pipeline);

    const auto& objects = scene.GetRenderObjects();
    const std::uint32_t objectCount = std::min<std::uint32_t>(
        static_cast<std::uint32_t>(objects.size()),
        m_maxRenderObjects);
    for (std::uint32_t objectIndex = 0;
         objectIndex < objectCount;
         ++objectIndex)
    {
        const Scene::RenderObject& object = objects[objectIndex];
        const Asset::Mesh* const mesh = scene.GetMesh(objectIndex);
        if (!scene.IsObjectSelected(objectIndex)
            || !object.visible
            || mesh == nullptr
            || (scene.FindMaterialBinding(objectIndex) == nullptr
                && object.material == nullptr))
        {
            continue;
        }
        commandContext.BindDescriptorSet(
            *m_frames[frameIndex].descriptorSets[objectIndex],
            std::array<RHI::DynamicBufferOffset, 2>{
                RHI::DynamicBufferOffset{
                    1,
                    objectIndex * ConstantStride},
                RHI::DynamicBufferOffset{
                    2,
                    objectIndex * ConstantStride}});
        mesh->Draw(commandContext);
    }
    commandContext.EndRendering();
}

void PlanarReflections::EndFrame()
{
    if (m_enabled)
    {
        m_colorState = RHI::ResourceState::ShaderResource;
        m_depthState = RHI::ResourceState::DepthWrite;
    }
}

RHI::ITexture& PlanarReflections::GetColorTexture() const
{
    return *m_colorTexture;
}

RHI::ITexture& PlanarReflections::GetDepthTexture() const
{
    return *m_depthTexture;
}

std::shared_ptr<RHI::ITexture>
PlanarReflections::GetColorTextureShared() const
{
    return m_colorTexture;
}

std::shared_ptr<RHI::ITextureView>
PlanarReflections::GetColorSampledView() const
{
    return m_colorSampledView;
}

RHI::ResourceState PlanarReflections::GetColorInitialState() const
{
    return m_colorState;
}

RHI::ResourceState PlanarReflections::GetDepthInitialState() const
{
    return m_depthState;
}

const XMMATRIX&
PlanarReflections::GetReflectedViewProjection() const
{
    return m_reflectedViewProjection;
}

float PlanarReflections::GetPlaneHeight() const
{
    return m_planeHeight;
}

void PlanarReflections::EnsureSceneDescriptors(
    const Scene::RenderSceneView& scene)
{
    const auto& objects = scene.GetRenderObjects();
    Core::Check(
        objects.size() <= m_maxRenderObjects,
        "Planar reflections exceeded the render object capacity.");
    bool changed = m_materialIdentities.size() != objects.size();
    if (!changed)
    {
        for (std::size_t index = 0; index < objects.size(); ++index)
        {
            const Scene::RenderSceneMaterialBinding* const material =
                scene.FindMaterialBinding(index);
            const std::uint64_t materialIdentity = material != nullptr
                ? material->revision.value
                : static_cast<std::uint64_t>(
                      reinterpret_cast<std::uintptr_t>(
                          objects[index].material.get()));
            if (m_materialIdentities[index]
                != materialIdentity)
            {
                changed = true;
                break;
            }
        }
    }
    if (!changed)
    {
        return;
    }

    m_materialIdentities.resize(objects.size());
    for (FrameResources& frame : m_frames)
    {
        frame.descriptorSets.clear();
        frame.descriptorSets.resize(objects.size());
    }
    for (std::size_t objectIndex = 0;
         objectIndex < objects.size();
         ++objectIndex)
    {
        const Scene::RenderObject& object = objects[objectIndex];
        const Scene::RenderSceneMaterialBinding* const material =
            scene.FindMaterialBinding(objectIndex);
        const Asset::Texture* const albedo = scene.GetMaterialTexture(
            objectIndex, Scene::MaterialTextureSlot::Albedo);
        Core::Check(
            (material != nullptr || object.material != nullptr)
                && albedo != nullptr
                && albedo->GetRhiTexture() != nullptr,
            "Planar reflection materials require an albedo texture.");
        m_materialIdentities[objectIndex] = material != nullptr
            ? material->revision.value
            : static_cast<std::uint64_t>(
                  reinterpret_cast<std::uintptr_t>(
                      object.material.get()));
        for (FrameResources& frame : m_frames)
        {
            std::shared_ptr<RHI::IDescriptorSet> descriptorSet =
                m_device->CreateDescriptorSet(m_layout);
            descriptorSet->WriteBuffer(0, frame.frameConstants);
            descriptorSet->WriteBuffer(
                1,
                frame.objectConstants,
                0,
                sizeof(ObjectConstants));
            descriptorSet->WriteBuffer(
                2,
                frame.materialConstants,
                0,
                sizeof(SharedMaterialConstants));
            descriptorSet->WriteTexture(
                16,
                albedo->GetRhiTexture());
            descriptorSet->WriteSampler(48, m_sampler);
            frame.descriptorSets[objectIndex] =
                std::move(descriptorSet);
        }
    }
}
PlanarReflectionsGraphContribution
PlanarReflections::CreateRenderGraphContribution(
    const std::uint32_t frameIndex,
    std::shared_ptr<const Scene::RenderSceneView> scene)
{
    Core::Check(scene != nullptr,
        "Planar reflections require a retained scene view.");
    PlanarReflectionsGraphContribution contribution{};
    contribution.color =
        &GetColorTexture();
    contribution.depth =
        &GetDepthTexture();
    contribution.colorInitialState =
        GetColorInitialState();
    contribution.depthInitialState =
        GetDepthInitialState();
    contribution.execute =
        [this, frameIndex, scene = std::move(scene)](
            RHI::ICommandContext& commandContext,
            const RenderGraphPassResources&)
        {
            Execute(
                commandContext,
                frameIndex,
                *scene);
        };
    return contribution;
}

void PlanarReflections::AddPasses(
    RenderGraph& graph,
    TextureHandle& color,
    TextureHandle& depth,
    RenderGraph::ParameterExecuteCallback execute,
    const TextureHandle environment)
{
    Core::Check(
        static_cast<bool>(execute),
        "Planar reflections require an execute callback.");
    auto parameters = graph.CreatePassParameters();
    if (environment.IsValid())
        parameters.ReadTexture(environment, RHI::ResourceState::ShaderResource);
    color = parameters.WriteTexture(
        color,
        RHI::ResourceState::RenderTarget);
    depth = parameters.WriteTexture(
        depth,
        RHI::ResourceState::DepthWrite);
    graph.AddParameterPass(
        "PlanarReflection",
        std::move(parameters),
        std::move(execute));
}
} // namespace Prism::Renderer
