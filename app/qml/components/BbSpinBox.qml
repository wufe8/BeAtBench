// SPDX-License-Identifier: GPL-3.0-only
// 通用数值输入（主题统一样式，M2 2026-09）：QQuick Controls 2 SpinBox + surface 底/
// 圆角/边框。⚠️ Qt 自定义 SpinBox 的 up/down indicator 需**自己定位**（默认不排布，
// 两个都 anchors.fill → 三角重叠成 ✕，2026-09 实测反馈）：up 右上、down 右下各半高。
import QtQuick
import QtQuick.Controls

SpinBox {
    id: root

    property color textColor: Theme.text

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
    }

    up.indicator: Item {
        x: root.width - 18
        y: 0
        width: 18
        height: Math.floor(root.height / 2)
        enabled: root.enabled
        opacity: root.up.hovered ? 1.0 : 0.65
        Canvas {
            anchors.centerIn: parent
            width: 6; height: 4
            onPaint: {
                const ctx = getContext("2d")
                ctx.reset()
                ctx.fillStyle = "#ffffff"
                ctx.beginPath()
                ctx.moveTo(0, 4); ctx.lineTo(3, 0); ctx.lineTo(6, 4)
                ctx.closePath(); ctx.fill()
            }
        }
    }
    down.indicator: Item {
        x: root.width - 18
        y: Math.floor(root.height / 2)
        width: 18
        height: Math.ceil(root.height / 2)
        enabled: root.enabled
        opacity: root.down.hovered ? 1.0 : 0.65
        Canvas {
            anchors.centerIn: parent
            width: 6; height: 4
            onPaint: {
                const ctx = getContext("2d")
                ctx.reset()
                ctx.fillStyle = "#ffffff"
                ctx.beginPath()
                ctx.moveTo(0, 0); ctx.lineTo(3, 4); ctx.lineTo(6, 0)
                ctx.closePath(); ctx.fill()
            }
        }
    }

    background: Rectangle {
        radius: Theme.radiusSm
        border.width: 1
        border.color: root.activeFocus ? Theme.primary : Theme.borderStrong
        color: Theme.surface2
    }
}
