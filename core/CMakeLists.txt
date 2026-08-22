# SPDX-License-Identifier: GPL-3.0-only
# core：纯逻辑库，零 Qt/音频/第三方依赖。
# M1 起含 .cpp 实现（BMS codec / 编码层），保持静态库形态；GUI/CLI/测试三方复用。
add_library(beatbench_core STATIC
    src/bms/encoding.cpp
    src/bms/channel_map.cpp
    src/bms/bms_parser.cpp
    src/bms/bms_writer.cpp
    src/bms/bms_flatten.cpp
    src/bms/chart_check.cpp
    src/timing/timing_engine.cpp
    src/json/json.cpp
    src/command/registry.cpp
    src/command/builtins.cpp
    src/codec/codec_registry.cpp
    src/codec/bms_adapter.cpp
    src/codec/bms_channel_maps.cpp
    src/codec/chart_modes.cpp
    src/edit/edit_commands.cpp
    src/edit/session_registry.cpp
)
target_include_directories(beatbench_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_features(beatbench_core PUBLIC cxx_std_20)
beatbench_set_warnings(beatbench_core)
