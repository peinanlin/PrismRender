#pragma once

#include <string>

namespace Prism::Platform
{
class Window;
struct WindowDiagnostics
{
    int x = 0, y = 0, width = 0, height = 0;
    int framebufferWidth = 0, framebufferHeight = 0;
    int monitorX = 0, monitorY = 0, monitorWidth = 0, monitorHeight = 0, refreshHz = 0;
    float scaleX = 0, scaleY = 0;
    bool visible = false, focused = false, minimized = false, displayAvailable = false;
    std::string displayName;
};
// Read-only, main/window thread. The nearest Win32 monitor is not proof of
// physical scan-out (especially for a hidden/occluded window).
WindowDiagnostics ReadWindowDiagnostics(const Window& window);
} // namespace Prism::Platform
