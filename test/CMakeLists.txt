cmake_minimum_required(VERSION 3.22)

project(
    SkiptracerTest
    LANGUAGES CXX
)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(skiptest test.cpp)
target_include_directories(skiptest PUBLIC $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/..>)