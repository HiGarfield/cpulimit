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
    -Wold-style-declaration
    -Wmissing-prototypes
    -Wmissing-declarations
    -Wmissing-parameter-type
    -Wredundant-decls
    -Wnested-externs
    -Wvariadic-macros
    -Wdeclaration-after-statement
    -Wc90-c99-compat
    -Wc99-c11-compat
    -Wc11-c2x-compat
    -Wpre-c11-compat
    -Wc11-extensions
    -Wc2x-extensions

    # Common programming errors
    -Winit-self
    -Waggregate-return
    -Wstrict-aliasing=2
    -Wcast-qual
    -Wwrite-strings
    -Warray-compare
    -Wbitwise-instead-of-logical
    -Wxor-used-as-pow
    -Wenum-int-mismatch
    -Warith-conversion
    -Wcast-function-type-strict
    -Wuse-after-free=3
    -Wdangling-pointer=2
    -Wstrict-flex-arrays=3
    -Wflex-array-member-not-at-end
    -Wunterminated-string-initialization
    -Wheader-guard
    -Wkeyword-macro
    -Wuseless-casts
    -Wmultiple-parameter-fwd-decl-lists
    -Wfree-labels
    -Wtrampolines
    -Wunsafe-loop-optimizations
    -Wvector-operation-performance

    # Type conversion and precision
    -Wconversion
    -Wsign-conversion
    -Wdouble-promotion
    -Wfloat-equal
    -Wfloat-conversion

    # Variables and scope
    -Wshadow
    -Wshadow=local
    -Wshadow-all
    -Wmissing-variable-declarations
    -Wcast-align=strict
    -Wpointer-arith
    -Wtype-limits
    -Wbad-function-cast

    # Logic and expressions
    -Wlogical-op
    -Wnull-dereference
    -Wassign-enum
    -Wenum-conversion
    -Wcomma
    -Wtautological-constant-out-of-range-compare
    -Wtautological-unsigned-zero-compare
    -Wtautological-unsigned-enum-zero-compare
    -Wtautological-type-limit-compare
    -Wtautological-constant-compare
    -Wtautological-bitwise-compare
    -Wtautological-overlap-compare
    -Wtautological-pointer-compare
    -Wtautological-undefined-compare

    -Wrestrict
    -Wimplicit-fallthrough
    -Wimplicit-fallthrough=5

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
    -Wformat-nonliteral
    -Wformat-type-confusion
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

    # Additional warnings
    -Wundef
    -Winline
    -Wmissing-include-dirs
    -Wstrict-overflow=5
    -Wmisleading-indentation

    # Compiler optimization flags
    -pipe

    # Static analyzer
    -fanalyzer
    -Wanalyzer-null-dereference
    -Wanalyzer-null-argument
    -Wanalyzer-double-free
    -Wanalyzer-use-after-free
    -Wanalyzer-malloc-leak
    -Wanalyzer-possible-null-dereference
    -Wanalyzer-shift-count-negative
    -Wanalyzer-shift-count-overflow
    -Wanalyzer-out-of-bounds
    -Wanalyzer-unsafe-call-within-signal-handler
    -Wanalyzer-write-to-const
    -Wanalyzer-write-to-string-literal
    -Wanalyzer-file-leak
    -Wanalyzer-exposure-through-output-file

    # Clang-only diagnostics, skipped where the compiler does not know them
    -Wextra-semi
    -Wnewline-eof
    -Wreserved-identifier
    -Wconditional-uninitialized
    -Wshift-bool
    -Walloc-size
    -Wenum-compare-typo
    -Wunique-object-duplication
    -Winvalid-utf8
    -Wbitfield-width
    -Wimplicit-int-conversion
    -Wshorten-64-to-32
    -Wnonnull
    -Wnullable-to-nonnull-conversion
    -Wnullability-extension
    -Wheader-hygiene
    -Wloop-analysis
    -Wunreachable-code-aggressive
    -Wthread-safety-pointer
)

# --- 3b. Warnings as errors ---
#
# Off by default: the flags above are already strict, and a new compiler
# release may add diagnostics that a user's toolchain cannot satisfy.  Turn
# it on in CI or locally with -DCPULIMIT_WERROR=ON.
if(CPULIMIT_WERROR)
    list(APPEND CPULIMIT_C_FLAGS -Werror)
endif()

# --- 4. Common Source Files ---
#
# Library sources shared between the main executable and the test
# binary.  main.c is intentionally excluded; it provides the entry
# point and is added per-target.
set(CPULIMIT_SRC_COMMON
    ${PROJECT_SOURCE_DIR}/src/child_exec.c
    ${PROJECT_SOURCE_DIR}/src/child_wait.c
    ${PROJECT_SOURCE_DIR}/src/cli.c
    ${PROJECT_SOURCE_DIR}/src/cpu_count.c
    ${PROJECT_SOURCE_DIR}/src/exec_sync.c
    ${PROJECT_SOURCE_DIR}/src/file_io.c
    ${PROJECT_SOURCE_DIR}/src/limit_process.c
    ${PROJECT_SOURCE_DIR}/src/limiter.c
    ${PROJECT_SOURCE_DIR}/src/list.c
    ${PROJECT_SOURCE_DIR}/src/path_util.c
    ${PROJECT_SOURCE_DIR}/src/process_finder.c
    ${PROJECT_SOURCE_DIR}/src/process_set.c
    ${PROJECT_SOURCE_DIR}/src/process_iterator_common.c
    ${PROJECT_SOURCE_DIR}/src/process_iterator_linux.c
    ${PROJECT_SOURCE_DIR}/src/process_iterator_freebsd.c
    ${PROJECT_SOURCE_DIR}/src/process_iterator_apple.c
    ${PROJECT_SOURCE_DIR}/src/process_table.c
    ${PROJECT_SOURCE_DIR}/src/script_check.c
    ${PROJECT_SOURCE_DIR}/src/signal_forward.c
    ${PROJECT_SOURCE_DIR}/src/signal_handler.c
    ${PROJECT_SOURCE_DIR}/src/time_util.c
    ${PROJECT_SOURCE_DIR}/src/util.c
)
