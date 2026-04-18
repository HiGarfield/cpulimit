#!/bin/sh
set -e

# Check if cmake is installed
command -v cmake >/dev/null 2>&1 || {
	echo "cmake is not installed. Please install it to build the project."
	exit 1
}

# Clean previous build artifacts and create a fresh build directory
rm -rf build
mkdir build

# Configure the project.  Run cmake from inside the build directory so
# that the working directory of subsequent commands is unaffected.
(cd build && cmake -DCMAKE_BUILD_TYPE=Release ..)

# Detect the number of logical processors for parallel builds.
# nproc is available on Linux; sysctl is available on macOS and FreeBSD
# (hw.logicalcpu on macOS, hw.ncpu on FreeBSD); getconf provides a
# portable fallback on systems without either tool.
if command -v nproc >/dev/null 2>&1; then
	_nproc=$(nproc)
elif command -v sysctl >/dev/null 2>&1; then
	_nproc=$(sysctl -n hw.logicalcpu 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 1)
elif command -v getconf >/dev/null 2>&1; then
	_nproc=$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 1)
else
	_nproc=1
fi

# Build all targets, passing the job count to the native build tool
# so parallel compilation is used regardless of the CMake version.
# The -- separator forwards the following arguments directly to make
# or ninja (CMake 3.5 compatible).
cmake --build build -- -j"${_nproc}"

# Information about the build
echo "Build completed successfully."
echo "Executable: build/src/cpulimit"
echo "Test binary: build/tests/cpulimit_test"
