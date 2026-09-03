cmake_minimum_required(VERSION 3.20)
project(dsc)

add_subdirectory(src)

# Protocol conformance test for src/pcm_v4.hpp:
#   cmake --build build --target pcmv4_conformance
#   ctest --test-dir build --output-on-failure
enable_testing()
add_subdirectory(test)
