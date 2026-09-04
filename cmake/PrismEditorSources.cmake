set(PRISM_EDITOR_SOURCES
    src/UI/ImGuiFactory.cpp
    src/UI/ImGuiSystem.cpp
    src/UI/UiDrawPacket.cpp
    src/Platform/FileDialog.cpp
    src/UI/ContentBrowserPanel.cpp
    src/UI/DebugPanel.cpp
    src/UI/FluidLabPanel.cpp
    src/UI/OceanLabPanel.cpp
    src/UI/WaterOpticsPanel.cpp
    src/UI/EditorCoordinator.cpp
    src/UI/EditorLayer.cpp
    src/UI/PropertyGrid.cpp
    src/UI/GameViewportDebugOverlay.cpp
    src/UI/PerformanceProfilerPanel.cpp
    src/UI/PerformanceProfilerPanelModel.cpp
)

set(PRISM_EDITOR_HEADERS
    src/UI/ImGuiFactory.h
    src/UI/IImGuiRenderer.h
    src/UI/ImGuiSystem.h
    src/UI/UiDrawPacket.h
    src/Platform/FileDialog.h
    src/UI/ContentBrowserPanel.h
    src/UI/DebugPanel.h
    src/UI/FluidLabPanel.h
    src/UI/OceanLabPanel.h
    src/UI/EditorCoordinator.h
    src/UI/EditorLayer.h
    src/UI/PropertyGrid.h
    src/UI/GameViewportDebugOverlay.h
    src/UI/PerformanceProfilerPanel.h
    src/UI/PerformanceProfilerPanelModel.h
)

if(PRISM_RENDER_ENABLE_D3D12)
    list(APPEND PRISM_EDITOR_SOURCES
        src/UI/Backends/D3D12/D3D12ImGuiRenderer.cpp)
    list(APPEND PRISM_EDITOR_HEADERS
        src/UI/Backends/D3D12/D3D12ImGuiRenderer.h)
endif()

list(APPEND PRISM_EDITOR_SOURCES
    src/UI/Backends/Vulkan/VulkanImGuiRenderer.cpp)
list(APPEND PRISM_EDITOR_HEADERS
    src/UI/Backends/Vulkan/VulkanImGuiRenderer.h
    src/vulkan/vulkan.h)
