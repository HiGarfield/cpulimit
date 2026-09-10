# cmake/CheckedFlags.cmake
#
# Provides the add_checked_flags() helper function, which probes each
# flag in a list variable and applies only those accepted by the active
# C compiler to a target's PRIVATE compile options.
#
# Include this file before any target that calls add_checked_flags().

include(CheckCSourceCompiles)

# cpulimit_check_c_flag(<flag> <result_var>)
#
# Wrapper around check_c_source_compiles() that compiles a trivial source
# with <flag> and stores whether the compiler took it in <result_var>.
#
# Compilers that do not know a flag often only warn about it, and
# check_c_compiler_flag() reports success in that case: the flag then
# repeats that warning for every source file of every target.  Apple
# clang is one of them -- it drops -fanalyzer with "argument unused
# during compilation" and exits 0 -- and old CMake releases ship a check
# module whose only rejection pattern is the GNU wording, so the flag
# slipped through.  The diagnostics below are therefore treated as a
# rejection here, whatever CMake version is driving the build.
function(cpulimit_check_c_flag _flag _result)
    set(_ccf_flags "${CMAKE_REQUIRED_FLAGS}")
    set(_ccf_quiet "${CMAKE_REQUIRED_QUIET}")
    set(CMAKE_REQUIRED_FLAGS "${_flag}")
    set(CMAKE_REQUIRED_QUIET TRUE)
    check_c_source_compiles("int main(void) { return 0; }" ${_result}
        FAIL_REGEX "argument unused during compilation"
        FAIL_REGEX "unknown warning option"
        FAIL_REGEX "unknown argument"
        FAIL_REGEX "unknown .*option"
        FAIL_REGEX "unsupported .*option"
        FAIL_REGEX "unrecognized .*option"
        FAIL_REGEX "command line option .* is valid for .* but not for C"
        FAIL_REGEX "ignoring unknown option"
        FAIL_REGEX "[Ww]arning: [Oo]ption"
        FAIL_REGEX "not supported in this configuration; ignored"
    )
    set(CMAKE_REQUIRED_FLAGS "${_ccf_flags}")
    set(CMAKE_REQUIRED_QUIET "${_ccf_quiet}")
endfunction()

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

    foreach(_acf_flag IN LISTS ${flag_list_var})
        string(MAKE_C_IDENTIFIER "HAVE_FLAG_${_acf_flag}" _acf_cache_var)
        cpulimit_check_c_flag("${_acf_flag}" ${_acf_cache_var})
        if(${_acf_cache_var})
            target_compile_options(${target} PRIVATE ${_acf_flag})
        endif()
    endforeach()

endfunction()
