// SPDX-License-Identifier: GPL-3.0-only
// lint 面板（左 Dock「lint」标签，M2 第 4 步）：check 命令结果（诊断 + missing_wav 等）。
// 数据来自 C++ LintListModel（装配逻辑在桥接层）；带采样 id 的行点击 →
// 切到「采样」标签并定位该采样（双向往返，见 EditPage）。
// 行高自适应：消息宽度不足时自动换行（dock 横向空间有限，不做 elide 截断）。
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

ListView {
    id: listView

    signal issuePicked(string id)

    model: lintModel
    clip: true
    spacing: 2
    ScrollBar.vertical: ScrollBar { policy: ScrollBar.AsNeeded }

    delegate: Rectangle {
        id: row
        required property string severity
        required property string message
        required property string id

        width: ListView.view.width
        implicitHeight: col.implicitHeight + 8
        radius: Theme.radiusSm
        color: mouse.containsMouse ? Theme.surface3 : "transparent"
        Column {
            id: col
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.top: parent.top
            anchors.bottom: parent.bottom
            anchors.leftMargin: 6
            anchors.rightMargin: 6
            anchors.topMargin: 4
            anchors.bottomMargin: 4
            RowLayout {
                width: parent.width
                spacing: 6
                Label {
                    text: row.severity === "error" ? qsTr("ERROR")
                          : row.severity === "warning" ? qsTr("WARN") : qsTr("INFO")
                    color: row.severity === "error" ? Theme.danger
                           : row.severity === "warning" ? Theme.warning : Theme.success
                    font.pixelSize: Theme.fsTiny
                    font.bold: true
                }
                Label {
                    text: row.message
                    color: Theme.textMuted
                    wrapMode: Text.WrapAnywhere  // 自动换行（min 分词单位；长消息不再被截断）
                    Layout.fillWidth: true
                    font.pixelSize: Theme.fsTiny
                }
            }
        }
        MouseArea {
            id: mouse
            anchors.fill: parent
            hoverEnabled: true
            onClicked: {
                if (row.id !== "")
                    listView.issuePicked(row.id)
            }
        }
    }
}
