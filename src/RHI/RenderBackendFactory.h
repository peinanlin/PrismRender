#pragma once

#include "RHI/GraphicsApi.h"
#include "RHI/FramePacing.h"

#include <memory>

namespace Prism::RHI
{
class IRenderBackend;

// The only place where a requested API is mapped to a concrete backend.
std::unique_ptr<IRenderBackend> CreateRenderBackend(
    GraphicsApi graphicsApi,
    const FramePacingConfiguration& framePacing = {});
} // namespace Prism::RHI
