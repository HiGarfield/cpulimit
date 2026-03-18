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

# Build all targets
cmake --build build

# Information about the build
echo "Build completed successfully."
echo "Executable: build/src/cpulimit"
echo "Test binary: build/tests/cpulimit_test"
