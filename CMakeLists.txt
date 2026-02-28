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
include(CheckCCompilerFlag)

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

# --- 5. Utility Function Definitions ---
function(add_checked_flags target flag_list)
    if(NOT TARGET ${target})
        message(WARNING "Target '${target}' not found. Skipping add_checked_flags.")
        return()
    endif()
    if(NOT DEFINED ${flag_list})
        message(WARNING "Flag list variable '${flag_list}' is not defined.")
        return()
    endif()

    set(_saved_quiet ${CMAKE_REQUIRED_QUIET})
    set(CMAKE_REQUIRED_QUIET TRUE)

    foreach(_flag IN LISTS ${flag_list})
        string(MAKE_C_IDENTIFIER "HAVE_FLAG_${_flag}" _test_var)
        check_c_compiler_flag("${_flag}" ${_test_var})
        if(${_test_var})
            target_compile_options(${target} PRIVATE ${_flag})
        endif()
    endforeach()

    set(CMAKE_REQUIRED_QUIET ${_saved_quiet})
endfunction()

# --- 6. Cross-Platform Definitions ---
add_definitions(-D_TIME_BITS=64 -D_FILE_OFFSET_BITS=64)

set(CPULIMIT_PLATFORM_LIBS)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    list(APPEND CPULIMIT_PLATFORM_LIBS rt)
elseif(CMAKE_SYSTEM_NAME STREQUAL "FreeBSD")
    list(APPEND CPULIMIT_PLATFORM_LIBS kvm)
elseif(CMAKE_SYSTEM_NAME STREQUAL "Darwin")
    list(APPEND CPULIMIT_PLATFORM_LIBS proc)
endif()

# --- 7. Source File Definitions ---
set(CPULIMIT_SRC_COMMON
    src/cli.c
    src/limit_process.c
    src/limiter.c
    src/list.c
    src/process_group.c
    src/process_iterator.c
    src/process_table.c
    src/signal_handler.c
    src/util.c
)

# --- 8. Compiler Flags ---
set(CPULIMIT_C_FLAGS
    # Basic warnings
    -Wall
    -Wextra
    -pedantic
    -Wpedantic

    # C language feature compliance
    -Wc++-compat
    -Wstrict-prototypes
    -Wold-style-definition
    -Wmissing-prototypes
    -Wmissing-declarations
    -Wredundant-decls
    -Wnested-externs

    # Common programming errors
    -Winit-self
    -Waggregate-return
    -Wstrict-aliasing=2
    -Wcast-qual
    -Wwrite-strings

    # Type conversion and precision
    -Wconversion
    -Wsign-conversion
    -Wdouble-promotion
    -Wfloat-equal

    # Variables and scope
    -Wshadow
    -Wcast-align
    -Wcast-align=strict
    -Wpointer-arith
    -Wbad-function-cast

    # Logic and expressions
    -Wlogical-op
    -Wrestrict
    -Wimplicit-fallthrough

    # Initialization and structures
    -Wmissing-field-initializers
    -Wmissing-braces

    # Array and memory bounds
    -Warray-bounds=2
    -Wtautological-compare
    -Walloc-zero
    -Walloca
    -Wfree-nonheap-object
    -Wvla

    # Size limits
    -Wlarger-than=256
    -Wframe-larger-than=512
    -Wstack-usage=512

    # Stack and string safety
    -Wstack-protector
    -Wstringop-overflow=4
    -Wstringop-overread
    -Wstringop-truncation

    # Functions and parameters
    -Warray-parameter=2
    -Wattribute-alias=2
    -Wzero-length-bounds

    # Uninitialized variables
    -Wuninitialized
    -Wmaybe-uninitialized
    -Wclobbered

    # Structure layout
    -Wpacked-not-aligned
    -Wpacked

    # Switch statements
    -Wswitch-enum
    -Wswitch-default
    -Wswitch-bool
    -Wjump-misses-init
    -Wswitch-unreachable

    # Code duplication
    -Wduplicated-cond
    -Wduplicated-branches
    -Wduplicated-arg

    # Format strings
    -Wformat=2
    -Wformat-overflow=2
    -Wformat-truncation=2
    -Wformat-signedness
    -Wformat-contains-nul
    -Wformat-zero-length
    -Wmissing-format-attribute

    # Unused and optimization
    -Wunused
    -Wdisabled-optimization
    -Waggressive-loop-optimizations

    # Macros and others
    -Wmultistatement-macros
    -Wcast-function-type

    # Internationalization/Localization
    -Wbidi-chars=any
    -Wnormalized=nfc

    # Flexible array members
    -Wstrict-flex-arrays=3

    # Additional warnings from old version
    -Wundef
    -Wunreachable-code
    -Winline
    -Wmissing-include-dirs
    -Wstrict-overflow=5

    # Compiler optimization flags
    -pipe

    # Static analyzer
    -fanalyzer
    -Wanalyzer-null-dereference
    -Wanalyzer-double-free
    -Wanalyzer-use-after-free
    -Wanalyzer-malloc-leak
    -Wanalyzer-possible-null-dereference
    -Wanalyzer-shift-count-negative
    -Wanalyzer-shift-count-overflow
    -Wanalyzer-out-of-bounds
    -Wanalyzer-unsafe-call-within-signal-handler
)

# --- 9. Main Target Build ---
add_executable(cpulimit
    src/main.c
    ${CPULIMIT_SRC_COMMON}
)
target_include_directories(cpulimit PRIVATE src)
target_link_libraries(cpulimit ${CPULIMIT_PLATFORM_LIBS})
add_checked_flags(cpulimit CPULIMIT_C_FLAGS)

install(TARGETS cpulimit RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

# --- 10. Test Build (Optional) ---
if(CPULIMIT_BUILD_TESTS)
    enable_testing()
    find_package(Threads REQUIRED)

    add_executable(busy tests/busy.c src/signal_handler.c src/util.c)
    target_link_libraries(busy ${CPULIMIT_PLATFORM_LIBS} Threads::Threads)
    add_checked_flags(busy CPULIMIT_C_FLAGS)

    add_executable(multi_process_busy tests/multi_process_busy.c src/signal_handler.c src/util.c)
    target_link_libraries(multi_process_busy ${CPULIMIT_PLATFORM_LIBS})
    add_checked_flags(multi_process_busy CPULIMIT_C_FLAGS)

    add_executable(cpulimit_test tests/cpulimit_test.c ${CPULIMIT_SRC_COMMON})
    target_include_directories(cpulimit_test PRIVATE src)
    target_link_libraries(cpulimit_test ${CPULIMIT_PLATFORM_LIBS})
    add_checked_flags(cpulimit_test CPULIMIT_C_FLAGS)

    add_test(NAME cpulimit_unit_tests COMMAND cpulimit_test)
endif()
