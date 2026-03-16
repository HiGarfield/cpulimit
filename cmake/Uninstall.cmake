# cmake/Uninstall.cmake
#
# Removes all files that were recorded in the CMake install manifest.
# Invoked by the 'uninstall' custom target in the root CMakeLists.txt.
#
# Required variables (pass with -D on the cmake -P command line):
#   BUILD_DIR - absolute path to the cmake build directory

if(NOT EXISTS "${BUILD_DIR}/install_manifest.txt")
    message(FATAL_ERROR
        "Cannot find install manifest: "
        "${BUILD_DIR}/install_manifest.txt\n"
        "The project has not been installed yet."
    )
endif()

file(READ "${BUILD_DIR}/install_manifest.txt" _files)
# Normalise Windows line endings before splitting.
string(REPLACE "\r" "" _files "${_files}")
string(REGEX REPLACE "\n" ";" _files "${_files}")

foreach(_file ${_files})
    # Skip empty entries produced by a trailing newline in the manifest.
    if(NOT _file)
        continue()
    endif()
    if(EXISTS "$ENV{DESTDIR}${_file}" OR
       IS_SYMLINK "$ENV{DESTDIR}${_file}")
        message(STATUS "Removing: $ENV{DESTDIR}${_file}")
        file(REMOVE "$ENV{DESTDIR}${_file}")
    else()
        message(STATUS "File not found: $ENV{DESTDIR}${_file}")
    endif()
endforeach()
