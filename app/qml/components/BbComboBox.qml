// SPDX-License-Identifier: GPL-3.0-only
// 通用下拉选择（主题统一样式，M2 2026-09）：surface2 底 + 圆角 + 主色 hover 边框 +
// 右侧小三角。⚠️ ComboBox.delegate 是 FINAL 属性（不可覆写），内建 delegate 文字用系统
// 调色板（黑字，深色主题不可读，2026-09 实测反馈）→ popup 完全自定义 ListView：
// 自管 model/高亮/点击（鼠标交互完备；键盘导航作为取舍暂不迁移）。
import QtQuick
import QtQuick.Controls

ComboBox {
    id: root

    property color textColor: Theme.text

    font.pixelSize: Theme.fsSmall
    font.family: Theme.fontSans

    contentItem: Label {
        text: root.displayText
        color: root.textColor
        font: root.font
        verticalAlignment: Text.AlignVCenter
        leftPadding: 8
    }

    indicator: Item {
        anchors.right: parent.right
        anchors.rightMargin: 8
        anchors.verticalCenter: parent.verticalCenter
        width: 7; height: 5
        Canvas {
            anchors.fill: parent
            onPaint: {
                const ctx = getContext("2d")
                ctx.reset()
                ctx.fillStyle = Theme.textMuted
                ctx.beginPath()
                ctx.moveTo(0, 0); ctx.lineTo(3.5, 5); ctx.lineTo(7, 0)
                ctx.closePath(); ctx.fill()
            }
        }
    }

    background: Rectangle {
        implicitHeight: 24
        radius: Theme.boxRadius
        border.width: 1
        border.color: root.hovered ? Theme.accent : Theme.borderStrong
        color: Theme.surface2
        opacity: root.enabled ? 1.0 : 0.45
    }

    popup: Popup {
        y: root.height
        width: root.width
        implicitHeight: contentItem.implicitHeight + 8
        padding: 4
        background: Rectangle {
            radius: Theme.boxRadius
            border.width: 1
            border.color: Theme.borderStrong
            color: Theme.surface
        }
        contentItem: ListView {
            implicitHeight: Math.min(contentHeight + 4, 220)
            clip: true
            model: root.model
            currentIndex: root.highlightedIndex
            delegate: ItemDelegate {
                width: root.popup ? root.popup.width - 8 : root.width
                required property int index
                required property string modelData
                contentItem: Label {
                    text: modelData
                    color: Theme.text
                    font: root.font
                    verticalAlignment: Text.AlignVCenter
                    leftPadding: 8
                }
                background: Rectangle {
                    radius: Theme.boxRadius
                    color: ListView.isCurrentItem ? Theme.surface3 : "transparent"
                }
                onClicked: {
                    // ⚠️ 自管 popup 绕过了 ComboBox 内建激活链：手动派发「激活」信号，
                    // 否则使用方绑定的 onActivated(idx) 永不触发（2026 反馈：采样框选择无反应）。
                    root.currentIndex = index
                    root.activated(index)
                    root.popup.close()
                }
            }
        }
    }
}
