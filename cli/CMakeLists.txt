# SPDX-License-Identifier: GPL-3.0-only
# M4.3b：CLI render 子命令链音频库（渲染/解码）；portaudio_static 由 beatbench_audio 私有传递。
add_executable(beatbench-cli main.cpp)
target_link_libraries(beatbench-cli PRIVATE beatbench_core beatbench_audio)
beatbench_set_warnings(beatbench-cli)
