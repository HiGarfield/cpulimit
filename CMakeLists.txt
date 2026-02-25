cmake_minimum_required(VERSION 3.5)

project(cpulimit C)

include(GNUInstallDirs)
include(CTest)
include(CheckCCompilerFlag)

set(CMAKE_C_STANDARD 90)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

option(CPULIMIT_BUILD_TESTS "Build test binaries" ON)

function(add_c_compiler_flag_if_supported FLAG)
    string(MAKE_C_IDENTIFIER "HAVE_FLAG_${FLAG}" _test_var)
    check_c_compiler_flag("${FLAG}" ${_test_var})
    if(${_test_var})
        add_compile_options(${FLAG})
    endif()
endfunction()

add_c_compiler_flag_if_supported("-pipe")
add_c_compiler_flag_if_supported("-pedantic")

add_c_compiler_flag_if_supported("-Wall")
add_c_compiler_flag_if_supported("-Wextra")
add_c_compiler_flag_if_supported("-Wpedantic")
add_c_compiler_flag_if_supported("-pedantic-errors")

add_c_compiler_flag_if_supported("-Wconversion")
add_c_compiler_flag_if_supported("-Wsign-conversion")
add_c_compiler_flag_if_supported("-Wdouble-promotion")
add_c_compiler_flag_if_supported("-Wfloat-equal")

add_c_compiler_flag_if_supported("-Wshadow")

add_c_compiler_flag_if_supported("-Wcast-align")
add_c_compiler_flag_if_supported("-Wcast-align=strict")
add_c_compiler_flag_if_supported("-Wstrict-aliasing=2")
add_c_compiler_flag_if_supported("-Wcast-qual")
add_c_compiler_flag_if_supported("-Wwrite-strings")
add_c_compiler_flag_if_supported("-Wpointer-arith")
add_c_compiler_flag_if_supported("-Wbad-function-cast")

add_c_compiler_flag_if_supported("-Wstrict-prototypes")
add_c_compiler_flag_if_supported("-Wold-style-definition")
add_c_compiler_flag_if_supported("-Wmissing-prototypes")
add_c_compiler_flag_if_supported("-Wmissing-declarations")
add_c_compiler_flag_if_supported("-Wmissing-parameter-type")
add_c_compiler_flag_if_supported("-Wredundant-decls")
add_c_compiler_flag_if_supported("-Wnested-externs")

add_c_compiler_flag_if_supported("-Wstrict-overflow=1")
add_c_compiler_flag_if_supported("-Wstrict-overflow=2")
add_c_compiler_flag_if_supported("-Wstrict-overflow=3")
add_c_compiler_flag_if_supported("-Wstrict-overflow=4")
add_c_compiler_flag_if_supported("-Wstrict-overflow=5")

add_c_compiler_flag_if_supported("-Wundef")
add_c_compiler_flag_if_supported("-Wendif-labels")
add_c_compiler_flag_if_supported("-Wexpansion-to-defined")

add_c_compiler_flag_if_supported("-Wformat=2")
add_c_compiler_flag_if_supported("-Wformat-overflow=1")
add_c_compiler_flag_if_supported("-Wformat-overflow=2")
add_c_compiler_flag_if_supported("-Wformat-truncation=1")
add_c_compiler_flag_if_supported("-Wformat-truncation=2")
add_c_compiler_flag_if_supported("-Wformat-security")
add_c_compiler_flag_if_supported("-Wformat-signedness")
add_c_compiler_flag_if_supported("-Wnull-dereference")

add_c_compiler_flag_if_supported("-Wswitch")
add_c_compiler_flag_if_supported("-Wswitch-enum")
add_c_compiler_flag_if_supported("-Wswitch-default")
add_c_compiler_flag_if_supported("-Wswitch-bool")

add_c_compiler_flag_if_supported("-Wimplicit")
add_c_compiler_flag_if_supported("-Wimplicit-int")
add_c_compiler_flag_if_supported("-Wimplicit-function-declaration")
add_c_compiler_flag_if_supported("-Wimplicit-fallthrough")
add_c_compiler_flag_if_supported("-Wimplicit-fallthrough=5")

add_c_compiler_flag_if_supported("-Wmissing-field-initializers")
add_c_compiler_flag_if_supported("-Wmissing-braces")
add_c_compiler_flag_if_supported("-Winit-self")

add_c_compiler_flag_if_supported("-Waggregate-return")
add_c_compiler_flag_if_supported("-Wpacked")
add_c_compiler_flag_if_supported("-Wvla")
add_c_compiler_flag_if_supported("-Winline")

add_c_compiler_flag_if_supported("-Wlogical-op")
add_c_compiler_flag_if_supported("-Wduplicated-cond")
add_c_compiler_flag_if_supported("-Wduplicated-branches")
add_c_compiler_flag_if_supported("-Wrestrict")

add_c_compiler_flag_if_supported("-Wjump-misses-init")
add_c_compiler_flag_if_supported("-Wc90-c99-compat")
add_c_compiler_flag_if_supported("-Wc99-c11-compat")
add_c_compiler_flag_if_supported("-Wc11-c23-compat")
add_c_compiler_flag_if_supported("-Wc23-c2y-compat")
add_c_compiler_flag_if_supported("-Wc++-compat")

add_c_compiler_flag_if_supported("-Wlarger-than=256")
add_c_compiler_flag_if_supported("-Wstack-usage=512")
add_c_compiler_flag_if_supported("-Wframe-larger-than=512")
add_c_compiler_flag_if_supported("-Wstack-protector")

add_c_compiler_flag_if_supported("-Wunused")
add_c_compiler_flag_if_supported("-Wmissing-format-attribute")
add_c_compiler_flag_if_supported("-Wdisabled-optimization")
add_c_compiler_flag_if_supported("-Warray-bounds=2")
add_c_compiler_flag_if_supported("-Wtautological-compare")

add_c_compiler_flag_if_supported("-fanalyzer")
add_c_compiler_flag_if_supported("-Wanalyzer-null-dereference")
add_c_compiler_flag_if_supported("-Wanalyzer-double-free")
add_c_compiler_flag_if_supported("-Wanalyzer-use-after-free")
add_c_compiler_flag_if_supported("-Wanalyzer-malloc-leak")
add_c_compiler_flag_if_supported("-Wanalyzer-possible-null-dereference")
add_c_compiler_flag_if_supported("-Wanalyzer-shift-count-negative")
add_c_compiler_flag_if_supported("-Wanalyzer-shift-count-overflow")
add_c_compiler_flag_if_supported("-Wanalyzer-out-of-bounds")
add_c_compiler_flag_if_supported("-Wanalyzer-unsafe-call-within-signal-handler")

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
