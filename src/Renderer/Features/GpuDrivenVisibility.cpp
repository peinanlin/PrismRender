#include "Renderer/Features/GpuDrivenVisibility.h"
#include "Renderer/Pipeline/PipelineCache.h"

#include "Asset/Mesh.h"
#include "Core/Assert.h"
#include "Renderer/Frustum.h"
#include "Renderer/MaterialParameterResolver.h"
#include "RHI/IGraphicsDevice.h"
#include "RHI/ShaderLayoutBuilder.h"
#include "Scene/RenderScene.h"
#include "Scene/RenderSceneView.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <limits>
#include <unordered_map>
#include <utility>
#include <utility>

namespace Prism::Renderer
{
namespace
{
float GetMaximumAbsoluteScale(
    const Scene::Transform& transform)
{
    const DirectX::XMFLOAT3& scale =
        transform.GetScale();
    return std::max(
        {std::abs(scale.x),
         std::abs(scale.y),
         std::abs(scale.z)});
}
} // namespace

void GpuDrivenVisibility::Initialize(
    RHI::IGraphicsDevice& device,
    Asset::ShaderManager& shaderManager,
    PipelineCache& pipelineCache,
    const std::filesystem::path& shaderPath,
    const RHI::ShaderBinaryFormat shaderFormat,
    const std::uint32_t framesInFlight,
    const std::uint32_t maxObjectCount)
{
    Core::Check(
        framesInFlight > 0
            && maxObjectCount > 0,
        "GPU-driven visibility requires frame and object capacity.");
    m_device = &device;
    m_maxObjectCount = maxObjectCount;

    const RHI::ShaderBinary& shader =
        shaderManager.LoadShader(
            shaderPath,
            "BuildDrawArgumentsCS",
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
            "Feature.GpuDrivenVisibility",
            pipeline);
    RHI::ComputePipelineDescription clearPipeline{};
    clearPipeline.computeShader =
        shaderManager.LoadShader(
            shaderPath,
            "ClearDrawCountsCS",
            RHI::ShaderStage::Compute,
            shaderFormat);
    clearPipeline.descriptorSetLayout =
        m_descriptorSetLayout;
    m_clearCountsPipeline =
        pipelineCache.GetOrCreateCompute(
            device,
            "Feature.GpuDrivenVisibility.ClearCounts",
            clearPipeline);

    m_frames.resize(framesInFlight);
    m_frameArgumentsExecuted.assign(
        framesInFlight,
        false);
    m_frameCountsExecuted.assign(
        framesInFlight,
        false);
    for (FrameResources& frame : m_frames)
    {
        RHI::BufferDescription constants{};
        constants.size = sizeof(CullingConstants);
        constants.stride = sizeof(CullingConstants);
        constants.usage =
            RHI::BufferUsage::Constant;
        constants.memoryAccess =
            RHI::MemoryAccess::CpuToGpu;
        frame.constants =
            device.CreateBuffer(constants);
    }
    RebuildSceneResources({});
}

void GpuDrivenVisibility::Update(
    const Scene::RenderSceneView& scene,
    const Scene::Camera& cullingCamera,
    const std::uint32_t frameIndex,
    const bool frustumCullingEnabled,
    const bool occlusionCullingEnabled,
    const bool hiZValid,
    const bool diagnosticsReadbackEnabled,
    const std::uint32_t viewportWidth,
    const std::uint32_t viewportHeight,
    const float oceanDisplacementMargin)
{
    Core::Check(
        IsInitialized()
            && frameIndex < m_frames.size(),
        "GPU-driven visibility update uses an invalid frame.");
    // BeginFrame has already waited for this frame slot's fence, so reading the
    // slot here never stalls the GPU. The result is intentionally a few frames
    // old and is used only for diagnostics.
    m_diagnosticsReadbackEnabled =
        diagnosticsReadbackEnabled;
    if (m_diagnosticsReadbackEnabled)
    {
        ResolveReadback(frameIndex);
    }
    else
    {
        m_statistics.valid = false;
        m_resolvedReasons.clear();
        m_resolvedCullingCameraValid = false;
        m_resolvedFeedback.reset();
    }

    const std::vector<Scene::RenderObject>&
        renderObjects =
            scene.GetRenderObjects();
    Core::Check(
        renderObjects.size()
            <= std::numeric_limits<std::uint32_t>::max(),
        "GPU-driven visibility object count exceeds the supported range.");
    const std::size_t objectCount =
        renderObjects.size();
    m_maxObjectCount = std::max(
        m_maxObjectCount,
        static_cast<std::uint32_t>(
            objectCount));
    struct DrawBatchKey
    {
        const Asset::Mesh* mesh = nullptr;
        std::uint64_t materialIdentity = 0;
        std::uint64_t materialOverrideSignature = 0;
        bool doubleSided = false;
        std::size_t uniqueObject = 0;

        bool operator==(const DrawBatchKey&) const = default;
    };
    struct DrawBatchKeyHash
    {
        std::size_t operator()(const DrawBatchKey& key) const
        {
            std::size_t hash = std::hash<const void*>{}(
                key.mesh);
            const auto combine = [&](const std::size_t value)
            {
                hash ^= value + 0x9e3779b9u
                    + (hash << 6u)
                    + (hash >> 2u);
            };
            combine(std::hash<std::uint64_t>{}(
                key.materialIdentity));
            combine(std::hash<std::uint64_t>{}(
                key.materialOverrideSignature));
            combine(std::hash<bool>{}(key.doubleSided));
            combine(std::hash<std::size_t>{}(
                key.uniqueObject));
            return hash;
        }
    };
    std::vector<ObjectRecord> records(objectCount);
    std::vector<std::vector<std::uint32_t>>
        batchObjectIndices;
    std::unordered_map<
        DrawBatchKey,
        std::size_t,
        DrawBatchKeyHash> batchLookup;
    std::vector<std::uint32_t> objectBatchIndices(
        objectCount);
    for (std::size_t index = 0;
         index < objectCount;
         ++index)
    {
        const Scene::RenderObject& object =
            renderObjects[index];
        const Asset::Mesh* const mesh = scene.GetMesh(index);
        const Scene::RenderSceneMaterialBinding* const material =
            scene.FindMaterialBinding(index);
        const bool batchable =
            scene.IsObjectSelected(index)
            && object.visible
            && mesh != nullptr
            && (material != nullptr || object.material != nullptr)
            && !object.editorOnly
            && object.surfaceType
                == Scene::RenderSurfaceType::Default
            && ResolveMaterialRenderQueue(scene, index)
                != MaterialRenderQueue::Transparent;
        const DrawBatchKey key{
            mesh,
            material != nullptr
                ? material->revision.value
                : static_cast<std::uint64_t>(
                      reinterpret_cast<std::uintptr_t>(
                          object.material.get())),
            ResolveMaterialOverrideSignature(object),
            IsDoubleSidedMaterial(scene, index),
            batchable ? 0u : index + 1u};
        const auto [iterator, inserted] =
            batchLookup.emplace(
                key,
                batchObjectIndices.size());
        if (inserted)
        {
            batchObjectIndices.emplace_back();
        }
        objectBatchIndices[index] =
            static_cast<std::uint32_t>(
                iterator->second);
        batchObjectIndices[iterator->second]
            .push_back(
                static_cast<std::uint32_t>(index));
    }
    std::vector<GpuDrawBatch> drawBatches;
    drawBatches.reserve(batchObjectIndices.size());
    std::uint32_t firstArgument = 0;
    for (std::size_t batchIndex = 0;
         batchIndex < batchObjectIndices.size();
         ++batchIndex)
    {
        const std::vector<std::uint32_t>& batch =
            batchObjectIndices[batchIndex];
        Core::Check(
            !batch.empty(),
            "A GPU draw batch cannot be empty.");
        drawBatches.push_back({
            batch.front(),
            firstArgument,
            static_cast<std::uint32_t>(
                batch.size())});
        for (const std::uint32_t objectIndex : batch)
        {
            records[objectIndex].drawBatchIndex =
                static_cast<std::uint32_t>(
                    batchIndex);
            records[objectIndex].drawArgumentOffset =
                firstArgument;
        }
        firstArgument +=
            static_cast<std::uint32_t>(
                batch.size());
    }
    std::unordered_map<std::string, std::uint32_t>
        objectIndicesByName;
    objectIndicesByName.reserve(objectCount);
    for (std::size_t index = 0; index < objectCount; ++index)
    {
        objectIndicesByName.emplace(
            renderObjects[index].name,
            static_cast<std::uint32_t>(index));
    }
    for (std::size_t index = 0;
         index < objectCount;
         ++index)
    {
        const Scene::RenderObject& object =
            renderObjects[index];
        const Asset::Mesh* const mesh = scene.GetMesh(index);
        ObjectRecord& record = records[index];
        const bool enabled =
            scene.IsObjectSelected(index)
            && object.visible
            && mesh != nullptr
            && (scene.FindMaterialBinding(index) != nullptr
                || object.material != nullptr);
        DirectX::XMFLOAT3 boundsCenter =
            object.transform.GetPosition();
        if (enabled)
        {
            DirectX::XMStoreFloat3(
                &boundsCenter,
                DirectX::XMVector3TransformCoord(
                    DirectX::XMLoadFloat3(
                        &mesh->GetBoundsCenter()),
                    object.transform.GetWorldMatrix()));
        }
        record.centerRadius = {
            boundsCenter.x,
            boundsCenter.y,
            boundsCenter.z,
            enabled
                ? mesh->GetBoundsRadius()
                      * GetMaximumAbsoluteScale(
                        object.transform)
                : 0.0f};
        if (enabled
            && object.surfaceType
                == Scene::RenderSurfaceType::FftOcean)
        {
            record.centerRadius.w += std::max(
                oceanDisplacementMargin,
                0.0f);
        }
        record.indexCount =
            enabled
            ? mesh->GetRhiIndexCount()
            : 0u;
        record.firstInstance =
            static_cast<std::uint32_t>(index);
        record.enabled = enabled ? 1u : 0u;
        if (object.quadtreePatch.enabled)
        {
            const Scene::GpuQuadtreePatch& patch =
                object.quadtreePatch;
            record.centerRadius = {
                patch.boundsCenter.x,
                patch.boundsCenter.y,
                patch.boundsCenter.z,
                patch.boundsRadius};
            record.lodData = {
                patch.halfExtent,
                patch.splitThresholdPixels,
                static_cast<float>(patch.level),
                static_cast<float>(patch.maxLevel)};
            record.parentObjectIndex =
                patch.parentObjectIndex;
            if (!patch.parentPatchName.empty())
            {
                const auto parent = objectIndicesByName.find(
                    patch.parentPatchName);
                record.parentObjectIndex =
                    parent != objectIndicesByName.end()
                    ? parent->second
                    : Scene::GpuQuadtreePatch::InvalidParent;
            }
            record.hierarchyEnabled = 1u;
        }
    }

    const bool recordsChanged =
        records.size() != m_records.size()
        || (!records.empty()
            && std::memcmp(
                   records.data(),
                   m_records.data(),
                   records.size()
                       * sizeof(ObjectRecord))
                != 0);
    if (recordsChanged)
    {
        m_drawBatches = std::move(drawBatches);
        m_objectBatchIndices =
            std::move(objectBatchIndices);
        RebuildSceneResources(
            std::move(records));
    }
    else
    {
        m_drawBatches = std::move(drawBatches);
        m_objectBatchIndices =
            std::move(objectBatchIndices);
    }
    m_objectCount =
        static_cast<std::uint32_t>(
            objectCount);
    m_frames[frameIndex].cullingCameraSnapshot =
        cullingCamera;
    m_frames[frameIndex].cullingCameraSnapshotValid =
        true;
    FrameResources& activeFrame = m_frames[frameIndex];
    activeFrame.feedbackIdentityValid = false;
    if (scene.IsPacketBacked()
        && scene.GetPacket() != nullptr
        && scene.GetView() != nullptr)
    {
        activeFrame.feedbackIdentity = {
            scene.GetPacket()->GetSceneData()->GetSceneGeneration(),
            scene.GetPacket()->GetSceneData()->GetDataRevision(),
            scene.GetView()->id,
            scene.GetPacket()->GetLogicalFrameId()};
        activeFrame.feedbackIdentityValid = true;
    }

    Frustum frustum;
    frustum.Build(
        cullingCamera.GetViewProjectionMatrix());
    CullingConstants constants{};
    std::ranges::copy(
        frustum.GetPlanes(),
        constants.frustumPlanes);
    DirectX::XMStoreFloat4x4(
        &constants.worldToView,
        DirectX::XMMatrixTranspose(
            cullingCamera.GetViewMatrix()));
    DirectX::XMStoreFloat4x4(
        &constants.viewProjection,
        DirectX::XMMatrixTranspose(
            cullingCamera.GetViewProjectionMatrix()));
    const float projectionScaleY =
        1.0f
        / std::tan(
            cullingCamera.GetFieldOfViewYRadians()
            * 0.5f);
    const float projectionScaleX =
        projectionScaleY
        / cullingCamera.GetAspectRatio();
    constants.viewportProjection = {
        static_cast<float>(viewportWidth),
        static_cast<float>(viewportHeight),
        projectionScaleX,
        projectionScaleY};
    constants.objectCount = m_objectCount;
    constants.drawBatchCount =
        static_cast<std::uint32_t>(
            m_drawBatches.size());
    constants.frustumCullingEnabled =
        frustumCullingEnabled ? 1u : 0u;
    constants.occlusionCullingEnabled =
        occlusionCullingEnabled
            && hiZValid
            && m_occlusionTexture != nullptr
            && viewportWidth > 0
            && viewportHeight > 0
        ? 1u
        : 0u;
    m_frames[frameIndex].constants->Update(
        &constants,
        sizeof(constants));
}

void GpuDrivenVisibility::SetOcclusionTexture(
    std::shared_ptr<RHI::ITexture> texture)
{
    m_occlusionTexture = std::move(texture);
    if (m_occlusionTexture == nullptr)
    {
        return;
    }
    for (FrameResources& frame : m_frames)
    {
        if (frame.descriptorSet != nullptr)
        {
            frame.descriptorSet->WriteTexture(
                16,
                m_occlusionTexture);
        }
    }
}

void GpuDrivenVisibility::NotifySceneChanged()
{
    // Readback is intentionally delayed by the frame ring. A scene switch
    // invalidates its object-index and camera association, but the buffers
    // remain alive until their normal frame-slot retirement point.
    m_statistics = {};
    m_resolvedReasons.clear();
    m_resolvedCullingCameraValid = false;
    m_resolvedFeedback.reset();
    for (FrameResources& frame : m_frames)
    {
        frame.readbackPending = false;
        frame.readbackObjectCount = 0;
        frame.cullingCameraSnapshotValid = false;
        frame.feedbackIdentityValid = false;
    }
}

void GpuDrivenVisibility::Execute(
    RHI::ICommandContext& commandContext,
    const std::uint32_t frameIndex)
{
    Core::Check(
        IsInitialized()
            && frameIndex < m_frames.size(),
        "GPU-driven visibility execution uses an invalid frame.");
    if (m_objectCount == 0)
    {
        return;
    }
    FrameResources& frame = m_frames[frameIndex];
    if (frame.visibilityResultsCopied)
    {
        commandContext.BufferBarrier(
            RHI::BufferBarrier{
                frame.visibilityResults.get(),
                RHI::ResourceState::CopySource,
                RHI::ResourceState::UnorderedAccess});
        frame.visibilityResultsCopied = false;
    }
    commandContext.BindComputePipeline(
        *m_clearCountsPipeline);
    commandContext.BindDescriptorSet(
        *frame.descriptorSet);
    commandContext.Dispatch(
        (static_cast<std::uint32_t>(
             m_drawBatches.size())
         + 63u)
            / 64u,
        1,
        1);
    commandContext.GlobalBarrier({
        RHI::ResourceState::UnorderedAccess,
        RHI::ResourceState::UnorderedAccess});
    commandContext.BindComputePipeline(
        *m_pipeline);
    commandContext.BindDescriptorSet(
        *frame.descriptorSet);
    commandContext.Dispatch(
        (m_objectCount + 63u) / 64u,
        1,
        1);
    if (m_diagnosticsReadbackEnabled)
    {
        commandContext.BufferBarrier(
            RHI::BufferBarrier{
                frame.visibilityResults.get(),
                RHI::ResourceState::UnorderedAccess,
                RHI::ResourceState::CopySource});
        commandContext.CopyBuffer(
            *frame.visibilityResults,
            *frame.visibilityReadback,
            static_cast<std::size_t>(m_objectCount)
                * sizeof(std::uint32_t));
        frame.visibilityResultsCopied = true;
        frame.readbackPending = true;
        frame.readbackObjectCount = m_objectCount;
    }
    m_frameArgumentsExecuted[frameIndex] =
        true;
    m_frameCountsExecuted[frameIndex] =
        true;
}

bool GpuDrivenVisibility::IsInitialized() const
{
    return m_device != nullptr
        && m_pipeline != nullptr
        && m_objectBuffer != nullptr
        && !m_frames.empty();
}

std::uint32_t
GpuDrivenVisibility::GetObjectCount() const
{
    return m_objectCount;
}

RHI::IBuffer&
GpuDrivenVisibility::GetObjectBuffer() const
{
    Core::Check(
        m_objectBuffer != nullptr,
        "GPU-driven object data is not initialized.");
    return *m_objectBuffer;
}

RHI::IBuffer&
GpuDrivenVisibility::GetArgumentBuffer(
    const std::uint32_t frameIndex) const
{
    Core::Check(
        frameIndex < m_frames.size()
            && m_frames[frameIndex].arguments
                   != nullptr,
        "GPU-driven arguments use an invalid frame.");
    return *m_frames[frameIndex].arguments;
}

RHI::IBuffer&
GpuDrivenVisibility::GetCountBuffer(
    const std::uint32_t frameIndex) const
{
    Core::Check(
        frameIndex < m_frames.size()
            && m_frames[frameIndex].drawCounts
                   != nullptr,
        "GPU-driven draw counts use an invalid frame.");
    return *m_frames[frameIndex].drawCounts;
}

RHI::ResourceState
GpuDrivenVisibility::GetArgumentInitialState(
    const std::uint32_t frameIndex) const
{
    Core::Check(
        frameIndex
            < m_frameArgumentsExecuted.size(),
        "GPU-driven argument state uses an invalid frame.");
    return m_frameArgumentsExecuted[frameIndex]
        ? RHI::ResourceState::IndirectArgument
        : RHI::ResourceState::UnorderedAccess;
}

RHI::ResourceState
GpuDrivenVisibility::GetCountInitialState(
    const std::uint32_t frameIndex) const
{
    Core::Check(
        frameIndex < m_frameCountsExecuted.size(),
        "GPU-driven count state uses an invalid frame.");
    return m_frameCountsExecuted[frameIndex]
        ? RHI::ResourceState::IndirectArgument
        : RHI::ResourceState::UnorderedAccess;
}

const std::vector<GpuDrawBatch>&
GpuDrivenVisibility::GetDrawBatches() const
{
    return m_drawBatches;
}

const GpuDrawBatch&
GpuDrivenVisibility::GetDrawBatchForObject(
    const std::size_t objectIndex) const
{
    Core::Check(
        objectIndex < m_objectBatchIndices.size()
            && m_objectBatchIndices[objectIndex]
                < m_drawBatches.size(),
        "GPU-driven object batch lookup is invalid.");
    return m_drawBatches[
        m_objectBatchIndices[objectIndex]];
}

const GpuVisibilityStatistics&
GpuDrivenVisibility::GetStatistics() const
{
    return m_statistics;
}

const std::vector<Scene::GpuVisibilityReason>&
GpuDrivenVisibility::GetResolvedReasons() const
{
    return m_resolvedReasons;
}

const Scene::Camera*
GpuDrivenVisibility::GetResolvedCullingCamera() const
{
    return m_resolvedCullingCameraValid
        ? &m_resolvedCullingCamera
        : nullptr;
}

const std::shared_ptr<const Scene::RenderViewFeedback>&
GpuDrivenVisibility::GetResolvedFeedback() const noexcept
{
    return m_resolvedFeedback;
}

void GpuDrivenVisibility::RebuildSceneResources(
    std::vector<ObjectRecord> records)
{
    Core::Check(
        m_device != nullptr,
        "GPU-driven scene resources require a graphics device.");
    const ObjectRecord fallbackRecord{};
    const void* initialData =
        records.empty()
        ? static_cast<const void*>(
              &fallbackRecord)
        : static_cast<const void*>(
              records.data());
    RHI::BufferDescription objectDescription{};
    objectDescription.size =
        std::max<std::size_t>(
            records.size(),
            1u)
        * sizeof(ObjectRecord);
    objectDescription.stride =
        sizeof(ObjectRecord);
    objectDescription.usage =
        RHI::BufferUsage::Storage;
    objectDescription.memoryAccess =
        RHI::MemoryAccess::GpuOnly;
    m_objectBuffer =
        m_device->CreateBuffer(
            objectDescription,
            initialData);

    for (FrameResources& frame : m_frames)
    {
        RHI::BufferDescription arguments{};
        arguments.size =
            std::max<std::size_t>(
                records.size(),
                1u)
            * GetArgumentStride();
        arguments.stride =
            static_cast<std::uint32_t>(
                GetArgumentStride());
        arguments.usage =
            RHI::BufferUsage::Storage
            | RHI::BufferUsage::Indirect
            | RHI::BufferUsage::CopyDestination;
        arguments.memoryAccess =
            RHI::MemoryAccess::GpuOnly;
        frame.arguments =
            m_device->CreateBuffer(arguments);
        RHI::BufferDescription drawCounts{};
        drawCounts.size =
            std::max<std::size_t>(
                m_drawBatches.size(),
                1u)
            * sizeof(std::uint32_t);
        drawCounts.stride = sizeof(std::uint32_t);
        drawCounts.usage =
            RHI::BufferUsage::Storage
            | RHI::BufferUsage::Indirect
            | RHI::BufferUsage::CopyDestination;
        drawCounts.memoryAccess =
            RHI::MemoryAccess::GpuOnly;
        frame.drawCounts =
            m_device->CreateBuffer(drawCounts);
        RHI::BufferDescription visibilityResults{};
        visibilityResults.size =
            std::max<std::size_t>(records.size(), 1u)
            * sizeof(std::uint32_t);
        visibilityResults.stride =
            sizeof(std::uint32_t);
        visibilityResults.usage =
            RHI::BufferUsage::Storage
            | RHI::BufferUsage::CopySource
            | RHI::BufferUsage::CopyDestination;
        visibilityResults.memoryAccess =
            RHI::MemoryAccess::GpuOnly;
        frame.visibilityResults =
            m_device->CreateBuffer(
                visibilityResults);
        RHI::BufferDescription visibilityReadback{};
        visibilityReadback.size =
            visibilityResults.size;
        visibilityReadback.stride =
            sizeof(std::uint32_t);
        visibilityReadback.usage =
            RHI::BufferUsage::CopyDestination;
        visibilityReadback.memoryAccess =
            RHI::MemoryAccess::GpuToCpu;
        frame.visibilityReadback =
            m_device->CreateBuffer(
                visibilityReadback);
        frame.descriptorSet =
            m_device->CreateDescriptorSet(
                m_descriptorSetLayout);
        frame.descriptorSet->WriteBuffer(
            0,
            frame.constants);
        if (m_occlusionTexture != nullptr)
        {
            frame.descriptorSet->WriteTexture(
                16,
                m_occlusionTexture);
        }
        frame.descriptorSet->WriteBuffer(
            32,
            m_objectBuffer);
        frame.descriptorSet->WriteBuffer(
            33,
            frame.arguments);
        frame.descriptorSet->WriteBuffer(
            34,
            frame.visibilityResults);
        frame.descriptorSet->WriteBuffer(
            35,
            frame.drawCounts);
        frame.visibilityResultsCopied = false;
        frame.readbackPending = false;
        frame.readbackObjectCount = 0;
        frame.cullingCameraSnapshotValid = false;
        frame.feedbackIdentityValid = false;
    }
    m_records = std::move(records);
    m_resolvedReasons.assign(
        m_records.size(),
        Scene::GpuVisibilityReason::Unknown);
    m_statistics = {};
    m_resolvedCullingCameraValid = false;
    m_resolvedFeedback.reset();
    std::ranges::fill(
        m_frameArgumentsExecuted,
        false);
    std::ranges::fill(
        m_frameCountsExecuted,
        false);
    m_objectCount =
        static_cast<std::uint32_t>(
            m_records.size());
}

void GpuDrivenVisibility::ResolveReadback(
    const std::uint32_t frameIndex)
{
    FrameResources& frame = m_frames[frameIndex];
    if (!frame.readbackPending)
    {
        return;
    }
    const std::uint32_t resultCount =
        frame.readbackObjectCount;
    std::vector<std::uint32_t> rawResults(
        resultCount);
    if (!rawResults.empty())
    {
        frame.visibilityReadback->Read(
            rawResults.data(),
            rawResults.size()
                * sizeof(std::uint32_t));
    }

    m_resolvedReasons.assign(
        resultCount,
        Scene::GpuVisibilityReason::Unknown);
    m_statistics = {};
    m_statistics.candidateObjects = resultCount;
    m_statistics.valid = true;
    if (frame.cullingCameraSnapshotValid)
    {
        m_resolvedCullingCamera =
            frame.cullingCameraSnapshot;
        m_resolvedCullingCameraValid = true;
    }
    for (std::size_t index = 0;
         index < rawResults.size();
         ++index)
    {
        const auto reason =
            rawResults[index]
                    <= static_cast<std::uint32_t>(
                        Scene::GpuVisibilityReason::Disabled)
                ? static_cast<Scene::GpuVisibilityReason>(
                      rawResults[index])
                : Scene::GpuVisibilityReason::Unknown;
        m_resolvedReasons[index] = reason;
        switch (reason)
        {
        case Scene::GpuVisibilityReason::Visible:
            ++m_statistics.visibleObjects;
            break;
        case Scene::GpuVisibilityReason::LodRejected:
            ++m_statistics.lodRejectedObjects;
            break;
        case Scene::GpuVisibilityReason::FrustumCulled:
            ++m_statistics.frustumCulledObjects;
            break;
        case Scene::GpuVisibilityReason::OcclusionCulled:
            ++m_statistics.occlusionCulledObjects;
            break;
        case Scene::GpuVisibilityReason::Disabled:
        case Scene::GpuVisibilityReason::Unknown:
            ++m_statistics.disabledObjects;
            break;
        }
    }
    if (frame.feedbackIdentityValid
        && frame.cullingCameraSnapshotValid)
    {
        m_resolvedFeedback =
            std::make_shared<const Scene::RenderViewFeedback>(
                frame.feedbackIdentity,
                m_resolvedReasons,
                frame.cullingCameraSnapshot);
    }
    else
    {
        m_resolvedFeedback.reset();
    }
    frame.readbackPending = false;
}
GpuDrivenVisibilityGraphContribution
GpuDrivenVisibility::CreateRenderGraphContribution(
    const std::uint32_t frameIndex)
{
    GpuDrivenVisibilityGraphContribution contribution{};
    contribution.objectRecords = &GetObjectBuffer();
    contribution.indirectArguments = &GetArgumentBuffer(frameIndex);
    contribution.indirectDrawCounts = &GetCountBuffer(frameIndex);
    contribution.indirectInitialState = GetArgumentInitialState(frameIndex);
    contribution.countInitialState = GetCountInitialState(frameIndex);
    contribution.execute =
        [this, frameIndex](
            RHI::ICommandContext& commandContext,
            const RenderGraphPassResources&)
        {
            Execute(commandContext, frameIndex);
        };
    return contribution;
}

void GpuDrivenVisibility::AddPasses(
    RenderGraph& graph,
    const BufferHandle gpuObjectRecords,
    const TextureHandle hiZ,
    BufferHandle& indirectArguments,
    BufferHandle& indirectDrawCounts,
    RenderGraph::ParameterExecuteCallback execute)
{
    Core::Check(
        static_cast<bool>(execute),
        "GPU visibility requires an execute callback.");
    auto parameters = graph.CreatePassParameters();
    parameters.ReadBuffer(
        gpuObjectRecords,
        RHI::ResourceState::UnorderedAccess);
    parameters.ReadTexture(
        hiZ,
        RHI::ResourceState::ShaderResource);
    indirectArguments = parameters.WriteBuffer(
        indirectArguments,
        RHI::ResourceState::UnorderedAccess);
    indirectDrawCounts = parameters.WriteBuffer(
        indirectDrawCounts,
        RHI::ResourceState::UnorderedAccess);
    graph.AddParameterPass(
        "GpuVisibility",
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
