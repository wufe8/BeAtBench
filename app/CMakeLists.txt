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
    bridge/ClipboardBridge.cpp
    bridge/ClipboardBridge.hpp   # Q_OBJECT：须显式列入（AUTOMOC 自动包含扫描不可靠）
    bridge/EditUtils.cpp
    bridge/EditUtils.hpp         # Q_OBJECT：须显式列入（AUTOMOC 自动包含扫描不可靠）
    bridge/AudioEngine.cpp
    bridge/WaveformOverviewItem.cpp
    bridge/PlayheadOverlayItem.cpp
    bridge/SliceWorkspace.cpp
    bridge/SliceWaveformItem.cpp
    bridge/ThemeManager.hpp   # 显式列入：AUTOMOC 扫描 target sources 生成 moc（自动包含扫描不可靠）
    bridge/SampleListModel.hpp
    bridge/LintListModel.hpp
    bridge/ChartSession.hpp
    bridge/KeyMonitor.hpp
    bridge/UiActionRegistry.hpp
    bridge/AudioEngine.hpp    # Q_OBJECT：须显式列入（AUTOMOC 自动包含扫描不可靠）
    bridge/WaveformOverviewItem.hpp
    bridge/PlayheadOverlayItem.hpp
    bridge/SliceWorkspace.hpp
    bridge/SliceWaveformItem.hpp
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
        qml/components/BbDialog.qml
        qml/components/BbComboBox.qml
        qml/components/SamplePanel.qml
        qml/components/SettingsDialog.qml
        qml/components/LintPanel.qml
        qml/components/ChartView.qml
        qml/components/PropertiesPanel.qml
        qml/components/TimelinePanel.qml
        qml/components/BgaPanel.qml
        qml/components/SliceStatusBar.qml
    # C++ 注册类型（QML_ELEMENT，经 qmltyperegistrar 注册为 BeatBench 组件）：
    # ChartViewItem = 竖向时间轴自绘视口（皮肤 L3 按组件覆写的落点，doc/05 §8）
    # WaveformOverviewItem = 波形总览条（M4.3c；Space 渲染后显示，点击/拖动跳转）
    # PlayheadOverlayItem = 播放头/A-B 标记叠层（M5.2 性能拆层：播放时钟只重绘本层）
    # SliceWaveformItem = 切音工作台波形（M6.1：参考音频波形 + note 刻度 + 播放头）
    SOURCES
        bridge/ChartViewItem.cpp
        bridge/ChartViewItem.hpp
        bridge/WaveformOverviewItem.cpp
        bridge/WaveformOverviewItem.hpp
        bridge/PlayheadOverlayItem.cpp
        bridge/PlayheadOverlayItem.hpp
        bridge/SliceWaveformItem.cpp
        bridge/SliceWaveformItem.hpp
)

# qmltyperegistrar 生成 #include <ChartViewItem.hpp>（按 basename）；
# 头文件在 bridge/ 子目录，需显式加入包含路径，否则注册文件编译失败
target_include_directories(beatbench PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/bridge")

set_target_properties(beatbench PROPERTIES
  AUTOMOC ON               # qt_add_qml_module 需要（metatype 生成）
  WIN32_EXECUTABLE TRUE   # Windows 下不弹控制台
  MACOSX_BUNDLE TRUE
)

# 发行版安装的皮肤数据目录（ThemeManager 兜底搜索基准；与安装规则同一事实来源）
target_compile_definitions(beatbench PRIVATE
  BEATBENCH_DATA_DIR="${CMAKE_INSTALL_FULL_DATADIR}/beatbench")

# ---------- Linux 安装规则（发行版打包；doc/04 §4） ----------
# 布局遵循各发行版惯例：可执行进 bin；QML 模块进构建所用 Qt 的 QML 导入目录；皮肤进
# <datadir>/beatbench/skins；desktop/图标/metainfo 进标准位置。打包脚本用
# `DESTDIR=<pkgroot> cmake --install` 复用本规则，包格式（deb/rpm/arch）只加元数据。
# Windows 发布包由 scripts/package-release.sh 组装；macOS 为 .app，均不经本块。
if(UNIX AND NOT APPLE)
  install(TARGETS beatbench RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

  # QML 模块目录（qmldir + qml/ + qmltypes）：引擎按导入路径找 qmldir；类型文件由 qmldir 的
  # prefer :/BeatBench/ 指向可执行内嵌资源（与磁盘副本同源）。构建期 dir_map.qrc 含绝对
  # 路径、仅供 qmlcachegen 使用，不安装。
  if(NOT DEFINED BEATBENCH_QML_IMPORT_DIR)
    if(DEFINED QT6_INSTALL_PREFIX AND DEFINED QT6_INSTALL_QML)
      # Qt6 安装信息（QtInstallPaths.cmake）：Debian 系 /usr/lib/<triplet>/qt6/qml、
      # Fedora /usr/lib64/qt6/qml、Arch /usr/lib/qt6/qml；QT6_INSTALL_QML 为相对路径。
      set(BEATBENCH_QML_IMPORT_DIR "${QT6_INSTALL_PREFIX}/${QT6_INSTALL_QML}")
    else()
      set(BEATBENCH_QML_IMPORT_DIR "${CMAKE_INSTALL_PREFIX}/${CMAKE_INSTALL_LIBDIR}/qt6/qml")
    endif()
  endif()
  message(STATUS "QML 模块安装目录: ${BEATBENCH_QML_IMPORT_DIR}")
  install(DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/BeatBench/"
          DESTINATION "${BEATBENCH_QML_IMPORT_DIR}/BeatBench"
          PATTERN "beatbench_qml_module_dir_map.qrc" EXCLUDE)

  # 内置皮肤与桌面集成素材
  install(DIRECTORY "${CMAKE_SOURCE_DIR}/skins/" DESTINATION "${CMAKE_INSTALL_DATADIR}/beatbench/skins")
  install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/beatbench.desktop" DESTINATION "${CMAKE_INSTALL_DATADIR}/applications")
  install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/io.github.wufe8.BeAtBench.metainfo.xml" DESTINATION "${CMAKE_INSTALL_DATADIR}/metainfo")
  foreach(size IN ITEMS 16 24 32 48 64 128 256 512)
    install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/icons/beatbench-${size}.png"
            DESTINATION "${CMAKE_INSTALL_DATADIR}/icons/hicolor/${size}x${size}/apps"
            RENAME beatbench.png)
  endforeach()

  # 许可与文档（Arch/Fedora 惯例用 share/licenses；deb 打包脚本另补 copyright）
  install(FILES "${CMAKE_SOURCE_DIR}/LICENSE" DESTINATION "${CMAKE_INSTALL_DATADIR}/licenses/beatbench")
  install(FILES "${CMAKE_SOURCE_DIR}/README.md" "${CMAKE_SOURCE_DIR}/CHANGELOG.md"
          DESTINATION "${CMAKE_INSTALL_DATADIR}/doc/beatbench")
endif()
