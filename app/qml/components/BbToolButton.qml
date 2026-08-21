// SPDX-License-Identifier: GPL-3.0-only
// 通用工具条按钮（对照 doc/beatbench-ui-preview.html .tb / .tool-btn）。
// flatStyle=false：.tb 样式（surface2 底 + 边框）——页面工具条普通按钮；
// flatStyle=true：.tool-btn 样式（透明底，激活时主色底 + 主色边框）——编辑工具/工具选择。
// 激活态两种来源：
//   active = 外部驱动的激活（互斥单选组用——分组/工具，样式不依赖控件自身 checked，
//            避免 checkable 点击断绑导致的「多选残留」，见 doc/05 §4.3）；
//   checkable+checked = 控件自管（独立开关类按钮）。
// 颜色一律走 Theme token（doc/07 §4），本组件是「内置默认皮肤」控件样式基线。
import QtQuick
import QtQuick.Controls

ToolButton {
    id: root

    property bool flatStyle: false
    property bool active: false      // 外部激活（互斥单选组）
    readonly property bool lit: root.active || (root.checkable && root.checked)

    opacity: root.enabled ? 1.0 : 0.45

    background: Rectangle {
        radius: Theme.radiusSm
        color: {
            if (root.lit)
                return Theme.primarySoft
            if (root.hovered && root.enabled)
                return Theme.surface3
            return root.flatStyle ? "transparent" : Theme.surface2
        }
        border.width: 1
        border.color: root.lit
                      ? Theme.primary
                      : (root.flatStyle ? "transparent" : Theme.borderStrong)
    }
    contentItem: Label {
        text: root.text
        color: root.lit ? Theme.primary : Theme.textMuted
        font.pixelSize: Theme.fsBase
        font.family: Theme.fontSans
        horizontalAlignment: Text.AlignHCenter
        verticalAlignment: Text.AlignVCenter
        elide: Text.ElideRight
    }
}
