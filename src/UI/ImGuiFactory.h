#pragma once

#include "RHI/GraphicsApi.h"

#include <memory>

namespace Prism::UI
{
class ImGuiSystem;

[[nodiscard]] std::unique_ptr<ImGuiSystem>
CreateImGuiSystem(RHI::GraphicsApi graphicsApi);
} // namespace Prism::UI
