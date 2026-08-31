# SPDX-License-Identifier: GPL-3.0-only
# GUI：Qt Quick/QML（doc/08 决策）。默认皮肤（qml/）经 qt_add_qml_module 打进资源，
# URI = BeatBench；将来 L3 皮肤包可注册同名/扩展 URI（doc/08 §3）。
qt_standard_project_setup()   # AUTOMOC/AUTORCC/AUTOUIC 统一策略（Qt 6.3+）

add_executable(beatbench
    main.cpp
    bridge/CommandDispatcher.cpp
    bridge/SampleListModel.cpp
    bridge/LintListModel.cpp
    bridge/ChartSession.cpp
    bridge/KeyMonitor.cpp
    bridge/UiActionRegistry.cpp
    bridge/ThemeManager.cpp
    bridge/AudioEngine.cpp
    bridge/WaveformOverviewItem.cpp
    bridge/ThemeManager.hpp   # 显式列入：AUTOMOC 扫描 target sources 生成 moc（自动包含扫描不可靠）
    bridge/SampleListModel.hpp
    bridge/LintListModel.hpp
    bridge/ChartSession.hpp
    bridge/KeyMonitor.hpp
    bridge/UiActionRegistry.hpp
    bridge/AudioEngine.hpp    # Q_OBJECT：须显式列入（AUTOMOC 自动包含扫描不可靠）
    bridge/WaveformOverviewItem.hpp
)
target_link_libraries(beatbench PRIVATE beatbench_core beatbench_audio Qt6::Quick Qt6::QuickControls2)
beatbench_set_warnings(beatbench)

qt_add_qml_module(beatbench
    URI BeatBench
    VERSION 1.0
    QML_FILES
        qml/Main.qml
        qml/SessionController.qml
        qml/pages/EditPage.qml
        qml/pages/SlicePage.qml
        qml/pages/TestPage.qml
        qml/components/PageSwitcher.qml
        qml/components/MetaPanel.qml
        qml/components/BbToolButton.qml
        qml/components/BbTabButton.qml
        qml/components/BbTabStrip.qml
        qml/components/BbCheckBox.qml
        qml/components/BbSpinBox.qml
        qml/components/BbTextField.qml
        qml/components/BbComboBox.qml
        qml/components/SamplePanel.qml
        qml/components/SettingsDialog.qml
        qml/components/LintPanel.qml
        qml/components/ChartView.qml
        qml/components/PropertiesPanel.qml
        qml/components/TimelinePanel.qml
        qml/components/BgaPanel.qml
    # C++ 注册类型（QML_ELEMENT，经 qmltyperegistrar 注册为 BeatBench 组件）：
    # ChartViewItem = 竖向时间轴自绘视口（皮肤 L3 按组件覆写的落点，doc/05 §8）
    # WaveformOverviewItem = 波形总览条（M4.3c；Space 渲染后显示，点击/拖动跳转）
    SOURCES
        bridge/ChartViewItem.cpp
        bridge/ChartViewItem.hpp
        bridge/WaveformOverviewItem.cpp
        bridge/WaveformOverviewItem.hpp
)

# qmltyperegistrar 生成 #include <ChartViewItem.hpp>（按 basename）；
# 头文件在 bridge/ 子目录，需显式加入包含路径，否则注册文件编译失败
target_include_directories(beatbench PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/bridge")

set_target_properties(beatbench PROPERTIES
  AUTOMOC ON               # qt_add_qml_module 需要（metatype 生成）
  WIN32_EXECUTABLE TRUE   # Windows 下不弹控制台
  MACOSX_BUNDLE TRUE
)
