include(FetchContent)
FetchContent_Declare(
  Catch2
  GIT_REPOSITORY https://github.com/catchorg/Catch2.git
  GIT_TAG v3.5.2
)
FetchContent_MakeAvailable(Catch2)

add_executable(tests test_cobs.cpp test_crc16.cpp test_frame.cpp test_reader.cpp test_ring_buffer.cpp test_log_writer.cpp)
target_link_libraries(tests PRIVATE blackbox_core Catch2::Catch2WithMain)
target_compile_options(tests PRIVATE -Wall -Wextra -Werror)

add_test(NAME tests COMMAND tests)