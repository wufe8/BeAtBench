# SPDX-License-Identifier: GPL-3.0-only
include(FetchContent)
FetchContent_Declare(
  googletest
  GIT_REPOSITORY https://github.com/google/googletest.git
  GIT_TAG v1.15.2
  GIT_SHALLOW TRUE
)
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
# 禁止 googletest 注册安装规则：`cmake --install` 会执行第三方子项目的 install()，
# 会把 gtest/gmock 头与库装进系统（且按需构建的目标不存在时直接失败）——打包必须干净。
set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

add_executable(beatbench_tests rational_test.cpp bms_codec_test.cpp ln_codec_test.cpp timing_test.cpp
    ln_identity_test.cpp
    json_test.cpp command_test.cpp codec_registry_test.cpp chart_mode_test.cpp
    pms_variant_test.cpp pms_integrity_test.cpp
    edit_commands_test.cpp session_registry_test.cpp timing_commands_test.cpp
    note_move_lane_test.cpp transform_quantize_test.cpp autosave_test.cpp
    meta_lint_test.cpp ln_mine_put_test.cpp move_region_test.cpp
    convert_note_test.cpp sub_line_move_test.cpp ln_move_region_test.cpp
    toggle_ln_test.cpp sample_rename_test.cpp bga_test.cpp midi_parse_test.cpp
    slice_test.cpp slice_export_test.cpp atomic_write_test.cpp project_scan_test.cpp)
target_link_libraries(beatbench_tests PRIVATE beatbench_core GTest::gtest_main)
target_compile_definitions(beatbench_tests PRIVATE
    BEATBENCH_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
beatbench_set_warnings(beatbench_tests)
add_test(NAME core_tests COMMAND beatbench_tests)

# 可选 Node >=18：执行真实 QML 函数级回归（不等同于 Qt/鼠标集成测试）。
find_program(BEATBENCH_NODE_EXECUTABLE NAMES node nodejs)
if(BEATBENCH_NODE_EXECUTABLE)
    execute_process(COMMAND "${BEATBENCH_NODE_EXECUTABLE}" --version
        OUTPUT_VARIABLE BEATBENCH_NODE_VERSION OUTPUT_STRIP_TRAILING_WHITESPACE)
    if(BEATBENCH_NODE_VERSION MATCHES "^v([0-9]+)" AND CMAKE_MATCH_1 GREATER_EQUAL 18)
        add_test(NAME qml_session_regression_tests
            COMMAND "${BEATBENCH_NODE_EXECUTABLE}" --test
                "${CMAKE_CURRENT_SOURCE_DIR}/qml_session_regression_test.js")
        set_tests_properties(qml_session_regression_tests PROPERTIES TIMEOUT 20)
    endif()
endif()

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

add_test(NAME asio_configuration_tests COMMAND ${CMAKE_COMMAND}
    -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/asio_configuration_test.cmake")

# ASIO capability checks never initialize PortAudio or load a driver.
add_executable(audio_backend_capability_test audio_backend_capability_test.cpp)
target_link_libraries(audio_backend_capability_test PRIVATE beatbench_audio GTest::gtest_main)
target_compile_definitions(audio_backend_capability_test PRIVATE
    BEATBENCH_TEST_EXPECT_ASIO=$<BOOL:${BEATBENCH_ENABLE_ASIO}>)
beatbench_set_warnings(audio_backend_capability_test)
add_test(NAME audio_backend_capability_tests COMMAND audio_backend_capability_test)

add_executable(asio_buffer_sizes_test asio_buffer_sizes_test.cpp)
target_include_directories(asio_buffer_sizes_test PRIVATE ${CMAKE_SOURCE_DIR}/audio/include)
target_link_libraries(asio_buffer_sizes_test PRIVATE GTest::gtest_main)
beatbench_set_warnings(asio_buffer_sizes_test)
add_test(NAME asio_buffer_sizes_tests COMMAND asio_buffer_sizes_test)

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

# M5 播放单测（PcmPlayback + PlaybackPlan）：纯逻辑 + SamplePlayer 内核（无设备/无 Qt）。
# 依赖 core（Chart/TimingEngine）；sample_player.cpp 引用 decode_audio_file*（解码器）
# → 链 decoder_miniaudio.cpp？不——只有 sample_cache.cpp 引用；本测试只链
# sample_player.cpp + pcm_playback.cpp + playback_plan.cpp（playback_plan 引用
# resolve_audio_path_w → audio_path.cpp）。play 本身零解码（DecodedSample 直接喂）。
add_executable(m5_playback_test m5_playback_test.cpp
    ${CMAKE_SOURCE_DIR}/audio/src/sample_player.cpp
    ${CMAKE_SOURCE_DIR}/audio/src/pcm_playback.cpp
    ${CMAKE_SOURCE_DIR}/audio/src/playback_plan.cpp
    ${CMAKE_SOURCE_DIR}/audio/src/audio_path.cpp)
target_include_directories(m5_playback_test PRIVATE
    ${CMAKE_SOURCE_DIR}/audio/include ${CMAKE_SOURCE_DIR}/third_party/miniaudio)
target_compile_definitions(m5_playback_test PRIVATE
    BEATBENCH_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
target_link_libraries(m5_playback_test PRIVATE beatbench_core GTest::gtest_main)
beatbench_set_warnings(m5_playback_test)
add_test(NAME m5_playback_tests COMMAND m5_playback_test)

# OnsetDetector 单测（M6.5 瞬态检测）：STFT 谱通量 onset（纯 DSP，零依赖、零 Qt）。
add_executable(onset_detector_test onset_detector_test.cpp
    ${CMAKE_SOURCE_DIR}/audio/src/onset_detector.cpp)
target_include_directories(onset_detector_test PRIVATE ${CMAKE_SOURCE_DIR}/audio/include)
target_link_libraries(onset_detector_test PRIVATE GTest::gtest_main)
beatbench_set_warnings(onset_detector_test)
add_test(NAME onset_detector_tests COMMAND onset_detector_test)

# ReferenceTrack 单测（M6.1 参考音轨）：解码 + 金字塔 + 窗口截取（无设备/无 Qt）。
add_executable(reference_track_test reference_track_test.cpp
    ${CMAKE_SOURCE_DIR}/audio/src/reference_track.cpp
    ${CMAKE_SOURCE_DIR}/audio/src/waveform_pyramid.cpp
    ${CMAKE_SOURCE_DIR}/audio/src/decoder_miniaudio.cpp)
target_include_directories(reference_track_test PRIVATE
    ${CMAKE_SOURCE_DIR}/audio/include ${CMAKE_SOURCE_DIR}/third_party/miniaudio)
target_link_libraries(reference_track_test PRIVATE GTest::gtest_main)
beatbench_set_warnings(reference_track_test)
add_test(NAME reference_track_tests COMMAND reference_track_test)

# UiActionRegistry 单测（doc/09 §7 验收 4）：只依赖 Qt6::Core 的轻量目标。
# 仅在找到 Qt6 的构建配置中启用：MSVC build/（无 Qt）→ 自动跳过；
# 需要时用 `cmake -S . -B build-uitest -DBEATBENCH_BUILD_TESTS=ON -DCMAKE_PREFIX_PATH=<Qt>`
# 配置（gtest 走 FetchContent）。
find_package(Qt6 COMPONENTS Core QUIET)
if (Qt6Core_FOUND)
    # Settings rollback uses an injected backend and synthetic PCM; Qt Core only, no devices.
    add_executable(audio_engine_test audio_engine_test.cpp
        ${CMAKE_SOURCE_DIR}/app/bridge/AudioEngine.cpp
        ${CMAKE_SOURCE_DIR}/app/bridge/AudioEngine.hpp
        ${CMAKE_SOURCE_DIR}/app/bridge/ChartSession.cpp
        ${CMAKE_SOURCE_DIR}/app/bridge/ChartSession.hpp)
    target_include_directories(audio_engine_test PRIVATE ${CMAKE_SOURCE_DIR}/app)
    target_link_libraries(audio_engine_test PRIVATE
        beatbench_core beatbench_audio Qt6::Core GTest::gtest)
    set_target_properties(audio_engine_test PROPERTIES AUTOMOC ON)
    beatbench_set_warnings(audio_engine_test)
    add_test(NAME audio_engine_tests COMMAND audio_engine_test)
    set_tests_properties(audio_engine_tests PROPERTIES TIMEOUT 20)

    # 项目控制器：异步快照/取消/销毁与打开路由（无窗口/音频）。
    add_executable(project_controller_test project_controller_test.cpp
        ${CMAKE_SOURCE_DIR}/app/bridge/ProjectController.cpp
        ${CMAKE_SOURCE_DIR}/app/bridge/ProjectController.hpp
        ${CMAKE_SOURCE_DIR}/app/bridge/DocumentService.cpp
        ${CMAKE_SOURCE_DIR}/app/bridge/DocumentService.hpp)
    target_include_directories(project_controller_test PRIVATE ${CMAKE_SOURCE_DIR}/app)
    target_link_libraries(project_controller_test PRIVATE beatbench_core Qt6::Core GTest::gtest_main)
    set_target_properties(project_controller_test PROPERTIES AUTOMOC ON)
    beatbench_set_warnings(project_controller_test)
    add_test(NAME project_controller_tests COMMAND project_controller_test)

    # 多文档服务：持有固定 ID 的 ChartSession；渲染库不启动音频设备。
    add_executable(document_service_test document_service_test.cpp sample_list_model_test.cpp
        ${CMAKE_SOURCE_DIR}/app/bridge/SampleListModel.cpp
        ${CMAKE_SOURCE_DIR}/app/bridge/SampleListModel.hpp
        ${CMAKE_SOURCE_DIR}/app/bridge/DocumentService.cpp
        ${CMAKE_SOURCE_DIR}/app/bridge/DocumentService.hpp)  # Q_OBJECT 头须显式列入
    target_include_directories(document_service_test PRIVATE ${CMAKE_SOURCE_DIR}/app)
    target_link_libraries(document_service_test PRIVATE
        beatbench_core Qt6::Core GTest::gtest_main)
    set_target_properties(document_service_test PROPERTIES AUTOMOC ON)
    beatbench_set_warnings(document_service_test)
    add_test(NAME document_service_tests COMMAND document_service_test)
    target_sources(document_service_test PRIVATE document_instances_test.cpp)
    target_compile_definitions(document_service_test PRIVATE BEATBENCH_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
    foreach(document_test project_controller_test document_service_test)
        target_sources(${document_test} PRIVATE
            ${CMAKE_SOURCE_DIR}/app/bridge/ChartSession.cpp
            ${CMAKE_SOURCE_DIR}/app/bridge/ChartSession.hpp)
        target_link_libraries(${document_test} PRIVATE beatbench_audio)
    endforeach()

    # Bridge queries use real session/workspace implementations without opening audio devices.
    add_executable(slice_workspace_test slice_workspace_test.cpp
        ${CMAKE_SOURCE_DIR}/app/bridge/SliceWorkspace.cpp
        ${CMAKE_SOURCE_DIR}/app/bridge/SliceWorkspace.hpp
        ${CMAKE_SOURCE_DIR}/app/bridge/DocumentService.cpp
        ${CMAKE_SOURCE_DIR}/app/bridge/DocumentService.hpp
        ${CMAKE_SOURCE_DIR}/app/bridge/ChartSession.cpp
        ${CMAKE_SOURCE_DIR}/app/bridge/ChartSession.hpp
        ${CMAKE_SOURCE_DIR}/app/bridge/AudioEngine.cpp
        ${CMAKE_SOURCE_DIR}/app/bridge/AudioEngine.hpp
        ${CMAKE_SOURCE_DIR}/audio/src/onset_detector.cpp)
    target_include_directories(slice_workspace_test PRIVATE ${CMAKE_SOURCE_DIR}/app)
    target_link_libraries(slice_workspace_test PRIVATE
        beatbench_core beatbench_audio Qt6::Core GTest::gtest_main)
    set_target_properties(slice_workspace_test PROPERTIES AUTOMOC ON)
    beatbench_set_warnings(slice_workspace_test)
    add_test(NAME slice_workspace_tests COMMAND slice_workspace_test)
    set_tests_properties(slice_workspace_tests PROPERTIES TIMEOUT 20)

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

    # EditUtils 单测（2026-09 SessionController 纯函数回抽）：只链 Qt6::Core。
    add_executable(edit_utils_test edit_utils_test.cpp
        ${CMAKE_SOURCE_DIR}/app/bridge/EditUtils.cpp
        ${CMAKE_SOURCE_DIR}/app/bridge/EditUtils.hpp)  # Q_OBJECT 头须显式列入（AUTOMOC 扫描不可靠）
    target_include_directories(edit_utils_test PRIVATE
        ${CMAKE_SOURCE_DIR}/app ${CMAKE_SOURCE_DIR}/core/include)
    target_link_libraries(edit_utils_test PRIVATE Qt6::Core GTest::gtest_main)
    set_target_properties(edit_utils_test PROPERTIES AUTOMOC ON)
    beatbench_set_warnings(edit_utils_test)
    add_test(NAME edit_utils_tests COMMAND edit_utils_test)

    # CommandDispatcher 单测（doc/06 §3）：QML 实际入口的 JSON 协议契约，
    # 覆盖 GUI 独有的「请求 JSON 非法 → bad_request」解析路径（与 CLI 入口一致性）。
    add_executable(command_dispatcher_test command_dispatcher_test.cpp
        ${CMAKE_SOURCE_DIR}/app/bridge/CommandDispatcher.cpp
        ${CMAKE_SOURCE_DIR}/app/bridge/CommandDispatcher.hpp)  # Q_OBJECT 头须显式列入
    target_include_directories(command_dispatcher_test PRIVATE
        ${CMAKE_SOURCE_DIR}/app ${CMAKE_SOURCE_DIR}/core/include)
    target_link_libraries(command_dispatcher_test PRIVATE
        beatbench_core Qt6::Core GTest::gtest_main)
    set_target_properties(command_dispatcher_test PROPERTIES AUTOMOC ON)
    beatbench_set_warnings(command_dispatcher_test)
    add_test(NAME command_dispatcher_tests COMMAND command_dispatcher_test)

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
        # 发行版数据目录兜底基准与 app 目标同值（保证单测与安装布局一致）
        target_compile_definitions(theme_manager_test PRIVATE
            BEATBENCH_DATA_DIR="${CMAKE_INSTALL_FULL_DATADIR}/beatbench")
        beatbench_set_warnings(theme_manager_test)
        add_test(NAME theme_manager_tests COMMAND theme_manager_test)
    endif()

    # Qt Quick 鼠标注入：实际弹窗/设置组件，无声卡、无用户偏好写入；兼容 Qt 6.4。
    find_package(Qt6 6.4 COMPONENTS QuickTest QuickControls2 QUIET)
    if (Qt6QuickTest_FOUND AND Qt6QuickControls2_FOUND)
        add_executable(dialog_drag_test dialog_drag_test.cpp
            ${CMAKE_SOURCE_DIR}/app/bridge/ThemeManager.cpp
            ${CMAKE_SOURCE_DIR}/app/bridge/ThemeManager.hpp)
        target_include_directories(dialog_drag_test PRIVATE ${CMAKE_SOURCE_DIR}/app)
        target_link_libraries(dialog_drag_test PRIVATE
            beatbench_core Qt6::QuickTest Qt6::QuickControls2)
        target_compile_definitions(dialog_drag_test PRIVATE
            QUICK_TEST_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/qml"
            BEATBENCH_DATA_DIR="${CMAKE_INSTALL_FULL_DATADIR}/beatbench")
        set_target_properties(dialog_drag_test PROPERTIES AUTOMOC ON)
        beatbench_set_warnings(dialog_drag_test)
        add_test(NAME dialog_drag_tests COMMAND dialog_drag_test)
        set_tests_properties(dialog_drag_tests PROPERTIES
            TIMEOUT 60
            ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_QUICK_BACKEND=software;QML_DISABLE_DISK_CACHE=1")

        # Full production Main/SessionController integration; audio is a no-device spy.
        add_executable(main_integration_test main_integration_test.cpp
            ${CMAKE_SOURCE_DIR}/audio/src/onset_detector.cpp)
        foreach(bridge ChartSession DocumentService CommandDispatcher SampleListModel LintListModel
                ChartViewItem PlayheadOverlayItem WaveformOverviewItem SliceWaveformItem
                SliceWorkspace AudioEngine ThemeManager UiActionRegistry KeyMonitor ClipboardBridge EditUtils ProjectController)
            target_sources(main_integration_test PRIVATE
                ${CMAKE_SOURCE_DIR}/app/bridge/${bridge}.cpp
                ${CMAKE_SOURCE_DIR}/app/bridge/${bridge}.hpp)
        endforeach()
        target_include_directories(main_integration_test PRIVATE ${CMAKE_SOURCE_DIR}/app)
        target_link_libraries(main_integration_test PRIVATE
            beatbench_core beatbench_audio Qt6::QuickTest Qt6::QuickControls2)
        target_compile_definitions(main_integration_test PRIVATE
            QUICK_TEST_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/qml-main-integration"
            BEATBENCH_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
            BEATBENCH_DATA_DIR="${CMAKE_INSTALL_FULL_DATADIR}/beatbench")
        set_target_properties(main_integration_test PROPERTIES AUTOMOC ON)
        beatbench_set_warnings(main_integration_test)
        add_test(NAME main_integration_tests COMMAND main_integration_test)
        # Isolate this case: leaving the final snapshot alive exercises setup teardown
        # after QGuiApplication/global QThreadPool destruction (the full suite resets it).
        add_test(NAME main_project_scan_teardown_tests COMMAND main_integration_test
            ProductionMainIntegration::test_project_large_snapshot_publication_keeps_visible_models_and_details_live)
        set_tests_properties(main_project_scan_teardown_tests PROPERTIES
            TIMEOUT 45
            ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_QUICK_BACKEND=software;QML_DISABLE_DISK_CACHE=1")
        set_tests_properties(main_integration_tests PROPERTIES
            TIMEOUT 90
            ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_QUICK_BACKEND=software;QML_DISABLE_DISK_CACHE=1")

        add_executable(chart_view_binding_test chart_view_binding_test.cpp)
        foreach(bridge ChartSession DocumentService ChartViewItem PlayheadOverlayItem WaveformOverviewItem ThemeManager)
            target_sources(chart_view_binding_test PRIVATE
                ${CMAKE_SOURCE_DIR}/app/bridge/${bridge}.cpp
                ${CMAKE_SOURCE_DIR}/app/bridge/${bridge}.hpp)
        endforeach()
        target_include_directories(chart_view_binding_test PRIVATE ${CMAKE_SOURCE_DIR}/app)
        target_link_libraries(chart_view_binding_test PRIVATE
            beatbench_core beatbench_audio Qt6::QuickTest Qt6::QuickControls2)
        target_compile_definitions(chart_view_binding_test PRIVATE
            QUICK_TEST_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/qml-chart-view"
            BEATBENCH_DATA_DIR="${CMAKE_INSTALL_FULL_DATADIR}/beatbench")
        set_target_properties(chart_view_binding_test PROPERTIES AUTOMOC ON)
        beatbench_set_warnings(chart_view_binding_test)
        add_test(NAME chart_view_binding_tests COMMAND chart_view_binding_test)
        set_tests_properties(chart_view_binding_tests PROPERTIES
            TIMEOUT 60
            ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_QUICK_BACKEND=software;QML_DISABLE_DISK_CACHE=1")

        add_executable(slice_export_confirmation_test slice_export_confirmation_test.cpp
            ${CMAKE_SOURCE_DIR}/audio/src/onset_detector.cpp)
        foreach(bridge ChartSession SliceWorkspace SliceWaveformItem DocumentService AudioEngine ThemeManager UiActionRegistry KeyMonitor)
            target_sources(slice_export_confirmation_test PRIVATE
                ${CMAKE_SOURCE_DIR}/app/bridge/${bridge}.cpp
                ${CMAKE_SOURCE_DIR}/app/bridge/${bridge}.hpp)
        endforeach()
        target_include_directories(slice_export_confirmation_test PRIVATE ${CMAKE_SOURCE_DIR}/app)
        target_link_libraries(slice_export_confirmation_test PRIVATE
            beatbench_core beatbench_audio Qt6::QuickTest Qt6::QuickControls2)
        target_compile_definitions(slice_export_confirmation_test PRIVATE
            QUICK_TEST_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/qml-slice-export"
            BEATBENCH_DATA_DIR="${CMAKE_INSTALL_FULL_DATADIR}/beatbench")
        set_target_properties(slice_export_confirmation_test PROPERTIES AUTOMOC ON)
        beatbench_set_warnings(slice_export_confirmation_test)
        add_test(NAME slice_export_confirmation_tests COMMAND slice_export_confirmation_test)
        set_tests_properties(slice_export_confirmation_tests PROPERTIES
            TIMEOUT 60
            ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_QUICK_BACKEND=software;QML_DISABLE_DISK_CACHE=1")
    endif()
endif()
