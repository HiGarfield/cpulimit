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
# A file that DEFINES a renamed function must be excluded from that one
# rename only, otherwise its definition would be renamed too and collide
# with the replacement. Excluding it from the whole set would silently
# leave that file on the real clock, iterator and sleeps:
#   - time_util.c defines get_current_time() and sleep_timespec()
#   - the platform iterators define init_process_iterator(),
#     get_next_process() and close_process_iterator()
#   - limit_process.c defines limit_process()
# so each of the three groups below gets every rename except its own.
#
# getloadavg() is only renamed here because util.h redirects it to
# cpulimit_getloadavg() solely for uClibc < 1.0.42, which is not a
# supported test host; on Linux/macOS/FreeBSD it is the libc symbol.

set(CPULIMIT_TEST_SEAM_ALL
    kill=cpulimit_test_kill
    random=cpulimit_test_random
    getloadavg=cpulimit_test_getloadavg
    waitpid=cpulimit_test_waitpid)

set(CPULIMIT_TEST_SEAM_TIME_ITER
    get_current_time=cpulimit_test_get_current_time
    sleep_timespec=cpulimit_test_sleep_timespec
    init_process_iterator=cpulimit_test_init_process_iterator
    get_next_process=cpulimit_test_get_next_process
    close_process_iterator=cpulimit_test_close_process_iterator)

set(CPULIMIT_TEST_SEAM_LIMIT
    limit_process=cpulimit_test_limit_process)

set(CPULIMIT_TEST_SEAM_TIME_ITER_DEFINERS
    ${PROJECT_SOURCE_DIR}/src/time_util.c
    ${PROJECT_SOURCE_DIR}/src/process_iterator_linux.c
    ${PROJECT_SOURCE_DIR}/src/process_iterator_freebsd.c
    ${PROJECT_SOURCE_DIR}/src/process_iterator_apple.c)

set(CPULIMIT_TEST_SEAM_LIMIT_DEFINERS
    ${PROJECT_SOURCE_DIR}/src/limit_process.c)

# The three groups have to be disjoint: a source file property is replaced,
# not merged, so a file listed twice would end up with the last set only.
set(CPULIMIT_TEST_SEAM_PLAIN_SOURCES ${CPULIMIT_SRC_COMMON})
list(REMOVE_ITEM CPULIMIT_TEST_SEAM_PLAIN_SOURCES
     ${CPULIMIT_TEST_SEAM_TIME_ITER_DEFINERS}
     ${CPULIMIT_TEST_SEAM_LIMIT_DEFINERS})

set_source_files_properties(${CPULIMIT_TEST_SEAM_PLAIN_SOURCES} PROPERTIES
    COMPILE_DEFINITIONS
    "${CPULIMIT_TEST_SEAM_ALL};${CPULIMIT_TEST_SEAM_TIME_ITER};${CPULIMIT_TEST_SEAM_LIMIT}")
set_source_files_properties(${CPULIMIT_TEST_SEAM_TIME_ITER_DEFINERS} PROPERTIES
    COMPILE_DEFINITIONS
    "${CPULIMIT_TEST_SEAM_ALL};${CPULIMIT_TEST_SEAM_LIMIT}")
set_source_files_properties(${CPULIMIT_TEST_SEAM_LIMIT_DEFINERS} PROPERTIES
    COMPILE_DEFINITIONS
    "${CPULIMIT_TEST_SEAM_ALL};${CPULIMIT_TEST_SEAM_TIME_ITER}")

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
