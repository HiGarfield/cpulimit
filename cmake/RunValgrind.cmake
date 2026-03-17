# cmake/RunValgrind.cmake
#
# Runs the test binary under valgrind to detect memory errors and
# leaks.  Invoked by the 'valgrind' custom target in the root
# CMakeLists.txt.
#
# Required variables (pass with -D on the cmake -P command line):
#   VALGRIND_EXECUTABLE - full path to the valgrind binary
#   TEST_BINARY         - full path to the test executable

# --- 1. Validate required tool ---

if(NOT VALGRIND_EXECUTABLE OR NOT EXISTS "${VALGRIND_EXECUTABLE}")
    message(FATAL_ERROR
        "valgrind not found. "
        "Install valgrind and re-run cmake."
    )
endif()

# --- 2. Validate test binary ---

if(NOT TEST_BINARY OR NOT EXISTS "${TEST_BINARY}")
    message(FATAL_ERROR
        "Test binary not found: ${TEST_BINARY}"
    )
endif()

# --- 3. Run valgrind ---

message(STATUS "Running dynamic analysis with valgrind...")
execute_process(
    COMMAND
        "${VALGRIND_EXECUTABLE}"
        --leak-check=full
        --show-leak-kinds=all
        --error-exitcode=1
        "${TEST_BINARY}"
    RESULT_VARIABLE _rc
)
if(NOT _rc EQUAL 0)
    message(FATAL_ERROR
        "valgrind reported errors (exit code ${_rc})."
    )
endif()
message(STATUS "Dynamic analysis passed.")
