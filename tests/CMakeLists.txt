find_package(Threads REQUIRED)

# --- busy ---
add_executable(busy
    busy.c
    ${PROJECT_SOURCE_DIR}/src/signal_handler.c
    ${PROJECT_SOURCE_DIR}/src/util.c
)
target_link_libraries(busy ${CPULIMIT_PLATFORM_LIBS} Threads::Threads)
add_checked_flags(busy CPULIMIT_C_FLAGS)

# --- multi_process_busy ---
add_executable(multi_process_busy
    multi_process_busy.c
    ${PROJECT_SOURCE_DIR}/src/signal_handler.c
    ${PROJECT_SOURCE_DIR}/src/util.c
)
target_link_libraries(multi_process_busy ${CPULIMIT_PLATFORM_LIBS})
add_checked_flags(multi_process_busy CPULIMIT_C_FLAGS)

# --- 21. Deterministic timing seam (test binary only) ---
#
# The race tests in cpulimit_test.c must not depend on scheduling luck, so
# they replace the sources of non-determinism: the monotonic clock and the
# sleeps driven by it, the process iterator, kill(), random() (duty-cycle
# jitter) and getloadavg() (time slot scaling).
#
# The replacements are reached by renaming those entry points at compile
# time, only for the sources compiled into this test binary; cpulimit_test.c
# itself is deliberately left alone so that it keeps calling the real
# implementations and can therefore forward to them while the seam is
# inactive. Linker interposition (-Wl,--wrap=...) is not used because
# Apple's ld64 rejects it ("ld: unknown options: --wrap=kill").
#
# A file that DEFINES a renamed function must be excluded from that rename,
# otherwise its definition would be renamed too and collide with the
# replacement: time_util.c defines get_current_time()/sleep_timespec(),
# and the platform iterators define init_process_iterator(),
# get_next_process() and close_process_iterator().
#
# getloadavg() is only renamed here because util.h redirects it to
# cpulimit_getloadavg() solely for uClibc < 1.0.42, which is not a
# supported test host; on Linux/macOS/FreeBSD it is the libc symbol.

set(CPULIMIT_TEST_SEAM_ALL
    kill=cpulimit_test_kill
    random=cpulimit_test_random
    getloadavg=cpulimit_test_getloadavg
    waitpid=cpulimit_test_waitpid)

set(CPULIMIT_TEST_SEAM_NO_DEF
    get_current_time=cpulimit_test_get_current_time
    sleep_timespec=cpulimit_test_sleep_timespec
    init_process_iterator=cpulimit_test_init_process_iterator
    get_next_process=cpulimit_test_get_next_process
    close_process_iterator=cpulimit_test_close_process_iterator
    limit_process=cpulimit_test_limit_process)

set(CPULIMIT_TEST_SEAM_DEFINING_SOURCES
    ${PROJECT_SOURCE_DIR}/src/time_util.c
    ${PROJECT_SOURCE_DIR}/src/limit_process.c
    ${PROJECT_SOURCE_DIR}/src/process_iterator_linux.c
    ${PROJECT_SOURCE_DIR}/src/process_iterator_freebsd.c
    ${PROJECT_SOURCE_DIR}/src/process_iterator_apple.c)

set(CPULIMIT_TEST_SEAM_OTHER_SOURCES ${CPULIMIT_SRC_COMMON})
list(REMOVE_ITEM CPULIMIT_TEST_SEAM_OTHER_SOURCES
     ${CPULIMIT_TEST_SEAM_DEFINING_SOURCES})

set_source_files_properties(${CPULIMIT_TEST_SEAM_DEFINING_SOURCES} PROPERTIES
    COMPILE_DEFINITIONS "${CPULIMIT_TEST_SEAM_ALL}")
set_source_files_properties(${CPULIMIT_TEST_SEAM_OTHER_SOURCES} PROPERTIES
    COMPILE_DEFINITIONS "${CPULIMIT_TEST_SEAM_ALL};${CPULIMIT_TEST_SEAM_NO_DEF}")

# --- cpulimit_test ---
add_executable(cpulimit_test
    cpulimit_test.c
    ${CPULIMIT_SRC_COMMON}
)
target_include_directories(cpulimit_test PRIVATE ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(cpulimit_test ${CPULIMIT_PLATFORM_LIBS})
add_checked_flags(cpulimit_test CPULIMIT_C_FLAGS)

add_test(NAME cpulimit_unit_tests COMMAND cpulimit_test)
set_tests_properties(cpulimit_unit_tests PROPERTIES TIMEOUT 120)
