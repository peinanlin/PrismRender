#include "Renderer/Features/LocalLightShadows.h"
#include "Renderer/Pipeline/PipelineCache.h"

#include "Asset/Mesh.h"
#include "Core/Assert.h"
#include "RHI/IGraphicsDevice.h"
#include "RHI/ShaderLayoutBuilder.h"
#include "Renderer/Frustum.h"
#include "Renderer/MaterialParameterResolver.h"
#include "Scene/RenderScene.h"
#include "Scene/RenderSceneView.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <string>
#include <utility>

namespace Prism::Renderer
{
namespace
{
using namespace DirectX;

constexpr std::array<XMFLOAT3, 6>
    PointDirections = {{
        {1.0f, 0.0f, 0.0f},
        {-1.0f, 0.0f, 0.0f},
        {0.0f, 1.0f, 0.0f},
        {0.0f, -1.0f, 0.0f},
        {0.0f, 0.0f, 1.0f},
        {0.0f, 0.0f, -1.0f}}};
constexpr std::array<XMFLOAT3, 6>
    PointUpDirections = {{
        {0.0f, 1.0f, 0.0f},
        {0.0f, 1.0f, 0.0f},
        {0.0f, 0.0f, -1.0f},
        {0.0f, 0.0f, 1.0f},
        {0.0f, 1.0f, 0.0f},
        {0.0f, 1.0f, 0.0f}}};

constexpr std::uint64_t FnvOffsetBasis =
    14695981039346656037ull;
constexpr std::uint64_t FnvPrime =
    1099511628211ull;

void HashBytes(
    std::uint64_t& hash,
    const void* data,
    const std::size_t size)
{
    const auto* bytes =
        static_cast<const std::byte*>(data);
    for (std::size_t index = 0;
         index < size;
         ++index)
    {
        hash ^= static_cast<std::uint64_t>(
            bytes[index]);
        hash *= FnvPrime;
    }
}

float GetWorldBoundsRadius(
    const Scene::RenderObject& object,
    const Asset::Mesh& mesh)
{
    const XMFLOAT3& scale =
        object.transform.GetScale();
    const float maximumScale = std::max({
        std::abs(scale.x),
        std::abs(scale.y),
        std::abs(scale.z)});
    return mesh.GetBoundsRadius()
        * maximumScale;
}

XMFLOAT3 GetWorldBoundsCenter(
    const Scene::RenderObject& object,
    const Asset::Mesh& mesh)
{
    XMFLOAT3 center{};
    XMStoreFloat3(
        &center,
        XMVector3TransformCoord(
            XMLoadFloat3(
                &mesh.GetBoundsCenter()),
            object.transform.GetWorldMatrix()));
    return center;
}

XMMATRIX BuildSpotViewProjection(
    const Scene::SpotLight& light)
{
    const XMVECTOR position =
        XMLoadFloat3(&light.position);
    const XMVECTOR direction =
        XMVector3Normalize(
            XMLoadFloat3(&light.direction));
    const float verticalAlignment =
        std::abs(
            XMVectorGetX(
                XMVector3Dot(
                    direction,
                    XMVectorSet(
                        0.0f,
                        1.0f,
                        0.0f,
                        0.0f))));
    const XMVECTOR up =
        verticalAlignment > 0.98f
        ? XMVectorSet(
              0.0f,
              0.0f,
              1.0f,
              0.0f)
        : XMVectorSet(
              0.0f,
              1.0f,
              0.0f,
              0.0f);
    return XMMatrixLookToLH(
               position,
               direction,
               up)
        * XMMatrixPerspectiveFovLH(
              std::max(
                  light.outerAngleRadians
                      * 2.0f,
                  0.05f),
              1.0f,
              0.1f,
              std::max(light.range, 0.2f));
}

XMMATRIX BuildPointViewProjection(
    const Scene::PointLight& light,
    const std::uint32_t faceIndex)
{
    return XMMatrixLookToLH(
               XMLoadFloat3(&light.position),
               XMLoadFloat3(
                   &PointDirections[faceIndex]),
               XMLoadFloat3(
                   &PointUpDirections[faceIndex]))
        * XMMatrixPerspectiveFovLH(
              XM_PIDIV2,
              1.0f,
              0.1f,
              std::max(light.range, 0.2f));
}
} // namespace

void LocalLightShadows::Initialize(
    RHI::IGraphicsDevice& device,
    Asset::ShaderManager& shaderManager,
    PipelineCache& pipelineCache,
    const std::filesystem::path& shaderPath,
    const RHI::ShaderBinaryFormat shaderFormat,
    const std::uint32_t framesInFlight,
    const std::uint32_t maxRenderObjects)
{
    Core::Check(
        framesInFlight > 0
            && maxRenderObjects > 0,
        "Local shadows require frame and object capacity.");
    m_device = &device;
    m_maxRenderObjects = maxRenderObjects;
    const RHI::ShaderBinary& vertexShader =
        shaderManager.LoadShader(
            shaderPath,
            "LocalShadowVS",
            RHI::ShaderStage::Vertex,
            shaderFormat);
    const std::array stages = {
        RHI::ShaderLayoutStage{
            &vertexShader.reflection,
            RHI::ShaderStage::Vertex}};
    const std::array<std::uint32_t, 1>
        dynamicBindings = {0};
    m_renderLayout =
        device.CreateDescriptorSetLayout(
            RHI::BuildDescriptorSetLayout(
                stages,
                dynamicBindings));
    RHI::GraphicsPipelineDescription pipeline{};
    pipeline.vertexShader = vertexShader;
    pipeline.descriptorSetLayout = m_renderLayout;
    pipeline.vertexBindings = {{
        0,
        sizeof(Asset::MeshVertex),
        RHI::VertexInputRate::PerVertex}};
    pipeline.vertexAttributes = {{
        0,
        0,
        RHI::VertexElementFormat::Float3,
        static_cast<std::uint32_t>(
            offsetof(
                Asset::MeshVertex,
                position))}};
    pipeline.rasterizer.cullMode =
        RHI::CullMode::None;
    pipeline.rasterizer.depthBias = 2;
    pipeline.rasterizer.slopeScaledDepthBias =
        2.0f;
    pipeline.depthStencil.depthTestEnabled =
        true;
    pipeline.depthStencil.depthWriteEnabled =
        true;
    pipeline.depthStencil.depthComparison =
        RHI::CompareOperation::Less;
    pipeline.colorFormats.clear();
    pipeline.blendAttachments.clear();
    pipeline.depthFormat = RHI::Format::D32Float;
    m_pipeline =
        pipelineCache.GetOrCreateGraphics(
            device,
            "Feature.LocalLightShadows",
            pipeline);
    m_pipeline->SetDebugName(
        "GraphicsPSO.LocalShadow");

    RHI::TextureDescription spot{};
    spot.width = ShadowResolution;
    spot.height = ShadowResolution;
    spot.arrayLayers = MaxShadowedSpotLights;
    spot.format = RHI::Format::D32Float;
    spot.usage =
        RHI::TextureUsage::DepthStencil
        | RHI::TextureUsage::ShaderResource;
    m_spotShadowTexture =
        device.CreateTexture(spot);
    m_spotShadowTexture->SetDebugName(
        "LocalShadow.SpotDepthArray");
    for (std::uint32_t layer = 0;
         layer < MaxShadowedSpotLights;
         ++layer)
    {
        RHI::TextureViewDescription view{};
        view.type =
            RHI::TextureViewType::DepthStencil;
        view.baseArrayLayer = layer;
        m_spotDepthViews[layer] =
            device.CreateTextureView(
                m_spotShadowTexture,
                view);
    }
    RHI::TextureViewDescription spotSampled{};
    spotSampled.type =
        RHI::TextureViewType::Sampled;
    spotSampled.arrayLayerCount =
        MaxShadowedSpotLights;
    m_spotShadowSampledView =
        device.CreateTextureView(
            m_spotShadowTexture,
            spotSampled);

    RHI::TextureDescription point = spot;
    point.dimension =
        RHI::TextureDimension::TextureCube;
    point.arrayLayers =
        MaxShadowedPointLights
        * PointFaceCount;
    m_pointShadowTexture =
        device.CreateTexture(point);
    m_pointShadowTexture->SetDebugName(
        "LocalShadow.PointDepthCubeArray");
    for (std::uint32_t layer = 0;
         layer < point.arrayLayers;
         ++layer)
    {
        RHI::TextureViewDescription view{};
        view.type =
            RHI::TextureViewType::DepthStencil;
        view.baseArrayLayer = layer;
        m_pointDepthViews[layer] =
            device.CreateTextureView(
                m_pointShadowTexture,
                view);
    }
    RHI::TextureViewDescription pointSampled{};
    pointSampled.type =
        RHI::TextureViewType::Sampled;
    pointSampled.arrayLayerCount =
        point.arrayLayers;
    m_pointShadowSampledView =
        device.CreateTextureView(
            m_pointShadowTexture,
            pointSampled);

    m_spotState = RHI::ResourceState::Undefined;
    m_pointState = RHI::ResourceState::Undefined;

    const std::uint32_t passCount =
        MaxShadowedSpotLights
        + MaxShadowedPointLights
            * PointFaceCount;
    m_frames.resize(framesInFlight);
    for (std::uint32_t frameIndex = 0;
         frameIndex < framesInFlight;
         ++frameIndex)
    {
        FrameResources& frame =
            m_frames[frameIndex];
        RHI::BufferDescription lighting{};
        lighting.size = sizeof(LightingConstants);
        lighting.stride =
            sizeof(LightingConstants);
        lighting.usage =
            RHI::BufferUsage::Constant;
        lighting.memoryAccess =
            RHI::MemoryAccess::CpuToGpu;
        frame.lightingConstants =
            device.CreateBuffer(lighting);
        frame.lightingConstants->SetDebugName(
            "LocalShadow.LightingConstants["
            + std::to_string(frameIndex) + "]");

        RHI::BufferDescription render{};
        render.size =
            static_cast<std::size_t>(passCount)
            * maxRenderObjects
            * m_alignedRenderConstantSize;
        render.stride =
            m_alignedRenderConstantSize;
        render.usage =
            RHI::BufferUsage::Constant;
        render.memoryAccess =
            RHI::MemoryAccess::CpuToGpu;
        frame.renderConstants =
            device.CreateBuffer(render);
        frame.renderConstants->SetDebugName(
            "LocalShadow.RenderConstants["
            + std::to_string(frameIndex) + "]");
        frame.renderDescriptorSet =
            device.CreateDescriptorSet(
                m_renderLayout);
        frame.renderDescriptorSet->WriteBuffer(
            0,
            frame.renderConstants,
            0,
            sizeof(RenderConstants));
    }
}

void LocalLightShadows::Update(
    const Scene::RenderSceneView& scene,
    const std::uint32_t frameIndex,
    const bool spotLightsEnabled,
    const bool shadowsEnabled)
{
    Core::Check(
        frameIndex < m_frames.size(),
        "Local shadow update uses an invalid frame.");
    LightingConstants lighting{};
    m_statistics = {};
    m_activeSpotLayers.fill(false);
    m_activePointLayers.fill(false);
    m_renderRequired.fill(false);
    for (std::vector<std::uint32_t>& casters :
         m_shadowCasters)
    {
        casters.clear();
    }
    std::array<
        XMMATRIX,
        MaxShadowedSpotLights>
        spotViewProjections{};
    std::array<
        XMMATRIX,
        MaxShadowedPointLights
            * PointFaceCount>
        pointViewProjections{};

    const std::uint32_t spotCount =
        spotLightsEnabled
        ? std::min<std::uint32_t>(
              scene.GetActiveSpotLightCount(),
              MaxShadowedSpotLights)
        : 0u;
    for (std::uint32_t index = 0;
         index < spotCount;
         ++index)
    {
        const Scene::SpotLight& source =
            scene.GetSpotLights()[index];
        const XMMATRIX viewProjection =
            BuildSpotViewProjection(source);
        spotViewProjections[index] =
            viewProjection;
        SpotLightData& target =
            lighting.spotLights[index];
        XMStoreFloat4x4(
            &target.viewProjection,
            XMMatrixTranspose(
                viewProjection));
        target.positionRange = {
            source.position.x,
            source.position.y,
            source.position.z,
            source.range};
        const XMVECTOR direction =
            XMVector3Normalize(
                XMLoadFloat3(
                    &source.direction));
        XMFLOAT3 normalizedDirection{};
        XMStoreFloat3(
            &normalizedDirection,
            direction);
        target.directionOuterCos = {
            normalizedDirection.x,
            normalizedDirection.y,
            normalizedDirection.z,
            std::cos(
                source.outerAngleRadians)};
        target.colorIntensity = {
            source.color.x,
            source.color.y,
            source.color.z,
            source.intensity};
        const bool shadowEnabled =
            shadowsEnabled
            && source.castsShadow;
        target.parameters = {
            std::cos(
                source.innerAngleRadians),
            static_cast<float>(index),
            shadowEnabled ? 1.0f : 0.0f,
            0.0025f};
        m_activeSpotLayers[index] =
            shadowEnabled;
    }

    std::uint32_t pointShadowCount = 0;
    for (std::uint32_t lightIndex = 0;
         lightIndex
             < scene.GetActivePointLightCount()
         && pointShadowCount
                < MaxShadowedPointLights;
         ++lightIndex)
    {
        const Scene::PointLight& source =
            scene.GetPointLights()[lightIndex];
        if (!shadowsEnabled
            || !source.castsShadow)
        {
            continue;
        }
        PointShadowData& target =
            lighting.pointShadows[
                pointShadowCount];
        target.positionRange = {
            source.position.x,
            source.position.y,
            source.position.z,
            source.range};
        target.colorIntensity = {
            source.color.x,
            source.color.y,
            source.color.z,
            source.intensity};
        target.parameters = {
            static_cast<float>(lightIndex),
            static_cast<float>(
                pointShadowCount),
            1.0f,
            0.0025f};
        for (std::uint32_t face = 0;
             face < PointFaceCount;
             ++face)
        {
            const std::uint32_t layer =
                pointShadowCount
                    * PointFaceCount
                + face;
            pointViewProjections[layer] =
                BuildPointViewProjection(
                    source,
                    face);
            m_activePointLayers[layer] =
                true;
        }
        ++pointShadowCount;
    }
    lighting.countsAndNear = {
        static_cast<float>(spotCount),
        static_cast<float>(pointShadowCount),
        0.1f,
        0.0f};
    FrameResources& frame =
        m_frames[frameIndex];
    frame.lightingConstants->Update(
        &lighting,
        sizeof(lighting));

    const std::uint32_t passCount =
        MaxShadowedSpotLights
        + MaxShadowedPointLights
            * PointFaceCount;
    std::vector<std::byte> upload(
        static_cast<std::size_t>(passCount)
        * m_maxRenderObjects
        * m_alignedRenderConstantSize);
    const auto& objects =
        scene.GetRenderObjects();
    const std::uint32_t objectCount =
        std::min<std::uint32_t>(
            static_cast<std::uint32_t>(
                objects.size()),
            m_maxRenderObjects);
    const auto buildLayer =
        [&](const std::uint32_t passIndex,
            const XMMATRIX& lightViewProjection)
        {
            Frustum lightFrustum;
            lightFrustum.Build(
                lightViewProjection);
            std::vector<std::uint32_t>& casters =
                m_shadowCasters[passIndex];
            for (std::uint32_t objectIndex = 0;
                 objectIndex < objectCount;
                 ++objectIndex)
            {
                const Scene::RenderObject& object =
                    objects[objectIndex];
                const Asset::Mesh* const mesh =
                    scene.GetMesh(objectIndex);
                if (!scene.IsObjectSelected(objectIndex)
                    || !object.visible
                    || object.editorOnly
                    || mesh == nullptr
                    || (scene.FindMaterialBinding(objectIndex) == nullptr
                        && object.material == nullptr)
                    || object.surfaceType
                        == Scene::RenderSurfaceType::EditorDebugLine
                    || ResolveMaterialRenderQueue(scene, objectIndex)
                        == MaterialRenderQueue::Transparent)
                {
                    continue;
                }
                ++m_statistics.casterCandidates;
                if (!lightFrustum.IntersectsSphere(
                        GetWorldBoundsCenter(object, *mesh),
                        GetWorldBoundsRadius(object, *mesh)))
                {
                    ++m_statistics.culledCasters;
                    continue;
                }
                casters.push_back(objectIndex);
            }

            std::uint64_t signature =
                FnvOffsetBasis;
            XMFLOAT4X4 lightMatrix{};
            XMStoreFloat4x4(
                &lightMatrix,
                lightViewProjection);
            HashBytes(
                signature,
                &lightMatrix,
                sizeof(lightMatrix));
            for (const std::uint32_t objectIndex :
                 casters)
            {
                const Scene::RenderObject& object =
                    objects[objectIndex];
                const XMMATRIX world =
                    object.transform.GetWorldMatrix();
                XMFLOAT4X4 worldMatrix{};
                XMStoreFloat4x4(
                    &worldMatrix,
                    world);
                const std::uintptr_t meshIdentity =
                    reinterpret_cast<std::uintptr_t>(
                        scene.GetMesh(objectIndex));
                HashBytes(
                    signature,
                    &objectIndex,
                    sizeof(objectIndex));
                HashBytes(
                    signature,
                    &meshIdentity,
                    sizeof(meshIdentity));
                HashBytes(
                    signature,
                    &worldMatrix,
                    sizeof(worldMatrix));
            }
            m_pendingSignatures[passIndex] =
                signature;
            m_renderRequired[passIndex] =
                !m_cacheValid[passIndex]
                || m_cachedSignatures[passIndex]
                       != signature;
            if (m_renderRequired[passIndex])
            {
                ++m_statistics.renderedLayers;
                m_statistics.drawCalls +=
                    static_cast<std::uint32_t>(
                        casters.size());
            }
            else
            {
                ++m_statistics.cachedLayers;
            }
        };
    for (std::uint32_t index = 0;
         index < m_activeSpotLayers.size();
         ++index)
    {
        if (m_activeSpotLayers[index])
        {
            buildLayer(
                index,
                spotViewProjections[index]);
        }
    }
    for (std::uint32_t layer = 0;
         layer < m_activePointLayers.size();
         ++layer)
    {
        if (m_activePointLayers[layer])
        {
            buildLayer(
                MaxShadowedSpotLights + layer,
                pointViewProjections[layer]);
        }
    }

    const auto writePass =
        [&](const std::uint32_t passIndex,
            const XMMATRIX& lightViewProjection)
        {
            for (const std::uint32_t objectIndex :
                 m_shadowCasters[passIndex])
            {
                RenderConstants constants{};
                XMStoreFloat4x4(
                    &constants
                         .worldViewProjection,
                    XMMatrixTranspose(
                        objects[objectIndex]
                                .transform
                                .GetWorldMatrix()
                            * lightViewProjection));
                std::memcpy(
                    upload.data()
                        + GetRenderConstantOffset(
                            passIndex,
                            objectIndex),
                    &constants,
                    sizeof(constants));
            }
        };
    for (std::uint32_t index = 0;
         index < spotCount;
         ++index)
    {
        if (m_activeSpotLayers[index]
            && m_renderRequired[index])
        {
            writePass(
                index,
                spotViewProjections[index]);
        }
    }
    for (std::uint32_t layer = 0;
         layer < m_activePointLayers.size();
         ++layer)
    {
        if (m_activePointLayers[layer])
        {
            const std::uint32_t passIndex =
                MaxShadowedSpotLights
                + layer;
            if (m_renderRequired[passIndex])
            {
                writePass(
                    passIndex,
                    pointViewProjections[layer]);
            }
        }
    }
    frame.renderConstants->Update(
        upload.data(),
        upload.size());
}

void LocalLightShadows::ExecuteSpotShadows(
    RHI::ICommandContext& commandContext,
    const std::uint32_t frameIndex,
    const Scene::RenderSceneView& scene)
{
    ExecuteShadowLayers(
        commandContext,
        frameIndex,
        scene,
        0,
        m_activeSpotLayers,
        m_spotDepthViews,
        "SpotShadow");
}

void LocalLightShadows::ExecutePointShadows(
    RHI::ICommandContext& commandContext,
    const std::uint32_t frameIndex,
    const Scene::RenderSceneView& scene)
{
    ExecuteShadowLayers(
        commandContext,
        frameIndex,
        scene,
        MaxShadowedSpotLights,
        m_activePointLayers,
        m_pointDepthViews,
        "PointShadow");
}

void LocalLightShadows::EndFrame(const bool executed)
{
    // Both lighting paths declare the sampled descriptors even when disabled.
    (void)executed;
    {
        m_spotState =
            RHI::ResourceState::ShaderResource;
        m_pointState =
            RHI::ResourceState::ShaderResource;
    }
}

std::shared_ptr<RHI::IBuffer>
LocalLightShadows::GetLightingConstants(
    const std::uint32_t frameIndex) const
{
    return m_frames.at(frameIndex)
        .lightingConstants;
}

std::shared_ptr<RHI::IBuffer>
LocalLightShadows::GetRenderConstants(
    const std::uint32_t frameIndex) const
{
    return m_frames.at(frameIndex)
        .renderConstants;
}

RHI::ITexture&
LocalLightShadows::GetSpotShadowTexture() const
{
    return *m_spotShadowTexture;
}

RHI::ITexture&
LocalLightShadows::GetPointShadowTexture() const
{
    return *m_pointShadowTexture;
}

std::shared_ptr<RHI::ITextureView>
LocalLightShadows::GetSpotShadowSampledView() const
{
    return m_spotShadowSampledView;
}

std::shared_ptr<RHI::ITextureView>
LocalLightShadows::GetPointShadowSampledView() const
{
    return m_pointShadowSampledView;
}

RHI::ResourceState
LocalLightShadows::GetSpotShadowInitialState() const
{
    return m_spotState;
}

RHI::ResourceState
LocalLightShadows::GetPointShadowInitialState() const
{
    return m_pointState;
}

const LocalShadowStatistics&
LocalLightShadows::GetStatistics() const
{
    return m_statistics;
}

std::size_t
LocalLightShadows::GetRenderConstantOffset(
    const std::uint32_t passIndex,
    const std::uint32_t objectIndex) const
{
    return (
        static_cast<std::size_t>(passIndex)
            * m_maxRenderObjects
        + objectIndex)
        * m_alignedRenderConstantSize;
}

void LocalLightShadows::ExecuteShadowLayers(
    RHI::ICommandContext& commandContext,
    const std::uint32_t frameIndex,
    const Scene::RenderSceneView& scene,
    const std::uint32_t firstPass,
    const std::span<const bool> activeLayers,
    const std::span<const std::shared_ptr<
        RHI::ITextureView>> depthViews,
    const std::string_view labelPrefix)
{
    Core::Check(
        frameIndex < m_frames.size()
            && activeLayers.size()
                == depthViews.size(),
        "Local shadow execution uses invalid resources.");
    for (std::uint32_t layer = 0;
         layer < activeLayers.size();
         ++layer)
    {
        const std::uint32_t passIndex =
            firstPass + layer;
        if (!activeLayers[layer]
            || !m_renderRequired[passIndex])
        {
            continue;
        }
        const std::string label =
            firstPass == MaxShadowedSpotLights
            ? std::string(labelPrefix)
                  + ".Light["
                  + std::to_string(
                        layer / PointFaceCount)
                  + "].Face["
                  + std::to_string(
                        layer % PointFaceCount)
                  + "]"
            : std::string(labelPrefix)
                  + ".Layer["
                  + std::to_string(layer)
                  + "]";
        RHI::ScopedDebugLabel debugLabel(
            commandContext,
            label);
        RHI::RenderingInfo rendering{};
        rendering.width = ShadowResolution;
        rendering.height = ShadowResolution;
        RHI::RenderingAttachment depth{};
        depth.view = depthViews[layer].get();
        depth.loadOperation =
            RHI::LoadOperation::Clear;
        depth.storeOperation =
            RHI::StoreOperation::Store;
        depth.clearDepthStencil.depth = 1.0f;
        depth.stateBefore =
            RHI::ResourceState::DepthWrite;
        depth.stateAfter =
            RHI::ResourceState::DepthWrite;
        rendering.depthAttachment = depth;
        commandContext.BeginRendering(rendering);
        commandContext.BindGraphicsPipeline(
            *m_pipeline);
        for (const std::uint32_t objectIndex :
             m_shadowCasters[passIndex])
        {
            const Asset::Mesh* const mesh =
                scene.GetMesh(objectIndex);
            Core::Check(mesh != nullptr,
                "A local-shadow caster lost its retained mesh binding.");
            commandContext.BindDescriptorSet(
                *m_frames[frameIndex]
                     .renderDescriptorSet,
                std::array<
                    RHI::DynamicBufferOffset,
                    1>{RHI::DynamicBufferOffset{
                    0,
                    static_cast<std::uint32_t>(
                        GetRenderConstantOffset(
                            passIndex,
                            objectIndex))}});
            mesh->BindGeometry(
                commandContext);
            commandContext.DrawIndexed(
                mesh->GetRhiIndexCount());
        }
        commandContext.EndRendering();
        m_cachedSignatures[passIndex] =
            m_pendingSignatures[passIndex];
        m_cacheValid[passIndex] = true;
        m_renderRequired[passIndex] = false;
    }
}
LocalLightShadowsGraphContribution
LocalLightShadows::CreateRenderGraphContribution(
    const std::uint32_t frameIndex,
    std::shared_ptr<const Scene::RenderSceneView> scene)
{
    Core::Check(scene != nullptr,
        "Local-light shadows require a retained scene view.");
    LocalLightShadowsGraphContribution contribution{};
    contribution.lightingConstants =
        GetLightingConstants(frameIndex).get();
    contribution.renderConstants =
        GetRenderConstants(frameIndex).get();
    contribution.spotShadowMap =
        &GetSpotShadowTexture();
    contribution.pointShadowMap =
        &GetPointShadowTexture();
    contribution.spotShadowInitialState =
        GetSpotShadowInitialState();
    contribution.pointShadowInitialState =
        GetPointShadowInitialState();
    contribution.spotExecute =
        [this, frameIndex, scene](
            RHI::ICommandContext& commandContext,
            const RenderGraphPassResources&)
        {
            ExecuteSpotShadows(
                commandContext,
                frameIndex,
                *scene);
        };
    contribution.pointExecute =
        [this, frameIndex, scene = std::move(scene)](
            RHI::ICommandContext& commandContext,
            const RenderGraphPassResources&)
        {
            ExecutePointShadows(
                commandContext,
                frameIndex,
                *scene);
        };
    return contribution;
}

void LocalLightShadows::AddPasses(
    RenderGraph& graph,
    const BufferHandle renderConstants,
    TextureHandle& spotShadowMap,
    TextureHandle& pointShadowMap,
    const bool spotEnabled,
    const bool pointEnabled,
    RenderGraph::ParameterExecuteCallback spotExecute,
    RenderGraph::ParameterExecuteCallback pointExecute)
{
    Core::Check(
        (!spotEnabled || spotExecute)
            && (!pointEnabled || pointExecute),
        "Enabled local-light shadows require a pass callback.");
    if (spotEnabled)
    {
        auto spotParameters = graph.CreatePassParameters();
        spotParameters.ReadBuffer(
            renderConstants,
            RHI::ResourceState::ConstantBuffer);
        spotShadowMap = spotParameters.WriteTexture(
            spotShadowMap,
            RHI::ResourceState::DepthWrite);
        graph.AddParameterPass(
            "SpotShadows",
            std::move(spotParameters),
            std::move(spotExecute));
    }

    if (pointEnabled)
    {
        auto pointParameters = graph.CreatePassParameters();
        pointParameters.ReadBuffer(
            renderConstants,
            RHI::ResourceState::ConstantBuffer);
        pointShadowMap = pointParameters.WriteTexture(
            pointShadowMap,
            RHI::ResourceState::DepthWrite);
        graph.AddParameterPass(
            "PointShadows",
            std::move(pointParameters),
            std::move(pointExecute));
    }
}
} // namespace Prism::Renderer
