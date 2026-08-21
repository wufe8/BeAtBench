// SPDX-License-Identifier: GPL-3.0-only
// 编辑页（M2 核心）：三栏模板 = 空模板的默认布局（左 Dock + 中央时间轴 + 右 Dock）。
// 面板内容在 Main.qml 的 StackLayout 中装配；本页负责页面级占位与后续接入。
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Item {
    id: root
    property var chartMeta: null
    property string chartPath: ""

    RowLayout {
        anchors.fill: parent
        spacing: 0
        // 占位骨架：内容实际由 Main.qml 三栏装配；本组件保留页面级语义
        Rectangle {
            Layout.fillWidth: true
            Layout.fillHeight: true
            color: "#0b0e13"
            Label {
                anchors.centerIn: parent
                text: qsTr("编辑页（三栏布局，见 Main.qml）")
                color: "#5b6472"
            }
        }
    }
}
