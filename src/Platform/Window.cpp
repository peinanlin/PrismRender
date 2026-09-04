#include "Platform/Window.h"

#include "Core/Assert.h"
#include "Core/Environment.h"

#if defined(_WIN32)
#define GLFW_EXPOSE_NATIVE_WIN32
#endif
#include <GLFW/glfw3.h>
#if defined(_WIN32)
#include <GLFW/glfw3native.h>
#endif

#include <string>

namespace Prism::Platform
{
namespace
{
#if defined(_WIN32)
constexpr wchar_t PerformanceFocusReturnProperty[] = L"PrismRender.PerformanceFocusReturnWindow";
#endif

void FocusPerformanceWindow(GLFWwindow* window)
{
#if defined(_WIN32)
    const HWND nativeWindow = glfwGetWin32Window(window);
    if (IsIconic(nativeWindow) != FALSE)
    {
        ShowWindow(nativeWindow, SW_RESTORE);
    }
    SetWindowPos(nativeWindow, HWND_TOPMOST, 0, 0, 0, 0,
        SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
    const HWND foregroundWindow = GetForegroundWindow();
    const DWORD currentThread = GetCurrentThreadId();
    const DWORD foregroundThread = foregroundWindow != nullptr
        ? GetWindowThreadProcessId(foregroundWindow, nullptr)
        : 0;
    const bool attached = foregroundThread != 0 && foregroundThread != currentThread
        && AttachThreadInput(currentThread, foregroundThread, TRUE) != FALSE;
    BringWindowToTop(nativeWindow);
    SetForegroundWindow(nativeWindow);
    SetActiveWindow(nativeWindow);
    SetFocus(nativeWindow);
    if (attached)
    {
        AttachThreadInput(currentThread, foregroundThread, FALSE);
    }
#else
    glfwFocusWindow(window);
#endif
}

void StabilizeUnfocusedPerformanceWindow(GLFWwindow* window)
{
#if defined(_WIN32)
    const HWND nativeWindow = glfwGetWin32Window(window);
    const LONG_PTR extendedStyle = GetWindowLongPtrW(nativeWindow, GWL_EXSTYLE);
    if ((extendedStyle & WS_EX_NOACTIVATE) == 0)
    {
        SetWindowLongPtrW(nativeWindow, GWL_EXSTYLE, extendedStyle | WS_EX_NOACTIVATE);
    }
    SetWindowPos(nativeWindow, HWND_TOPMOST, 0, 0, 0, 0,
        SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW | SWP_FRAMECHANGED);
    if (GetFocus() == nativeWindow)
    {
        SetFocus(nullptr);
    }
    if (GetForegroundWindow() == nativeWindow
        || glfwGetWindowAttrib(window, GLFW_FOCUSED) == GLFW_TRUE)
    {
        HWND returnWindow = reinterpret_cast<HWND>(
            GetPropW(nativeWindow, PerformanceFocusReturnProperty));
        if (returnWindow == nullptr || returnWindow == nativeWindow
            || IsWindow(returnWindow) == FALSE || IsWindowVisible(returnWindow) == FALSE)
        {
            returnWindow = GetShellWindow();
        }
        if (returnWindow != nullptr && returnWindow != nativeWindow)
        {
            const DWORD currentThread = GetCurrentThreadId();
            const DWORD returnThread = GetWindowThreadProcessId(returnWindow, nullptr);
            const bool attached = returnThread != 0 && returnThread != currentThread
                && AttachThreadInput(currentThread, returnThread, TRUE) != FALSE;
            BringWindowToTop(returnWindow);
            SetForegroundWindow(returnWindow);
            SetActiveWindow(returnWindow);
            SetFocus(returnWindow);
            if (attached)
            {
                AttachThreadInput(currentThread, returnThread, FALSE);
            }
        }
    }
#else
    (void)window;
#endif
}
} // namespace

Window::Window(const std::string& title, const std::uint32_t width, const std::uint32_t height,
    const WindowFocusPolicy focusPolicy)
    : m_width(width)
    , m_height(height)
    , m_performanceWidth(width)
    , m_performanceHeight(height)
    , m_focusPolicy(focusPolicy)
{
#if defined(_WIN32)
    const HWND focusReturnWindow = focusPolicy == WindowFocusPolicy::Unfocused
        ? GetForegroundWindow()
        : nullptr;
#endif
    Core::Check(glfwInit() == GLFW_TRUE, "Failed to initialize GLFW.");

    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    glfwWindowHint(GLFW_RESIZABLE, GLFW_TRUE);
    // Hints are global to GLFW: set both paths so an unfocused test window
    // cannot leak its policy into a subsequently created ordinary window.
    const int requestFocus = focusPolicy == WindowFocusPolicy::Unfocused ? GLFW_FALSE : GLFW_TRUE;
    glfwWindowHint(GLFW_FOCUSED, requestFocus);
    glfwWindowHint(GLFW_FOCUS_ON_SHOW, requestFocus);

    if (Core::IsEnvironmentVariableEnabled("PRISM_RENDER_HEADLESS"))
    {
        glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
    }

    m_window = glfwCreateWindow(static_cast<int>(width), static_cast<int>(height), title.c_str(), nullptr, nullptr);
    Core::Check(m_window != nullptr, "Failed to create GLFW window.");
    if (focusPolicy == WindowFocusPolicy::Focused)
    {
        FocusPerformanceWindow(m_window);
    }
    else if (focusPolicy == WindowFocusPolicy::Unfocused)
    {
#if defined(_WIN32)
        if (focusReturnWindow != nullptr)
        {
            SetPropW(glfwGetWin32Window(m_window), PerformanceFocusReturnProperty, focusReturnWindow);
        }
#endif
        StabilizeUnfocusedPerformanceWindow(m_window);
    }

    glfwSetWindowUserPointer(m_window, this);
    glfwSetFramebufferSizeCallback(m_window, FramebufferSizeCallback);
    glfwSetScrollCallback(m_window, ScrollCallback);
}

Window::~Window()
{
    if (m_window != nullptr)
    {
        glfwDestroyWindow(m_window);
        m_window = nullptr;
    }

    glfwTerminate();
}

void Window::PollEvents() const
{
    glfwPollEvents();
    if (m_focusPolicy != WindowFocusPolicy::Default)
    {
        int width = 0, height = 0, x = 0, y = 0;
        glfwGetWindowSize(m_window, &width, &height);
        glfwGetWindowPos(m_window, &x, &y);
        const bool invalidState = glfwGetWindowAttrib(m_window, GLFW_ICONIFIED) == GLFW_TRUE
            || glfwGetWindowAttrib(m_window, GLFW_MAXIMIZED) == GLFW_TRUE;
        const bool invalidExtent = width != static_cast<int>(m_performanceWidth)
            || height != static_cast<int>(m_performanceHeight);
        const bool invalidPosition = m_performancePositionSet
            && (x != m_performanceX || y != m_performanceY);
        const bool focused = glfwGetWindowAttrib(m_window, GLFW_FOCUSED) == GLFW_TRUE;
        const bool invalidFocus = m_focusPolicy == WindowFocusPolicy::Focused ? !focused : focused;
        if (invalidState || invalidExtent || invalidPosition || invalidFocus)
        {
            if (invalidState)
            {
                glfwRestoreWindow(m_window);
            }
            if (invalidExtent || invalidState)
            {
                glfwSetWindowSize(m_window, static_cast<int>(m_performanceWidth),
                    static_cast<int>(m_performanceHeight));
            }
            if (m_performancePositionSet && (invalidPosition || invalidState))
            {
                glfwSetWindowPos(m_window, m_performanceX, m_performanceY);
            }
            if (m_focusPolicy == WindowFocusPolicy::Focused)
            {
                FocusPerformanceWindow(m_window);
            }
            else
            {
                StabilizeUnfocusedPerformanceWindow(m_window);
            }
            glfwPollEvents();
        }
    }
}

bool Window::ShouldClose() const
{
    return glfwWindowShouldClose(m_window) == GLFW_TRUE;
}

bool Window::ConsumeResize(std::uint32_t& width, std::uint32_t& height)
{
    if (!m_resizePending)
    {
        return false;
    }

    width = m_width;
    height = m_height;
    m_resizePending = false;
    return true;
}

GLFWwindow* Window::GetNativeWindow() const
{
    return m_window;
}

void* Window::GetNativeHandle() const
{
#if defined(_WIN32)
    return glfwGetWin32Window(m_window);
#else
    return nullptr;
#endif
}

std::uint32_t Window::GetWidth() const
{
    return m_width;
}

std::uint32_t Window::GetHeight() const
{
    return m_height;
}

bool Window::IsKeyDown(const int key) const
{
    return glfwGetKey(m_window, key) == GLFW_PRESS;
}

bool Window::IsMouseButtonDown(const int button) const
{
    return glfwGetMouseButton(m_window, button) == GLFW_PRESS;
}

void Window::GetCursorPosition(double& x, double& y) const
{
    glfwGetCursorPos(m_window, &x, &y);
}

double Window::ConsumeScrollDelta()
{
    const double scrollDelta = m_scrollDelta;
    m_scrollDelta = 0.0;
    return scrollDelta;
}

void Window::SetPosition(const int x, const int y)
{
    glfwSetWindowPos(m_window, x, y);
    int actualX = 0, actualY = 0;
    glfwGetWindowPos(m_window, &actualX, &actualY);
    Core::Check(actualX == x && actualY == y,
        "Window position request was not applied: requested " + std::to_string(x) + "," + std::to_string(y)
        + "; actual " + std::to_string(actualX) + "," + std::to_string(actualY));
    if (m_focusPolicy != WindowFocusPolicy::Default)
    {
        m_performanceX = x;
        m_performanceY = y;
        m_performancePositionSet = true;
    }
}

void Window::SetSize(const std::uint32_t width, const std::uint32_t height)
{
    if (width == 0 || height == 0)
    {
        return;
    }
    glfwSetWindowSize(
        m_window,
        static_cast<int>(width),
        static_cast<int>(height));
}

void Window::SetCursorCaptured(const bool captured)
{
    if (m_cursorCaptured == captured)
    {
        return;
    }

    m_cursorCaptured = captured;
    glfwSetInputMode(m_window, GLFW_CURSOR, captured ? GLFW_CURSOR_DISABLED : GLFW_CURSOR_NORMAL);
    if (captured && glfwRawMouseMotionSupported() == GLFW_TRUE)
    {
        glfwSetInputMode(m_window, GLFW_RAW_MOUSE_MOTION, GLFW_TRUE);
    }
    else
    {
        glfwSetInputMode(m_window, GLFW_RAW_MOUSE_MOTION, GLFW_FALSE);
    }
}

bool Window::IsCursorCaptured() const
{
    return m_cursorCaptured;
}

void Window::FramebufferSizeCallback(GLFWwindow* window, const int width, const int height)
{
    if (width <= 0 || height <= 0)
    {
        return;
    }

    Window* instance = static_cast<Window*>(glfwGetWindowUserPointer(window));
    if (instance == nullptr)
    {
        return;
    }

    instance->m_width = static_cast<std::uint32_t>(width);
    instance->m_height = static_cast<std::uint32_t>(height);
    instance->m_resizePending = true;
}

void Window::ScrollCallback(GLFWwindow* window, const double, const double yOffset)
{
    Window* instance = static_cast<Window*>(glfwGetWindowUserPointer(window));
    if (instance != nullptr)
    {
        instance->m_scrollDelta += yOffset;
    }
}
} // namespace Prism::Platform
