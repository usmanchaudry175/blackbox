cmake_minimum_required(VERSION 3.16)
project(blackbox CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

add_library(blackbox_core src/frame.cpp src/cobs.cpp src/crc16.cpp src/reader.cpp src/log_writer.cpp src/log_reader.cpp src/log_index.cpp src/replay.cpp)
target_include_directories(blackbox_core PUBLIC include)
target_compile_options(blackbox_core PRIVATE -Wall -Wextra -Werror)

add_executable(replay_cli tools/replay_cli.cpp)
target_link_libraries(replay_cli PRIVATE blackbox_core)
target_compile_options(replay_cli PRIVATE -Wall -Wextra -Werror)

add_executable(capture_cli tools/capture_cli.cpp)
target_link_libraries(capture_cli PRIVATE blackbox_core)
target_compile_options(capture_cli PRIVATE -Wall -Wextra -Werror)

enable_testing()
add_subdirectory(tests)
