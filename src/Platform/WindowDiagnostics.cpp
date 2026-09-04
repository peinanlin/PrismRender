#include "Platform/WindowDiagnostics.h"
#include "Platform/Window.h"
#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#endif
#include <GLFW/glfw3.h>

namespace Prism::Platform
{
WindowDiagnostics ReadWindowDiagnostics(const Window& window)
{
    WindowDiagnostics result;
    auto* native = window.GetNativeWindow();
    glfwGetWindowPos(native, &result.x, &result.y);
    glfwGetWindowSize(native, &result.width, &result.height);
    glfwGetFramebufferSize(native, &result.framebufferWidth, &result.framebufferHeight);
    glfwGetWindowContentScale(native, &result.scaleX, &result.scaleY);
    result.visible = glfwGetWindowAttrib(native, GLFW_VISIBLE) == GLFW_TRUE;
    result.focused = glfwGetWindowAttrib(native, GLFW_FOCUSED) == GLFW_TRUE;
    result.minimized = glfwGetWindowAttrib(native, GLFW_ICONIFIED) == GLFW_TRUE;
#if defined(_WIN32)
    const auto monitor = MonitorFromWindow(static_cast<HWND>(window.GetNativeHandle()), MONITOR_DEFAULTTONEAREST);
    MONITORINFOEXA info{};
    info.cbSize = sizeof(info);
    if (monitor && GetMonitorInfoA(monitor, &info))
    {
        result.displayName = info.szDevice;
        result.monitorX = info.rcMonitor.left;
        result.monitorY = info.rcMonitor.top;
        result.monitorWidth = info.rcMonitor.right - info.rcMonitor.left;
        result.monitorHeight = info.rcMonitor.bottom - info.rcMonitor.top;
        DEVMODEA mode{};
        mode.dmSize = sizeof(mode);
        if (EnumDisplaySettingsA(info.szDevice, ENUM_CURRENT_SETTINGS, &mode))
        {
            result.refreshHz = static_cast<int>(mode.dmDisplayFrequency);
            result.displayAvailable = result.refreshHz > 1;
        }
    }
#endif
    // Unsupported platforms explicitly report unavailable, never guess a monitor.
    return result;
}
} // namespace Prism::Platform
