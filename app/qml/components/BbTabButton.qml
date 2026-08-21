// SPDX-License-Identifier: GPL-3.0-only
// 左 Dock 标签（对照 doc/beatbench-ui-preview.html .dock-tab）：
// 未选中 muted 文字；选中文字变亮 + 底部 2px 主色下划线。
import QtQuick
import QtQuick.Controls

TabButton {
    id: root

    background: Rectangle {
        color: "transparent"
        Rectangle {
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.bottom: parent.bottom
            height: 2
            color: root.checked ? Theme.primary : "transparent"
        }
    }
    contentItem: Label {
        text: root.text
        color: root.checked ? Theme.text : Theme.textMuted
        font.pixelSize: Theme.fsSmall
        font.family: Theme.fontSans
        horizontalAlignment: Text.AlignHCenter
        verticalAlignment: Text.AlignVCenter
        elide: Text.ElideRight
    }
}
