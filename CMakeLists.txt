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

option(CPULIMIT_BUILD_TESTS "Build test binaries" ON)

function(cpulimit_collect_supported_c_flags OUT_VAR)
    set(_supported_flags)
    foreach(_flag ${ARGN})
        string(MAKE_C_IDENTIFIER "HAVE_FLAG_${_flag}" _test_var)
        check_c_compiler_flag("${_flag}" ${_test_var})
        if(${_test_var})
            list(APPEND _supported_flags ${_flag})
        endif()
    endforeach()
    set(${OUT_VAR} "${_supported_flags}" PARENT_SCOPE)
endfunction()

set(CPULIMIT_WARNING_FLAGS
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
)

cpulimit_collect_supported_c_flags(
    CPULIMIT_SUPPORTED_WARNING_FLAGS
    ${CPULIMIT_WARNING_FLAGS}
)

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

function(cpulimit_apply_common_target_settings TARGET_NAME)
    target_compile_options(
        ${TARGET_NAME}
        PRIVATE ${CPULIMIT_SUPPORTED_WARNING_FLAGS}
    )
endfunction()

add_executable(cpulimit
    src/main.c
    ${CPULIMIT_SRC_COMMON}
)

cpulimit_apply_common_target_settings(cpulimit)
target_include_directories(cpulimit PRIVATE src)
target_link_libraries(cpulimit PRIVATE ${CPULIMIT_PLATFORM_LIBS})

install(TARGETS cpulimit RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

if(CPULIMIT_BUILD_TESTS)
    enable_testing()
    find_package(Threads REQUIRED)

    add_executable(busy
        tests/busy.c
        src/signal_handler.c
        src/util.c
    )
    cpulimit_apply_common_target_settings(busy)
    target_link_libraries(
        busy
        PRIVATE
            ${CPULIMIT_PLATFORM_LIBS}
            Threads::Threads
    )

    add_executable(multi_process_busy
        tests/multi_process_busy.c
        src/signal_handler.c
        src/util.c
    )
    cpulimit_apply_common_target_settings(multi_process_busy)
    target_link_libraries(
        multi_process_busy
        PRIVATE ${CPULIMIT_PLATFORM_LIBS}
    )

    add_executable(cpulimit_test
        tests/cpulimit_test.c
        ${CPULIMIT_SRC_COMMON}
    )
    cpulimit_apply_common_target_settings(cpulimit_test)
    target_include_directories(cpulimit_test PRIVATE src)
    target_link_libraries(cpulimit_test PRIVATE ${CPULIMIT_PLATFORM_LIBS})

    add_test(NAME cpulimit_unit_tests COMMAND cpulimit_test)
endif()
