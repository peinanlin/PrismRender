#pragma once

#include <array>
#include <vector>

#include "Scene/Camera.h"
#include "Scene/Light.h"
#include "Scene/RenderObject.h"

namespace Prism::Scene
{
class RenderScene
{
public:
    static constexpr std::size_t MaxAuxiliaryDirectionalLights = 2;
    static constexpr std::size_t MaxPointLights = 128;
    static constexpr std::size_t MaxSpotLights = 16;

    Camera& GetCamera();
    const Camera& GetCamera() const;
    Camera& GetGameCamera();
    const Camera& GetGameCamera() const;

    // Produces the immutable scene consumed by the Game/Final Output renderer.
    // The editor camera remains in the source scene while the game camera
    // becomes the active render camera and editor-only helpers are hidden.
    [[nodiscard]] RenderScene CreateGameView(
        bool activateGameCamera = true,
        bool preserveGpuTerrainHierarchy = true) const;
    // Scene View keeps the editor helpers but collapses GPU quadtree terrain
    // to its root tiles. The full hierarchy remains exclusive to Game View,
    // avoiding a second copy of thousands of runtime descriptor sets.
    [[nodiscard]] RenderScene CreateEditorView() const;

    // Read access is intentionally const even for a mutable RenderScene. This
    // keeps accidental writes out of the versioned publication path.
    const std::vector<RenderObject>& GetRenderObjects() const;

    // Transitional escape hatch for import/factory code that must edit object
    // values in place. Merely acquiring it advances an O(1) mutation stamp;
    // extraction without a matching committed data revision then uses the
    // visible conservative full-rebuild path.
    std::vector<RenderObject>& EditRenderObjectsForFullRebuild();
    [[nodiscard]] std::uint64_t
        GetRawMutationRevision() const noexcept;

    RenderObject& AddRenderObject(RenderObject renderObject);
    void ClearRenderObjects();
    void ReplaceRenderObjects(
        std::vector<RenderObject> renderObjects);
    [[nodiscard]] std::uint64_t GetTopologyRevision() const noexcept;

    DirectionalLight& GetDirectionalLight();
    const DirectionalLight& GetDirectionalLight() const;
    std::array<DirectionalLight, MaxAuxiliaryDirectionalLights>&
        GetAuxiliaryDirectionalLights();
    const std::array<DirectionalLight, MaxAuxiliaryDirectionalLights>&
        GetAuxiliaryDirectionalLights() const;

    std::array<PointLight, MaxPointLights>& GetPointLights();
    const std::array<PointLight, MaxPointLights>& GetPointLights() const;
    std::uint32_t GetActivePointLightCount() const;
    void SetActivePointLightCount(std::uint32_t activePointLightCount);
    std::array<SpotLight, MaxSpotLights>&
        GetSpotLights();
    const std::array<SpotLight, MaxSpotLights>&
        GetSpotLights() const;
    std::uint32_t GetActiveSpotLightCount() const;
    void SetActiveSpotLightCount(
        std::uint32_t activeSpotLightCount);

private:
    Camera m_camera;
    // Scene View navigation never mutates this camera. It is the camera used
    // by Game/Final Output and therefore by that view's GPU visibility pass.
    Camera m_gameCamera;
    std::vector<RenderObject> m_renderObjects;
    std::uint64_t m_topologyRevision = 1;
    std::uint64_t m_rawMutationRevision = 0;
    DirectionalLight m_directionalLight;
    std::array<DirectionalLight, MaxAuxiliaryDirectionalLights>
        m_auxiliaryDirectionalLights{
            DirectionalLight{{0.0f, -1.0f, 0.0f}, 0.0f},
            DirectionalLight{{0.0f, -1.0f, 0.0f}, 0.0f}};
    std::array<PointLight, MaxPointLights> m_pointLights{};
    std::uint32_t m_activePointLightCount = 1;
    std::array<SpotLight, MaxSpotLights>
        m_spotLights{};
    std::uint32_t m_activeSpotLightCount = 1;
};
} // namespace Prism::Scene
