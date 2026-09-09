include(FetchContent)
FetchContent_Declare(
  Catch2
  GIT_REPOSITORY https://github.com/catchorg/Catch2.git
  GIT_TAG v3.5.2
)
FetchContent_MakeAvailable(Catch2)

add_executable(tests test_frame.cpp)
target_link_libraries(tests PRIVATE blackbox_core Catch2::Catch2WithMain)

add_test(NAME tests COMMAND tests)