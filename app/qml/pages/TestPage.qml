// SPDX-License-Identifier: GPL-3.0-only
// 测试页（Phase B 起占位）：大预览 + 试玩。
import QtQuick
import QtQuick.Controls

Item {
    Rectangle {
        anchors.fill: parent
        color: "#0b0e13"
        Label {
            anchors.centerIn: parent
            text: qsTr("测试页（占位）\nPhase B：试玩 / 预览")
            horizontalAlignment: Text.AlignHCenter
            color: "#5b6472"
        }
    }
}
