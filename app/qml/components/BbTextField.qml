// SPDX-License-Identifier: GPL-3.0-only
// 通用文本输入（主题统一样式，M2 2026-09）：TextField 深色底 + 圆角 + 边框 + 主色 focus。
// 与 BbSpinBox/BbComboBox 视觉一致（surface2 底 + radiusSm 圆角 + borderStrong 边框）。
import QtQuick
import QtQuick.Controls

TextField {
    id: root

    font.pixelSize: Theme.fsBase
    font.family: Theme.fontSans
    color: Theme.text
    placeholderTextColor: Theme.textFaint
    selectByMouse: true
    implicitHeight: 28
    leftPadding: 8
    rightPadding: 8

    background: Rectangle {
        radius: Theme.radiusSm
        border.width: 1
        border.color: root.activeFocus ? Theme.primary : Theme.borderStrong
        color: Theme.surface2
    }
}
