// SPDX-License-Identifier: GPL-3.0-only
// 左 Dock 标签（对照 doc/beatbench-ui-preview.html .dock-tab）：
// 未选中 muted 文字；选中文字变亮 + 底部 2px 主色下划线。
// active = 外部激活（互斥单选组，样式不依赖控件自身 checked——避免断绑残留，doc/04 §5）。
import QtQuick
import QtQuick.Controls

TabButton {
    id: root

    property bool active: false
    readonly property bool lit: root.active || root.checked

    background: Rectangle {
        color: "transparent"
        Rectangle {
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.bottom: parent.bottom
            height: 2
            color: root.lit ? Theme.primary : "transparent"
        }
    }
    contentItem: Label {
        text: root.text
        color: root.lit ? Theme.text : Theme.textMuted
        font.pixelSize: Theme.fsSmall
        font.family: Theme.fontSans
        horizontalAlignment: Text.AlignHCenter
        verticalAlignment: Text.AlignVCenter
        elide: Text.ElideRight
    }
}