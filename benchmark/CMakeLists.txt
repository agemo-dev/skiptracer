cmake_minimum_required(VERSION 3.22)

project(
    SkipTracerBenchmark
    LANGUAGES CXX
)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(FetchContent)

FetchContent_Declare(
    googlebenchmark
    GIT_REPOSITORY https://github.com/google/benchmark.git
    GIT_TAG v1.9.1
)

# Disable internal test
set(BENCHMARK_ENABLE_TESTING     OFF CACHE BOOL "" FORCE)
set(BENCHMARK_ENABLE_GTEST_TESTS OFF CACHE BOOL "" FORCE)

FetchContent_MakeAvailable(googlebenchmark)

add_executable(skipbench benchmark.cpp)
target_link_libraries(skipbench PRIVATE benchmark::benchmark)
target_include_directories(skipbench PUBLIC $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/..>)