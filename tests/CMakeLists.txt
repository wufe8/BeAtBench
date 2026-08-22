# SPDX-License-Identifier: GPL-3.0-only
include(FetchContent)
FetchContent_Declare(
  googletest
  GIT_REPOSITORY https://github.com/google/googletest.git
  GIT_TAG v1.15.2
  GIT_SHALLOW TRUE
)
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

add_executable(beatbench_tests rational_test.cpp bms_codec_test.cpp timing_test.cpp
    json_test.cpp command_test.cpp codec_registry_test.cpp chart_mode_test.cpp
    edit_commands_test.cpp session_registry_test.cpp)
target_link_libraries(beatbench_tests PRIVATE beatbench_core GTest::gtest_main)
target_compile_definitions(beatbench_tests PRIVATE
    BEATBENCH_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
beatbench_set_warnings(beatbench_tests)
add_test(NAME core_tests COMMAND beatbench_tests)
