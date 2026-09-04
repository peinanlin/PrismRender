if(NOT DEFINED PRISM_BOUNDARY_CHECKER OR
   NOT DEFINED PRISM_BOUNDARY_FIXTURE_SOURCE OR
   NOT DEFINED PRISM_BOUNDARY_FIXTURE_BINARY OR
   NOT DEFINED PRISM_BOUNDARY_GENERATOR)
    message(FATAL_ERROR "ModuleBoundaryTests requires checker, fixture source and fixture binary paths.")
endif()

set(passing_cases
    valid_editor_backend
    valid_test_exception)
set(failing_cases
    rhi_ui
    renderer_application
    internal_header
    target_cycle
    missing_direct
    duplicate_owner
    missing_owner)

set(expected_rhi_ui "forbidden target edge PrismRHI -> PrismEditor")
set(expected_renderer_application "forbidden target edge PrismRenderer -> PrismApplication")
set(expected_internal_header "internal header boundary PrismApplication -> PrismRHI")
set(expected_target_cycle "target cycle reaches")
set(expected_missing_direct "missing direct dependency PrismApplication -> PrismRHI")
set(expected_duplicate_owner "source ownership src/Shared.cpp: expected exactly one owner")
set(expected_missing_owner "source ownership src/Orphan.cpp: expected exactly one owner")

file(REMOVE_RECURSE "${PRISM_BOUNDARY_FIXTURE_BINARY}")
file(MAKE_DIRECTORY "${PRISM_BOUNDARY_FIXTURE_BINARY}")

foreach(case_name IN LISTS passing_cases failing_cases)
    set(case_binary "${PRISM_BOUNDARY_FIXTURE_BINARY}/${case_name}")
    execute_process(
        COMMAND "${CMAKE_COMMAND}"
            -G "${PRISM_BOUNDARY_GENERATOR}"
            -S "${PRISM_BOUNDARY_FIXTURE_SOURCE}"
            -B "${case_binary}"
            -DPRISM_CASE=${case_name}
            -DPRISM_CHECKER=${PRISM_BOUNDARY_CHECKER}
        RESULT_VARIABLE configure_result
        OUTPUT_VARIABLE configure_stdout
        ERROR_VARIABLE configure_stderr)
    set(configure_output "${configure_stdout}\n${configure_stderr}")

    if(case_name IN_LIST passing_cases)
        if(NOT configure_result EQUAL 0)
            message(FATAL_ERROR
                "Boundary fixture '${case_name}' should pass but failed:\n${configure_output}")
        endif()
    else()
        if(configure_result EQUAL 0)
            message(FATAL_ERROR
                "Boundary fixture '${case_name}' should fail but passed.")
        endif()
        set(expected_variable "expected_${case_name}")
        if(NOT configure_output MATCHES "${${expected_variable}}")
            message(FATAL_ERROR
                "Boundary fixture '${case_name}' failed for the wrong reason. "
                "Expected '${${expected_variable}}':\n${configure_output}")
        endif()
    endif()
endforeach()

message(STATUS "Module boundary fixtures passed: 2 positive, 7 negative.")
