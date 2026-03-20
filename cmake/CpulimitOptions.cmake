# cmake/CpulimitOptions.cmake
#
# Project-wide build settings: platform libraries, compiler warning
# and diagnostic flags, and the list of shared source files.
#
# Depends on: cmake/CheckedFlags.cmake (must be included beforehand).

# --- 1. Global Preprocessor Definitions ---
#
# Force 64-bit time_t and file offsets to prevent Y2038 issues and
# handle large files on 32-bit platforms.
add_definitions(-D_TIME_BITS=64 -D_FILE_OFFSET_BITS=64)

# --- 2. Platform-Specific Libraries ---
#
# Each supported OS requires a different set of additional libraries:
#   Linux   - librt  (POSIX real-time extensions)
#   FreeBSD - libkvm (kernel virtual memory access)
#   macOS   - libproc (process information)
set(CPULIMIT_PLATFORM_LIBS)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    list(APPEND CPULIMIT_PLATFORM_LIBS rt)
elseif(CMAKE_SYSTEM_NAME STREQUAL "FreeBSD")
    list(APPEND CPULIMIT_PLATFORM_LIBS kvm)
elseif(CMAKE_SYSTEM_NAME STREQUAL "Darwin")
    list(APPEND CPULIMIT_PLATFORM_LIBS proc)
endif()

# --- 3. Compiler Warning and Optimization Flags ---
#
# These flags are applied per-target via add_checked_flags().
# Each flag is probed at configure time; unsupported flags are silently
# skipped so the build remains portable across compiler versions.
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

    # Additional warnings
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

# --- Platform-specific process iterator source ---
#
# Each supported platform has a dedicated implementation file.
# Only the file for the current build platform is compiled.
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    set(CPULIMIT_PROCESS_ITERATOR_SRC
        ${PROJECT_SOURCE_DIR}/src/process_iterator_linux.c)
elseif(CMAKE_SYSTEM_NAME STREQUAL "FreeBSD")
    set(CPULIMIT_PROCESS_ITERATOR_SRC
        ${PROJECT_SOURCE_DIR}/src/process_iterator_freebsd.c)
elseif(CMAKE_SYSTEM_NAME STREQUAL "Darwin")
    set(CPULIMIT_PROCESS_ITERATOR_SRC
        ${PROJECT_SOURCE_DIR}/src/process_iterator_apple.c)
endif()

# --- 4. Common Source Files ---
#
# Library sources shared between the main executable and the test
# binary.  main.c is intentionally excluded; it provides the entry
# point and is added per-target.
set(CPULIMIT_SRC_COMMON
    ${PROJECT_SOURCE_DIR}/src/cli.c
    ${PROJECT_SOURCE_DIR}/src/process_finder.c
    ${PROJECT_SOURCE_DIR}/src/limit_process.c
    ${PROJECT_SOURCE_DIR}/src/limiter.c
    ${PROJECT_SOURCE_DIR}/src/list.c
    ${PROJECT_SOURCE_DIR}/src/process_group.c
    ${CPULIMIT_PROCESS_ITERATOR_SRC}
    ${PROJECT_SOURCE_DIR}/src/process_table.c
    ${PROJECT_SOURCE_DIR}/src/signal_handler.c
    ${PROJECT_SOURCE_DIR}/src/util.c
)
