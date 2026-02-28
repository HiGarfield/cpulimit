#!/bin/sh
set -e

# Check if cmake is installed
command -v cmake >/dev/null 2>&1 || {
	echo "cmake is not installed. Please install it to build the project."
	exit 1
}

# Clean previous build artifacts
rm -rf build

# Create a new build directory
mkdir build
cd build

# Run CMake to configure the project
cmake -DCMAKE_BUILD_TYPE=Release ..

# Build the project using the generated Makefiles
cmake --build .

cd ..

echo "Build completed successfully."
echo "The executable can be found in the 'build' directory."
