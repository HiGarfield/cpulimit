# cmake/CheckedFlags.cmake
#
# Provides the add_checked_flags() helper function, which probes each
# flag in a list variable and applies only those accepted by the active
# C compiler to a target's PRIVATE compile options.
#
# Include this file before any target that calls add_checked_flags().

include(CheckCCompilerFlag)

# add_checked_flags(<target> <flag_list_var>)
#
# For every flag in the list variable named by <flag_list_var>, tests
# whether the current C compiler accepts it and, if so, appends it as
# a PRIVATE compile option to <target>.  Flags rejected by the compiler
# are silently skipped, so the build remains portable across compiler
# versions and vendors.
#
# Arguments:
#   target       - Name of an existing CMake target.
#   flag_list_var - Name of the variable that holds the list of flags
#                  (pass the variable name, not its value).
#
# The function preserves the value of CMAKE_REQUIRED_QUIET so callers
# are not affected by the temporary suppression used during probing.
function(add_checked_flags target flag_list_var)
    if(NOT TARGET ${target})
        message(WARNING
            "add_checked_flags: target '${target}' does not exist."
        )
        return()
    endif()
    if(NOT DEFINED ${flag_list_var})
        message(WARNING
            "add_checked_flags: variable '${flag_list_var}' is not defined."
        )
        return()
    endif()

    set(_acf_saved_quiet ${CMAKE_REQUIRED_QUIET})
    set(CMAKE_REQUIRED_QUIET TRUE)

    foreach(_acf_flag IN LISTS ${flag_list_var})
        string(MAKE_C_IDENTIFIER "HAVE_FLAG_${_acf_flag}" _acf_cache_var)
        check_c_compiler_flag("${_acf_flag}" ${_acf_cache_var})
        if(${_acf_cache_var})
            target_compile_options(${target} PRIVATE ${_acf_flag})
        endif()
    endforeach()

    set(CMAKE_REQUIRED_QUIET ${_acf_saved_quiet})
endfunction()
