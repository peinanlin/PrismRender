cmake_minimum_required(VERSION 3.24)

project(PrismRender VERSION 0.1.0 LANGUAGES CXX)

# Ninja parses MSVC's /showIncludes output byte-for-byte. This MSVC toolchain
# emits the Chinese prefix as UTF-8, while CMake's compiler-detection cache can
# transcode it through the active ANSI code page. Set the exact emitted prefix
# at project scope so header edits cannot silently mix incompatible object ABIs.
if(MSVC AND CMAKE_GENERATOR MATCHES "Ninja")
    set(CMAKE_CXX_CL_SHOWINCLUDES_PREFIX "注意: 包含文件:")
    set(CMAKE_C_CL_SHOWINCLUDES_PREFIX "注意: 包含文件:")
    set(CMAKE_CL_SHOWINCLUDES_PREFIX "注意: 包含文件:")
endif()

include(CTest)
find_package(Threads REQUIRED)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(PRISM_RENDER_FETCH_DEPS "Fetch GLFW, ImGui, and ImGuizmo when local packages are unavailable." ON)
option(PRISM_RENDER_ENABLE_GLTF "Enable glTF loading when tinygltf headers are available." ON)
option(PRISM_RENDER_FETCH_GLTF_DEPS "Allow CMake to fetch tinygltf automatically when headers are missing." OFF)
option(PRISM_RENDER_ENABLE_D3D12 "Build the Direct3D 12 backend on Windows." ${WIN32})
option(PRISM_RENDER_BUILD_EDITOR "Build the Windows ImGui editor." ${WIN32})
option(PRISM_RENDER_BUILD_HARNESS "Build the Agent Harness tools." ${WIN32})
option(PRISM_RENDER_CHECK_MODULE_BOUNDARIES
    "Run the strict source/include/target boundary audit during configure." OFF)
option(PRISM_RENDER_CHECK_PUBLIC_HEADERS
    "Compile each public project header in an isolated translation unit." OFF)

if(PRISM_RENDER_ENABLE_D3D12 AND NOT WIN32)
    message(FATAL_ERROR "PRISM_RENDER_ENABLE_D3D12 is only supported on Windows.")
endif()
if(PRISM_RENDER_BUILD_EDITOR AND NOT WIN32)
    message(FATAL_ERROR "PrismEditor currently uses the Win32 ImGui backend and can only be built on Windows.")
endif()
set(PRISM_RENDER_VENDOR_DIR "${CMAKE_SOURCE_DIR}/third_party")
set(PRISM_RENDER_JSON_INCLUDE_DIR "${PRISM_RENDER_VENDOR_DIR}/tinygltf")
set(PRISM_GLAD_VULKAN_INCLUDE_DIR "${PRISM_RENDER_VENDOR_DIR}/glad/include")
set(PRISM_SLANG_ROOT "${PRISM_RENDER_VENDOR_DIR}/slang" CACHE PATH "Path to the pinned Slang SDK.")

if(NOT EXISTS "${PRISM_GLAD_VULKAN_INCLUDE_DIR}/glad/vulkan.h")
    message(FATAL_ERROR "The vendored GLAD Vulkan loader was not found.")
endif()

set(PRISM_SLANG_INCLUDE_DIR "${PRISM_SLANG_ROOT}/include")
if(WIN32)
    set(PRISM_SLANG_LIBRARY "${PRISM_SLANG_ROOT}/lib/slang.lib")
    set(PRISM_SLANG_RUNTIME "${PRISM_SLANG_ROOT}/bin/slang.dll")
    set(PRISM_SLANG_RUNTIME_FILES
        "${PRISM_SLANG_RUNTIME}"
        "${PRISM_SLANG_ROOT}/bin/slang-compiler.dll"
        "${PRISM_SLANG_ROOT}/bin/slang-glslang.dll")
else()
    find_library(PRISM_SLANG_LIBRARY
        NAMES slang slang-compiler
        PATHS "${PRISM_SLANG_ROOT}/lib" "${PRISM_SLANG_ROOT}/bin"
        NO_DEFAULT_PATH
    )
    if(NOT PRISM_SLANG_LIBRARY)
        find_library(PRISM_SLANG_LIBRARY NAMES slang slang-compiler)
    endif()
    set(PRISM_SLANG_RUNTIME "${PRISM_SLANG_LIBRARY}")
    set(PRISM_SLANG_RUNTIME_FILES "${PRISM_SLANG_RUNTIME}")
    foreach(PRISM_SLANG_OPTIONAL_LIBRARY
            "${PRISM_SLANG_ROOT}/lib/libslang-glslang.so"
            "${PRISM_SLANG_ROOT}/lib/libslang-compiler.so")
        if(EXISTS "${PRISM_SLANG_OPTIONAL_LIBRARY}")
            list(APPEND PRISM_SLANG_RUNTIME_FILES
                "${PRISM_SLANG_OPTIONAL_LIBRARY}")
        endif()
    endforeach()
endif()

foreach(PRISM_SLANG_FILE
        "${PRISM_SLANG_INCLUDE_DIR}/slang.h"
        "${PRISM_SLANG_INCLUDE_DIR}/slang-com-ptr.h"
        "${PRISM_SLANG_LIBRARY}")
    if(NOT EXISTS "${PRISM_SLANG_FILE}")
        message(FATAL_ERROR
            "The Slang SDK for ${CMAKE_SYSTEM_NAME} is incomplete: ${PRISM_SLANG_FILE} was not found. "
            "Set PRISM_SLANG_ROOT to a Slang v2026.8.1 SDK for the host platform.")
    endif()
endforeach()

add_library(PrismSlang SHARED IMPORTED GLOBAL)
set_target_properties(PrismSlang PROPERTIES
    IMPORTED_LOCATION "${PRISM_SLANG_RUNTIME}"
    INTERFACE_INCLUDE_DIRECTORIES "${PRISM_SLANG_INCLUDE_DIR}"
)
if(WIN32)
    set_target_properties(PrismSlang PROPERTIES
        IMPORTED_IMPLIB "${PRISM_SLANG_LIBRARY}")
endif()

if(PRISM_RENDER_ENABLE_D3D12)
    set(PRISM_WINDOWS_SDK_D3D_PATHS
        "C:/Program Files (x86)/Windows Kits/10/Redist/D3D/x64"
        "C:/Program Files (x86)/Windows Kits/10/bin/${CMAKE_VS_WINDOWS_TARGET_PLATFORM_VERSION}/x64"
    )
    find_file(PRISM_DXCOMPILER_RUNTIME NAMES dxcompiler.dll PATHS ${PRISM_WINDOWS_SDK_D3D_PATHS})
    find_file(PRISM_DXIL_RUNTIME NAMES dxil.dll PATHS ${PRISM_WINDOWS_SDK_D3D_PATHS})
    if(NOT PRISM_DXCOMPILER_RUNTIME OR NOT PRISM_DXIL_RUNTIME)
        message(FATAL_ERROR "dxcompiler.dll and dxil.dll are required by Slang's DXIL target. Install the Windows SDK D3D tools.")
    endif()
    list(APPEND PRISM_SLANG_RUNTIME_FILES
        "${PRISM_DXCOMPILER_RUNTIME}"
        "${PRISM_DXIL_RUNTIME}")
endif()

function(prism_copy_slang_runtime target_name)
    foreach(runtime_file IN LISTS PRISM_SLANG_RUNTIME_FILES)
        add_custom_command(TARGET ${target_name} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${runtime_file}"
                "$<TARGET_FILE_DIR:${target_name}>"
        )
    endforeach()
endfunction()

function(prism_enable_ms_debug_symbols target_name)
    if(MSVC)
        target_compile_options(${target_name} PRIVATE
            "$<$<OR:$<CONFIG:Debug>,$<CONFIG:RelWithDebInfo>>:/Zi>")
        target_link_options(${target_name} PRIVATE
            "$<$<OR:$<CONFIG:Debug>,$<CONFIG:RelWithDebInfo>>:/DEBUG>")
    endif()
endfunction()

if(NOT EXISTS "${PRISM_RENDER_JSON_INCLUDE_DIR}/json.hpp")
    find_path(PRISM_RENDER_JSON_INCLUDE_DIR json.hpp)
endif()

if(NOT PRISM_RENDER_JSON_INCLUDE_DIR)
    message(FATAL_ERROR "json.hpp was not found. Scene serialization requires nlohmann/json.")
endif()

include(FetchContent)

if(WIN32)
    # DirectXMath ships with the Windows SDK and is provided by the MSVC
    # developer environment. Avoid injecting a second SDK include tree ahead
    # of the active Windows Kit.
    set(PRISM_DIRECTXMATH_INCLUDE_DIR "")
else()
    set(PRISM_DIRECTXMATH_INCLUDE_DIR "" CACHE PATH
        "Directory containing DirectXMath.h")
    find_path(PRISM_DIRECTXMATH_INCLUDE_DIR DirectXMath.h)
    if(NOT PRISM_DIRECTXMATH_INCLUDE_DIR
       AND PRISM_RENDER_FETCH_DEPS)
        FetchContent_Declare(
            directxmath
            GIT_REPOSITORY https://github.com/microsoft/DirectXMath.git
            GIT_TAG may2026
            GIT_SHALLOW TRUE
        )
        FetchContent_MakeAvailable(directxmath)
        set(PRISM_DIRECTXMATH_INCLUDE_DIR
            "${directxmath_SOURCE_DIR}/Inc")
    endif()
    if(NOT EXISTS
       "${PRISM_DIRECTXMATH_INCLUDE_DIR}/DirectXMath.h")
        message(FATAL_ERROR
            "DirectXMath headers are required on Linux. Install DirectXMath or enable PRISM_RENDER_FETCH_DEPS.")
    endif()
endif()

set(PRISM_GLFW_TARGET "")

find_package(glfw3 CONFIG QUIET)
if(TARGET glfw)
    set(PRISM_GLFW_TARGET glfw)
elseif(TARGET glfw3::glfw)
    set(PRISM_GLFW_TARGET glfw3::glfw)
elseif(PRISM_RENDER_FETCH_DEPS)
    FetchContent_Declare(
        glfw
        GIT_REPOSITORY https://github.com/glfw/glfw.git
        GIT_TAG 3.4
        GIT_SHALLOW TRUE
    )
    FetchContent_MakeAvailable(glfw)
    set(PRISM_GLFW_TARGET glfw)
endif()

if(PRISM_GLFW_TARGET STREQUAL "")
    message(FATAL_ERROR "GLFW was not found. Install glfw3 or enable PRISM_RENDER_FETCH_DEPS.")
endif()

if(PRISM_RENDER_BUILD_EDITOR)
    find_path(IMGUI_ROOT_DIR imgui.h)

    if(NOT IMGUI_ROOT_DIR AND PRISM_RENDER_FETCH_DEPS)
        FetchContent_Declare(
            imgui
            GIT_REPOSITORY https://github.com/ocornut/imgui.git
            GIT_TAG v1.91.9b-docking
            GIT_SHALLOW TRUE
        )
        FetchContent_MakeAvailable(imgui)
        set(IMGUI_ROOT_DIR ${imgui_SOURCE_DIR})
    endif()

    set(IMGUIZMO_ROOT_DIR "")
    if(EXISTS "${PRISM_RENDER_VENDOR_DIR}/ImGuizmo/src/ImGuizmo.h")
        set(IMGUIZMO_ROOT_DIR "${PRISM_RENDER_VENDOR_DIR}/ImGuizmo/src")
    elseif(EXISTS "${PRISM_RENDER_VENDOR_DIR}/ImGuizmo/ImGuizmo.h")
        set(IMGUIZMO_ROOT_DIR "${PRISM_RENDER_VENDOR_DIR}/ImGuizmo")
    elseif(PRISM_RENDER_FETCH_DEPS)
        FetchContent_Declare(
            imguizmo
            GIT_REPOSITORY https://github.com/CedricGuillemet/ImGuizmo.git
            GIT_TAG 1.10
            GIT_SHALLOW TRUE
            SOURCE_SUBDIR prismrender_no_cmake
        )
        FetchContent_MakeAvailable(imguizmo)
        set(IMGUIZMO_ROOT_DIR ${imguizmo_SOURCE_DIR}/src)
    endif()

    if(NOT IMGUIZMO_ROOT_DIR)
        message(FATAL_ERROR "ImGuizmo was not found. Provide third_party/ImGuizmo or enable PRISM_RENDER_FETCH_DEPS.")
    endif()

    if(NOT IMGUI_ROOT_DIR)
        message(FATAL_ERROR "ImGui was not found. Install ImGui sources or enable PRISM_RENDER_FETCH_DEPS.")
    endif()
endif()

set(TINYGLTF_INCLUDE_DIR "")
if(PRISM_RENDER_ENABLE_GLTF)
    set(PRISM_RENDER_LOCAL_TINYGLTF_HEADER "${PRISM_RENDER_VENDOR_DIR}/tinygltf/tiny_gltf.h")
    if(EXISTS "${PRISM_RENDER_LOCAL_TINYGLTF_HEADER}")
        set(TINYGLTF_INCLUDE_DIR "${PRISM_RENDER_VENDOR_DIR}/tinygltf")
    else()
        find_path(TINYGLTF_INCLUDE_DIR tiny_gltf.h)
    endif()
endif()

if(PRISM_RENDER_ENABLE_GLTF AND TINYGLTF_INCLUDE_DIR STREQUAL "" AND PRISM_RENDER_FETCH_GLTF_DEPS)
    FetchContent_Declare(
        tinygltf
        GIT_REPOSITORY https://github.com/syoyo/tinygltf.git
        GIT_TAG v2.9.5
        GIT_SHALLOW TRUE
    )
    FetchContent_MakeAvailable(tinygltf)
    set(TINYGLTF_INCLUDE_DIR ${tinygltf_SOURCE_DIR})
endif()

if(PRISM_RENDER_ENABLE_GLTF AND TINYGLTF_INCLUDE_DIR STREQUAL "")
    message(STATUS "tinygltf headers were not found. PrismRender will build with glTF loading disabled and fall back to built-in scenes. Set PRISM_RENDER_FETCH_GLTF_DEPS=ON or provide tiny_gltf.h manually to enable it.")
elseif(PRISM_RENDER_ENABLE_GLTF)
    message(STATUS "tinygltf headers found. glTF scene loading is enabled.")
endif()

if(PRISM_RENDER_BUILD_EDITOR)
    set(IMGUI_SOURCES
        ${IMGUI_ROOT_DIR}/imgui.cpp
        ${IMGUI_ROOT_DIR}/imgui_draw.cpp
        ${IMGUI_ROOT_DIR}/imgui_tables.cpp
        ${IMGUI_ROOT_DIR}/imgui_widgets.cpp
        ${IMGUI_ROOT_DIR}/backends/imgui_impl_dx12.cpp
        ${IMGUI_ROOT_DIR}/backends/imgui_impl_glfw.cpp
        ${IMGUI_ROOT_DIR}/backends/imgui_impl_vulkan.cpp
    )

    add_library(imgui STATIC ${IMGUI_SOURCES})
    target_include_directories(imgui
        PUBLIC
            ${PRISM_GLAD_VULKAN_INCLUDE_DIR}
            ${CMAKE_SOURCE_DIR}/src
            ${IMGUI_ROOT_DIR}
            ${IMGUI_ROOT_DIR}/backends
    )
    target_link_libraries(imgui PUBLIC ${PRISM_GLFW_TARGET})
    target_compile_definitions(imgui PUBLIC
        IMGUI_IMPL_API=
        IMGUI_IMPL_VULKAN_NO_PROTOTYPES)
    set_target_properties(imgui PROPERTIES FOLDER ThirdParty)

    add_library(imguizmo STATIC ${IMGUIZMO_ROOT_DIR}/ImGuizmo.cpp)
    target_include_directories(imguizmo PUBLIC ${IMGUIZMO_ROOT_DIR})
    target_link_libraries(imguizmo PUBLIC imgui)
    set_target_properties(imguizmo PROPERTIES FOLDER ThirdParty)
endif()

file(TO_CMAKE_PATH "${CMAKE_SOURCE_DIR}/assets/shaders" PRISM_RENDER_SHADER_DIR)
file(TO_CMAKE_PATH "${CMAKE_SOURCE_DIR}" PRISM_RENDER_PROJECT_DIR)

include(cmake/PrismCoreSources.cmake)
include(cmake/PrismPlatformSources.cmake)
include(cmake/PrismAssetSources.cmake)
include(cmake/PrismSceneSources.cmake)
include(cmake/PrismToolsSources.cmake)
include(cmake/PrismAutomationSources.cmake)
include(cmake/PrismRendererSources.cmake)
include(cmake/PrismRhiSources.cmake)
include(cmake/PrismEngineSources.cmake)
include(cmake/PrismEditorSources.cmake)
include(cmake/PrismApplicationSources.cmake)
add_library(PrismCore STATIC ${PRISM_CORE_SOURCES} ${PRISM_CORE_HEADERS})
add_library(PrismPlatform STATIC ${PRISM_PLATFORM_SOURCES} ${PRISM_PLATFORM_HEADERS})
add_library(PrismRHI STATIC ${PRISM_RHI_SOURCES} ${PRISM_RHI_HEADERS})
add_library(PrismAsset STATIC ${PRISM_ASSET_SOURCES} ${PRISM_ASSET_HEADERS})
add_library(PrismScene STATIC ${PRISM_SCENE_SOURCES} ${PRISM_SCENE_HEADERS})
add_library(PrismRenderer STATIC ${PRISM_RENDERER_SOURCES} ${PRISM_RENDERER_HEADERS})
add_library(PrismEngine STATIC ${PRISM_ENGINE_SOURCES} ${PRISM_ENGINE_HEADERS})
add_library(PrismApplication STATIC
    ${PRISM_APPLICATION_SOURCES}
    ${PRISM_APPLICATION_HEADERS})
add_library(PrismTools STATIC
    ${PRISM_TOOLS_SOURCES}
    ${PRISM_TOOLS_HEADERS})
add_executable(PrismRender src/main.cpp)
if(WIN32)
    add_library(PrismDiagnostics STATIC
        ${PRISM_DIAGNOSTICS_SOURCES}
        ${PRISM_DIAGNOSTICS_HEADERS}
    )
endif()
if(PRISM_RENDER_BUILD_EDITOR)
    add_library(PrismEditor STATIC
        ${PRISM_EDITOR_SOURCES}
        ${PRISM_EDITOR_HEADERS})
endif()
if(PRISM_RENDER_BUILD_HARNESS)
    add_library(PrismAutomation STATIC
        ${PRISM_AUTOMATION_SOURCES}
        ${PRISM_AUTOMATION_HEADERS})
    add_executable(PrismHarness
        src/Automation/HarnessMain.cpp
    )
    add_executable(PrismMcpServer
        src/Automation/McpMain.cpp
    )
endif()

add_executable(PrismGoldenImageCompare
    src/Tools/GoldenImageMain.cpp
)
add_executable(PrismFrameDiagnosticsCompare
    src/Tools/FrameDiagnosticsMain.cpp)
target_include_directories(PrismFrameDiagnosticsCompare PRIVATE
    ${CMAKE_SOURCE_DIR}/src ${PRISM_RENDER_JSON_INCLUDE_DIR})
add_executable(PrismImageVisibilityCheck
    src/Tools/ImageVisibilityMain.cpp
)
if(WIN32)
    add_executable(PrismMinidumpSymbolize
        src/Tools/MinidumpSymbolizeMain.cpp
    )
endif()
target_include_directories(PrismGoldenImageCompare PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_include_directories(PrismImageVisibilityCheck PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_include_directories(PrismTools PUBLIC
    ${CMAKE_SOURCE_DIR}/src
    ${PRISM_RENDER_JSON_INCLUDE_DIR})
target_link_libraries(PrismGoldenImageCompare PRIVATE PrismTools)
target_link_libraries(PrismFrameDiagnosticsCompare PRIVATE PrismTools)
target_link_libraries(PrismImageVisibilityCheck PRIVATE PrismTools)
if(MSVC)
    target_compile_options(PrismGoldenImageCompare PRIVATE /W4 /EHsc)
    target_compile_options(PrismFrameDiagnosticsCompare PRIVATE /W4 /EHsc)
    target_compile_options(PrismImageVisibilityCheck PRIVATE /W4 /EHsc)
else()
    target_compile_options(PrismGoldenImageCompare PRIVATE -Wall -Wextra -Wpedantic)
    target_compile_options(PrismFrameDiagnosticsCompare PRIVATE -Wall -Wextra -Wpedantic)
    target_compile_options(PrismImageVisibilityCheck PRIVATE -Wall -Wextra -Wpedantic)
endif()
set_target_properties(PrismGoldenImageCompare PROPERTIES FOLDER Tools)
set_target_properties(PrismFrameDiagnosticsCompare PROPERTIES FOLDER Tools)
set_target_properties(PrismImageVisibilityCheck PROPERTIES FOLDER Tools)
if(WIN32)
    target_include_directories(PrismMinidumpSymbolize PRIVATE
        ${CMAKE_SOURCE_DIR}/src
    )
    target_link_libraries(PrismMinidumpSymbolize PRIVATE
        PrismDiagnostics
    )
    if(MSVC)
        target_compile_options(PrismMinidumpSymbolize PRIVATE /W4 /EHsc)
    else()
        target_compile_options(PrismMinidumpSymbolize PRIVATE
            -Wall -Wextra -Wpedantic)
    endif()
    set_target_properties(PrismMinidumpSymbolize PROPERTIES FOLDER Tools)
endif()

if(WIN32)
    target_include_directories(PrismDiagnostics
        PUBLIC
            ${CMAKE_SOURCE_DIR}/src
            ${PRISM_RENDER_JSON_INCLUDE_DIR}
    )
endif()
target_include_directories(PrismCore
    PUBLIC
        ${CMAKE_SOURCE_DIR}/src
        ${PRISM_RENDER_JSON_INCLUDE_DIR}
)
target_include_directories(PrismPlatform
    PUBLIC
        ${CMAKE_SOURCE_DIR}/src
)
target_include_directories(PrismAsset
    PUBLIC
        ${CMAKE_SOURCE_DIR}/src
        ${PRISM_RENDER_JSON_INCLUDE_DIR}
    PRIVATE
        ${TINYGLTF_INCLUDE_DIR}
)
target_include_directories(PrismScene
    PUBLIC
        ${CMAKE_SOURCE_DIR}/src
        ${PRISM_RENDER_JSON_INCLUDE_DIR}
        ${PRISM_DIRECTXMATH_INCLUDE_DIR}
)
target_include_directories(PrismRenderer
    PUBLIC
        ${CMAKE_SOURCE_DIR}/src
        ${PRISM_GLAD_VULKAN_INCLUDE_DIR}
        ${PRISM_RENDER_JSON_INCLUDE_DIR}
        ${PRISM_DIRECTXMATH_INCLUDE_DIR}
)
target_include_directories(PrismRHI
    PUBLIC
        ${CMAKE_SOURCE_DIR}/src
        ${PRISM_GLAD_VULKAN_INCLUDE_DIR}
        ${PRISM_RENDER_JSON_INCLUDE_DIR}
        ${PRISM_DIRECTXMATH_INCLUDE_DIR}
)

target_include_directories(PrismEngine PUBLIC
    ${CMAKE_SOURCE_DIR}/src
    ${PRISM_RENDER_JSON_INCLUDE_DIR}
)
target_include_directories(PrismApplication
    PUBLIC
        ${CMAKE_SOURCE_DIR}/src
    PRIVATE
        ${PRISM_RENDER_JSON_INCLUDE_DIR}
        ${PRISM_GLAD_VULKAN_INCLUDE_DIR}
)
if(PRISM_RENDER_BUILD_HARNESS)
    target_include_directories(PrismAutomation PUBLIC
        ${CMAKE_SOURCE_DIR}/src
        ${PRISM_RENDER_JSON_INCLUDE_DIR})
endif()
if(PRISM_RENDER_BUILD_EDITOR)
    target_include_directories(PrismEditor PUBLIC
        ${CMAKE_SOURCE_DIR}/src)
endif()

target_include_directories(PrismRender PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${PRISM_RENDER_JSON_INCLUDE_DIR}
    ${PRISM_GLAD_VULKAN_INCLUDE_DIR}
)

if(PRISM_RENDER_BUILD_HARNESS)
    target_include_directories(PrismHarness PRIVATE
        ${CMAKE_SOURCE_DIR}/src)
    target_include_directories(PrismMcpServer PRIVATE
        ${CMAKE_SOURCE_DIR}/src)
endif()

target_compile_definitions(PrismRenderer
    PUBLIC
        GLFW_INCLUDE_NONE
    PRIVATE
        PRISM_RENDER_PROJECT_DIR="${PRISM_RENDER_PROJECT_DIR}"
        PRISM_RENDER_BUILD_CONFIGURATION="$<CONFIG>"
)
target_compile_definitions(PrismAsset PRIVATE
    PRISM_RENDER_PROJECT_DIR="${PRISM_RENDER_PROJECT_DIR}"
    PRISM_RENDER_BUILD_CONFIGURATION="$<CONFIG>"
)
target_compile_definitions(PrismPlatform PUBLIC GLFW_INCLUDE_NONE)
target_compile_definitions(PrismRHI PUBLIC GLFW_INCLUDE_NONE)
if(WIN32)
    target_compile_definitions(PrismRenderer PUBLIC
        UNICODE
        _UNICODE
        NOMINMAX
        WIN32_LEAN_AND_MEAN
    )
    target_compile_definitions(PrismDiagnostics PUBLIC
        UNICODE
        _UNICODE
        NOMINMAX
        WIN32_LEAN_AND_MEAN
    )
    target_compile_definitions(PrismRHI PUBLIC
        UNICODE
        _UNICODE
        NOMINMAX
        WIN32_LEAN_AND_MEAN
    )
    target_compile_definitions(PrismCore PUBLIC
        UNICODE
        _UNICODE
        NOMINMAX
        WIN32_LEAN_AND_MEAN
    )
    target_compile_definitions(PrismPlatform PUBLIC
        UNICODE
        _UNICODE
        NOMINMAX
        WIN32_LEAN_AND_MEAN
    )
endif()

target_compile_definitions(PrismApplication PRIVATE
        PRISM_RENDER_PROJECT_DIR="${PRISM_RENDER_PROJECT_DIR}"
        PRISM_RENDER_SHADER_DIR="${PRISM_RENDER_SHADER_DIR}"
)
if(PRISM_RENDER_ENABLE_D3D12)
    target_compile_definitions(PrismApplication PRIVATE
        PRISM_RENDER_HAS_D3D12=1)
endif()

if(TINYGLTF_INCLUDE_DIR)
    target_compile_definitions(PrismAsset PRIVATE PRISM_RENDER_HAS_TINYGLTF=1)
    target_compile_definitions(PrismApplication PRIVATE PRISM_RENDER_HAS_TINYGLTF=1)
endif()

target_link_libraries(PrismRenderer
    PUBLIC
        PrismCore
        PrismRHI
        PrismAsset
        PrismScene
)
target_link_libraries(PrismCore PUBLIC Threads::Threads)
target_link_libraries(PrismPlatform PRIVATE
    PrismCore
    ${PRISM_GLFW_TARGET})
target_link_libraries(PrismRHI PUBLIC
    PrismCore
    PrismPlatform)
target_link_libraries(PrismRHI PRIVATE ${PRISM_GLFW_TARGET})
target_link_libraries(PrismEngine PUBLIC PrismCore)
target_link_libraries(PrismAsset PUBLIC
    PrismCore
    PrismRHI)
target_link_libraries(PrismAsset PRIVATE PrismSlang)
target_link_libraries(PrismScene PUBLIC
    PrismCore
    PrismPlatform
    PrismEngine
    PrismAsset
    PrismRHI)
target_link_libraries(PrismScene PRIVATE ${PRISM_GLFW_TARGET})
target_link_libraries(PrismApplication PUBLIC
    PrismCore
    PrismPlatform
    PrismEngine
    PrismAsset
    PrismScene
    PrismRHI
    PrismRenderer)
target_link_libraries(PrismApplication PRIVATE ${PRISM_GLFW_TARGET})
if(PRISM_RENDER_ENABLE_D3D12)
    target_compile_definitions(PrismRHI PUBLIC
        PRISM_RENDER_HAS_D3D12=1)
    target_link_libraries(PrismRHI PUBLIC
        d3d12
        dxgi
        d3dcompiler)
endif()
if(UNIX AND NOT APPLE)
    target_link_libraries(PrismRHI PUBLIC dl pthread)
endif()

if(PRISM_RENDER_BUILD_EDITOR)
    target_link_libraries(PrismEditor PUBLIC
        PrismCore
        PrismPlatform
        PrismEngine
        PrismAsset
        PrismScene
        PrismRHI
        PrismRenderer
        ${PRISM_GLFW_TARGET}
        imgui
        imguizmo
        ole32
        shell32)
    target_link_libraries(PrismApplication PRIVATE PrismEditor)
    target_compile_definitions(PrismApplication PRIVATE PRISM_RENDER_HAS_EDITOR=1)
endif()
if(WIN32)
    target_link_libraries(PrismDiagnostics PUBLIC PrismCore dbghelp)
    target_link_libraries(PrismApplication PRIVATE PrismDiagnostics)
endif()
target_link_libraries(PrismRender PRIVATE PrismApplication PrismCore)
prism_enable_ms_debug_symbols(PrismRender)
if(WIN32)
    target_link_libraries(PrismRender PRIVATE PrismDiagnostics)
endif()
if(PRISM_RENDER_BUILD_HARNESS)
    target_link_libraries(PrismAutomation PUBLIC
        PrismCore
        PrismEngine
        PrismAsset
        PrismRHI
        PrismTools)
    if(WIN32)
        target_link_libraries(PrismAutomation PRIVATE PrismDiagnostics)
    endif()
    target_link_libraries(PrismHarness PRIVATE
        PrismAutomation PrismEngine PrismDiagnostics)
    target_link_libraries(PrismMcpServer PRIVATE
        PrismAutomation PrismEngine PrismDiagnostics)
endif()

if(MSVC)
    if(WIN32)
        target_compile_options(PrismDiagnostics PRIVATE /W4 /EHsc)
    endif()
    target_compile_options(PrismRenderer PRIVATE /W4 /EHsc)
    target_compile_options(PrismCore PRIVATE /W4 /EHsc)
    target_compile_options(PrismPlatform PRIVATE /W4 /EHsc)
    target_compile_options(PrismAsset PRIVATE /W4 /EHsc)
    target_compile_options(PrismScene PRIVATE /W4 /EHsc)
    target_compile_options(PrismRHI PRIVATE /W4 /EHsc)
    target_compile_options(PrismEngine PRIVATE /W4 /EHsc)
    target_compile_options(PrismApplication PRIVATE /W4 /EHsc)
    target_compile_options(PrismTools PRIVATE /W4 /EHsc)
    if(PRISM_RENDER_BUILD_EDITOR)
        target_compile_options(PrismEditor PRIVATE /W4 /EHsc)
    endif()
    target_compile_options(PrismRender PRIVATE /W4 /EHsc)
    if(PRISM_RENDER_BUILD_HARNESS)
        target_compile_options(PrismAutomation PRIVATE /W4 /EHsc)
        target_compile_options(PrismHarness PRIVATE /W4 /EHsc)
        target_compile_options(PrismMcpServer PRIVATE /W4 /EHsc)
    endif()
    if(PRISM_RENDER_BUILD_EDITOR)
        target_compile_options(imgui PRIVATE /W3 /EHsc)
        target_compile_options(imguizmo PRIVATE /W3 /EHsc)
    endif()
else()
    target_compile_options(PrismRenderer PRIVATE -Wall -Wextra -Wpedantic)
    target_compile_options(PrismCore PRIVATE -Wall -Wextra -Wpedantic)
    target_compile_options(PrismPlatform PRIVATE -Wall -Wextra -Wpedantic)
    target_compile_options(PrismAsset PRIVATE -Wall -Wextra -Wpedantic)
    target_compile_options(PrismScene PRIVATE -Wall -Wextra -Wpedantic)
    target_compile_options(PrismRHI PRIVATE -Wall -Wextra -Wpedantic)
    target_compile_options(PrismEngine PRIVATE -Wall -Wextra -Wpedantic)
    target_compile_options(PrismApplication PRIVATE -Wall -Wextra -Wpedantic)
    target_compile_options(PrismTools PRIVATE -Wall -Wextra -Wpedantic)
    if(PRISM_RENDER_BUILD_EDITOR)
        target_compile_options(PrismEditor PRIVATE -Wall -Wextra -Wpedantic)
    endif()
    target_compile_options(PrismRender PRIVATE -Wall -Wextra -Wpedantic)
    if(PRISM_RENDER_BUILD_HARNESS)
        target_compile_options(PrismAutomation PRIVATE -Wall -Wextra -Wpedantic)
        target_compile_options(PrismHarness PRIVATE -Wall -Wextra -Wpedantic)
        target_compile_options(PrismMcpServer PRIVATE -Wall -Wextra -Wpedantic)
    endif()
endif()

source_group(TREE ${CMAKE_SOURCE_DIR}/src FILES ${PRISM_CORE_SOURCES} ${PRISM_CORE_HEADERS})
source_group(TREE ${CMAKE_SOURCE_DIR}/src FILES ${PRISM_PLATFORM_SOURCES} ${PRISM_PLATFORM_HEADERS})
source_group(TREE ${CMAKE_SOURCE_DIR}/src FILES ${PRISM_ASSET_SOURCES} ${PRISM_ASSET_HEADERS})
source_group(TREE ${CMAKE_SOURCE_DIR}/src FILES ${PRISM_SCENE_SOURCES} ${PRISM_SCENE_HEADERS})
source_group(TREE ${CMAKE_SOURCE_DIR}/src FILES ${PRISM_RENDERER_SOURCES} ${PRISM_RENDERER_HEADERS})
source_group(TREE ${CMAKE_SOURCE_DIR}/src FILES ${PRISM_RHI_SOURCES} ${PRISM_RHI_HEADERS})
source_group(TREE ${CMAKE_SOURCE_DIR}/src FILES ${PRISM_ENGINE_SOURCES} ${PRISM_ENGINE_HEADERS})
if(PRISM_RENDER_BUILD_EDITOR)
    source_group(TREE ${CMAKE_SOURCE_DIR}/src FILES ${PRISM_EDITOR_SOURCES} ${PRISM_EDITOR_HEADERS})
endif()
source_group(TREE ${CMAKE_SOURCE_DIR}/src FILES ${PRISM_APPLICATION_SOURCES} ${PRISM_APPLICATION_HEADERS})
source_group(TREE ${CMAKE_SOURCE_DIR}/src FILES ${PRISM_TOOLS_SOURCES} ${PRISM_TOOLS_HEADERS})
if(PRISM_RENDER_BUILD_HARNESS)
    source_group(TREE ${CMAKE_SOURCE_DIR}/src FILES ${PRISM_AUTOMATION_SOURCES} ${PRISM_AUTOMATION_HEADERS})
endif()
source_group(TREE ${CMAKE_SOURCE_DIR}/src FILES src/main.cpp)
set_target_properties(PrismCore PrismPlatform PrismRHI PrismAsset PrismScene PrismRenderer PrismEngine PrismApplication PrismTools PROPERTIES FOLDER Libraries)
if(WIN32)
    source_group(TREE ${CMAKE_SOURCE_DIR}/src FILES ${PRISM_DIAGNOSTICS_SOURCES} ${PRISM_DIAGNOSTICS_HEADERS})
    set_target_properties(PrismDiagnostics PROPERTIES FOLDER Libraries)
endif()
if(PRISM_RENDER_BUILD_EDITOR)
    set_target_properties(PrismEditor PROPERTIES FOLDER Libraries)
endif()
if(PRISM_RENDER_BUILD_HARNESS)
    set_target_properties(PrismAutomation PROPERTIES FOLDER Libraries)
    set_target_properties(
        PrismHarness PrismMcpServer
        PROPERTIES FOLDER Tools)
endif()
prism_copy_slang_runtime(PrismRender)
if(PRISM_RENDER_BUILD_HARNESS)
    prism_copy_slang_runtime(PrismHarness)
    prism_copy_slang_runtime(PrismMcpServer)
endif()

if(BUILD_TESTING)
    add_executable(PrismRhiTranslationTests
        tests/RhiTypeTranslationTests.cpp
        src/RHI/GraphicsApi.cpp
        src/RHI/GraphicsResources.cpp
        src/RHI/GraphicsTypes.cpp
        src/RHI/PipelineState.cpp
        src/RHI/RayTracing.cpp
        src/RHI/Rendering.cpp
        src/RHI/ShaderTypes.cpp
        src/RHI/ShaderLayoutBuilder.cpp
        src/RHI/Vulkan/VulkanTypeConversions.cpp
        src/Renderer/Pipeline/PipelineCache.cpp
    )
    if(PRISM_RENDER_ENABLE_D3D12)
        target_compile_definitions(PrismRhiTranslationTests PRIVATE
            PRISM_RENDER_HAS_D3D12=1)
        target_sources(PrismRhiTranslationTests PRIVATE
            src/RHI/D3D12/D3D12TypeConversions.cpp
            src/RHI/D3D11/D3D11TypeConversions.cpp)
    endif()
    target_include_directories(PrismRhiTranslationTests PRIVATE
        ${CMAKE_SOURCE_DIR}/src
        ${PRISM_GLAD_VULKAN_INCLUDE_DIR}
    )
    if(WIN32)
        target_compile_definitions(PrismRhiTranslationTests PRIVATE
            NOMINMAX WIN32_LEAN_AND_MEAN)
    endif()
    if(MSVC)
        target_compile_options(PrismRhiTranslationTests PRIVATE /W4 /EHsc)
    else()
        target_compile_options(PrismRhiTranslationTests PRIVATE -Wall -Wextra -Wpedantic)
    endif()
    add_test(NAME RhiTypeTranslation COMMAND PrismRhiTranslationTests)
    set_tests_properties(RhiTypeTranslation PROPERTIES
        LABELS "unit"
        TIMEOUT 30)
    set_target_properties(PrismRhiTranslationTests PROPERTIES FOLDER Tests)

    add_executable(PrismShaderCompilerTests
        tests/ShaderCompilerTests.cpp
        src/RHI/ShaderTypes.cpp
        src/RHI/ShaderLayoutBuilder.cpp
        src/Asset/ShaderLoader.cpp
        src/Asset/ShaderManager.cpp
        src/Asset/SlangShaderCompiler.cpp
        src/Core/CpuTrace.cpp
        src/Core/Environment.cpp
    )
    target_include_directories(PrismShaderCompilerTests PRIVATE
        ${CMAKE_SOURCE_DIR}/src
        ${PRISM_RENDER_JSON_INCLUDE_DIR}
    )
    target_compile_definitions(PrismShaderCompilerTests PRIVATE
        NOMINMAX
        WIN32_LEAN_AND_MEAN
        PRISM_RENDER_SHADER_DIR="${PRISM_RENDER_SHADER_DIR}"
    )
    if(PRISM_RENDER_ENABLE_D3D12)
        target_compile_definitions(PrismShaderCompilerTests PRIVATE
            PRISM_RENDER_HAS_D3D12=1)
    endif()
    target_link_libraries(PrismShaderCompilerTests PRIVATE PrismSlang)
    if(MSVC)
        target_compile_options(PrismShaderCompilerTests PRIVATE /W4 /EHsc)
    else()
        target_compile_options(PrismShaderCompilerTests PRIVATE -Wall -Wextra -Wpedantic)
    endif()
    prism_copy_slang_runtime(PrismShaderCompilerTests)
    add_test(NAME ShaderCompiler COMMAND PrismShaderCompilerTests)
    set_tests_properties(ShaderCompiler PROPERTIES
        LABELS "unit;shader"
        TIMEOUT 60)
    set_target_properties(PrismShaderCompilerTests PROPERTIES FOLDER Tests)

    add_executable(PrismVulkanRuntimeTests
        tests/VulkanRuntimeTests.cpp
        src/RHI/Vulkan/VulkanLoader.cpp
    )
    target_include_directories(PrismVulkanRuntimeTests PRIVATE
        ${CMAKE_SOURCE_DIR}/src
        ${PRISM_GLAD_VULKAN_INCLUDE_DIR}
    )
    target_compile_definitions(PrismVulkanRuntimeTests PRIVATE GLFW_INCLUDE_NONE)
    target_link_libraries(PrismVulkanRuntimeTests PRIVATE ${PRISM_GLFW_TARGET})
    if(MSVC)
        target_compile_options(PrismVulkanRuntimeTests PRIVATE /W4 /EHsc)
    else()
        target_compile_options(PrismVulkanRuntimeTests PRIVATE -Wall -Wextra -Wpedantic)
    endif()
    add_test(NAME VulkanRuntime COMMAND PrismVulkanRuntimeTests)
    set_tests_properties(VulkanRuntime PROPERTIES
        LABELS "gpu;runtime"
        TIMEOUT 60)
    set_target_properties(PrismVulkanRuntimeTests PROPERTIES FOLDER Tests)

    add_executable(PrismRenderGraphTests
        tests/RenderGraphTests.cpp
        src/Core/CpuTrace.cpp
        src/Core/Environment.cpp
        src/RHI/DeferredCommandContext.cpp
        src/Renderer/QueueSchedulingCostModel.cpp
        src/Renderer/RenderGraph.cpp
        src/Renderer/RenderGraphResources.cpp
        src/Renderer/RenderGraphDiagnostics.cpp
        src/Renderer/SharedRenderGraphFrontend.cpp
        src/Renderer/RhiPasses.cpp
        src/RHI/GraphicsApi.cpp
        src/RHI/GraphicsResources.cpp
        src/RHI/GraphicsTypes.cpp
        src/RHI/Rendering.cpp
    )
    target_include_directories(PrismRenderGraphTests PRIVATE
        ${CMAKE_SOURCE_DIR}/src
        ${PRISM_RENDER_JSON_INCLUDE_DIR}
    )
    # Feature-owned AddPasses implementations live in PrismRenderer. The test
    # keeps lightweight mock RHI sources locally while resolving feature graph
    # schemas from the production renderer target.
    target_link_libraries(PrismRenderGraphTests PRIVATE PrismRenderer)
    target_compile_definitions(PrismRenderGraphTests PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
    if(MSVC)
        target_compile_options(PrismRenderGraphTests PRIVATE /W4 /EHsc)
    else()
        target_compile_options(PrismRenderGraphTests PRIVATE -Wall -Wextra -Wpedantic)
    endif()
    add_test(NAME RenderGraph COMMAND PrismRenderGraphTests)
    set_tests_properties(RenderGraph PROPERTIES
        LABELS "unit;renderer"
        TIMEOUT 60)
    set_target_properties(PrismRenderGraphTests PROPERTIES FOLDER Tests)

    add_executable(PrismRenderGraphCompilerTests
        tests/RenderGraphCompilerTests.cpp)
    target_include_directories(PrismRenderGraphCompilerTests PRIVATE
        ${CMAKE_SOURCE_DIR}/src
        ${PRISM_RENDER_JSON_INCLUDE_DIR})
    target_link_libraries(PrismRenderGraphCompilerTests PRIVATE PrismRenderer)
    target_compile_definitions(PrismRenderGraphCompilerTests PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
    if(MSVC)
        target_compile_options(PrismRenderGraphCompilerTests PRIVATE /W4 /EHsc)
    else()
        target_compile_options(PrismRenderGraphCompilerTests PRIVATE -Wall -Wextra -Wpedantic)
    endif()
    add_test(NAME RenderGraphCompiler COMMAND PrismRenderGraphCompilerTests)
    set_tests_properties(RenderGraphCompiler PROPERTIES
        LABELS "unit;renderer"
        TIMEOUT 60)
    set_target_properties(PrismRenderGraphCompilerTests PROPERTIES FOLDER Tests)

    add_executable(PrismRenderFeatureLifecycleTests
        tests/RenderFeatureLifecycleTests.cpp)
    target_include_directories(PrismRenderFeatureLifecycleTests PRIVATE
        ${CMAKE_SOURCE_DIR}/src)
    target_link_libraries(PrismRenderFeatureLifecycleTests PRIVATE PrismRenderer)
    target_compile_definitions(PrismRenderFeatureLifecycleTests PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
    if(MSVC)
        target_compile_options(PrismRenderFeatureLifecycleTests PRIVATE /W4 /EHsc)
    else()
        target_compile_options(PrismRenderFeatureLifecycleTests PRIVATE -Wall -Wextra -Wpedantic)
    endif()
    add_test(NAME RenderFeatureLifecycle COMMAND PrismRenderFeatureLifecycleTests)
    set_tests_properties(RenderFeatureLifecycle PROPERTIES
        LABELS "unit;renderer"
        TIMEOUT 30)
    set_target_properties(PrismRenderFeatureLifecycleTests PROPERTIES FOLDER Tests)

    add_executable(PrismVirtualTextureCacheTests
        tests/VirtualTextureCacheTests.cpp)
    target_include_directories(PrismVirtualTextureCacheTests PRIVATE
        ${CMAKE_SOURCE_DIR}/src)
    target_link_libraries(PrismVirtualTextureCacheTests PRIVATE PrismRenderer)
    target_compile_definitions(PrismVirtualTextureCacheTests PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
    if(MSVC)
        target_compile_options(PrismVirtualTextureCacheTests PRIVATE /W4 /EHsc)
    else()
        target_compile_options(PrismVirtualTextureCacheTests PRIVATE -Wall -Wextra -Wpedantic)
    endif()
    add_test(NAME VirtualTextureCache COMMAND PrismVirtualTextureCacheTests)
    set_tests_properties(VirtualTextureCache PROPERTIES
        LABELS "unit;renderer"
        TIMEOUT 30)
    set_target_properties(PrismVirtualTextureCacheTests PROPERTIES FOLDER Tests)

    add_executable(PrismFeatureGraphContractTests
        tests/FeatureGraphContractTests.cpp)
    target_include_directories(PrismFeatureGraphContractTests PRIVATE
        ${CMAKE_SOURCE_DIR}/src)
    target_compile_definitions(PrismFeatureGraphContractTests PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
    if(MSVC)
        target_compile_options(PrismFeatureGraphContractTests PRIVATE /W4 /EHsc)
    else()
        target_compile_options(PrismFeatureGraphContractTests PRIVATE -Wall -Wextra -Wpedantic)
    endif()
    add_test(NAME FeatureGraphContract COMMAND PrismFeatureGraphContractTests)
    set_tests_properties(FeatureGraphContract PROPERTIES
        LABELS "unit;renderer"
        TIMEOUT 30)
    set_target_properties(PrismFeatureGraphContractTests PROPERTIES FOLDER Tests)

    add_executable(PrismFrameDiagnosticsTests
        tests/FrameDiagnosticsTests.cpp
        tests/FrameCaptureSequenceTests.cpp
        src/Core/Application/FrameCaptureSequence.cpp
        src/Renderer/FrameDiagnostics.cpp
        src/Tools/FrameDiagnosticsComparison.cpp)
    target_include_directories(PrismFrameDiagnosticsTests PRIVATE
        ${CMAKE_SOURCE_DIR}/src ${PRISM_RENDER_JSON_INCLUDE_DIR})
    if(MSVC)
        target_compile_options(PrismFrameDiagnosticsTests PRIVATE /W4 /EHsc)
    else()
        target_compile_options(PrismFrameDiagnosticsTests PRIVATE -Wall -Wextra -Wpedantic)
    endif()
    add_test(NAME FrameDiagnostics COMMAND PrismFrameDiagnosticsTests)
    set_tests_properties(FrameDiagnostics PROPERTIES LABELS "unit;renderer" TIMEOUT 60)
    set_target_properties(PrismFrameDiagnosticsTests PROPERTIES FOLDER Tests)

    add_executable(PrismFramePerformanceTests
        tests/FramePerformanceTests.cpp
        src/RHI/FramePacing.cpp
        src/Core/Application/FramePacingAutomation.cpp
        src/Core/Application/FrameRateLimiter.cpp
        src/Core/Application/FramePerformanceRecorder.cpp)
    target_include_directories(PrismFramePerformanceTests PRIVATE
        ${CMAKE_SOURCE_DIR}/src ${PRISM_RENDER_JSON_INCLUDE_DIR})
    target_link_libraries(PrismFramePerformanceTests PRIVATE Threads::Threads)
    if(MSVC)
        target_compile_options(PrismFramePerformanceTests PRIVATE /W4 /EHsc)
    else()
        target_compile_options(PrismFramePerformanceTests PRIVATE -Wall -Wextra -Wpedantic)
    endif()
    add_test(NAME FramePerformance COMMAND PrismFramePerformanceTests)
    set_tests_properties(FramePerformance PROPERTIES LABELS "unit;application" TIMEOUT 60)
    set_target_properties(PrismFramePerformanceTests PROPERTIES FOLDER Tests)

    add_executable(PrismPresentationPacingTests
        tests/PresentationPacingTests.cpp)
    target_link_libraries(PrismPresentationPacingTests PRIVATE PrismRHI)
    if(MSVC)
        target_compile_options(PrismPresentationPacingTests PRIVATE /W4 /EHsc)
    else()
        target_compile_options(PrismPresentationPacingTests PRIVATE
            -Wall -Wextra -Wpedantic)
    endif()
    add_test(NAME PresentationPacing COMMAND PrismPresentationPacingTests)
    set_tests_properties(PresentationPacing PROPERTIES
        LABELS "unit;rhi" TIMEOUT 60)
    set_target_properties(PrismPresentationPacingTests PROPERTIES FOLDER Tests)

    add_executable(PrismFrameProfilerTests
        tests/FrameProfilerTests.cpp
        src/Core/Profiling/FrameProfiler.cpp
        src/Core/Profiling/FrameProfilerReport.cpp
        src/Core/Profiling/ProfilingLevelSchedule.cpp)
    target_include_directories(PrismFrameProfilerTests PRIVATE
        ${CMAKE_SOURCE_DIR}/src ${PRISM_RENDER_JSON_INCLUDE_DIR})
    target_link_libraries(PrismFrameProfilerTests PRIVATE Threads::Threads)
    if(MSVC)
        target_compile_options(PrismFrameProfilerTests PRIVATE /W4 /EHsc)
    else()
        target_compile_options(PrismFrameProfilerTests PRIVATE -Wall -Wextra -Wpedantic)
    endif()
    add_test(NAME FrameProfiler COMMAND PrismFrameProfilerTests)
    set_tests_properties(FrameProfiler PROPERTIES LABELS "unit;application" TIMEOUT 60)
    set_target_properties(PrismFrameProfilerTests PROPERTIES FOLDER Tests)

    add_executable(PrismTaskSchedulerTests
        tests/TaskSchedulerTests.cpp)
    target_include_directories(PrismTaskSchedulerTests PRIVATE
        ${CMAKE_SOURCE_DIR}/src)
    target_link_libraries(PrismTaskSchedulerTests PRIVATE PrismCore)
    if(MSVC)
        target_compile_options(PrismTaskSchedulerTests PRIVATE /W4 /EHsc)
    else()
        target_compile_options(PrismTaskSchedulerTests PRIVATE
            -Wall -Wextra -Wpedantic)
    endif()
    add_test(NAME TaskScheduler COMMAND PrismTaskSchedulerTests)
    set_tests_properties(TaskScheduler PROPERTIES
        LABELS "unit;core;threading"
        TIMEOUT 60)
    set_target_properties(PrismTaskSchedulerTests PROPERTIES FOLDER Tests)

    add_executable(PrismRenderFrameQueueTests
        tests/RenderFrameQueueTests.cpp)
    target_include_directories(PrismRenderFrameQueueTests PRIVATE
        ${CMAKE_SOURCE_DIR}/src)
    target_link_libraries(PrismRenderFrameQueueTests PRIVATE
        PrismApplication)
    if(MSVC)
        target_compile_options(PrismRenderFrameQueueTests PRIVATE
            /W4 /EHsc)
    else()
        target_compile_options(PrismRenderFrameQueueTests PRIVATE
            -Wall -Wextra -Wpedantic)
    endif()
    add_test(NAME RenderFrameQueue COMMAND PrismRenderFrameQueueTests)
    set_tests_properties(RenderFrameQueue PROPERTIES
        LABELS "unit;application;threading"
        TIMEOUT 60)
    set_target_properties(PrismRenderFrameQueueTests PROPERTIES
        FOLDER Tests)

    add_executable(PrismSceneViewRefreshPolicyTests
        tests/SceneViewRefreshPolicyTests.cpp
        src/Core/Profiling/FrameProfiler.cpp
        src/Core/Profiling/SceneViewRefreshController.cpp)
    target_include_directories(PrismSceneViewRefreshPolicyTests PRIVATE
        ${CMAKE_SOURCE_DIR}/src)
    target_link_libraries(PrismSceneViewRefreshPolicyTests PRIVATE
        Threads::Threads)
    if(MSVC)
        target_compile_options(PrismSceneViewRefreshPolicyTests PRIVATE
            /W4 /EHsc)
    else()
        target_compile_options(PrismSceneViewRefreshPolicyTests PRIVATE
            -Wall -Wextra -Wpedantic)
    endif()
    add_test(NAME SceneViewRefreshPolicy
        COMMAND PrismSceneViewRefreshPolicyTests)
    set_tests_properties(SceneViewRefreshPolicy PROPERTIES
        LABELS "unit;application"
        TIMEOUT 60)
    set_target_properties(PrismSceneViewRefreshPolicyTests PROPERTIES
        FOLDER Tests)

    add_executable(PrismApplicationCompositionTests
        tests/ApplicationCompositionTests.cpp
        src/Core/Application/ApplicationComposition.cpp)
    target_include_directories(PrismApplicationCompositionTests PRIVATE
        ${CMAKE_SOURCE_DIR}/src)
    if(MSVC)
        target_compile_options(PrismApplicationCompositionTests PRIVATE
            /W4 /EHsc)
    else()
        target_compile_options(PrismApplicationCompositionTests PRIVATE
            -Wall -Wextra -Wpedantic)
    endif()
    add_test(NAME ApplicationComposition
        COMMAND PrismApplicationCompositionTests)
    set_tests_properties(ApplicationComposition PROPERTIES
        LABELS "unit;application"
        TIMEOUT 60)
    set_target_properties(PrismApplicationCompositionTests PROPERTIES
        FOLDER Tests)

    add_executable(PrismAssetRuntimeCoordinatorTests
        tests/AssetRuntimeCoordinatorTests.cpp
        src/Core/Application/AssetRuntimeCoordinator.cpp)
    target_include_directories(PrismAssetRuntimeCoordinatorTests PRIVATE
        ${CMAKE_SOURCE_DIR}/src)
    target_compile_definitions(PrismAssetRuntimeCoordinatorTests PRIVATE
        PRISM_RENDER_PROJECT_DIR="${PRISM_RENDER_PROJECT_DIR}")
    target_link_libraries(PrismAssetRuntimeCoordinatorTests PRIVATE
        PrismRenderer)
    if(MSVC)
        target_compile_options(PrismAssetRuntimeCoordinatorTests PRIVATE
            /W4 /EHsc)
    else()
        target_compile_options(PrismAssetRuntimeCoordinatorTests PRIVATE
            -Wall -Wextra -Wpedantic)
    endif()
    add_test(NAME AssetRuntimeCoordinator
        COMMAND PrismAssetRuntimeCoordinatorTests)
    set_tests_properties(AssetRuntimeCoordinator PROPERTIES
        LABELS "unit;application;asset"
        TIMEOUT 60)
    set_target_properties(PrismAssetRuntimeCoordinatorTests PROPERTIES
        FOLDER Tests)

    add_executable(PrismSceneSessionTests
        tests/SceneSessionTests.cpp)
    target_include_directories(PrismSceneSessionTests PRIVATE
        ${CMAKE_SOURCE_DIR}/src)
    target_compile_definitions(PrismSceneSessionTests PRIVATE
        PRISM_RENDER_PROJECT_DIR="${PRISM_RENDER_PROJECT_DIR}")
    target_link_libraries(PrismSceneSessionTests PRIVATE
        PrismRenderer)
    if(MSVC)
        target_compile_options(PrismSceneSessionTests PRIVATE
            /W4 /EHsc)
    else()
        target_compile_options(PrismSceneSessionTests PRIVATE
            -Wall -Wextra -Wpedantic)
    endif()
    add_test(NAME SceneSession
        COMMAND PrismSceneSessionTests)
    set_tests_properties(SceneSession PROPERTIES
        LABELS "unit;scene;application"
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
        TIMEOUT 60)
    set_target_properties(PrismSceneSessionTests PROPERTIES
        FOLDER Tests)

    add_executable(PrismRenderFrameCoordinatorTests
        tests/RenderFrameCoordinatorTests.cpp)
    if(WIN32)
        target_sources(PrismRenderFrameCoordinatorTests PRIVATE
            src/Core/ProcessDiagnostics.cpp)
    else()
        target_sources(PrismRenderFrameCoordinatorTests PRIVATE
            src/Core/ProcessDiagnosticsPosix.cpp)
    endif()
    target_include_directories(PrismRenderFrameCoordinatorTests PRIVATE
        ${CMAKE_SOURCE_DIR}/src)
    target_link_libraries(PrismRenderFrameCoordinatorTests PRIVATE
        PrismRenderer)
    if(WIN32)
        target_link_libraries(PrismRenderFrameCoordinatorTests PRIVATE
            PrismDiagnostics)
    endif()
    if(MSVC)
        target_compile_options(PrismRenderFrameCoordinatorTests PRIVATE
            /W4 /EHsc)
    else()
        target_compile_options(PrismRenderFrameCoordinatorTests PRIVATE
            -Wall -Wextra -Wpedantic)
    endif()
    add_test(NAME RenderFrameCoordinator
        COMMAND PrismRenderFrameCoordinatorTests)
    set_tests_properties(RenderFrameCoordinator PROPERTIES
        LABELS "unit;renderer;application"
        TIMEOUT 60)
    set_target_properties(PrismRenderFrameCoordinatorTests PROPERTIES
        FOLDER Tests)

    if(PRISM_RENDER_BUILD_EDITOR)
        add_executable(PrismUiDrawPacketTests
            tests/UiDrawPacketTests.cpp)
        target_include_directories(PrismUiDrawPacketTests PRIVATE
            ${CMAKE_SOURCE_DIR}/src)
        target_link_libraries(PrismUiDrawPacketTests PRIVATE
            PrismEditor)
        if(MSVC)
            target_compile_options(PrismUiDrawPacketTests PRIVATE
                /W4 /EHsc)
        else()
            target_compile_options(PrismUiDrawPacketTests PRIVATE
                -Wall -Wextra -Wpedantic)
        endif()
        add_test(NAME UiDrawPacket
            COMMAND PrismUiDrawPacketTests)
        set_tests_properties(UiDrawPacket PROPERTIES
            LABELS "unit;editor;threading"
            TIMEOUT 60)
        set_target_properties(PrismUiDrawPacketTests PROPERTIES
            FOLDER Tests)

        add_executable(PrismEditorCoordinatorTests
            tests/EditorCoordinatorTests.cpp
            src/Core/Profiling/FrameProfiler.cpp
            src/Core/Profiling/SceneViewRefreshController.cpp)
        if(WIN32)
            target_sources(PrismEditorCoordinatorTests PRIVATE
                src/Core/ProcessDiagnostics.cpp)
        else()
            target_sources(PrismEditorCoordinatorTests PRIVATE
                src/Core/ProcessDiagnosticsPosix.cpp)
        endif()
        target_include_directories(PrismEditorCoordinatorTests PRIVATE
            ${CMAKE_SOURCE_DIR}/src)
        target_link_libraries(PrismEditorCoordinatorTests PRIVATE
            PrismEditor)
        if(WIN32)
            target_link_libraries(PrismEditorCoordinatorTests PRIVATE
                PrismDiagnostics)
        endif()
        if(MSVC)
            target_compile_options(PrismEditorCoordinatorTests PRIVATE
                /W4 /EHsc)
        else()
            target_compile_options(PrismEditorCoordinatorTests PRIVATE
                -Wall -Wextra -Wpedantic)
        endif()
        add_test(NAME EditorCoordinator
            COMMAND PrismEditorCoordinatorTests)
        set_tests_properties(EditorCoordinator PROPERTIES
            LABELS "unit;editor;application"
            TIMEOUT 60)
        set_target_properties(PrismEditorCoordinatorTests PROPERTIES
            FOLDER Tests)
    endif()

    add_executable(PrismPerformanceProfilerPanelModelTests
        tests/PerformanceProfilerPanelModelTests.cpp
        src/UI/PerformanceProfilerPanelModel.cpp)
    target_include_directories(PrismPerformanceProfilerPanelModelTests
        PRIVATE ${CMAKE_SOURCE_DIR}/src)
    if(MSVC)
        target_compile_options(PrismPerformanceProfilerPanelModelTests
            PRIVATE /W4 /EHsc)
    else()
        target_compile_options(PrismPerformanceProfilerPanelModelTests
            PRIVATE -Wall -Wextra -Wpedantic)
    endif()
    add_test(NAME PerformanceProfilerPanelModel
        COMMAND PrismPerformanceProfilerPanelModelTests)
    set_tests_properties(PerformanceProfilerPanelModel PROPERTIES
        LABELS "unit;application;ui"
        TIMEOUT 60)
    set_target_properties(PrismPerformanceProfilerPanelModelTests
        PROPERTIES FOLDER Tests)

    if(WIN32)
        find_program(PRISM_PWSH_EXECUTABLE
            NAMES pwsh
            REQUIRED)
        add_test(NAME ArchitectureFrameProfilerReport
            COMMAND ${PRISM_PWSH_EXECUTABLE}
                -NoLogo -NoProfile -ExecutionPolicy Bypass
                -File
                ${CMAKE_SOURCE_DIR}/tests/scripts/ArchitectureFrameProfilerTests.ps1)
        set_tests_properties(ArchitectureFrameProfilerReport PROPERTIES
            LABELS "unit;application;script"
            TIMEOUT 60)
        add_test(NAME ArchitecturePerformanceScenarioSummary
            COMMAND ${PRISM_PWSH_EXECUTABLE}
                -NoLogo -NoProfile -ExecutionPolicy Bypass
                -File
                ${CMAKE_SOURCE_DIR}/tests/scripts/ArchitecturePerformanceScenarioSummaryTests.ps1)
        set_tests_properties(ArchitecturePerformanceScenarioSummary PROPERTIES
            LABELS "unit;application;script"
            TIMEOUT 60)
    endif()

    add_executable(PrismGoldenImageTests
        tests/GoldenImageTests.cpp
    )
    target_include_directories(PrismGoldenImageTests PRIVATE ${CMAKE_SOURCE_DIR}/src)
    target_link_libraries(PrismGoldenImageTests PRIVATE PrismTools)
    if(MSVC)
        target_compile_options(PrismGoldenImageTests PRIVATE /W4 /EHsc)
    else()
        target_compile_options(PrismGoldenImageTests PRIVATE -Wall -Wextra -Wpedantic)
    endif()
    add_test(NAME GoldenImageMetrics COMMAND PrismGoldenImageTests)
    set_tests_properties(GoldenImageMetrics PROPERTIES
        LABELS "unit;image"
        TIMEOUT 30)
    set_target_properties(PrismGoldenImageTests PROPERTIES FOLDER Tests)

    add_executable(PrismOceanSettingsTests
        tests/OceanSettingsTests.cpp
        src/Renderer/Features/Ocean/OceanFeature.cpp
        src/Renderer/Features/Ocean/OceanSettings.cpp
        src/Renderer/Features/Ocean/OceanSpectrumMath.cpp
        src/Renderer/Features/Ocean/OceanFft.cpp
        src/Renderer/Features/Ocean/OceanSpectrumGenerator.cpp
        src/Renderer/Features/Ocean/OceanFoamSimulation.cpp
        src/Renderer/Features/Ocean/OceanMapBuilder.cpp
        src/Renderer/Features/Ocean/LocalWaveSimulation.cpp
        src/Renderer/Features/Ocean/LocalWaveGpuResources.cpp
        src/Renderer/Features/Ocean/WaterOpticsSettings.cpp
        src/Renderer/Features/Ocean/OceanQuadtree.cpp
        src/Renderer/Features/Ocean/OceanQueryService.cpp
        src/Renderer/Pipeline/ScenePipelinePlan.cpp
    )
    target_include_directories(PrismOceanSettingsTests PRIVATE
        ${CMAKE_SOURCE_DIR}/src
        ${PRISM_DIRECTXMATH_INCLUDE_DIR})
    target_compile_definitions(PrismOceanSettingsTests PRIVATE
        PRISM_LOCAL_WAVE_RESOURCES_NO_GPU=1)
    if(MSVC)
        target_compile_options(PrismOceanSettingsTests PRIVATE /W4 /EHsc)
    else()
        target_compile_options(PrismOceanSettingsTests PRIVATE -Wall -Wextra -Wpedantic)
    endif()
    add_test(NAME OceanSettings COMMAND PrismOceanSettingsTests)
    set_tests_properties(OceanSettings PROPERTIES
        LABELS "unit;renderer"
        TIMEOUT 30)
    set_target_properties(PrismOceanSettingsTests PROPERTIES FOLDER Tests)

    add_executable(PrismWaterOpticsTests
        tests/WaterOpticsTests.cpp
        src/Renderer/Features/Ocean/OceanSettings.cpp
        src/Renderer/Features/Ocean/WaterOpticsHistory.cpp
        src/Renderer/Features/Ocean/WaterCaustics.cpp
        src/Renderer/Features/Ocean/WaterMediumState.cpp
        src/Renderer/Features/Ocean/WaterVolumetricHistory.cpp
        src/Renderer/Features/Ocean/WaterRefractionModel.cpp
        src/Renderer/Features/Ocean/WaterOpticalModel.cpp
        src/Renderer/Features/Ocean/WaterOpticsSettings.cpp)
    target_include_directories(PrismWaterOpticsTests PRIVATE
        ${CMAKE_SOURCE_DIR}/src
        ${PRISM_DIRECTXMATH_INCLUDE_DIR})
    if(MSVC)
        target_compile_options(PrismWaterOpticsTests PRIVATE /W4 /EHsc)
    else()
        target_compile_options(PrismWaterOpticsTests PRIVATE
            -Wall -Wextra -Wpedantic)
    endif()
    add_test(NAME WaterOptics COMMAND PrismWaterOpticsTests)
    set_tests_properties(WaterOptics PROPERTIES
        LABELS "unit;renderer"
        TIMEOUT 30)
    set_target_properties(PrismWaterOpticsTests PROPERTIES FOLDER Tests)

    add_executable(PrismPbfFluidGpuTests tests/PbfFluidGpuTests.cpp)
    target_link_libraries(PrismPbfFluidGpuTests PRIVATE PrismRenderer)
    target_compile_definitions(PrismPbfFluidGpuTests PRIVATE
        PRISM_RENDER_SHADER_DIR="${PRISM_RENDER_SHADER_DIR}"
        PRISM_RENDER_TEST_SHADER_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/shaders")
    prism_copy_slang_runtime(PrismPbfFluidGpuTests)
    foreach(api IN ITEMS d3d12 vulkan)
        if(api STREQUAL "d3d12" AND NOT PRISM_RENDER_ENABLE_D3D12)
            continue()
        endif()
        add_test(NAME PbfFluidGpu_${api} COMMAND PrismPbfFluidGpuTests ${api})
        set_tests_properties(PbfFluidGpu_${api} PROPERTIES
            LABELS "gpu;renderer;fluid;${api}"
            ENVIRONMENT "PRISM_RENDER_HEADLESS=1;PRISM_RENDER_GPU_VALIDATION=1;PRISM_RENDER_QUEUE_COST_MODEL_PATH=${CMAKE_CURRENT_BINARY_DIR}/pbf-gpu-${api}.queue-cache.json"
            TIMEOUT 180)
    endforeach()
    set_target_properties(PrismPbfFluidGpuTests PROPERTIES FOLDER Tests)

    add_executable(PrismSkyAtmosphereGpuTests tests/SkyAtmosphereGpuTests.cpp src/Core/ProcessDiagnostics.cpp)
    target_link_libraries(PrismSkyAtmosphereGpuTests PRIVATE
        PrismRenderer
        ${PRISM_GLFW_TARGET})
    if(WIN32)
        target_link_libraries(PrismSkyAtmosphereGpuTests PRIVATE PrismDiagnostics)
    endif()
    target_compile_definitions(PrismSkyAtmosphereGpuTests PRIVATE PRISM_RENDER_SHADER_DIR="${PRISM_RENDER_SHADER_DIR}")
    prism_copy_slang_runtime(PrismSkyAtmosphereGpuTests)
    if(PRISM_RENDER_ENABLE_D3D12)
        add_test(NAME SkyAtmosphereGpuD3D12 COMMAND PrismSkyAtmosphereGpuTests d3d12)
        set_tests_properties(SkyAtmosphereGpuD3D12 PROPERTIES
            LABELS "gpu;renderer;d3d12"
            ENVIRONMENT "PRISM_RENDER_HEADLESS=1;PRISM_RENDER_GPU_VALIDATION=1;PRISM_RENDER_QUEUE_COST_MODEL_PATH=${CMAKE_CURRENT_BINARY_DIR}/sky-gpu-d3d12.queue-cache.json"
            TIMEOUT 120)
    endif()
    add_test(NAME SkyAtmosphereGpuVulkan COMMAND PrismSkyAtmosphereGpuTests vulkan)
    set_tests_properties(SkyAtmosphereGpuVulkan PROPERTIES
        LABELS "gpu;renderer;vulkan"
        ENVIRONMENT "PRISM_RENDER_HEADLESS=1;PRISM_RENDER_QUEUE_COST_MODEL_PATH=${CMAKE_CURRENT_BINARY_DIR}/sky-gpu-vulkan.queue-cache.json"
        FAIL_REGULAR_EXPRESSION "\\[Vulkan validation\\]"
        TIMEOUT 120)
    set_target_properties(PrismSkyAtmosphereGpuTests PROPERTIES FOLDER Tests)

    add_executable(PrismGpuDrivenVisibilityGpuTests
        tests/GpuDrivenVisibilityGpuTests.cpp
        src/Core/ProcessDiagnostics.cpp)
    target_link_libraries(PrismGpuDrivenVisibilityGpuTests PRIVATE
        PrismRenderer
        ${PRISM_GLFW_TARGET})
    if(WIN32)
        target_link_libraries(PrismGpuDrivenVisibilityGpuTests PRIVATE
            PrismDiagnostics)
    endif()
    target_compile_definitions(PrismGpuDrivenVisibilityGpuTests PRIVATE
        PRISM_RENDER_SHADER_DIR="${PRISM_RENDER_SHADER_DIR}")
    prism_copy_slang_runtime(PrismGpuDrivenVisibilityGpuTests)
    if(PRISM_RENDER_ENABLE_D3D12)
        add_test(NAME GpuDrivenVisibilityGpuD3D12
            COMMAND PrismGpuDrivenVisibilityGpuTests d3d12)
        set_tests_properties(GpuDrivenVisibilityGpuD3D12 PROPERTIES
            LABELS "gpu;renderer;d3d12"
            ENVIRONMENT "PRISM_RENDER_HEADLESS=1;PRISM_RENDER_GPU_VALIDATION=1;PRISM_RENDER_QUEUE_COST_MODEL_PATH=${CMAKE_CURRENT_BINARY_DIR}/gpu-driven-d3d12.queue-cache.json"
            TIMEOUT 180)
    endif()
    add_test(NAME GpuDrivenVisibilityGpuVulkan
        COMMAND PrismGpuDrivenVisibilityGpuTests vulkan)
    set_tests_properties(GpuDrivenVisibilityGpuVulkan PROPERTIES
        LABELS "gpu;renderer;vulkan"
        ENVIRONMENT "PRISM_RENDER_HEADLESS=1;PRISM_RENDER_QUEUE_COST_MODEL_PATH=${CMAKE_CURRENT_BINARY_DIR}/gpu-driven-vulkan.queue-cache.json"
        FAIL_REGULAR_EXPRESSION "\\[Vulkan validation\\]"
        TIMEOUT 180)
    set_target_properties(PrismGpuDrivenVisibilityGpuTests PROPERTIES
        FOLDER Tests)

    foreach(binding_case IN ITEMS optional ocean water)
        if(PRISM_RENDER_ENABLE_D3D12)
            add_test(NAME FeatureBindings_${binding_case}_D3D12
                COMMAND PrismSkyAtmosphereGpuTests d3d12 --${binding_case}-bindings)
            set_tests_properties(FeatureBindings_${binding_case}_D3D12 PROPERTIES
                LABELS "gpu;renderer;d3d12"
                ENVIRONMENT "PRISM_RENDER_HEADLESS=1;PRISM_RENDER_GPU_VALIDATION=1;PRISM_RENDER_QUEUE_COST_MODEL_PATH=${CMAKE_CURRENT_BINARY_DIR}/bindings-${binding_case}-d3d12.queue-cache.json"
                TIMEOUT 180)
        endif()
        add_test(NAME FeatureBindings_${binding_case}_Vulkan
            COMMAND PrismSkyAtmosphereGpuTests vulkan --${binding_case}-bindings)
        set_tests_properties(FeatureBindings_${binding_case}_Vulkan PROPERTIES
            LABELS "gpu;renderer;vulkan"
            ENVIRONMENT "PRISM_RENDER_HEADLESS=1;PRISM_RENDER_QUEUE_COST_MODEL_PATH=${CMAKE_CURRENT_BINARY_DIR}/bindings-${binding_case}-vulkan.queue-cache.json"
            FAIL_REGULAR_EXPRESSION "\\[Vulkan validation\\]"
            TIMEOUT 180)
    endforeach()

    add_executable(PrismOceanFftGpuTests
        tests/OceanFftGpuTests.cpp)
    target_include_directories(PrismOceanFftGpuTests PRIVATE
        ${CMAKE_SOURCE_DIR}/src)
    target_compile_definitions(PrismOceanFftGpuTests PRIVATE
        PRISM_RENDER_SHADER_DIR="${PRISM_RENDER_SHADER_DIR}")
    target_link_libraries(PrismOceanFftGpuTests PRIVATE PrismRenderer)
    if(MSVC)
        target_compile_options(PrismOceanFftGpuTests PRIVATE /W4 /EHsc)
    else()
        target_compile_options(PrismOceanFftGpuTests PRIVATE
            -Wall -Wextra -Wpedantic)
    endif()
    prism_copy_slang_runtime(PrismOceanFftGpuTests)
    if(PRISM_RENDER_ENABLE_D3D12)
        add_test(NAME OceanFftGpuD3D12
            COMMAND PrismOceanFftGpuTests d3d12)
        set_tests_properties(OceanFftGpuD3D12 PROPERTIES
            LABELS "gpu;renderer;d3d12"
            ENVIRONMENT "PRISM_RENDER_HEADLESS=1;PRISM_RENDER_GPU_VALIDATION=1"
            TIMEOUT 120)
    endif()
    add_test(NAME OceanFftGpuVulkan
        COMMAND PrismOceanFftGpuTests vulkan)
    set_tests_properties(OceanFftGpuVulkan PROPERTIES
        LABELS "gpu;renderer;vulkan"
        FAIL_REGULAR_EXPRESSION "\\[Vulkan validation\\]"
        ENVIRONMENT "PRISM_RENDER_HEADLESS=1"
        TIMEOUT 120)
    set_target_properties(PrismOceanFftGpuTests PROPERTIES FOLDER Tests)

    if(PRISM_RENDER_ENABLE_D3D12)
        add_test(NAME OceanFallbackGpuD3D12
            COMMAND PrismOceanFftGpuTests d3d12 --fallback-only)
        set_tests_properties(OceanFallbackGpuD3D12 PROPERTIES
            LABELS "gpu;renderer;d3d12"
            ENVIRONMENT "PRISM_RENDER_HEADLESS=1;PRISM_RENDER_GPU_VALIDATION=1"
            TIMEOUT 30)
    endif()
    add_test(NAME OceanFallbackGpuVulkan
        COMMAND PrismOceanFftGpuTests vulkan --fallback-only)
    set_tests_properties(OceanFallbackGpuVulkan PROPERTIES
        LABELS "gpu;renderer;vulkan"
        FAIL_REGULAR_EXPRESSION "\\[Vulkan validation\\]"
        ENVIRONMENT "PRISM_RENDER_HEADLESS=1"
        TIMEOUT 30)

    if(PRISM_RENDER_ENABLE_D3D12)
        add_executable(PrismTextureDescriptorGpuTests tests/TextureDescriptorGpuTests.cpp)
        target_link_libraries(PrismTextureDescriptorGpuTests PRIVATE PrismRHI)
        target_compile_options(PrismTextureDescriptorGpuTests PRIVATE /W4 /EHsc)
        add_test(NAME TextureDescriptorGpuD3D12 COMMAND PrismTextureDescriptorGpuTests)
        set_tests_properties(TextureDescriptorGpuD3D12 PROPERTIES
            LABELS "gpu;rhi;d3d12"
            ENVIRONMENT "PRISM_RENDER_HEADLESS=1;PRISM_RENDER_GPU_VALIDATION=1"
            TIMEOUT 120)
        set_target_properties(PrismTextureDescriptorGpuTests PROPERTIES FOLDER Tests)
    endif()

    add_executable(PrismOceanTessellationGpuTests
        tests/OceanTessellationGpuTests.cpp)
    target_include_directories(PrismOceanTessellationGpuTests PRIVATE
        ${CMAKE_SOURCE_DIR}/src)
    target_compile_definitions(PrismOceanTessellationGpuTests PRIVATE
        PRISM_RENDER_SHADER_DIR="${PRISM_RENDER_SHADER_DIR}")
    target_link_libraries(PrismOceanTessellationGpuTests PRIVATE PrismRenderer)
    if(MSVC)
        target_compile_options(PrismOceanTessellationGpuTests PRIVATE /W4 /EHsc)
    else()
        target_compile_options(PrismOceanTessellationGpuTests PRIVATE
            -Wall -Wextra -Wpedantic)
    endif()
    prism_copy_slang_runtime(PrismOceanTessellationGpuTests)
    if(PRISM_RENDER_ENABLE_D3D12)
        add_test(NAME OceanTessellationGpuD3D12
            COMMAND PrismOceanTessellationGpuTests d3d12)
        set_tests_properties(OceanTessellationGpuD3D12 PROPERTIES
            LABELS "gpu;renderer;d3d12;tessellation"
            ENVIRONMENT "PRISM_RENDER_HEADLESS=1;PRISM_RENDER_GPU_VALIDATION=1"
            TIMEOUT 120)
    endif()
    add_test(NAME OceanTessellationGpuVulkan
        COMMAND PrismOceanTessellationGpuTests vulkan)
    set_tests_properties(OceanTessellationGpuVulkan PROPERTIES
        LABELS "gpu;renderer;vulkan;tessellation"
        FAIL_REGULAR_EXPRESSION "\\[Vulkan validation\\]"
        ENVIRONMENT "PRISM_RENDER_HEADLESS=1;PRISM_RENDER_GPU_VALIDATION=1"
        TIMEOUT 120)
    set_target_properties(PrismOceanTessellationGpuTests PROPERTIES FOLDER Tests)

    if(PRISM_RENDER_BUILD_HARNESS)
        add_executable(PrismEngineHarnessTests
            tests/EngineHarnessTests.cpp
        )
        target_include_directories(PrismEngineHarnessTests PRIVATE ${CMAKE_SOURCE_DIR}/src)
        target_compile_definitions(PrismEngineHarnessTests PRIVATE
            PRISM_RENDER_PROJECT_DIR="${PRISM_RENDER_PROJECT_DIR}"
        )
        target_link_libraries(PrismEngineHarnessTests PRIVATE
            PrismAutomation
            PrismEngine
            PrismScene
            PrismRenderer
            PrismDiagnostics)
        if(MSVC)
            target_compile_options(PrismEngineHarnessTests PRIVATE /W4 /EHsc)
        else()
            target_compile_options(PrismEngineHarnessTests PRIVATE -Wall -Wextra -Wpedantic)
        endif()
        add_test(NAME EngineHarness COMMAND PrismEngineHarnessTests)
        set_tests_properties(EngineHarness PROPERTIES
            LABELS "integration;agent"
            TIMEOUT 120)
        set_target_properties(PrismEngineHarnessTests PROPERTIES FOLDER Tests)
    endif()

    add_executable(PrismWorldRenderSceneBridgeTests
        tests/WorldRenderSceneBridgeTests.cpp
    )
    target_include_directories(PrismWorldRenderSceneBridgeTests PRIVATE ${CMAKE_SOURCE_DIR}/src)
    target_link_libraries(PrismWorldRenderSceneBridgeTests PRIVATE PrismRenderer)
    if(MSVC)
        target_compile_options(PrismWorldRenderSceneBridgeTests PRIVATE /W4 /EHsc)
    else()
        target_compile_options(PrismWorldRenderSceneBridgeTests PRIVATE -Wall -Wextra -Wpedantic)
    endif()
    add_test(NAME WorldRenderSceneBridge COMMAND PrismWorldRenderSceneBridgeTests)
    set_tests_properties(WorldRenderSceneBridge PROPERTIES
        LABELS "unit;scene"
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
        TIMEOUT 30)
    set_target_properties(PrismWorldRenderSceneBridgeTests PROPERTIES FOLDER Tests)

    add_executable(PrismRenderScenePublicationTests
        tests/RenderScenePublicationTests.cpp
    )
    target_include_directories(PrismRenderScenePublicationTests PRIVATE
        ${CMAKE_SOURCE_DIR}/src)
    target_link_libraries(PrismRenderScenePublicationTests PRIVATE
        PrismRenderer)
    if(MSVC)
        target_compile_options(PrismRenderScenePublicationTests PRIVATE
            /W4 /EHsc)
    else()
        target_compile_options(PrismRenderScenePublicationTests PRIVATE
            -Wall -Wextra -Wpedantic)
    endif()
    add_test(NAME RenderScenePublication
        COMMAND PrismRenderScenePublicationTests)
    set_tests_properties(RenderScenePublication PROPERTIES
        LABELS "unit;scene"
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
        TIMEOUT 30)
    set_target_properties(PrismRenderScenePublicationTests PROPERTIES
        FOLDER Tests)

    add_executable(PrismApplicationCommandLineTests
        tests/ApplicationCommandLineTests.cpp
        src/Core/ApplicationCommandLine.cpp
    )
    target_include_directories(PrismApplicationCommandLineTests PRIVATE
        ${CMAKE_SOURCE_DIR}/src)
    target_link_libraries(PrismApplicationCommandLineTests PRIVATE
        PrismRenderer)
    if(PRISM_RENDER_ENABLE_D3D12)
        target_compile_definitions(PrismApplicationCommandLineTests PRIVATE
            PRISM_RENDER_HAS_D3D12=1)
    endif()
    if(MSVC)
        target_compile_options(PrismApplicationCommandLineTests PRIVATE
            /W4 /EHsc)
    else()
        target_compile_options(PrismApplicationCommandLineTests PRIVATE
            -Wall -Wextra -Wpedantic)
    endif()
    add_test(NAME ApplicationCommandLine
        COMMAND PrismApplicationCommandLineTests)
    set_tests_properties(ApplicationCommandLine PROPERTIES
        LABELS "unit;application"
        TIMEOUT 30)
    set_target_properties(PrismApplicationCommandLineTests PROPERTIES
        FOLDER Tests)

    add_executable(PrismProceduralErosionTerrainTests
        tests/ProceduralErosionTerrainTests.cpp
    )
    target_include_directories(PrismProceduralErosionTerrainTests PRIVATE
        ${CMAKE_SOURCE_DIR}/src)
    target_link_libraries(PrismProceduralErosionTerrainTests PRIVATE
        PrismRenderer)
    if(MSVC)
        target_compile_options(PrismProceduralErosionTerrainTests PRIVATE
            /W4 /EHsc)
    else()
        target_compile_options(PrismProceduralErosionTerrainTests PRIVATE
            -Wall -Wextra -Wpedantic)
    endif()
    add_test(NAME ProceduralErosionTerrain
        COMMAND PrismProceduralErosionTerrainTests)
    set_tests_properties(ProceduralErosionTerrain PROPERTIES
        LABELS "unit;scene;terrain"
        TIMEOUT 30)
    set_target_properties(PrismProceduralErosionTerrainTests PROPERTIES
        FOLDER Tests)

    if(WIN32)
        add_executable(PrismCrashFixture
            tests/CrashFixtureMain.cpp
            src/Core/ProcessDiagnostics.cpp
            src/Core/ProcessDiagnostics.h
            src/Core/CpuTrace.cpp
            src/Core/CpuTrace.h
            src/Core/Environment.cpp
            src/Core/Environment.h
        )
        target_include_directories(PrismCrashFixture PRIVATE
            ${CMAKE_SOURCE_DIR}/src
        )
        target_link_libraries(PrismCrashFixture PRIVATE
            PrismDiagnostics)
        if(MSVC)
            target_compile_options(PrismCrashFixture PRIVATE /W4 /EHsc)
        else()
            target_compile_options(PrismCrashFixture PRIVATE
                -Wall -Wextra -Wpedantic)
        endif()
        set_target_properties(PrismCrashFixture PROPERTIES FOLDER Tests)
        prism_enable_ms_debug_symbols(PrismCrashFixture)

        add_executable(PrismMinidumpSymbolizerTests
            tests/MinidumpSymbolizerTests.cpp
        )
        target_include_directories(PrismMinidumpSymbolizerTests PRIVATE
            ${CMAKE_SOURCE_DIR}/src
        )
        target_link_libraries(PrismMinidumpSymbolizerTests PRIVATE
            PrismDiagnostics)
        if(MSVC)
            target_compile_options(PrismMinidumpSymbolizerTests PRIVATE
                /W4 /EHsc)
        else()
            target_compile_options(PrismMinidumpSymbolizerTests PRIVATE
                -Wall -Wextra -Wpedantic)
        endif()
        add_test(
            NAME MinidumpSymbolizer
            COMMAND PrismMinidumpSymbolizerTests
                $<TARGET_FILE:PrismCrashFixture>
                $<TARGET_PDB_FILE:PrismCrashFixture>
                $<TARGET_PDB_FILE:PrismRender>
        )
        set_tests_properties(MinidumpSymbolizer PROPERTIES
            LABELS "integration;diagnostics"
            TIMEOUT 60)
        set_target_properties(PrismMinidumpSymbolizerTests PROPERTIES FOLDER Tests)
    endif()
endif()

include(cmake/CheckModuleBoundaries.cmake)
if(PRISM_RENDER_CHECK_MODULE_BOUNDARIES)
    prism_check_module_boundaries(
        MANIFEST "${CMAKE_SOURCE_DIR}/cmake/PrismModuleBoundaries.json"
        ROOT "${CMAKE_SOURCE_DIR}")
endif()
if(PRISM_RENDER_CHECK_PUBLIC_HEADERS)
    include(cmake/PrismPublicHeaderSelfContainment.cmake)
    prism_add_public_header_self_containment(
        MANIFEST "${CMAKE_SOURCE_DIR}/cmake/PrismModuleBoundaries.json"
        ROOT "${CMAKE_SOURCE_DIR}")
    if(BUILD_TESTING)
        add_test(NAME PublicHeaderSelfContainment
            COMMAND ${CMAKE_COMMAND}
                --build ${CMAKE_BINARY_DIR}
                --config $<CONFIG>
                --target PrismPublicHeaderSelfContainment
                --parallel 2)
        set_tests_properties(PublicHeaderSelfContainment PROPERTIES
            LABELS "architecture;cmake;compile"
            TIMEOUT 180)
    endif()
endif()
if(BUILD_TESTING)
    add_test(NAME ModuleBoundaries
        COMMAND ${CMAKE_COMMAND}
            -DPRISM_BOUNDARY_CHECKER=${CMAKE_SOURCE_DIR}/cmake/CheckModuleBoundaries.cmake
            -DPRISM_BOUNDARY_FIXTURE_SOURCE=${CMAKE_SOURCE_DIR}/tests/cmake/fixtures/module-boundary
            -DPRISM_BOUNDARY_FIXTURE_BINARY=${CMAKE_CURRENT_BINARY_DIR}/module-boundary-fixtures
            -DPRISM_BOUNDARY_GENERATOR=${CMAKE_GENERATOR}
            -P ${CMAKE_SOURCE_DIR}/tests/cmake/ModuleBoundaryTests.cmake)
    set_tests_properties(ModuleBoundaries PROPERTIES
        LABELS "unit;architecture;cmake"
        TIMEOUT 60)
endif()
