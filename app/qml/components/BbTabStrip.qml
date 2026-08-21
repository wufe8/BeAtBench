// SPDX-License-Identifier: GPL-3.0-only
// 可横向滚动的标签/分组条（左 Dock 栏目、采样分组行等——栏目多时横向滚动，不截断）。
// 选中 = active 外部激活态（互斥单选，无 checkable 断绑问题）；Flickable 拖拽 + 滚动。
import QtQuick
import QtQuick.Controls

Flickable {
    id: strip

    property var model: []
    property int currentIndex: 0
    signal indexRequested(int index)

    contentWidth: row.width
    contentHeight: row.height
    implicitHeight: row.height     // Flickable 无隐式高度；布局内（ColumnLayout 等）默认 0 会不可见
    boundsBehavior: Flickable.StopAtBounds
    clip: true

    Row {
        id: row
        spacing: 4
        Repeater {
            id: repeater
            model: strip.model
            delegate: BbTabButton {
                // 支持字符串模型与 {label,count} 对象模型（采样分组）
                text: typeof modelData === "string"
                      ? modelData
                      : String(modelData.label + " " + modelData.count)
                active: index === strip.currentIndex
                onClicked: strip.indexRequested(index)
            }
        }
    }

    // 选中变化 → 滚入视野（当前块居中偏左）
    onCurrentIndexChanged: ensureVisible(strip.currentIndex)
    Component.onCompleted: ensureVisible(strip.currentIndex)
    // 鼠标滚轮横向滚动（dock 窄时查看后续栏目/分组）；WheelHandler 接管 Flickable 默认纵向滚动
    WheelHandler {
        onWheel: (event) => {
            const dy = event.angleDelta.y !== 0 ? event.angleDelta.y : event.pixelDelta.y
            const step = dy > 0 ? -120 : 120
            strip.contentX = Math.max(0, Math.min(strip.contentX + step,
                                                  Math.max(0, strip.contentWidth - strip.width)))
            event.accepted = true
        }
    }

    function ensureVisible(i) {
        if (i < 0) return
        const item = repeater.itemAt(i)
        if (!item) return
        let target = item.x - (width - item.width) / 3
        target = Math.max(0, Math.min(target, Math.max(0, contentWidth - width)))
        contentX = target
    }
}