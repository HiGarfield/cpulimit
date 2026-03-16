cmake_minimum_required(VERSION 3.5)

project(cpulimit LANGUAGES C)

# --- 1. Operating System Compatibility Check ---
set(SUPPORTED_OS Linux Darwin FreeBSD)
list(FIND SUPPORTED_OS ${CMAKE_SYSTEM_NAME} _os_index)
if(_os_index EQUAL -1)
    message(FATAL_ERROR
        "Unsupported operating system: ${CMAKE_SYSTEM_NAME}\n"
        "Supported: ${SUPPORTED_OS}"
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

# Set default build type to Release
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

# --- 4. Project Options ---
option(CPULIMIT_BUILD_TESTS "Build test binaries" ON)

# --- 5. Common Build Options ---
include(cmake/CpulimitOptions.cmake)

# --- 6. Subdirectories ---
add_subdirectory(src)
if(CPULIMIT_BUILD_TESTS)
    add_subdirectory(tests)
endif()

# --- 7. Check Target (Static Analysis) ---
#
# Runs cppcheck and clang-tidy on src/ and tests/, generating
# per-directory report files.  Mirrors the behaviour of the
# src/Makefile and tests/Makefile "check" targets without invoking
# those Makefiles.
#
# Usage (from the build directory):
#   cmake --build . --target check

find_program(CLANG_FORMAT_EXECUTABLE clang-format)
find_program(CPPCHECK_EXECUTABLE cppcheck)
find_program(CLANG_TIDY_EXECUTABLE clang-tidy)

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

# --- 8. Format Target ---
#
# Runs clang-format on all C source and header files in src/ and
# tests/, formatting them in-place.  Mirrors the behaviour of the
# "format" targets in src/Makefile and tests/Makefile.
#
# Globbing is delegated to cmake/RunFormat.cmake which runs at build
# time, so newly added or renamed source files are always picked up
# without a CMake reconfiguration.  The script also validates that
# clang-format is available and fails with a clear message if not.
#
# Usage (from the build directory):
#   cmake --build . --target format

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

# --- 9. Uninstall Target ---
#
# Removes all files that were recorded in the CMake install manifest.
# Mirrors the behaviour of the "uninstall" target in src/Makefile.
#
# Usage (from the build directory):
#   sudo cmake --build . --target uninstall

add_custom_target(uninstall
    COMMAND
        ${CMAKE_COMMAND}
        "-DBUILD_DIR=${CMAKE_BINARY_DIR}"
        -P "${PROJECT_SOURCE_DIR}/cmake/Uninstall.cmake"
    WORKING_DIRECTORY "${CMAKE_BINARY_DIR}"
    COMMENT "Uninstalling..."
    VERBATIM
)
