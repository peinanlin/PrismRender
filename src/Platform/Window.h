#pragma once

#include <cstdint>
#include <string>

struct GLFWwindow;

namespace Prism::Platform
{
enum class WindowFocusPolicy { Default, Focused, Unfocused };

class Window
{
public:
    Window(const std::string& title, std::uint32_t width, std::uint32_t height,
        WindowFocusPolicy focusPolicy = WindowFocusPolicy::Default);
    ~Window();

    Window(const Window&) = delete;
    Window& operator=(const Window&) = delete;

    void PollEvents() const;
    bool ShouldClose() const;
    bool ConsumeResize(std::uint32_t& width, std::uint32_t& height);
    GLFWwindow* GetNativeWindow() const;
    void* GetNativeHandle() const;
    std::uint32_t GetWidth() const;
    std::uint32_t GetHeight() const;
    bool IsKeyDown(int key) const;
    bool IsMouseButtonDown(int button) const;
    void GetCursorPosition(double& x, double& y) const;
    void SetPosition(int x, int y);
    void SetSize(std::uint32_t width, std::uint32_t height);
    double ConsumeScrollDelta();
    void SetCursorCaptured(bool captured);
    bool IsCursorCaptured() const;
    WindowFocusPolicy GetFocusPolicy() const noexcept { return m_focusPolicy; }

private:
    static void FramebufferSizeCallback(GLFWwindow* window, int width, int height);
    static void ScrollCallback(GLFWwindow* window, double xOffset, double yOffset);

    GLFWwindow* m_window = nullptr;
    std::uint32_t m_width = 0;
    std::uint32_t m_height = 0;
    std::uint32_t m_performanceWidth = 0;
    std::uint32_t m_performanceHeight = 0;
    int m_performanceX = 0;
    int m_performanceY = 0;
    bool m_performancePositionSet = false;
    bool m_resizePending = false;
    bool m_cursorCaptured = false;
    WindowFocusPolicy m_focusPolicy = WindowFocusPolicy::Default;
    double m_scrollDelta = 0.0;
};
} // namespace Prism::Platform
