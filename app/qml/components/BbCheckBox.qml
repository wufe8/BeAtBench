// SPDX-License-Identifier: GPL-3.0-only
// 通用勾选框（主题统一样式，M2 2026-09）：文字 = Theme.text（修复黑字不可读）；
// indicator = 14×14 圆角边框（checked = 主色底 + 白勾）。控件交互语义 = Qt Quick
// Controls 2 CheckBox（checkable + onToggled 外部状态不入 checked 断绑——用 toggled 事件
// 手动驱动外部状态，checked 只作显示绑定，避免「点击断绑残留」，doc/05 §4.3）
import QtQuick
import QtQuick.Controls

CheckBox {
    id: root

    property color textColor: Theme.text
    property color boxColor: Theme.surface2
    property color boxBorder: Theme.borderStrong
    property color boxChecked: Theme.primary
    property color checkMark: Theme.onAccent

    spacing: 6

    contentItem: Label {
        text: root.text
        color: root.textColor
        font.pixelSize: Theme.fsSmall
        font.family: Theme.fontSans
        verticalAlignment: Text.AlignVCenter
        leftPadding: root.indicator.width + root.spacing
    }

    indicator: Rectangle {
        x: root.leftPadding
        y: root.topPadding + (root.availableHeight - height) / 2
        width: 14
        height: 14
        radius: Theme.boxRadius
        color: root.checked ? root.boxChecked : root.boxColor
        border.color: root.hovered ? Theme.accent : root.boxBorder
        border.width: 1
        Label {
            visible: root.checked
            anchors.centerIn: parent
            text: "✓"
            color: root.checkMark
            font.pixelSize: 10
            font.bold: true
        }
    }
}
