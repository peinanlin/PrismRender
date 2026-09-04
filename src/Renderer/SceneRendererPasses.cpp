#include "Renderer/SceneRenderer.h"

#include "Asset/Mesh.h"
#include "Core/Assert.h"
#include "RHI/ICommandContext.h"
#include "Renderer/MaterialParameterResolver.h"
#include "Renderer/RenderCapture.h"
#include "Scene/RenderScene.h"
#include "Scene/RenderSceneView.h"

#include <algorithm>
#include <array>
#include <limits>
#include <span>
#include <utility>
#include <vector>

namespace Prism::Renderer
{
namespace
{
float GetCameraRelativeBoundsDistanceSquared(
    const Scene::RenderObject& object,
    const Asset::Mesh& mesh,
    const Scene::Camera& camera)
{
    DirectX::XMFLOAT3 relativeCenter{};
    DirectX::XMStoreFloat3(
        &relativeCenter,
        DirectX::XMVector3TransformCoord(
            DirectX::XMLoadFloat3(
                &mesh.GetBoundsCenter()),
            object.transform.GetRelativeWorldMatrix(
                camera.GetWorldPosition())));
    return relativeCenter.x * relativeCenter.x
        + relativeCenter.y * relativeCenter.y
        + relativeCenter.z * relativeCenter.z;
}
} // namespace

std::vector<IndexedGeometryDraw>
SceneRenderer::BuildOpaqueGeometryDraws(
    const Scene::RenderSceneView& scene,
    const RHI::IGraphicsPipeline& doubleSidedPipeline,
    const RHI::IGraphicsPipeline* const oceanPipeline,
    const RHI::IGraphicsPipeline* const oceanDoubleSidedPipeline,
    const RHI::IGraphicsPipeline* const oceanWireframePipeline,
    const RHI::IGraphicsPipeline* const oceanWireframeDoubleSidedPipeline,
    const RHI::IGraphicsPipeline* const oceanTessellationPipeline,
    const RHI::IGraphicsPipeline* const oceanTessellationDoubleSidedPipeline,
    const RHI::IGraphicsPipeline* const oceanTessellationWireframePipeline,
    const OpaqueGeometrySelection selection) const
{
    const std::vector<Scene::RenderObject>& objects =
        scene.GetRenderObjects();
    std::vector<IndexedGeometryDraw> draws;
    const bool useInstancing =
        m_settings.gpuInstancingEnabled
        && m_indexedObjectDrawingSupported
        && !m_gpuDrivenActiveThisFrame;
    draws.reserve(
        useInstancing
        ? m_instanceBatches.size()
        : objects.size());
    const auto appendDraw =
        [&](const std::size_t objectIndex,
            const std::uint32_t instanceCount,
            const OceanAdaptiveDrawGroup* const adaptiveGroup = nullptr)
        {
            const Scene::RenderObject& object =
                objects[objectIndex];
            const Asset::Mesh* const mesh = scene.GetMesh(objectIndex);
            if (!scene.IsObjectSelected(objectIndex) || mesh == nullptr)
            {
                return;
            }
            const bool oceanObject = object.surfaceType
                == Scene::RenderSurfaceType::FftOcean;
            if ((selection == OpaqueGeometrySelection::ExcludeOcean
                    && oceanObject)
                || (selection == OpaqueGeometrySelection::OceanOnly
                    && !oceanObject))
            {
                return;
            }
            if (m_oceanTessellationActive && oceanObject
                && adaptiveGroup == nullptr)
            {
                return;
            }
            Core::Check(
                objectIndex * ObjectConstantStride
                    <= std::numeric_limits<std::uint32_t>::max(),
                "An object dynamic offset exceeds the RHI offset range.");
            IndexedGeometryDraw draw{};
            if (IsDoubleSidedMaterial(scene, objectIndex))
            {
                draw.pipelineOverride =
                    &doubleSidedPipeline;
            }
            if (m_settings.ocean.implementation
                    == OceanImplementation::SpectralOcean
                && object.surfaceType == Scene::RenderSurfaceType::FftOcean
                && oceanPipeline != nullptr)
            {
                const bool oceanWireframe =
                    m_settings.ocean.debug.wireframe
                    || m_settings.viewportShadingMode
                        == ViewportShadingMode::Wireframe;
                const bool useTessellation =
                    m_oceanTessellationActive
                    && m_settings.ocean.geometry.preferTessellation
                    && oceanTessellationWireframePipeline != nullptr;
                if (oceanWireframe && useTessellation)
                {
                    draw.pipelineOverride = oceanTessellationWireframePipeline;
                }
                else if (oceanWireframe && oceanWireframePipeline != nullptr)
                {
                    draw.pipelineOverride =
                        IsDoubleSidedMaterial(scene, objectIndex)
                        && oceanWireframeDoubleSidedPipeline != nullptr
                        ? oceanWireframeDoubleSidedPipeline
                        : oceanWireframePipeline;
                }
                else
                {
                    const bool useSolidTessellation =
                        m_oceanTessellationActive
                        && m_settings.ocean.geometry.preferTessellation
                        && oceanTessellationPipeline != nullptr;
                    if (useSolidTessellation)
                    {
                        draw.pipelineOverride =
                            IsDoubleSidedMaterial(scene, objectIndex)
                            && oceanTessellationDoubleSidedPipeline != nullptr
                            ? oceanTessellationDoubleSidedPipeline
                            : oceanTessellationPipeline;
                    }
                    else
                    {
                        draw.pipelineOverride =
                            IsDoubleSidedMaterial(scene, objectIndex)
                            && oceanDoubleSidedPipeline != nullptr
                            ? oceanDoubleSidedPipeline
                            : oceanPipeline;
                    }
                }
            }
            draw.descriptorSet =
                m_objectResources[objectIndex]
                    .materialResources
                    ->descriptorSets[m_activeFrame]
                    .get();
            if (m_settings.ocean.implementation
                    == OceanImplementation::SpectralOcean
                && oceanObject)
            {
                draw.descriptorSet =
                    m_objectResources[objectIndex]
                        .materialResources
                        ->oceanDescriptorSets[m_activeFrame]
                        .get();
            }
            draw.vertexBuffer = adaptiveGroup != nullptr
                ? m_oceanSurfaceRenderer.GetVertexBuffer().get()
                : mesh->GetRhiVertexBuffer().get();
            draw.indexBuffer = adaptiveGroup != nullptr
                ? adaptiveGroup->indexBuffer.get()
                : mesh->GetRhiIndexBuffer().get();
            draw.indexFormat = adaptiveGroup != nullptr
                ? RHI::IndexFormat::UInt32
                : RHI::IndexFormat::UInt16;
            draw.indexCount = adaptiveGroup != nullptr
                ? adaptiveGroup->indexCount
                : mesh->GetRhiIndexCount();
            draw.instanceCount = instanceCount;
            // Object indexing is based explicitly through ObjectConstants.
            // StartInstanceLocation is not part of SV_InstanceID on D3D12.
            draw.firstInstance = adaptiveGroup != nullptr
                ? adaptiveGroup->firstInstance : 0u;
            if (m_gpuDrivenActiveThisFrame && adaptiveGroup == nullptr)
            {
                const GpuDrawBatch& batch =
                    m_gpuDrivenVisibility
                        .GetDrawBatchForObject(
                            objectIndex);
                draw.indirectArguments =
                    &m_gpuDrivenVisibility.GetArgumentBuffer(
                        m_activeFrame);
                draw.indirectArgumentOffset =
                    batch.firstArgument
                    * GpuDrivenVisibility::GetArgumentStride();
                draw.indirectMaxDrawCount =
                    batch.maxDrawCount;
                draw.indirectCountBuffer =
                    &m_gpuDrivenVisibility.GetCountBuffer(
                        m_activeFrame);
                const auto& batches =
                    m_gpuDrivenVisibility.GetDrawBatches();
                draw.indirectCountOffset =
                    static_cast<std::size_t>(
                        &batch - batches.data())
                    * sizeof(std::uint32_t);
            }
            draw.dynamicBufferOffsets.push_back({
                1,
                static_cast<std::uint32_t>(
                    objectIndex
                    * ObjectConstantStride)});
            draws.push_back(std::move(draw));
        };
    const auto appendAdaptiveOceanDraws = [&]()
    {
        if (!m_oceanTessellationActive || !m_oceanSurfaceRenderer.IsReady())
            return;
        std::size_t objectIndex = objects.size();
        for (std::size_t index = 0; index < objects.size(); ++index)
        {
            if (scene.IsObjectSelected(index)
                && objects[index].visible
                && objects[index].surfaceType
                    == Scene::RenderSurfaceType::FftOcean)
            {
                objectIndex = index;
                break;
            }
        }
        if (objectIndex == objects.size())
            return;
        const OceanAdaptiveDrawGroup& group =
            m_oceanSurfaceRenderer.GetUnifiedDrawGroup();
        appendDraw(objectIndex, group.instanceCount, &group);
    };
    if (useInstancing)
    {
        for (const InstanceBatch& batch :
             m_instanceBatches)
        {
            appendDraw(
                batch.representativeObjectIndex,
                batch.instanceCount);
        }
        appendAdaptiveOceanDraws();
        return draws;
    }
    if (m_gpuDrivenActiveThisFrame)
    {
        for (const GpuDrawBatch& batch :
             m_gpuDrivenVisibility.GetDrawBatches())
        {
            const std::size_t objectIndex =
                batch.representativeObjectIndex;
            Core::Check(
                objectIndex < objects.size(),
                "A GPU draw batch references an object outside the active render scene.");
            const Scene::RenderObject& object =
                objects[objectIndex];
            if (!scene.IsObjectSelected(objectIndex)
                || !object.visible
                || object.surfaceType
                    == Scene::RenderSurfaceType::EditorDebugLine
                || ResolveMaterialRenderQueue(scene, objectIndex)
                    == MaterialRenderQueue::Transparent)
            {
                continue;
            }
            appendDraw(
                objectIndex,
                1u);
        }
        appendAdaptiveOceanDraws();
        return draws;
    }
    for (std::size_t objectIndex = 0;
         objectIndex < objects.size();
         ++objectIndex)
    {
        const Scene::RenderObject& object =
            objects[objectIndex];
        if (!scene.IsObjectSelected(objectIndex)
            || !object.visible
            || object.surfaceType
                == Scene::RenderSurfaceType::EditorDebugLine
            || ResolveMaterialRenderQueue(scene, objectIndex)
                == MaterialRenderQueue::Transparent)
        {
            continue;
        }
        appendDraw(
            objectIndex,
            1u);
    }
    appendAdaptiveOceanDraws();
    return draws;
}

void SceneRenderer::RenderGBufferPass(
    RHI::ICommandContext& commandContext,
    const RHI::ITextureView& depthView,
    const std::uint32_t width,
    const std::uint32_t height,
    const Scene::RenderSceneView& scene)
{
    RHI::RenderingInfo renderingInfo{};
    renderingInfo.width = width;
    renderingInfo.height = height;
    for (const std::shared_ptr<RHI::ITextureView>& view
         : m_gbufferRenderTargetViews)
    {
        RHI::RenderingAttachment attachment{};
        attachment.view = view.get();
        attachment.loadOperation = RHI::LoadOperation::Clear;
        attachment.storeOperation = RHI::StoreOperation::Store;
        attachment.clearColor = {0.0f, 0.0f, 0.0f, 0.0f};
        attachment.stateBefore = RHI::ResourceState::RenderTarget;
        attachment.stateAfter = RHI::ResourceState::RenderTarget;
        renderingInfo.colorAttachments.push_back(attachment);
    }
    RHI::RenderingAttachment motionAttachment{};
    motionAttachment.view =
        &m_temporalAntiAliasing.GetMotionVectorRenderTargetView();
    motionAttachment.loadOperation = RHI::LoadOperation::Clear;
    motionAttachment.storeOperation = RHI::StoreOperation::Store;
    motionAttachment.clearColor = {0.0f, 0.0f, 0.0f, 0.0f};
    motionAttachment.stateBefore = RHI::ResourceState::RenderTarget;
    motionAttachment.stateAfter = RHI::ResourceState::RenderTarget;
    renderingInfo.colorAttachments.push_back(motionAttachment);

    RHI::RenderingAttachment depthAttachment{};
    depthAttachment.view = &depthView;
    depthAttachment.loadOperation = RHI::LoadOperation::Clear;
    depthAttachment.storeOperation = RHI::StoreOperation::Store;
    depthAttachment.clearDepthStencil.depth = 1.0f;
    depthAttachment.stateBefore = RHI::ResourceState::DepthWrite;
    depthAttachment.stateAfter = RHI::ResourceState::DepthWrite;
    renderingInfo.depthAttachment = depthAttachment;

    const std::vector<Scene::RenderObject>& objects =
        scene.GetRenderObjects();
    const bool wireframe =
        m_settings.viewportShadingMode
            == ViewportShadingMode::Wireframe
        || (m_settings.ocean.implementation
                == OceanImplementation::SpectralOcean
            && m_settings.ocean.debug.wireframe);
    const RHI::IGraphicsPipeline& geometryPipeline =
        wireframe ? *m_gbufferWireframePipeline
                  : *m_gbufferPipeline;
    const RHI::IGraphicsPipeline& doubleSidedPipeline =
        wireframe ? *m_gbufferWireframePipeline
                  : *m_gbufferDoubleSidedPipeline;
    const std::vector<IndexedGeometryDraw> draws =
        BuildOpaqueGeometryDraws(
            scene,
            doubleSidedPipeline,
            m_settings.ocean.implementation
                == OceanImplementation::SpectralOcean
                ? m_oceanGBufferPipeline.get()
                : nullptr,
            m_settings.ocean.implementation
                == OceanImplementation::SpectralOcean
                ? m_oceanGBufferDoubleSidedPipeline.get()
                : nullptr,
            m_settings.ocean.implementation
                == OceanImplementation::SpectralOcean
                ? m_oceanGBufferWireframePipeline.get()
                : nullptr,
            m_settings.ocean.implementation
                == OceanImplementation::SpectralOcean
                ? m_oceanGBufferWireframePipeline.get()
                : nullptr,
            m_settings.ocean.implementation
                == OceanImplementation::SpectralOcean
                ? m_oceanGBufferTessellationPipeline.get()
                : nullptr,
            m_settings.ocean.implementation
                == OceanImplementation::SpectralOcean
                ? m_oceanGBufferTessellationDoubleSidedPipeline.get()
                : nullptr,
             m_settings.ocean.implementation
                 == OceanImplementation::SpectralOcean
                 ? m_oceanGBufferTessellationWireframePipeline.get()
                 : nullptr,
             m_settings.ocean.opticsModel == OceanOpticsModel::HpWater
                 ? OpaqueGeometrySelection::ExcludeOcean
                 : OpaqueGeometrySelection::All);
    m_geometryRenderingPass.Execute(
        commandContext,
        renderingInfo,
        geometryPipeline,
        draws);

    std::vector<IndexedGeometryDraw> debugLineDraws;
    for (std::size_t objectIndex = 0;
         objectIndex < objects.size();
         ++objectIndex)
    {
        const Scene::RenderObject& object = objects[objectIndex];
        const Asset::Mesh* const mesh = scene.GetMesh(objectIndex);
        if (!scene.IsObjectSelected(objectIndex)
            || !object.visible
            || mesh == nullptr
            || object.surfaceType
                != Scene::RenderSurfaceType::EditorDebugLine)
        {
            continue;
        }
        IndexedGeometryDraw draw{};
        draw.descriptorSet = m_objectResources[objectIndex]
                                 .materialResources
                                 ->descriptorSets[m_activeFrame]
                                 .get();
        draw.vertexBuffer = mesh->GetRhiVertexBuffer().get();
        draw.indexBuffer = mesh->GetRhiIndexBuffer().get();
        draw.indexFormat = RHI::IndexFormat::UInt16;
        draw.indexCount = mesh->GetRhiIndexCount();
        draw.firstInstance = 0;
        if (m_gpuDrivenActiveThisFrame)
        {
            const GpuDrawBatch& batch =
                m_gpuDrivenVisibility
                    .GetDrawBatchForObject(
                        objectIndex);
            draw.indirectArguments =
                &m_gpuDrivenVisibility.GetArgumentBuffer(m_activeFrame);
            draw.indirectArgumentOffset =
                batch.firstArgument
                * GpuDrivenVisibility::GetArgumentStride();
            draw.indirectMaxDrawCount =
                batch.maxDrawCount;
            draw.indirectCountBuffer =
                &m_gpuDrivenVisibility.GetCountBuffer(
                    m_activeFrame);
            const auto& batches =
                m_gpuDrivenVisibility.GetDrawBatches();
            draw.indirectCountOffset =
                static_cast<std::size_t>(
                    &batch - batches.data())
                * sizeof(std::uint32_t);
        }
        draw.dynamicBufferOffsets.push_back({
            1,
            static_cast<std::uint32_t>(
                objectIndex * ObjectConstantStride)});
        debugLineDraws.push_back(std::move(draw));
    }
    if (!debugLineDraws.empty())
    {
        RHI::RenderingInfo lineRenderingInfo = renderingInfo;
        for (RHI::RenderingAttachment& attachment :
             lineRenderingInfo.colorAttachments)
        {
            attachment.loadOperation = RHI::LoadOperation::Load;
        }
        lineRenderingInfo.depthAttachment->loadOperation =
            RHI::LoadOperation::Load;
        m_geometryRenderingPass.Execute(
            commandContext,
            lineRenderingInfo,
            *m_gbufferDebugLinePipeline,
            debugLineDraws);
    }
}

void SceneRenderer::RenderWaterDepthCopyPass(
    RHI::ICommandContext& commandContext,
    const std::uint32_t width,
    const std::uint32_t height)
{
    m_waterOpticsFeature.ExecuteDepthCopy(
        commandContext, width, height);
}

void SceneRenderer::RenderWaterVisibilityPass(
    RHI::ICommandContext& commandContext,
    const std::uint32_t width,
    const std::uint32_t height,
    const Scene::RenderSceneView& scene)
{
    Core::Check(m_waterOpticsFeature.HasResources(),
        "Water visibility resources are unavailable.");
    RHI::RenderingInfo renderingInfo{};
    renderingInfo.width = width;
    renderingInfo.height = height;
    for (const std::shared_ptr<RHI::ITextureView>& view :
         m_waterOpticsFeature.GBufferRenderTargetViews())
    {
        Core::Check(view != nullptr,
            "Water visibility GBuffer view is unavailable.");
        RHI::RenderingAttachment attachment{};
        attachment.view = view.get();
        attachment.loadOperation = RHI::LoadOperation::Clear;
        attachment.storeOperation = RHI::StoreOperation::Store;
        attachment.clearColor = {0.0f, 0.0f, 0.0f, 0.0f};
        attachment.stateBefore = RHI::ResourceState::RenderTarget;
        attachment.stateAfter = RHI::ResourceState::RenderTarget;
        renderingInfo.colorAttachments.push_back(attachment);
    }
    RHI::RenderingAttachment motionAttachment{};
    motionAttachment.view =
        &m_waterOpticsFeature.MotionRenderTargetView();
    motionAttachment.loadOperation = RHI::LoadOperation::Clear;
    motionAttachment.storeOperation = RHI::StoreOperation::Store;
    motionAttachment.clearColor = {0.0f, 0.0f, 0.0f, 0.0f};
    motionAttachment.stateBefore = RHI::ResourceState::RenderTarget;
    motionAttachment.stateAfter = RHI::ResourceState::RenderTarget;
    renderingInfo.colorAttachments.push_back(motionAttachment);
    RHI::RenderingAttachment depthAttachment{};
    depthAttachment.view = &m_waterOpticsFeature.CompositeDepthView();
    depthAttachment.loadOperation = RHI::LoadOperation::Load;
    depthAttachment.storeOperation = RHI::StoreOperation::Store;
    depthAttachment.stateBefore = RHI::ResourceState::DepthWrite;
    depthAttachment.stateAfter = RHI::ResourceState::DepthWrite;
    renderingInfo.depthAttachment = depthAttachment;

    const RHI::IGraphicsPipeline* tessellationPipeline =
        m_waterOpticsFeature.HasVisibilityTessellationPipeline()
        ? &m_waterOpticsFeature.VisibilityTessellationPipeline()
        : nullptr;
    const std::vector<IndexedGeometryDraw> draws =
        BuildOpaqueGeometryDraws(
            scene,
            m_waterOpticsFeature.VisibilityPipeline(),
            &m_waterOpticsFeature.VisibilityPipeline(),
            &m_waterOpticsFeature.VisibilityPipeline(),
            &m_waterOpticsFeature.VisibilityPipeline(),
            &m_waterOpticsFeature.VisibilityPipeline(),
            tessellationPipeline,
            tessellationPipeline,
            tessellationPipeline,
            OpaqueGeometrySelection::OceanOnly);
    m_geometryRenderingPass.Execute(
        commandContext,
        renderingInfo,
        m_waterOpticsFeature.VisibilityPipeline(),
        draws);
}

void SceneRenderer::RenderForwardPass(
    RHI::ICommandContext& commandContext,
    const RHI::ITextureView& depthView,
    const std::uint32_t width,
    const std::uint32_t height,
    const Scene::RenderSceneView& scene)
{
    const RHI::RenderingInfo skyRenderingInfo =
        CreateColorRenderingInfo(
            *m_hdrRenderTargetView,
            width,
            height,
            RHI::ResourceState::RenderTarget,
            RHI::ResourceState::RenderTarget);
    const std::array<FullscreenDraw, 1> skyDraws = {
        FullscreenDraw{
            m_skyPipeline.get(),
            m_skyDescriptorSets[m_activeFrame].get()}};
    m_fullscreenRenderingPass.Execute(
        commandContext,
        skyRenderingInfo,
        skyDraws);

    RHI::RenderingInfo geometryRenderingInfo{};
    geometryRenderingInfo.width = width;
    geometryRenderingInfo.height = height;
    RHI::RenderingAttachment colorAttachment{};
    colorAttachment.view = m_hdrRenderTargetView.get();
    colorAttachment.loadOperation = RHI::LoadOperation::Load;
    colorAttachment.storeOperation = RHI::StoreOperation::Store;
    colorAttachment.stateBefore = RHI::ResourceState::RenderTarget;
    colorAttachment.stateAfter = RHI::ResourceState::RenderTarget;
    geometryRenderingInfo.colorAttachments.push_back(colorAttachment);
    RHI::RenderingAttachment depthAttachment{};
    depthAttachment.view = &depthView;
    depthAttachment.loadOperation = RHI::LoadOperation::Clear;
    depthAttachment.storeOperation = RHI::StoreOperation::Store;
    depthAttachment.clearDepthStencil.depth = 1.0f;
    depthAttachment.stateBefore = RHI::ResourceState::DepthWrite;
    depthAttachment.stateAfter = RHI::ResourceState::DepthWrite;
    geometryRenderingInfo.depthAttachment = depthAttachment;

    const std::vector<Scene::RenderObject>& objects =
        scene.GetRenderObjects();
    const bool wireframe =
        m_settings.viewportShadingMode
            == ViewportShadingMode::Wireframe
        || (m_settings.ocean.implementation
                == OceanImplementation::SpectralOcean
            && m_settings.ocean.debug.wireframe);
    const RHI::IGraphicsPipeline& geometryPipeline =
        wireframe ? *m_forwardWireframePipeline
                  : *m_forwardPipeline;
    const RHI::IGraphicsPipeline& doubleSidedPipeline =
        wireframe ? *m_forwardWireframePipeline
                  : *m_forwardDoubleSidedPipeline;
    const std::vector<IndexedGeometryDraw> draws =
        BuildOpaqueGeometryDraws(
            scene,
            doubleSidedPipeline,
            m_settings.ocean.implementation
                == OceanImplementation::SpectralOcean
                ? m_oceanForwardPipeline.get()
                : nullptr,
            m_settings.ocean.implementation
                == OceanImplementation::SpectralOcean
                ? m_oceanForwardDoubleSidedPipeline.get()
                : nullptr,
            m_settings.ocean.implementation
                == OceanImplementation::SpectralOcean
                ? m_oceanForwardWireframePipeline.get()
                : nullptr,
            m_settings.ocean.implementation
                == OceanImplementation::SpectralOcean
                ? m_oceanForwardWireframePipeline.get()
                : nullptr,
            m_settings.ocean.implementation
                == OceanImplementation::SpectralOcean
                ? m_oceanForwardTessellationPipeline.get()
                : nullptr,
            m_settings.ocean.implementation
                == OceanImplementation::SpectralOcean
                ? m_oceanForwardTessellationDoubleSidedPipeline.get()
                : nullptr,
             m_settings.ocean.implementation
                 == OceanImplementation::SpectralOcean
                 ? m_oceanForwardTessellationWireframePipeline.get()
                 : nullptr,
             m_settings.ocean.opticsModel == OceanOpticsModel::HpWater
                 ? OpaqueGeometrySelection::ExcludeOcean
                 : OpaqueGeometrySelection::All);
    m_geometryRenderingPass.Execute(
        commandContext,
        geometryRenderingInfo,
        geometryPipeline,
        draws);

    std::vector<IndexedGeometryDraw> debugLineDraws;
    for (std::size_t objectIndex = 0;
         objectIndex < objects.size();
         ++objectIndex)
    {
        const Scene::RenderObject& object = objects[objectIndex];
        const Asset::Mesh* const mesh = scene.GetMesh(objectIndex);
        if (!scene.IsObjectSelected(objectIndex)
            || !object.visible
            || mesh == nullptr
            || object.surfaceType
                != Scene::RenderSurfaceType::EditorDebugLine)
        {
            continue;
        }
        IndexedGeometryDraw draw{};
        draw.descriptorSet = m_objectResources[objectIndex]
                                 .materialResources
                                 ->descriptorSets[m_activeFrame]
                                 .get();
        draw.vertexBuffer = mesh->GetRhiVertexBuffer().get();
        draw.indexBuffer = mesh->GetRhiIndexBuffer().get();
        draw.indexFormat = RHI::IndexFormat::UInt16;
        draw.indexCount = mesh->GetRhiIndexCount();
        draw.firstInstance = 0;
        if (m_gpuDrivenActiveThisFrame)
        {
            const GpuDrawBatch& batch =
                m_gpuDrivenVisibility
                    .GetDrawBatchForObject(
                        objectIndex);
            draw.indirectArguments =
                &m_gpuDrivenVisibility.GetArgumentBuffer(m_activeFrame);
            draw.indirectArgumentOffset =
                batch.firstArgument
                * GpuDrivenVisibility::GetArgumentStride();
            draw.indirectMaxDrawCount =
                batch.maxDrawCount;
            draw.indirectCountBuffer =
                &m_gpuDrivenVisibility.GetCountBuffer(
                    m_activeFrame);
            const auto& batches =
                m_gpuDrivenVisibility.GetDrawBatches();
            draw.indirectCountOffset =
                static_cast<std::size_t>(
                    &batch - batches.data())
                * sizeof(std::uint32_t);
        }
        draw.dynamicBufferOffsets.push_back({
            1,
            static_cast<std::uint32_t>(
                objectIndex * ObjectConstantStride)});
        debugLineDraws.push_back(std::move(draw));
    }
    if (!debugLineDraws.empty())
    {
        RHI::RenderingInfo lineRenderingInfo = geometryRenderingInfo;
        lineRenderingInfo.colorAttachments[0].loadOperation =
            RHI::LoadOperation::Load;
        lineRenderingInfo.depthAttachment->loadOperation =
            RHI::LoadOperation::Load;
        m_geometryRenderingPass.Execute(
            commandContext,
            lineRenderingInfo,
            *m_forwardDebugLinePipeline,
            debugLineDraws);
    }
}

void SceneRenderer::RenderTransparentPass(
    RHI::ICommandContext& commandContext,
    const RHI::ITextureView& depthView,
    const std::uint32_t width,
    const std::uint32_t height,
    const Scene::RenderSceneView& scene)
{
    RHI::RenderingInfo renderingInfo{};
    renderingInfo.width = width;
    renderingInfo.height = height;
    RHI::RenderingAttachment colorAttachment{};
    colorAttachment.view = m_hdrRenderTargetView.get();
    colorAttachment.loadOperation = RHI::LoadOperation::Load;
    colorAttachment.storeOperation = RHI::StoreOperation::Store;
    colorAttachment.stateBefore = RHI::ResourceState::RenderTarget;
    colorAttachment.stateAfter = RHI::ResourceState::RenderTarget;
    renderingInfo.colorAttachments.push_back(colorAttachment);
    RHI::RenderingAttachment depthAttachment{};
    depthAttachment.view = &depthView;
    depthAttachment.loadOperation = RHI::LoadOperation::Load;
    depthAttachment.storeOperation = RHI::StoreOperation::Store;
    depthAttachment.stateBefore = RHI::ResourceState::DepthWrite;
    depthAttachment.stateAfter = RHI::ResourceState::DepthWrite;
    renderingInfo.depthAttachment = depthAttachment;

    const std::vector<Scene::RenderObject>& objects =
        scene.GetRenderObjects();
    const bool wireframe =
        m_settings.viewportShadingMode
        == ViewportShadingMode::Wireframe;
    RHI::IGraphicsPipeline* transparentPipeline =
        wireframe ? m_transparentWireframePipeline.get()
                  : m_transparentPipeline.get();
    std::vector<std::size_t> sortedObjectIndices;
    sortedObjectIndices.reserve(objects.size());
    for (std::size_t objectIndex = 0;
         objectIndex < objects.size();
         ++objectIndex)
    {
        const Scene::RenderObject& object =
            objects[objectIndex];
        if (scene.IsObjectSelected(objectIndex)
            && object.visible
            && scene.GetMesh(objectIndex) != nullptr
            && (scene.FindMaterialBinding(objectIndex) != nullptr
                || object.material != nullptr)
            && object.surfaceType
                != Scene::RenderSurfaceType::EditorDebugLine
            && ResolveMaterialRenderQueue(scene, objectIndex)
                == MaterialRenderQueue::Transparent)
        {
            sortedObjectIndices.push_back(objectIndex);
        }
    }
    const Scene::Camera& camera = scene.GetCamera();
    std::stable_sort(
        sortedObjectIndices.begin(),
        sortedObjectIndices.end(),
        [&](const std::size_t left,
            const std::size_t right)
        {
            return GetCameraRelativeBoundsDistanceSquared(
                       objects[left],
                       *scene.GetMesh(left),
                       camera)
                > GetCameraRelativeBoundsDistanceSquared(
                       objects[right],
                       *scene.GetMesh(right),
                       camera);
        });

    std::vector<IndexedGeometryDraw> draws;
    draws.reserve(sortedObjectIndices.size());
    for (const std::size_t objectIndex :
         sortedObjectIndices)
    {
        const Asset::Mesh* const mesh = scene.GetMesh(objectIndex);
        Core::Check(
            objectIndex * ObjectConstantStride
                <= std::numeric_limits<std::uint32_t>::max(),
            "A transparent object dynamic offset exceeds the RHI offset range.");
        IndexedGeometryDraw draw{};
        if (IsDoubleSidedMaterial(scene, objectIndex))
        {
            draw.pipelineOverride =
                wireframe
                ? m_transparentWireframePipeline.get()
                : m_transparentDoubleSidedPipeline.get();
        }
        draw.descriptorSet =
            m_objectResources[objectIndex]
                .materialResources
                ->descriptorSets[m_activeFrame]
                .get();
        draw.vertexBuffer =
            mesh->GetRhiVertexBuffer().get();
        draw.indexBuffer =
            mesh->GetRhiIndexBuffer().get();
        draw.indexFormat = RHI::IndexFormat::UInt16;
        draw.indexCount =
            mesh->GetRhiIndexCount();
        draw.firstInstance = 0;
        if (m_gpuDrivenActiveThisFrame)
        {
            const GpuDrawBatch& batch =
                m_gpuDrivenVisibility
                    .GetDrawBatchForObject(
                        objectIndex);
            draw.indirectArguments =
                &m_gpuDrivenVisibility.GetArgumentBuffer(
                    m_activeFrame);
            draw.indirectArgumentOffset =
                batch.firstArgument
                * GpuDrivenVisibility::GetArgumentStride();
            draw.indirectMaxDrawCount =
                batch.maxDrawCount;
            draw.indirectCountBuffer =
                &m_gpuDrivenVisibility.GetCountBuffer(
                    m_activeFrame);
            const auto& batches =
                m_gpuDrivenVisibility.GetDrawBatches();
            draw.indirectCountOffset =
                static_cast<std::size_t>(
                    &batch - batches.data())
                * sizeof(std::uint32_t);
        }
        draw.dynamicBufferOffsets.push_back({
            1,
            static_cast<std::uint32_t>(
                objectIndex * ObjectConstantStride)});
        draws.push_back(std::move(draw));
    }
    m_geometryRenderingPass.Execute(
        commandContext,
        renderingInfo,
        *transparentPipeline,
        draws);
}

void SceneRenderer::RenderShadowPass(
    RHI::ICommandContext& commandContext,
    const Scene::RenderSceneView& scene)
{
    const std::vector<Scene::RenderObject>& objects =
        scene.GetRenderObjects();
    const std::uint32_t cascadeCount =
        m_settings.cascadeShadowsEnabled
        ? ShadowCascadeCount
        : 1u;
    for (std::uint32_t cascadeIndex = 0;
         cascadeIndex < cascadeCount;
         ++cascadeIndex)
    {
        RHI::RenderingInfo renderingInfo{};
        renderingInfo.width = ShadowMapResolution;
        renderingInfo.height = ShadowMapResolution;
        RHI::RenderingAttachment depthAttachment{};
        depthAttachment.view = m_shadowDepthViews[cascadeIndex].get();
        depthAttachment.loadOperation = RHI::LoadOperation::Clear;
        depthAttachment.storeOperation = RHI::StoreOperation::Store;
        depthAttachment.clearDepthStencil.depth = 1.0f;
        depthAttachment.stateBefore = RHI::ResourceState::DepthWrite;
        depthAttachment.stateAfter = RHI::ResourceState::DepthWrite;
        renderingInfo.depthAttachment = depthAttachment;

        std::vector<IndexedGeometryDraw> draws;
        draws.reserve(objects.size());
        for (std::size_t objectIndex = 0;
             objectIndex < objects.size();
             ++objectIndex)
        {
            const Scene::RenderObject& object = objects[objectIndex];
            const Asset::Mesh* const mesh = scene.GetMesh(objectIndex);
            if (!scene.IsObjectSelected(objectIndex)
                || !object.visible
                || mesh == nullptr
                || object.editorOnly
                || object.surfaceType
                    == Scene::RenderSurfaceType::EditorDebugLine
                || ResolveMaterialRenderQueue(scene, objectIndex)
                    == MaterialRenderQueue::Transparent)
            {
                continue;
            }
            IndexedGeometryDraw draw{};
            if (IsDoubleSidedMaterial(scene, objectIndex))
            {
                draw.pipelineOverride =
                    m_shadowDoubleSidedPipeline.get();
            }
            draw.descriptorSet =
                m_objectResources[objectIndex]
                    .materialResources
                    ->shadowDescriptorSets[m_activeFrame]
                    .get();
            draw.vertexBuffer =
                mesh->GetRhiVertexBuffer().get();
            draw.indexBuffer =
                mesh->GetRhiIndexBuffer().get();
            draw.indexFormat = RHI::IndexFormat::UInt16;
            draw.indexCount = mesh->GetRhiIndexCount();
            draw.dynamicBufferOffsets.push_back({
                1,
                static_cast<std::uint32_t>(
                    objectIndex * ObjectConstantStride)});
            draw.dynamicBufferOffsets.push_back({
                3,
                cascadeIndex
                    * static_cast<std::uint32_t>(
                        ShadowPassConstantStride)});
            draws.push_back(std::move(draw));
        }
        m_shadowPass.Execute(
            commandContext,
            renderingInfo,
            *m_shadowPipeline,
            draws);
    }
}

RHI::RenderingInfo SceneRenderer::CreateColorRenderingInfo(
    const RHI::ITextureView& target,
    const std::uint32_t width,
    const std::uint32_t height,
    const RHI::ResourceState stateBefore,
    const RHI::ResourceState stateAfter) const
{
    RHI::RenderingInfo renderingInfo{};
    renderingInfo.width = width;
    renderingInfo.height = height;
    RHI::RenderingAttachment colorAttachment{};
    colorAttachment.view = &target;
    colorAttachment.loadOperation = RHI::LoadOperation::Clear;
    colorAttachment.storeOperation = RHI::StoreOperation::Store;
    colorAttachment.clearColor = {0.0f, 0.0f, 0.0f, 1.0f};
    colorAttachment.stateBefore = stateBefore;
    colorAttachment.stateAfter = stateAfter;
    renderingInfo.colorAttachments.push_back(colorAttachment);
    return renderingInfo;
}

void SceneRenderer::RenderDeferredPass(
    RHI::ICommandContext& commandContext,
    const std::uint32_t width,
    const std::uint32_t height)
{
    const RHI::RenderingInfo renderingInfo =
        CreateColorRenderingInfo(
            *m_hdrRenderTargetView,
            width,
            height,
            RHI::ResourceState::RenderTarget,
            RHI::ResourceState::RenderTarget);
    const std::array<FullscreenDraw, 2> draws = {
        FullscreenDraw{
            m_skyPipeline.get(),
            m_skyDescriptorSets[m_activeFrame].get()},
        FullscreenDraw{
            m_deferredPipeline.get(),
            m_deferredDescriptorSets[m_activeFrame].get()}};
    m_fullscreenRenderingPass.Execute(
        commandContext,
        renderingInfo,
        draws);
}

void SceneRenderer::RenderBloomExtractPass(
    RHI::ICommandContext& commandContext)
{
    const RHI::TextureDescription& bloomDescription =
        m_bloomTextureA->GetDescription();
    commandContext.BindComputePipeline(
        *m_brightExtractComputePipeline);
    commandContext.BindDescriptorSet(
        *m_brightExtractComputeDescriptorSets[m_activeFrame]);
    const std::uint32_t workloadMultiplier =
        ReadAsyncComputeWorkloadMultiplier();
    for (std::uint32_t iteration = 0;
         iteration < workloadMultiplier;
         ++iteration)
    {
        commandContext.Dispatch(
            (bloomDescription.width + 7u) / 8u,
            (bloomDescription.height + 7u) / 8u,
            1);
    }
}

void SceneRenderer::RenderBloomHorizontalPass(
    RHI::ICommandContext& commandContext)
{
    const RHI::TextureDescription& bloomDescription =
        m_bloomTextureB->GetDescription();
    commandContext.BindComputePipeline(
        *m_blurHorizontalComputePipeline);
    commandContext.BindDescriptorSet(
        *m_blurHorizontalComputeDescriptorSets[m_activeFrame]);
    const std::uint32_t workloadMultiplier =
        ReadAsyncComputeWorkloadMultiplier();
    for (std::uint32_t iteration = 0;
         iteration < workloadMultiplier;
         ++iteration)
    {
        commandContext.Dispatch(
            (bloomDescription.width + 7u) / 8u,
            (bloomDescription.height + 7u) / 8u,
            1);
    }
}

void SceneRenderer::RenderBloomVerticalPass(
    RHI::ICommandContext& commandContext)
{
    const RHI::TextureDescription& bloomDescription =
        m_bloomTextureA->GetDescription();
    commandContext.BindComputePipeline(
        *m_blurVerticalComputePipeline);
    commandContext.BindDescriptorSet(
        *m_blurVerticalComputeDescriptorSets[m_activeFrame]);
    const std::uint32_t workloadMultiplier =
        ReadAsyncComputeWorkloadMultiplier();
    for (std::uint32_t iteration = 0;
         iteration < workloadMultiplier;
         ++iteration)
    {
        commandContext.Dispatch(
            (bloomDescription.width + 7u) / 8u,
            (bloomDescription.height + 7u) / 8u,
            1);
    }
}

void SceneRenderer::RenderHiZMip(
    RHI::ICommandContext& commandContext,
    const std::uint32_t mipIndex,
    const std::uint32_t width,
    const std::uint32_t height)
{
    Core::Check(
        !m_hiZDescriptorSets.empty()
            && m_hiZDescriptorSets.size()
                == m_hiZStorageViews.size(),
        "Hi-Z dispatches require one descriptor set per mip.");
    Core::Check(
        mipIndex < m_hiZDescriptorSets.size(),
        "Hi-Z dispatch uses an invalid mip index.");

    const std::uint32_t mipWidth =
        std::max(1u, width >> mipIndex);
    const std::uint32_t mipHeight =
        std::max(1u, height >> mipIndex);
    commandContext.BindComputePipeline(
        mipIndex == 0
            ? *m_hiZCopyPipeline
            : *m_hiZDownsamplePipeline);
    commandContext.BindDescriptorSet(
        *m_hiZDescriptorSets[mipIndex]);
    commandContext.Dispatch(
        (mipWidth + 7u) / 8u,
        (mipHeight + 7u) / 8u,
        1);
}

void SceneRenderer::RenderTonemapPass(
    RHI::ICommandContext& commandContext,
    const RHI::ITextureView& backBufferView,
    const std::uint32_t width,
    const std::uint32_t height)
{
    const RHI::RenderingInfo renderingInfo =
        CreateColorRenderingInfo(
            backBufferView,
            width,
            height,
            RHI::ResourceState::RenderTarget,
            RHI::ResourceState::RenderTarget);
    const FullscreenDraw draw =
        m_captureStage == RenderCaptureStage::Shadow
        ? FullscreenDraw{
              m_shadowDebugPipeline.get(),
              m_shadowDebugDescriptorSets[m_activeFrame].get()}
        : FullscreenDraw{
              m_tonemapPipeline.get(),
              m_tonemapDescriptorSets[m_activeFrame].get()};
    m_fullscreenRenderingPass.Execute(
        commandContext,
        renderingInfo,
        std::span<const FullscreenDraw>(&draw, 1));
}
} // namespace Prism::Renderer
