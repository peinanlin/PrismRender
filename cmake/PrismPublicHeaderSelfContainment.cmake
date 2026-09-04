include_guard(GLOBAL)

include("${CMAKE_CURRENT_LIST_DIR}/CheckModuleBoundaries.cmake")

function(prism_add_public_header_self_containment)
    cmake_parse_arguments(PARSE_ARGV 0 argument "" "MANIFEST;ROOT" "")
    if(NOT argument_MANIFEST OR NOT argument_ROOT)
        message(FATAL_ERROR
            "prism_add_public_header_self_containment requires MANIFEST and ROOT.")
    endif()

    get_filename_component(root "${argument_ROOT}" ABSOLUTE)
    file(READ "${argument_MANIFEST}" manifest)
    file(GLOB_RECURSE project_headers LIST_DIRECTORIES FALSE
        "${root}/src/*.h" "${root}/src/*.hpp")

    set(public_module_targets "")
    set(public_header_count 0)
    foreach(header IN LISTS project_headers)
        file(RELATIVE_PATH relative "${root}" "${header}")
        file(TO_CMAKE_PATH "${relative}" relative)
        _prism_boundary_path_owner(
            owner visibility "${manifest}" "${relative}" header)
        list(LENGTH owner owner_count)
        if(NOT owner_count EQUAL 1 OR
           NOT visibility STREQUAL "public" OR
           NOT TARGET "${owner}")
            continue()
        endif()
        list(APPEND public_headers_${owner} "${relative}")
        list(APPEND public_module_targets "${owner}")
        math(EXPR public_header_count "${public_header_count} + 1")
    endforeach()
    list(REMOVE_DUPLICATES public_module_targets)

    set(check_targets "")
    foreach(module_target IN LISTS public_module_targets)
        set(check_target "PrismPublicHeaders_${module_target}")
        set(generated_directory
            "${CMAKE_CURRENT_BINARY_DIR}/generated/public-header-self-containment/${module_target}")
        set(generated_sources "")
        foreach(header IN LISTS public_headers_${module_target})
            string(REGEX REPLACE "^src/" "" include_path "${header}")
            string(REGEX REPLACE "[^A-Za-z0-9_.-]" "_" generated_name "${header}")
            set(generated_source "${generated_directory}/${generated_name}.cpp")
            file(GENERATE OUTPUT "${generated_source}"
                CONTENT "#include \"${include_path}\"\n")
            set_source_files_properties("${generated_source}" PROPERTIES GENERATED TRUE)
            list(APPEND generated_sources "${generated_source}")
        endforeach()

        add_library(${check_target} OBJECT ${generated_sources})
        target_link_libraries(${check_target} PRIVATE ${module_target})
        target_compile_features(${check_target} PRIVATE cxx_std_20)
        set_target_properties(${check_target} PROPERTIES
            FOLDER "Tests/PublicHeaders")
        list(APPEND check_targets ${check_target})
    endforeach()

    add_custom_target(PrismPublicHeaderSelfContainment ALL
        DEPENDS ${check_targets})
    set_target_properties(PrismPublicHeaderSelfContainment PROPERTIES
        FOLDER "Tests/PublicHeaders")
    message(STATUS
        "Configured public-header self-containment: ${public_header_count} headers across ${public_module_targets}.")
endfunction()
