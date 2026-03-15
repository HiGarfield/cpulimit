# --- Main Executable ---
add_executable(cpulimit
    main.c
    ${CPULIMIT_SRC_COMMON}
)
target_include_directories(cpulimit PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(cpulimit ${CPULIMIT_PLATFORM_LIBS})
add_checked_flags(cpulimit CPULIMIT_C_FLAGS)

install(TARGETS cpulimit RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
