cmake_minimum_required(VERSION 3.24)
project(PrismModuleBoundaryFixture LANGUAGES NONE)

if(NOT DEFINED PRISM_CASE OR NOT DEFINED PRISM_CHECKER)
    message(FATAL_ERROR "Fixture requires PRISM_CASE and PRISM_CHECKER.")
endif()

set(fixture_root "${CMAKE_CURRENT_BINARY_DIR}/project")
file(MAKE_DIRECTORY
    "${fixture_root}/src/Core"
    "${fixture_root}/src/RHI"
    "${fixture_root}/src/Renderer"
    "${fixture_root}/src/UI"
    "${fixture_root}/src/Application")

file(WRITE "${fixture_root}/src/Core/Core.cpp" "#include \"Core/Public.h\"\n")
file(WRITE "${fixture_root}/src/Core/Public.h" "#pragma once\n")
file(WRITE "${fixture_root}/src/Core/Internal.h" "#pragma once\n")
file(WRITE "${fixture_root}/src/RHI/Rhi.cpp" "#include \"Core/Public.h\"\n")
file(WRITE "${fixture_root}/src/RHI/Public.h" "#pragma once\n")
file(WRITE "${fixture_root}/src/RHI/Internal.h" "#pragma once\n")
file(WRITE "${fixture_root}/src/Renderer/Renderer.cpp" "#include \"Core/Public.h\"\n#include \"RHI/Public.h\"\n")
file(WRITE "${fixture_root}/src/Renderer/Public.h" "#pragma once\n")
file(WRITE "${fixture_root}/src/UI/Backend.cpp" "#include \"RHI/Public.h\"\n")
file(WRITE "${fixture_root}/src/UI/Editor.h" "#pragma once\n")
file(WRITE "${fixture_root}/src/Application/App.cpp" "#include \"Core/Public.h\"\n#include \"Renderer/Public.h\"\n")
file(WRITE "${fixture_root}/src/Application/App.h" "#pragma once\n")

set(core_dependencies "")
set(rhi_dependencies "PrismCore")
set(renderer_dependencies "PrismCore;PrismRHI")
set(editor_dependencies "PrismRHI")
set(application_dependencies "PrismCore;PrismRenderer")
set(core_source_rules "src/Core/Core.cpp")
set(rhi_source_rules "src/RHI/Rhi.cpp")
set(extra_manifest_fields "")

if(PRISM_CASE STREQUAL "rhi_ui")
    list(APPEND rhi_dependencies PrismEditor)
elseif(PRISM_CASE STREQUAL "renderer_application")
    list(APPEND renderer_dependencies PrismApplication)
elseif(PRISM_CASE STREQUAL "internal_header")
    file(WRITE "${fixture_root}/src/Application/App.cpp"
        "#include \"Core/Public.h\"\n#include \"Renderer/Public.h\"\n#include \"RHI/Internal.h\"\n")
    list(APPEND application_dependencies PrismRHI)
elseif(PRISM_CASE STREQUAL "target_cycle")
    list(APPEND rhi_dependencies PrismRenderer)
elseif(PRISM_CASE STREQUAL "missing_direct")
    file(WRITE "${fixture_root}/src/Application/App.cpp"
        "#include \"Core/Public.h\"\n#include \"Renderer/Public.h\"\n#include \"RHI/Public.h\"\n")
elseif(PRISM_CASE STREQUAL "duplicate_owner")
    file(WRITE "${fixture_root}/src/Shared.cpp" "// duplicate owner fixture\n")
    list(APPEND core_source_rules "src/Shared.cpp")
    list(APPEND rhi_source_rules "src/Shared.cpp")
elseif(PRISM_CASE STREQUAL "missing_owner")
    file(WRITE "${fixture_root}/src/Orphan.cpp" "// missing owner fixture\n")
elseif(PRISM_CASE STREQUAL "valid_editor_backend")
    file(WRITE "${fixture_root}/src/UI/Backend.cpp" "#include \"RHI/Internal.h\"\n")
    set(extra_manifest_fields [=[
  "allowedInternalHeaderIncludes": [
    {
      "consumer": "PrismEditor",
      "provider": "PrismRHI",
      "sourceRules": ["src/UI/Backend.cpp"],
      "headerRules": ["src/RHI/Internal.h"]
    }
  ],]=])
elseif(PRISM_CASE STREQUAL "valid_test_exception")
    set(extra_manifest_fields [=[
  "allowedTestRecompilation": [
    {
      "targets": ["PrismFixtureTest"],
      "sourceRules": ["src/Core/Core.cpp"]
    }
  ],]=])
else()
    message(FATAL_ERROR "Unknown PRISM_CASE '${PRISM_CASE}'.")
endif()

function(json_array output)
    set(result "[")
    set(separator "")
    foreach(value IN LISTS ARGN)
        string(APPEND result "${separator}\"${value}\"")
        set(separator ", ")
    endforeach()
    string(APPEND result "]")
    set(${output} "${result}" PARENT_SCOPE)
endfunction()

json_array(core_dependency_json ${core_dependencies})
json_array(rhi_dependency_json ${rhi_dependencies})
json_array(renderer_dependency_json ${renderer_dependencies})
json_array(editor_dependency_json ${editor_dependencies})
json_array(application_dependency_json ${application_dependencies})
json_array(core_source_json ${core_source_rules})
json_array(rhi_source_json ${rhi_source_rules})

set(manifest "{\n")
string(APPEND manifest "${extra_manifest_fields}\n")
string(APPEND manifest [=[
  "modules": [
    {
      "target": "PrismCore",
      "sourceRules": @CORE_SOURCES@,
      "publicHeaders": ["src/Core/Public.h"],
      "internalHeaders": ["src/Core/Internal.h"],
      "directDependencies": @CORE_DEPS@
    },
    {
      "target": "PrismRHI",
      "sourceRules": @RHI_SOURCES@,
      "publicHeaders": ["src/RHI/Public.h"],
      "internalHeaders": ["src/RHI/Internal.h"],
      "directDependencies": @RHI_DEPS@
    },
    {
      "target": "PrismRenderer",
      "sourceRules": ["src/Renderer/Renderer.cpp"],
      "publicHeaders": ["src/Renderer/Public.h"],
      "directDependencies": @RENDERER_DEPS@
    },
    {
      "target": "PrismEditor",
      "sourceRules": ["src/UI/Backend.cpp"],
      "publicHeaders": ["src/UI/Editor.h"],
      "directDependencies": @EDITOR_DEPS@
    },
    {
      "target": "PrismApplication",
      "sourceRules": ["src/Application/App.cpp"],
      "publicHeaders": ["src/Application/App.h"],
      "directDependencies": @APPLICATION_DEPS@
    }
  ],
  "forbiddenDirectEdges": [
    ["PrismRHI", "PrismEditor"],
    ["PrismRenderer", "PrismApplication"]
  ]
}
]=])
string(REPLACE "@CORE_SOURCES@" "${core_source_json}" manifest "${manifest}")
string(REPLACE "@RHI_SOURCES@" "${rhi_source_json}" manifest "${manifest}")
string(REPLACE "@CORE_DEPS@" "${core_dependency_json}" manifest "${manifest}")
string(REPLACE "@RHI_DEPS@" "${rhi_dependency_json}" manifest "${manifest}")
string(REPLACE "@RENDERER_DEPS@" "${renderer_dependency_json}" manifest "${manifest}")
string(REPLACE "@EDITOR_DEPS@" "${editor_dependency_json}" manifest "${manifest}")
string(REPLACE "@APPLICATION_DEPS@" "${application_dependency_json}" manifest "${manifest}")
file(WRITE "${fixture_root}/manifest.json" "${manifest}")

add_library(PrismCore INTERFACE)
add_library(PrismRHI INTERFACE)
add_library(PrismRenderer INTERFACE)
add_library(PrismEditor INTERFACE)
add_library(PrismApplication INTERFACE)

target_sources(PrismCore INTERFACE "${fixture_root}/src/Core/Core.cpp")
target_sources(PrismRHI INTERFACE "${fixture_root}/src/RHI/Rhi.cpp")
target_sources(PrismRenderer INTERFACE "${fixture_root}/src/Renderer/Renderer.cpp")
target_sources(PrismEditor INTERFACE "${fixture_root}/src/UI/Backend.cpp")
target_sources(PrismApplication INTERFACE "${fixture_root}/src/Application/App.cpp")
target_link_libraries(PrismCore INTERFACE ${core_dependencies})
target_link_libraries(PrismRHI INTERFACE ${rhi_dependencies})
target_link_libraries(PrismRenderer INTERFACE ${renderer_dependencies})
target_link_libraries(PrismEditor INTERFACE ${editor_dependencies})
target_link_libraries(PrismApplication INTERFACE ${application_dependencies})

if(PRISM_CASE STREQUAL "valid_test_exception")
    add_library(PrismFixtureTest INTERFACE)
    target_sources(PrismFixtureTest INTERFACE "${fixture_root}/src/Core/Core.cpp")
endif()

include("${PRISM_CHECKER}")
prism_check_module_boundaries(
    MANIFEST "${fixture_root}/manifest.json"
    ROOT "${fixture_root}")
