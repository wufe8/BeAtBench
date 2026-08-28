// SPDX-License-Identifier: GPL-3.0-only
// 通用数值输入（主题统一样式，M2 2026-09）：QQuick Controls 2 SpinBox + surface 底/
// 圆角/边框。⚠️ Qt 自定义 SpinBox 的 up/down indicator 需**自己定位**（默认不排布，
// 两个都 anchors.fill → 三角重叠成 ✕，2026-09 实测反馈）：up 右上、down 右下各半高。
import QtQuick
import QtQuick.Controls

SpinBox {
    id: root

    property color textColor: Theme.text
    /// 2026-09 用户：snap 上下按钮 ×2/÷2（音乐常用拍子）；manual 输入不受影响（1/3、1/5 可手填）。
    /// 0 = 默认 ±1；>0 = 点击上下箭头时 value ×/÷ stepFactor（整数除，下限 from / 上限 to）。
    property int stepFactor: 0
    /// 2026-09：Esc 行为钩子（同 BbTextField）。对话框内 SpinBox 需一次 Esc 即关整个 Dialog——
    /// 默认内部 TextInput 的 Esc=释放焦点会让第二次 Esc 才到 Dialog（用户报告「要按两次才关」）。
    /// 设置后先调用该钩子（对话框自己 reject），再释放焦点。非对话框场景保持 null → 原行为不变。
    property var escapeHandler: null

    font.pixelSize: Theme.fsSmall
    font.family: Theme.fontSans
    implicitWidth: 72
    implicitHeight: 26

    // 内容（文本；右侧给箭头留白；TextInput 无 verticalAlignment → 用 anchors 垂直居中）
    contentItem: TextInput {
        anchors.left: parent.left
        anchors.leftMargin: 8
        anchors.right: parent.right
        anchors.rightMargin: 18   // 箭头区
        anchors.verticalCenter: parent.verticalCenter
        text: root.textFromValue(root.value, root.locale)
        color: root.textColor
        font: root.font
        readOnly: !root.editable
        validator: root.validator
        inputMethodHints: Qt.ImhFormattedNumbersOnly
        onAccepted: root.value = root.textFromValue(text, root.locale)
        selectByMouse: true
        // 2026-09：Esc 释放焦点（否则焦点粘住 → 快捷键被文本框吞掉）；有 escapeHandler 则先调用（关对话框）
        Keys.onEscapePressed: {
            if (root.escapeHandler) root.escapeHandler()
            root.focus = false
            deselect()
        }
    }

    // 上下按钮：stepFactor>0 时用 MouseArea 完全接管（吞掉点击，底层 QQuickIndicatorButton
    // 的默认 ±1 不再触发），在 onClicked 里直接设 ×2/÷2 结果。
    up.indicator: Item {
        x: root.width - 18
        y: 0
        width: 18
        height: Math.floor(root.height / 2)
        enabled: root.enabled
        // 启用时全不透明（箭头最大对比度；亮色 skins 不再因淡opacity显得白/看不见），
        // 禁用 0.45 弱化。hover 保持 1.0。
        opacity: root.up.hovered ? 1.0 : (root.enabled ? 1.0 : 0.45)
        Canvas {
            anchors.centerIn: parent
            width: 6; height: 4
            onPaint: {
                const ctx = getContext("2d")
                ctx.reset()
                ctx.fillStyle = Theme.text  // 直接 Theme.text；确认箭头用主题正文色（非 root.textColor）
                ctx.beginPath()
                ctx.moveTo(0, 4); ctx.lineTo(3, 0); ctx.lineTo(6, 4)
                ctx.closePath(); ctx.fill()
            }
        }
        MouseArea {
            anchors.fill: parent
            onClicked: {
                if (root.stepFactor <= 0) return
                const target = Math.min(root.to, root.value * root.stepFactor)
                root.value = Math.max(root.from, target)
            }
        }
    }
    down.indicator: Item {
        x: root.width - 18
        y: Math.floor(root.height / 2)
        width: 18
        height: Math.ceil(root.height / 2)
        enabled: root.enabled
        opacity: root.down.hovered ? 1.0 : (root.enabled ? 1.0 : 0.45)
        Canvas {
            anchors.centerIn: parent
            width: 6; height: 4
            onPaint: {
                const ctx = getContext("2d")
                ctx.reset()
                ctx.fillStyle = Theme.text  // 直接 Theme.text；确认箭头用主题正文色（非 root.textColor）
                ctx.beginPath()
                ctx.moveTo(0, 0); ctx.lineTo(3, 4); ctx.lineTo(6, 0)
                ctx.closePath(); ctx.fill()
            }
        }
        MouseArea {
            anchors.fill: parent
            onClicked: {
                if (root.stepFactor <= 0) return
                const target = Math.floor(root.value / root.stepFactor)
                root.value = Math.max(root.from, target)
            }
        }
    }

    // stepFactor>0：默认 ±1 步长置 0（键盘/自动步进也只走 ×/÷ 语义）
    stepSize: root.stepFactor > 0 ? 0 : 1

    background: Rectangle {
        radius: Theme.boxRadius
        border.width: 1
        border.color: root.activeFocus ? Theme.primary : Theme.borderStrong
        color: Theme.surface2
    }
}
