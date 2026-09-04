#pragma once

namespace Prism::Asset
{
class AssetRegistry;
}

namespace Prism::RHI
{
class IGraphicsDevice;
}

namespace Prism::Scene
{
class RenderScene;

class DefaultSceneFactory
{
public:
    static void PopulateEditorPreviewScene(
        Asset::AssetRegistry& assetRegistry,
        RHI::IGraphicsDevice& device,
        RenderScene& scene);
    static void PopulateEditorPreviewScene(
        RHI::IGraphicsDevice& device,
        RenderScene& scene);
    static void ConfigureEditorPreviewWorld(RenderScene& scene, float cameraAspectRatio);
};
} // namespace Prism::Scene
