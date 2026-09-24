# SPDX-License-Identifier: GPL-3.0-only
cmake_minimum_required(VERSION 3.24)

# CMake 4.x 移除了对 cmake_minimum_required(<3.5) 的兼容；PortAudio v19.7.0（FetchContent
# 嵌入）自声明 2.8，会让 4.x 直接配置失败。此变量把下游项目声明的最小版本锧到 3.5；
# 在 CMake 3.x 下它只是普通变量，无副作用。已验证：CMake 4.4.3 无 -D 旗标可直接配置。
set(CMAKE_POLICY_VERSION_MINIMUM 3.5)

# LANGUAGES 含 C：audio 内嵌 PortAudio（FetchContent）在**子项目模式**下 upstream 会跳过
# 自己的 PROJECT()（见 portaudio CMakeLists 的 IF(CMAKE_SOURCE_DIR STREQUAL ...) 分支），
# 于是没有语言被启用 → C 目标报 "cannot determine linker language"。在父项目启用 C 即可
# 全局覆盖，且**跨平台通用**（原先靠 audio/patch_portaudio.ps1 往 upstream 首行插
# ENABLE_LANGUAGE(C)，那既依赖 PowerShell，也与平台无关地必要——2026-09 改为本方案）。
project(beatbench VERSION 0.3.1 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(BEATBENCH_BUILD_APP "构建 Qt GUI（需要 Qt 6.11+ Qml/Quick/QuickControls2）" ON)
option(BEATBENCH_BUILD_TESTS "构建单元测试（FetchContent 下载 GoogleTest）" ON)

function(beatbench_set_warnings target)
  if(MSVC)
    # /utf-8：源文件为 UTF-8（含中文注释），避免代码页 936 误读
    target_compile_options(${target} PRIVATE /W4 /permissive- /utf-8)
  else()
    target_compile_options(${target} PRIVATE -Wall -Wextra)
  endif()
endfunction()

add_subdirectory(core)

# 音频库（Phase B，M4.1）：miniaudio 内嵌 + PortAudio FetchContent；零 Qt。
# 无网/PortAudio 构建失败 → 跳过（GUI/测试侧音频功能不可用，但不阻塞主链）。
add_subdirectory(audio)

# CLI：无第三方依赖，始终构建
add_subdirectory(cli)

# GUI：找不到 Qt 时自动跳过（见 doc/04 §构建）
if(BEATBENCH_BUILD_APP)
  find_package(Qt6 QUIET COMPONENTS Qml Quick QuickControls2)
  if(Qt6_FOUND)
    add_subdirectory(app)
  else()
    message(STATUS "Qt6 Qml/Quick/QuickControls2 未找到，跳过 app 目标（安装 Qt 6.11+ 后重新配置）")
  endif()
endif()

if(BEATBENCH_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()
