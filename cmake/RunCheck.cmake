# cmake/RunCheck.cmake
#
# Runs static analysis (cppcheck + clang-tidy) on src/ and tests/,
# producing per-directory report files.  Mirrors the behaviour of the
# src/Makefile and tests/Makefile "check" targets without invoking them.
#
# Required variables (pass with -D on the cmake -P command line):
#   CPPCHECK_EXECUTABLE    - full path to the cppcheck binary
#   CLANG_TIDY_EXECUTABLE  - full path to the clang-tidy binary
#   SRC_DIR                - absolute path to the src/ directory
#   TESTS_DIR              - absolute path to the tests/ directory
#   BUILD_DIR              - absolute path to the cmake build directory
#   CMAKE_SYSTEM_NAME      - value of CMAKE_SYSTEM_NAME (e.g. Linux)
#   BUILD_TESTS            - "ON" when tests/ should also be analysed
#
# Note: file(GLOB ...) is used inside this script (run at build time
# via cmake -P) so new source files are always picked up without
# requiring a cmake reconfiguration.

# --- 1. Validate required tools ---

if(NOT CPPCHECK_EXECUTABLE OR NOT EXISTS "${CPPCHECK_EXECUTABLE}")
    message(FATAL_ERROR
        "cppcheck not found. "
        "Install cppcheck and re-run cmake."
    )
endif()

if(NOT CLANG_TIDY_EXECUTABLE OR NOT EXISTS "${CLANG_TIDY_EXECUTABLE}")
    message(FATAL_ERROR
        "clang-tidy not found. "
        "Install clang-tidy and re-run cmake."
    )
endif()

# --- 2. Locate compile_commands.json ---

set(_compile_db "${BUILD_DIR}/compile_commands.json")
if(NOT EXISTS "${_compile_db}")
    message(FATAL_ERROR
        "compile_commands.json not found at ${_compile_db}.\n"
        "Re-configure with -DCMAKE_EXPORT_COMPILE_COMMANDS=ON."
    )
endif()

# --- 3. Build a filtered compile_commands.json for clang-tidy ---
#
# The cmake compile_commands.json is produced with the active C compiler
# (typically GCC).  It therefore contains GCC-specific flags that
# clang-tidy (a Clang-based tool) does not understand, such as
# -fanalyzer and -Wanalyzer-*.  Those flags are stripped from a
# temporary copy of the database that is used exclusively by clang-tidy.
# The original compile_commands.json is left untouched.

file(READ "${_compile_db}" _ct_db_content)

# Remove -fanalyzer (GCC static-analysis pass, unknown to Clang).
string(REPLACE " -fanalyzer" "" _ct_db_content "${_ct_db_content}")

# Remove -Wanalyzer-* flags (require -fanalyzer; unknown to Clang).
string(REGEX REPLACE
    " -Wanalyzer-[A-Za-z0-9_-]+"
    ""
    _ct_db_content
    "${_ct_db_content}"
)

set(_ct_db_dir "${BUILD_DIR}/clang_tidy_db")
file(MAKE_DIRECTORY "${_ct_db_dir}")
file(WRITE "${_ct_db_dir}/compile_commands.json" "${_ct_db_content}")

# --- 4. Optional macOS SDK flag for clang-tidy ---

set(_ct_extra "")
if(CMAKE_SYSTEM_NAME STREQUAL "Darwin")
    execute_process(
        COMMAND xcrun --show-sdk-path
        OUTPUT_VARIABLE _sdk
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
        RESULT_VARIABLE _sdk_rc
    )
    if(_sdk_rc EQUAL 0 AND _sdk)
        list(APPEND _ct_extra --extra-arg=-isysroot)
        list(APPEND _ct_extra "--extra-arg=${_sdk}")
    endif()
endif()

# Suppress clang warnings about other GCC-specific -W flags that
# remain in the compilation database (e.g. -Wlogical-op).
list(APPEND _ct_extra --extra-arg=-Wno-unknown-warning-option)

# --- 5. Common cppcheck options ---

set(_cppcheck_common
    --enable=all
    --language=c
    --inconclusive
    --check-level=exhaustive
    --library=gnu
    --force
    --std=c89
    "--project=${_compile_db}"
    --suppress=missingIncludeSystem
    --suppress=ConfigurationNotChecked
    --suppress=unmatchedSuppression
    --suppress=checkersReport
)

# --- 6. src/: cppcheck ---

message(STATUS "Running cppcheck on src/ ...")
execute_process(
    COMMAND
        "${CPPCHECK_EXECUTABLE}"
        ${_cppcheck_common}
        --max-ctu-depth=20
        "--file-filter=${SRC_DIR}/*"
    WORKING_DIRECTORY "${BUILD_DIR}"
    ERROR_FILE "${SRC_DIR}/cppcheck-report.txt"
    RESULT_VARIABLE _rc
)
if(NOT _rc EQUAL 0)
    message(FATAL_ERROR
        "cppcheck on src/ failed (exit code ${_rc}). "
        "See ${SRC_DIR}/cppcheck-report.txt for details."
    )
endif()
message(STATUS
    "See ${SRC_DIR}/cppcheck-report.txt for cppcheck report."
)

# --- 7. src/: clang-tidy ---

# file(GLOB) is evaluated at script-execution time (build time), so
# new files are always included without requiring reconfiguration.
file(GLOB _src_files
    "${SRC_DIR}/*.c"
    "${SRC_DIR}/*.h"
)

# Exclude non-current-platform iterator sources.
# On Linux, process_iterator_apple.c and process_iterator_freebsd.c are
# not compiled and have no compile commands in the database, so
# clang-tidy would fail with missing platform-specific headers.
set(_all_iter_srcs
    "${SRC_DIR}/process_iterator_linux.c"
    "${SRC_DIR}/process_iterator_freebsd.c"
    "${SRC_DIR}/process_iterator_apple.c"
)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    set(_current_iter_src "${SRC_DIR}/process_iterator_linux.c")
elseif(CMAKE_SYSTEM_NAME STREQUAL "FreeBSD")
    set(_current_iter_src "${SRC_DIR}/process_iterator_freebsd.c")
elseif(CMAKE_SYSTEM_NAME STREQUAL "Darwin")
    set(_current_iter_src "${SRC_DIR}/process_iterator_apple.c")
endif()
list(REMOVE_ITEM _all_iter_srcs "${_current_iter_src}")
list(REMOVE_ITEM _src_files ${_all_iter_srcs})

message(STATUS "Running clang-tidy on src/ ...")
execute_process(
    COMMAND
        "${CLANG_TIDY_EXECUTABLE}"
        ${_ct_extra}
        -p "${_ct_db_dir}"
        ${_src_files}
    WORKING_DIRECTORY "${SRC_DIR}"
    OUTPUT_FILE "${SRC_DIR}/clang-tidy-report.txt"
    RESULT_VARIABLE _rc
)
if(NOT _rc EQUAL 0)
    message(FATAL_ERROR
        "clang-tidy on src/ failed (exit code ${_rc}). "
        "See ${SRC_DIR}/clang-tidy-report.txt for details."
    )
endif()
message(STATUS
    "See ${SRC_DIR}/clang-tidy-report.txt for clang-tidy report."
)

# --- 8. tests/: analysis (only when tests are enabled) ---

if(BUILD_TESTS)

    # --- tests/: cppcheck ---

    message(STATUS "Running cppcheck on tests/ ...")
    execute_process(
        COMMAND
            "${CPPCHECK_EXECUTABLE}"
            ${_cppcheck_common}
            --max-ctu-depth=10
            "--file-filter=${TESTS_DIR}/*"
        WORKING_DIRECTORY "${BUILD_DIR}"
        ERROR_FILE "${TESTS_DIR}/cppcheck-report.txt"
        RESULT_VARIABLE _rc
    )
    if(NOT _rc EQUAL 0)
        message(FATAL_ERROR
            "cppcheck on tests/ failed (exit code ${_rc}). "
            "See ${TESTS_DIR}/cppcheck-report.txt for details."
        )
    endif()
    message(STATUS
        "See ${TESTS_DIR}/cppcheck-report.txt for cppcheck report."
    )

    # --- tests/: clang-tidy ---

    file(GLOB _tests_files "${TESTS_DIR}/*.c")

    message(STATUS "Running clang-tidy on tests/ ...")
    execute_process(
        COMMAND
            "${CLANG_TIDY_EXECUTABLE}"
            ${_ct_extra}
            -p "${_ct_db_dir}"
            ${_tests_files}
        WORKING_DIRECTORY "${TESTS_DIR}"
        OUTPUT_FILE "${TESTS_DIR}/clang-tidy-report.txt"
        RESULT_VARIABLE _rc
    )
    if(NOT _rc EQUAL 0)
        message(FATAL_ERROR
            "clang-tidy on tests/ failed (exit code ${_rc}). "
            "See ${TESTS_DIR}/clang-tidy-report.txt for details."
        )
    endif()
    message(STATUS
        "See ${TESTS_DIR}/clang-tidy-report.txt for clang-tidy report."
    )

endif()
