# cmake/RunStandardsCheck.cmake
#
# Compile-only compatibility checks for all .c project sources:
# - as C with each supported C standard flag (C89 and later)
# - as C++ with each supported C++ standard flag (C++98 and later)
#
# Required cache/script variables:
#   CPULIMIT_C_COMPILER
#   SRC_DIR
#   TESTS_DIR
#   BUILD_DIR

if(NOT DEFINED CPULIMIT_C_COMPILER OR CPULIMIT_C_COMPILER STREQUAL "")
    message(FATAL_ERROR "CPULIMIT_C_COMPILER is not set")
endif()
if(NOT DEFINED SRC_DIR OR SRC_DIR STREQUAL "")
    message(FATAL_ERROR "SRC_DIR is not set")
endif()
if(NOT DEFINED TESTS_DIR OR TESTS_DIR STREQUAL "")
    message(FATAL_ERROR "TESTS_DIR is not set")
endif()
if(NOT DEFINED BUILD_DIR OR BUILD_DIR STREQUAL "")
    message(FATAL_ERROR "BUILD_DIR is not set")
endif()

file(GLOB SRC_C_FILES "${SRC_DIR}/*.c")
file(GLOB TEST_C_FILES "${TESTS_DIR}/*.c")
set(ALL_C_FILES ${SRC_C_FILES} ${TEST_C_FILES})

if(NOT ALL_C_FILES)
    message(FATAL_ERROR "No .c source files found for standards checks")
endif()

set(PROBE_C_FILE "${BUILD_DIR}/.cpulimit-std-check-probe.c")
file(WRITE "${PROBE_C_FILE}" "int main(void) { return 0; }\n")

set(C_STD_FLAGS c89 c99 c11 c17 c23 c2x)
set(CXX_STD_FLAGS c++98 c++11 c++14 c++17 c++20 c++23 c++26 c++2b c++2c)

function(check_std_supported LANG_MODE STD_FLAG OUT_VAR)
    execute_process(
        COMMAND "${CPULIMIT_C_COMPILER}" -x "${LANG_MODE}" "-std=${STD_FLAG}"
                -Werror -fsyntax-only "${PROBE_C_FILE}"
        RESULT_VARIABLE _probe_result
        OUTPUT_QUIET
        ERROR_QUIET
    )
    if(_probe_result EQUAL 0)
        set(${OUT_VAR} TRUE PARENT_SCOPE)
    else()
        set(${OUT_VAR} FALSE PARENT_SCOPE)
    endif()
endfunction()

function(check_all_files_for_std LANG_MODE STD_FLAG)
    foreach(_file IN LISTS ALL_C_FILES)
        execute_process(
            COMMAND
                "${CPULIMIT_C_COMPILER}" -x "${LANG_MODE}" "-std=${STD_FLAG}"
                -Wall -Wextra -Werror -pedantic -fsyntax-only
                -I"${SRC_DIR}" -I"${TESTS_DIR}" "${_file}"
            RESULT_VARIABLE _result
            OUTPUT_VARIABLE _stdout
            ERROR_VARIABLE _stderr
        )
        if(NOT _result EQUAL 0)
            message(FATAL_ERROR
                "Standards check failed.\n"
                "compiler: ${CPULIMIT_C_COMPILER}\n"
                "language: ${LANG_MODE}\n"
                "standard: ${STD_FLAG}\n"
                "file: ${_file}\n"
                "stdout:\n${_stdout}\n"
                "stderr:\n${_stderr}\n"
            )
        endif()
    endforeach()
endfunction()

set(SUPPORTED_C_STD_FLAGS)
foreach(_std IN LISTS C_STD_FLAGS)
    check_std_supported("c" "${_std}" _is_supported)
    if(_is_supported)
        list(APPEND SUPPORTED_C_STD_FLAGS "${_std}")
    endif()
endforeach()

set(SUPPORTED_CXX_STD_FLAGS)
foreach(_std IN LISTS CXX_STD_FLAGS)
    check_std_supported("c++" "${_std}" _is_supported)
    if(_is_supported)
        list(APPEND SUPPORTED_CXX_STD_FLAGS "${_std}")
    endif()
endforeach()

if(NOT SUPPORTED_C_STD_FLAGS)
    message(FATAL_ERROR "No supported C standard flags detected")
endif()
if(NOT SUPPORTED_CXX_STD_FLAGS)
    message(FATAL_ERROR "No supported C++ standard flags detected")
endif()

message(STATUS "Detected C standards: ${SUPPORTED_C_STD_FLAGS}")
message(STATUS "Detected C++ standards: ${SUPPORTED_CXX_STD_FLAGS}")

foreach(_std IN LISTS SUPPORTED_C_STD_FLAGS)
    check_all_files_for_std("c" "${_std}")
endforeach()
foreach(_std IN LISTS SUPPORTED_CXX_STD_FLAGS)
    check_all_files_for_std("c++" "${_std}")
endforeach()

message(STATUS "Standards compatibility checks passed")
