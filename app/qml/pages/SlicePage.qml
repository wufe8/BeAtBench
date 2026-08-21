// SPDX-License-Identifier: GPL-3.0-only
// 切音页（Phase C / M6 占位）：横向时间轴 + 波形（DAW / 剪辑思路）。
import QtQuick
import QtQuick.Controls

Item {
    Rectangle {
        anchors.fill: parent
        color: "#0b0e13"
        Label {
            anchors.centerIn: parent
            text: qsTr("切音页（占位）\nPhase C / M6：横向时间轴 + 波形 + 采样管理强化")
            horizontalAlignment: Text.AlignHCenter
            color: "#5b6472"
        }
    }
}
