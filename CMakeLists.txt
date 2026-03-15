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
