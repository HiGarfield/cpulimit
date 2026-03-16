# cmake/RunFormat.cmake
#
# Runs clang-format -i on all C source and header files in src/ and
# tests/.  Invoked by the 'format' custom target in the root
# CMakeLists.txt.
#
# Globbing is performed here (at script-execution / build time) so that
# newly added or renamed source files are always included without
# requiring a CMake reconfiguration.
#
# Required variables (pass with -D on the cmake -P command line):
#   CLANG_FORMAT_EXECUTABLE - full path to the clang-format binary
#   SRC_DIR                 - absolute path to the src/ directory
#   TESTS_DIR               - absolute path to the tests/ directory

# --- 1. Validate required tool ---

if(NOT CLANG_FORMAT_EXECUTABLE OR NOT EXISTS "${CLANG_FORMAT_EXECUTABLE}")
    message(FATAL_ERROR
        "clang-format not found. "
        "Install clang-format and re-run cmake."
    )
endif()

# --- 2. Collect source files ---

# file(GLOB ...) is evaluated at script-execution time (build time), so
# new files are always included without requiring reconfiguration.
file(GLOB _src_files
    "${SRC_DIR}/*.c"
    "${SRC_DIR}/*.h"
)
file(GLOB _tests_files
    "${TESTS_DIR}/*.c"
    "${TESTS_DIR}/*.h"
)

set(_all_files ${_src_files} ${_tests_files})

if(NOT _all_files)
    message(STATUS "No source files found to format.")
    return()
endif()

# --- 3. Run clang-format ---

message(STATUS "Formatting source files with clang-format...")
execute_process(
    COMMAND "${CLANG_FORMAT_EXECUTABLE}" -i ${_all_files}
    RESULT_VARIABLE _rc
)
if(NOT _rc EQUAL 0)
    message(FATAL_ERROR
        "clang-format failed (exit code ${_rc})."
    )
endif()
message(STATUS "Source files formatted successfully.")
