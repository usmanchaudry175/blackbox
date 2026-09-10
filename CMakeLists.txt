cmake_minimum_required(VERSION 3.16)
project(blackbox CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

add_compile_options(-Wall -Wextra -Werror)

add_library(blackbox_core src/frame.cpp src/cobs.cpp)
target_include_directories(blackbox_core PUBLIC include)

enable_testing()
add_subdirectory(tests)
