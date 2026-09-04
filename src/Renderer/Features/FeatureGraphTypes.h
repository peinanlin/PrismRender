#pragma once

#include "Renderer/RenderGraphResources.h"

namespace Prism::Renderer
{
// Slot tags make the common graph contracts type-addressable without adding a
// string-keyed resource bus.
struct OpaqueSceneInputsSlot
{
};
struct LitSceneOutputsSlot
{
};
struct CompositeSceneOutputsSlot
{
};

// Only resources shared by pipeline stages belong here. Feature-private
// scratch resources remain in the owning Feature's graph contract.
struct OpaqueSceneInputs
{
    TextureHandle depth;
    TextureHandle motionVectors;
};

struct LitSceneOutputs
{
    TextureHandle sceneColor;
    TextureHandle depth;
    TextureHandle motionVectors;
};

struct CompositeSceneOutputs
{
    TextureHandle sceneColor;
    TextureHandle depth;
    TextureHandle motionVectors;
};
} // namespace Prism::Renderer
