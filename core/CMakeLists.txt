# SPDX-License-Identifier: GPL-3.0-only
# core：纯逻辑库，零 Qt/音频依赖。当前为 header-only 骨架，
# 需要 .cpp 实现时改为 STATIC 库（保持无第三方依赖约定）。
add_library(beatbench_core INTERFACE)
target_include_directories(beatbench_core INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_features(beatbench_core INTERFACE cxx_std_20)
