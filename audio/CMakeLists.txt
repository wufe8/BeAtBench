# SPDX-License-Identifier: GPL-3.0-only
# 音频库（Phase B，M4.1 最小闭环）：零 Qt（解码=miniaudio 内嵌，输出=PortAudio）。
# 依赖：miniaudio.h（third_party/ 仓库内）；PortAudio v19.7.0（FetchContent）。
# core 纪律：audio 也是纯逻辑库，可被 GUI/CLI/测试复用。
cmake_minimum_required(VERSION 3.24)

include(FetchContent)
FetchContent_Declare(
  portaudio
  GIT_REPOSITORY https://github.com/PortAudio/portaudio.git
  GIT_TAG v19.7.0
  GIT_SHALLOW TRUE
  # 子项目嵌入补丁：upstream 跳过 PROJECT() 导致无 linker language → 强制 ENABLE_LANGUAGE(C)。
  # 补丁脚本在 audio/patch_portaudio.ps1（幂等）；PATCH_COMMAND 工作目录 = portaudio-src，
  # 脚本靠 Get-Location 取目标文件，不传参。
  PATCH_COMMAND powershell -ExecutionPolicy Bypass -File
    "${CMAKE_CURRENT_SOURCE_DIR}/patch_portaudio.ps1"
)
# 嵌入式构建（本仓库）：跳过 install/export（否则子项目模式导出失败）；
# 只构建静态库；MinGW 下默认按平台挑 WASAPI/WMME。
set(PA_DISABLE_INSTALL ON CACHE BOOL "" FORCE)
set(PA_BUILD_SHARED OFF CACHE BOOL "" FORCE)
set(PA_BUILD_STATIC ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(portaudio)

# 静态库（M4.1：单声道试听简化；M5 再考虑缓存/波形）
add_library(beatbench_audio STATIC
  src/backend_portaudio.cpp
  src/decoder_miniaudio.cpp
  src/sample_player.cpp
  src/sample_cache.cpp
  src/chart_renderer.cpp
  src/audio_path.cpp
  src/waveform_pyramid.cpp
)
target_include_directories(beatbench_audio
  PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include ${CMAKE_SOURCE_DIR}/core/include
  PRIVATE ${CMAKE_SOURCE_DIR}/third_party/miniaudio
)
# ChartRenderer 依赖 core（Chart/TimingEngine）→ 链 core（仅头 + 客户端可复用）
target_link_libraries(beatbench_audio PRIVATE portaudio_static beatbench_core)
# portaudio_static 的 PUBLIC include 是 $<BUILD_INTERFACE>，意外不传播（embedded 场景）；
# 显式加 include 路径（源码目录）
target_include_directories(beatbench_audio PRIVATE
    ${portaudio_SOURCE_DIR}/include
    ${portaudio_SOURCE_DIR}/src/common)
target_compile_features(beatbench_audio PUBLIC cxx_std_20)
beatbench_set_warnings(beatbench_audio)
