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

# --- 11. Static Analysis Targets ---
# Implements cppcheck, clang-tidy, and check targets directly without
# delegating to src/Makefile or tests/Makefile.
# Reports are placed in the cmake binary (build) directory:
#   ${CMAKE_BINARY_DIR}/cppcheck-report.txt
#   ${CMAKE_BINARY_DIR}/clang-tidy-report.txt

find_program(CPPCHECK_EXECUTABLE NAMES cppcheck)
find_program(CLANG_TIDY_EXECUTABLE NAMES clang-tidy)

if(NOT CPPCHECK_EXECUTABLE)
    message(STATUS
        "cppcheck not found; 'cppcheck'/'check' targets will require it")
    set(CPPCHECK_EXECUTABLE cppcheck)
endif()

if(NOT CLANG_TIDY_EXECUTABLE)
    message(STATUS
        "clang-tidy not found; 'clang-tidy'/'check' targets will require it")
    set(CLANG_TIDY_EXECUTABLE clang-tidy)
endif()

# Report paths (all placed in the cmake binary / build directory).
set(CPPCHECK_REPORT "${CMAKE_BINARY_DIR}/cppcheck-report.txt")
set(CLANG_TIDY_REPORT "${CMAKE_BINARY_DIR}/clang-tidy-report.txt")
# clang-tidy's -p flag takes a directory and looks for compile_commands.json
# inside it, so the filtered database is stored in a dedicated subdirectory.
set(CLANG_TIDY_DB_DIR "${CMAKE_BINARY_DIR}/clang_tidy_db")

# All source files subject to static analysis (src + tests).
file(GLOB _analysis_src_files
    "${CMAKE_SOURCE_DIR}/src/*.c"
    "${CMAKE_SOURCE_DIR}/src/*.h"
)
file(GLOB _analysis_tests_files "${CMAKE_SOURCE_DIR}/tests/*.c")
set(_all_analysis_files ${_analysis_src_files} ${_analysis_tests_files})
string(REPLACE ";" " " _all_analysis_files_str "${_all_analysis_files}")

# macOS SDK path for clang-tidy (Darwin only).
set(_clang_tidy_sdk_args "")
if(CMAKE_SYSTEM_NAME STREQUAL "Darwin")
    execute_process(
        COMMAND xcrun --show-sdk-path
        OUTPUT_VARIABLE _macos_sdk
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
    )
    if(_macos_sdk)
        set(_clang_tidy_sdk_args
            "--extra-arg=-isysroot --extra-arg=${_macos_sdk}")
    endif()
endif()

# Build targets that must exist before running static analysis.
set(_analysis_deps cpulimit)
if(CPULIMIT_BUILD_TESTS)
    list(APPEND _analysis_deps cpulimit_test busy multi_process_busy)
endif()

# Generate a cmake script that filters GCC-only flags from
# compile_commands.json so that clang-tidy (which uses clang internally)
# can process the compilation database without errors.
# cmake's compile_commands.json may include GCC-only flags such as
# -fanalyzer (which causes a hard error in clang) and various -W* flags
# that produce "unknown warning option" noise in clang-tidy output.
# The script produces a filtered copy in clang_tidy_db/compile_commands.json;
# clang-tidy's -p flag takes a directory and reads compile_commands.json from
# it, so this subdirectory approach ensures the right database is used.
set(_filter_script "${CMAKE_BINARY_DIR}/filter_compile_db.cmake")
file(WRITE "${_filter_script}"
    "# Auto-generated: filter GCC-only flags for clang-tidy.\n"
    "file(READ \"${CMAKE_BINARY_DIR}/compile_commands.json\" _db)\n"
    "foreach(_flag IN ITEMS\n"
    "    \" -fanalyzer\"\n"
    "    \" -Wanalyzer-null-dereference\"\n"
    "    \" -Wanalyzer-double-free\"\n"
    "    \" -Wanalyzer-use-after-free\"\n"
    "    \" -Wanalyzer-malloc-leak\"\n"
    "    \" -Wanalyzer-possible-null-dereference\"\n"
    "    \" -Wanalyzer-shift-count-negative\"\n"
    "    \" -Wanalyzer-shift-count-overflow\"\n"
    "    \" -Wanalyzer-out-of-bounds\"\n"
    "    \" -Wanalyzer-unsafe-call-within-signal-handler\"\n"
    "    \" -Wlogical-op\"\n"
    "    \" -Wrestrict\"\n"
    "    \" -Wduplicated-cond\"\n"
    "    \" -Wduplicated-branches\"\n"
    "    \" -Wduplicated-arg\"\n"
    "    \" -Wpacked-not-aligned\"\n"
    "    \" -Wswitch-bool\"\n"
    "    \" -Wswitch-unreachable\"\n"
    "    \" -Wjump-misses-init\"\n"
    "    \" -Waggressive-loop-optimizations\"\n"
    "    \" -Wmultistatement-macros\"\n"
    "    \" -Wcast-function-type\"\n"
    "    \" -Wfree-nonheap-object\"\n"
    "    \" -Wmaybe-uninitialized\"\n"
    "    \" -Wclobbered\"\n"
    ")\n"
    "    string(REPLACE \"\${_flag}\" \"\" _db \"\${_db}\")\n"
    "endforeach()\n"
    "foreach(_prefix IN ITEMS\n"
    "    \" -Wformat-overflow\"\n"
    "    \" -Wformat-truncation\"\n"
    "    \" -Wformat-signedness\"\n"
    "    \" -Wformat-contains-nul\"\n"
    "    \" -Wformat-zero-length\"\n"
    "    \" -Wstringop-overflow\"\n"
    "    \" -Wstringop-overread\"\n"
    "    \" -Wstringop-truncation\"\n"
    "    \" -Warray-parameter\"\n"
    "    \" -Wattribute-alias\"\n"
    "    \" -Wzero-length-bounds\"\n"
    "    \" -Wstrict-flex-arrays\"\n"
    "    \" -Wstack-usage\"\n"
    "    \" -Wframe-larger-than\"\n"
    "    \" -Wbidi-chars\"\n"
    "    \" -Wnormalized\"\n"
    ")\n"
    "    string(REGEX REPLACE \"\${_prefix}[^ ]*\" \"\" _db \"\${_db}\")\n"
    "endforeach()\n"
    "file(MAKE_DIRECTORY \"${CLANG_TIDY_DB_DIR}\")\n"
    "file(WRITE \"${CLANG_TIDY_DB_DIR}/compile_commands.json\" \"\${_db}\")\n"
)

# cppcheck target: uses cmake's compile_commands.json which covers all
# source files (both src/ and tests/). Reports to build/cppcheck-report.txt.
string(CONCAT _cppcheck_cmd
    "${CPPCHECK_EXECUTABLE}"
    " --enable=all --language=c --inconclusive"
    " --check-level=exhaustive --max-ctu-depth=20"
    " --library=gnu --force --std=c89"
    " --project=${CMAKE_BINARY_DIR}/compile_commands.json"
    " --suppress=missingIncludeSystem"
    " --suppress=ConfigurationNotChecked"
    " --suppress=unmatchedSuppression"
    " --suppress=checkersReport"
    " 2>${CPPCHECK_REPORT}")

add_custom_target(cppcheck
    COMMAND sh -c "${_cppcheck_cmd}"
    COMMAND ${CMAKE_COMMAND} -E echo
        "See ${CPPCHECK_REPORT} for cppcheck report."
    DEPENDS ${_analysis_deps}
    COMMENT "Running cppcheck..."
    VERBATIM
)

# clang-tidy target: first generates a filtered compile database (removing
# GCC-only flags), then runs clang-tidy on all source files.
# Reports to build/clang-tidy-report.txt.
string(CONCAT _clang_tidy_cmd
    "${CLANG_TIDY_EXECUTABLE}"
    " ${_clang_tidy_sdk_args}"
    " -p ${CLANG_TIDY_DB_DIR}"
    " ${_all_analysis_files_str}"
    " >${CLANG_TIDY_REPORT}")

add_custom_target(clang-tidy
    COMMAND ${CMAKE_COMMAND} -P "${_filter_script}"
    COMMAND sh -c "${_clang_tidy_cmd}"
    COMMAND ${CMAKE_COMMAND} -E echo
        "See ${CLANG_TIDY_REPORT} for clang-tidy report."
    DEPENDS ${_analysis_deps}
    COMMENT "Running clang-tidy..."
    VERBATIM
)

# check target: runs both cppcheck and clang-tidy analyses.
# Reports are placed in ${CMAKE_BINARY_DIR}/:
#   cppcheck-report.txt   - cppcheck output covering src/ and tests/
#   clang-tidy-report.txt - clang-tidy output covering src/ and tests/
add_custom_target(check
    DEPENDS cppcheck clang-tidy
    COMMENT
        "Static analysis complete. Reports in ${CMAKE_BINARY_DIR}/"
)
