cmake_minimum_required(VERSION 3.5)

# Prevent in-source builds.
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_BINARY_DIR)
    message(FATAL_ERROR
        "In-source builds are not allowed.\n"
        "Run CMake from a separate build directory:\n"
        "  mkdir build && cd build && cmake .."
    )
endif()

project(cpulimit LANGUAGES C)

# --- 1. Operating System Compatibility Check ---
set(CPULIMIT_SUPPORTED_OS Linux Darwin FreeBSD)
list(FIND CPULIMIT_SUPPORTED_OS ${CMAKE_SYSTEM_NAME} _cpulimit_os_index)
if(_cpulimit_os_index EQUAL -1)
    message(FATAL_ERROR
        "Unsupported operating system: ${CMAKE_SYSTEM_NAME}\n"
        "Supported: ${CPULIMIT_SUPPORTED_OS}"
    )
endif()

# --- 2. Include CMake Modules ---
include(GNUInstallDirs)
include(CTest)

# --- 3. Global C Language Standard Settings ---
set(CMAKE_C_STANDARD 90)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# --- 4. Default Build Type ---
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
    set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS
        Debug Release RelWithDebInfo MinSizeRel
    )
endif()

# --- 5. Project Options ---
option(CPULIMIT_BUILD_TESTS "Build test binaries" ON)

# --- 6. Build Utilities and Settings ---
include(cmake/CheckedFlags.cmake)
include(cmake/CpulimitOptions.cmake)

# --- 7. Subdirectories ---
add_subdirectory(src)
if(CPULIMIT_BUILD_TESTS)
    add_subdirectory(tests)
endif()

# --- 8. Tool Discovery ---
find_program(CLANG_FORMAT_EXECUTABLE clang-format)
find_program(CPPCHECK_EXECUTABLE cppcheck)
find_program(CLANG_TIDY_EXECUTABLE clang-tidy)
find_program(VALGRIND_EXECUTABLE valgrind)

if(NOT CLANG_FORMAT_EXECUTABLE)
    message(WARNING
        "clang-format not found; 'format' target will fail at build time. "
        "Install clang-format to enable source formatting."
    )
endif()

if(NOT CPPCHECK_EXECUTABLE)
    message(WARNING
        "cppcheck not found; 'check' target will fail at build time. "
        "Install cppcheck to enable static analysis."
    )
endif()

if(NOT CLANG_TIDY_EXECUTABLE)
    message(WARNING
        "clang-tidy not found; 'check' target will fail at build time. "
        "Install clang-tidy to enable static analysis."
    )
endif()

# --- 9. Check Target (Static Analysis) ---
#
# Runs cppcheck and clang-tidy on src/ and tests/, generating
# per-directory report files.
#
# Usage:
#   cmake --build <build-dir> --target check

add_custom_target(check
    COMMAND
        ${CMAKE_COMMAND}
        "-DCPPCHECK_EXECUTABLE=${CPPCHECK_EXECUTABLE}"
        "-DCLANG_TIDY_EXECUTABLE=${CLANG_TIDY_EXECUTABLE}"
        "-DSRC_DIR=${PROJECT_SOURCE_DIR}/src"
        "-DTESTS_DIR=${PROJECT_SOURCE_DIR}/tests"
        "-DBUILD_DIR=${CMAKE_BINARY_DIR}"
        "-DCMAKE_SYSTEM_NAME=${CMAKE_SYSTEM_NAME}"
        "-DBUILD_TESTS=${CPULIMIT_BUILD_TESTS}"
        -P "${PROJECT_SOURCE_DIR}/cmake/RunCheck.cmake"
    WORKING_DIRECTORY "${CMAKE_BINARY_DIR}"
    COMMENT "Running static analysis (cppcheck + clang-tidy)..."
    VERBATIM
)

# --- 10. Format Target ---
#
# Runs clang-format -i on all C source and header files in src/ and
# tests/.
#
# Usage:
#   cmake --build <build-dir> --target format

add_custom_target(format
    COMMAND
        ${CMAKE_COMMAND}
        "-DCLANG_FORMAT_EXECUTABLE=${CLANG_FORMAT_EXECUTABLE}"
        "-DSRC_DIR=${PROJECT_SOURCE_DIR}/src"
        "-DTESTS_DIR=${PROJECT_SOURCE_DIR}/tests"
        -P "${PROJECT_SOURCE_DIR}/cmake/RunFormat.cmake"
    WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}"
    COMMENT "Formatting source files with clang-format..."
    VERBATIM
)

# --- 11. Valgrind Target (Dynamic Analysis) ---
#
# Runs the test binary under valgrind to detect memory errors and
# leaks.  Always created when tests are enabled; performs analysis
# when valgrind is found, otherwise a no-op target is created.
#
# Usage:
#   cmake --build <build-dir> --target valgrind

if(CPULIMIT_BUILD_TESTS)
    if(VALGRIND_EXECUTABLE)
        add_custom_target(valgrind
            COMMAND
                "${VALGRIND_EXECUTABLE}"
                --leak-check=full
                --show-leak-kinds=all
                --error-exitcode=1
                $<TARGET_FILE:cpulimit_test>
            COMMENT "Running dynamic analysis with valgrind..."
            VERBATIM
        )
        add_dependencies(valgrind cpulimit_test)
    else()
        add_custom_target(valgrind
            COMMAND
                ${CMAKE_COMMAND} -E echo
                "valgrind not found; skipping dynamic analysis."
            COMMENT "valgrind not found; skipping dynamic analysis."
            VERBATIM
        )
        message(STATUS
            "valgrind not found; 'valgrind' target will skip dynamic "
            "analysis."
        )
    endif()
endif()

# --- 12. Uninstall Target ---
#
# Removes all files recorded in the CMake install manifest.
#
# Usage:
#   sudo cmake --build <build-dir> --target uninstall

add_custom_target(uninstall
    COMMAND
        ${CMAKE_COMMAND}
        "-DBUILD_DIR=${CMAKE_BINARY_DIR}"
        -P "${PROJECT_SOURCE_DIR}/cmake/Uninstall.cmake"
    WORKING_DIRECTORY "${CMAKE_BINARY_DIR}"
    COMMENT "Uninstalling..."
    VERBATIM
)
