# Minimum CMake for the build (glslang asks for 3.27); build.bat and install.bat
# run it with cmake -P to check the cmake.exe they found, and it fails when that
# one is older
cmake_minimum_required(VERSION 3.27)
