# SPDX-License-Identifier: GPL-3.0-only
cmake_minimum_required(VERSION 3.24)
project(beatbench VERSION 0.1.0 LANGUAGES CXX)

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
