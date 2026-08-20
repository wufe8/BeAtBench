# SPDX-License-Identifier: GPL-3.0-only
add_executable(beatbench-cli main.cpp)
target_link_libraries(beatbench-cli PRIVATE beatbench_core)
beatbench_set_warnings(beatbench-cli)
