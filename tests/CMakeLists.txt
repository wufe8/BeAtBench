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
    edit_commands_test.cpp session_registry_test.cpp timing_commands_test.cpp
    note_move_lane_test.cpp transform_quantize_test.cpp autosave_test.cpp
    meta_lint_test.cpp ln_mine_put_test.cpp move_region_test.cpp
    convert_note_test.cpp bgm_line_move_test.cpp ln_move_region_test.cpp
    toggle_ln_test.cpp sample_rename_test.cpp bga_test.cpp)
target_link_libraries(beatbench_tests PRIVATE beatbench_core GTest::gtest_main)
target_compile_definitions(beatbench_tests PRIVATE
    BEATBENCH_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
beatbench_set_warnings(beatbench_tests)
add_test(NAME core_tests COMMAND beatbench_tests)

# 音频层单测（M4.1）：解码 + SamplePlayer 内核（无设备/无 Qt；不链 PortAudio——
# 只测纯逻辑 + miniaudio 解码，不碰设备 IO。链 beatbench_audio 即可）。
add_executable(audio_test audio_test.cpp ${CMAKE_SOURCE_DIR}/audio/src/decoder_miniaudio.cpp
    ${CMAKE_SOURCE_DIR}/audio/src/sample_player.cpp
    ${CMAKE_SOURCE_DIR}/audio/src/sample_cache.cpp)
target_include_directories(audio_test PRIVATE
    ${CMAKE_SOURCE_DIR}/audio/include ${CMAKE_SOURCE_DIR}/third_party/miniaudio)
target_compile_definitions(audio_test PRIVATE
    BEATBENCH_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
target_link_libraries(audio_test PRIVATE GTest::gtest_main)
beatbench_set_warnings(audio_test)
add_test(NAME audio_tests COMMAND audio_test)

# SampleCache 单测（M4.3a）：LRU/预算/并发合并/失效；注入解码器（不碰设备 I/O；
# sample_cache.cpp 引用 decode_audio_file*（默认解码器），须链 decoder_miniaudio.cpp）。
add_executable(sample_cache_test sample_cache_test.cpp
    ${CMAKE_SOURCE_DIR}/audio/src/sample_cache.cpp
    ${CMAKE_SOURCE_DIR}/audio/src/decoder_miniaudio.cpp)
target_include_directories(sample_cache_test PRIVATE
    ${CMAKE_SOURCE_DIR}/audio/include ${CMAKE_SOURCE_DIR}/third_party/miniaudio)
target_compile_definitions(sample_cache_test PRIVATE
    BEATBENCH_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
target_link_libraries(sample_cache_test PRIVATE GTest::gtest_main)
beatbench_set_warnings(sample_cache_test)
add_test(NAME sample_cache_tests COMMAND sample_cache_test)

# ChartRenderer 单测（M4.3b）：离线渲染（合成谱面 + 注入解码器；不碰设备/磁盘谱图）。
# 依赖 core（Chart/TimingEngine）+ audio（SampleCache）；sample_cache 引用
# decode_audio_file*（默认解码器）→ 链 decoder_miniaudio.cpp。
add_executable(chart_renderer_test chart_renderer_test.cpp
    ${CMAKE_SOURCE_DIR}/audio/src/chart_renderer.cpp
    ${CMAKE_SOURCE_DIR}/audio/src/sample_cache.cpp
    ${CMAKE_SOURCE_DIR}/audio/src/decoder_miniaudio.cpp
    ${CMAKE_SOURCE_DIR}/audio/src/audio_path.cpp)
target_include_directories(chart_renderer_test PRIVATE
    ${CMAKE_SOURCE_DIR}/audio/include ${CMAKE_SOURCE_DIR}/third_party/miniaudio)
target_compile_definitions(chart_renderer_test PRIVATE
    BEATBENCH_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
target_link_libraries(chart_renderer_test PRIVATE beatbench_core GTest::gtest_main)
beatbench_set_warnings(chart_renderer_test)
add_test(NAME chart_renderer_tests COMMAND chart_renderer_test)

# WaveformPyramid 单测（M4.3c）：min/max 金字塔构建/查询（纯内存，无依赖）。
add_executable(waveform_pyramid_test waveform_pyramid_test.cpp
    ${CMAKE_SOURCE_DIR}/audio/src/waveform_pyramid.cpp)
target_include_directories(waveform_pyramid_test PRIVATE ${CMAKE_SOURCE_DIR}/audio/include)
target_link_libraries(waveform_pyramid_test PRIVATE GTest::gtest_main)
beatbench_set_warnings(waveform_pyramid_test)
add_test(NAME waveform_pyramid_tests COMMAND waveform_pyramid_test)

# UiActionRegistry 单测（doc/09 §7 验收 4）：只依赖 Qt6::Core 的轻量目标。
# 仅在找到 Qt6 的构建配置中启用：MSVC build/（无 Qt）→ 自动跳过；
# 需要时用 `cmake -S . -B build-uitest -DBEATBENCH_BUILD_TESTS=ON -DCMAKE_PREFIX_PATH=<Qt>`
# 配置（gtest 走 FetchContent）。
find_package(Qt6 COMPONENTS Core QUIET)
if (Qt6Core_FOUND)
    add_executable(ui_action_registry_test ui_action_registry_test.cpp
        ${CMAKE_SOURCE_DIR}/app/bridge/UiActionRegistry.cpp
        ${CMAKE_SOURCE_DIR}/app/bridge/UiActionRegistry.hpp)  # Q_OBJECT 头须显式列入（AUTOMOC 扫描不可靠）
    target_include_directories(ui_action_registry_test PRIVATE
        ${CMAKE_SOURCE_DIR}/app ${CMAKE_SOURCE_DIR}/core/include)
    # UiActionRegistry.cpp 现依赖 core 的 Json（applyKeymapFile 解析 keymap.json）→ 链 beatbench_core。
    target_link_libraries(ui_action_registry_test PRIVATE
        beatbench_core Qt6::Core GTest::gtest_main)
    # UiActionRegistry 是 Q_OBJECT：需 AUTOMOC 生成 staticMetaObject/信号/vtable。
    # qt_standard_project_setup 只在 app 子目录启用（app/CMakeLists.txt）；tests 位于其外、且
    # 本目标不依赖 Quick，故在此显式开启 AUTOMOC（2026-09：此前缺它 → 链接 undefined reference，
    # 让「Qt 测试目标接入常规构建」实际不可编译，ui_action_registry_test 无法链接/运行）。
    set_target_properties(ui_action_registry_test PROPERTIES AUTOMOC ON)
    beatbench_set_warnings(ui_action_registry_test)
    add_test(NAME ui_action_registry_tests COMMAND ui_action_registry_test)

    # ThemeManager 单测（doc/08 §3.5）：链 Qt6::Gui（QColor）+ core 的 Json。
    # 验证 theme.json 颜色 + 非颜色 token 覆写（L2 皮肤布局改动幅度的根基）。
    find_package(Qt6 COMPONENTS Gui QUIET)
    if (Qt6Gui_FOUND)
        add_executable(theme_manager_test theme_manager_test.cpp
            ${CMAKE_SOURCE_DIR}/app/bridge/ThemeManager.cpp
            ${CMAKE_SOURCE_DIR}/app/bridge/ThemeManager.hpp)
        target_include_directories(theme_manager_test PRIVATE
            ${CMAKE_SOURCE_DIR}/app ${CMAKE_SOURCE_DIR}/core/include)
        target_link_libraries(theme_manager_test PRIVATE
            beatbench_core Qt6::Gui Qt6::Core GTest::gtest_main)
        # ThemeManager 是 Q_OBJECT：需 AUTOMOC 生成 staticMetaObject。
        set_target_properties(theme_manager_test PROPERTIES AUTOMOC ON)
        beatbench_set_warnings(theme_manager_test)
        add_test(NAME theme_manager_tests COMMAND theme_manager_test)
    endif()
endif()
