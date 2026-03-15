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

# --- cpulimit_test ---
add_executable(cpulimit_test
    cpulimit_test.c
    ${CPULIMIT_SRC_COMMON}
)
target_include_directories(cpulimit_test PRIVATE ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(cpulimit_test ${CPULIMIT_PLATFORM_LIBS})
add_checked_flags(cpulimit_test CPULIMIT_C_FLAGS)

add_test(NAME cpulimit_unit_tests COMMAND cpulimit_test)
