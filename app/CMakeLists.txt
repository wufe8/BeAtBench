# SPDX-License-Identifier: GPL-3.0-only
# GUI：Qt Quick/QML（doc/08 决策）。默认皮肤（qml/）经 qt_add_qml_module 打进资源，
# URI = BeatBench；将来 L3 皮肤包可注册同名/扩展 URI（doc/08 §3）。
qt_standard_project_setup()   # AUTOMOC/AUTORCC/AUTOUIC 统一策略（Qt 6.3+）

add_executable(beatbench main.cpp bridge/CommandDispatcher.cpp)
target_link_libraries(beatbench PRIVATE beatbench_core Qt6::Quick Qt6::QuickControls2)
beatbench_set_warnings(beatbench)

qt_add_qml_module(beatbench
    URI BeatBench
    VERSION 1.0
    QML_FILES
        qml/Main.qml
        qml/pages/EditPage.qml
        qml/pages/SlicePage.qml
        qml/pages/TestPage.qml
        qml/components/PageSwitcher.qml
        qml/components/MetaPanel.qml
)

set_target_properties(beatbench PROPERTIES
  AUTOMOC ON               # qt_add_qml_module 需要（metatype 生成）
  WIN32_EXECUTABLE TRUE   # Windows 下不弹控制台
  MACOSX_BUNDLE TRUE
)
