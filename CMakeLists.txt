cmake_minimum_required(VERSION 3.5)

project(cpulimit C)

# OS check - only supports Linux, macOS (Darwin), and FreeBSD
set(SUPPORTED_OS Linux Darwin FreeBSD)

list(FIND SUPPORTED_OS ${CMAKE_SYSTEM_NAME} _os_index)
if(_os_index EQUAL -1)
    message(FATAL_ERROR
        "Unsupported operating system: ${CMAKE_SYSTEM_NAME}\n"
        "This project only supports: ${SUPPORTED_OS}"
    )
endif()

include(GNUInstallDirs)
include(CTest)
include(CheckCCompilerFlag)

set(CMAKE_C_STANDARD 90)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

# Generate compile_commands.json for use by static analysis tools
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Set a default build type of Release if none is specified
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

option(CPULIMIT_BUILD_TESTS "Build test binaries" ON)

set(CPULIMIT_C_FLAGS
    -pipe
    -pedantic
    -Wall
    -Wextra
    -Wpedantic
    -pedantic-errors
    -Wconversion
    -Wsign-conversion
    -Wdouble-promotion
    -Wfloat-equal
    -Wshadow
    -Wcast-align
    -Wcast-align=strict
    -Wstrict-aliasing=2
    -Wcast-qual
    -Wwrite-strings
    -Wpointer-arith
    -Wbad-function-cast
    -Wstrict-prototypes
    -Wold-style-definition
    -Wmissing-prototypes
    -Wmissing-declarations
    -Wmissing-parameter-type
    -Wredundant-decls
    -Wnested-externs
    -Wstrict-overflow=1
    -Wstrict-overflow=2
    -Wstrict-overflow=3
    -Wstrict-overflow=4
    -Wstrict-overflow=5
    -Wundef
    -Wendif-labels
    -Wexpansion-to-defined
    -Wformat=2
    -Wformat-overflow=1
    -Wformat-overflow=2
    -Wformat-truncation=1
    -Wformat-truncation=2
    -Wformat-security
    -Wformat-signedness
    -Wnull-dereference
    -Wswitch
    -Wswitch-enum
    -Wswitch-default
    -Wswitch-bool
    -Wimplicit
    -Wimplicit-int
    -Wimplicit-function-declaration
    -Wimplicit-fallthrough
    -Wimplicit-fallthrough=5
    -Wmissing-field-initializers
    -Wmissing-braces
    -Winit-self
    -Waggregate-return
    -Wpacked
    -Wvla
    -Winline
    -Wlogical-op
    -Wduplicated-cond
    -Wduplicated-branches
    -Wrestrict
    -Wjump-misses-init
    -Wc90-c99-compat
    -Wc99-c11-compat
    -Wc11-c23-compat
    -Wc23-c2y-compat
    -Wc++-compat
    -Wlarger-than=256
    -Wstack-usage=512
    -Wframe-larger-than=512
    -Wstack-protector
    -Wunused
    -Wmissing-format-attribute
    -Wdisabled-optimization
    -Warray-bounds=2
    -Wtautological-compare
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
    -Werror
)

foreach(_flag IN LISTS CPULIMIT_C_FLAGS)
    string(MAKE_C_IDENTIFIER "HAVE_FLAG_${_flag}" _test_var)
    check_c_compiler_flag("${_flag}" ${_test_var})
    if(${_test_var})
        add_compile_options(${_flag})
    endif()
endforeach()

# Force 64-bit time_t and file offsets to prevent Y2038 issue
add_compile_definitions(_TIME_BITS=64 _FILE_OFFSET_BITS=64)

set(CPULIMIT_PLATFORM_LIBS)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    list(APPEND CPULIMIT_PLATFORM_LIBS rt)
elseif(CMAKE_SYSTEM_NAME STREQUAL "FreeBSD")
    list(APPEND CPULIMIT_PLATFORM_LIBS kvm)
elseif(CMAKE_SYSTEM_NAME STREQUAL "Darwin")
    list(APPEND CPULIMIT_PLATFORM_LIBS proc)
endif()

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

add_executable(cpulimit
    src/main.c
    ${CPULIMIT_SRC_COMMON}
)

target_include_directories(cpulimit PRIVATE src)
target_link_libraries(cpulimit ${CPULIMIT_PLATFORM_LIBS})

install(TARGETS cpulimit RUNTIME DESTINATION bin)

if(CPULIMIT_BUILD_TESTS)
    enable_testing()

    add_executable(busy
        tests/busy.c
        src/signal_handler.c
        src/util.c
    )
    find_package(Threads REQUIRED)
    target_link_libraries(busy ${CPULIMIT_PLATFORM_LIBS} Threads::Threads)

    add_executable(multi_process_busy
        tests/multi_process_busy.c
        src/signal_handler.c
        src/util.c
    )
    target_link_libraries(multi_process_busy ${CPULIMIT_PLATFORM_LIBS})

    add_executable(cpulimit_test
        tests/cpulimit_test.c
        ${CPULIMIT_SRC_COMMON}
    )
    target_include_directories(cpulimit_test PRIVATE src)
    target_link_libraries(cpulimit_test ${CPULIMIT_PLATFORM_LIBS})

    add_test(NAME cpulimit_unit_tests COMMAND cpulimit_test)
endif()
