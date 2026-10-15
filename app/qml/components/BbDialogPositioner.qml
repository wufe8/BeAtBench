// SPDX-License-Identifier: GPL-3.0-only
// 应用内模态弹窗定位：每次打开恢复默认位置，拖动/resize 后限制在主窗口内容区。
// Popup.x/y 相对其逻辑 parent，不一定是 Overlay；统一在窗口坐标计算后再映射回 parent。
import QtQuick
import QtQuick.Window

QtObject {
    id: root
    required property var dialog
    property int horizontalAlignment: Qt.AlignHCenter
    property int verticalAlignment: Qt.AlignVCenter
    property real edgeMargin: 8
    property real rightMargin: 12
    property real topMargin: 40
    property real titleHeight: 34

    readonly property var hostWindow: dialog && dialog.parent ? dialog.parent.Window.window : null
    readonly property Item boundsItem: hostWindow ? hostWindow.contentItem : (dialog ? dialog.parent : null)
    property point pressPoint: Qt.point(0, 0)
    property point pressOrigin: Qt.point(0, 0)
    property bool dragging: false

    function originInBounds() {
        return dialog.parent.mapToItem(boundsItem, dialog.x, dialog.y)
    }
    function setOrigin(x, y) {
        if (!dialog || !dialog.parent || !boundsItem) return
        const point = dialog.parent.mapFromItem(boundsItem, x, y)
        dialog.x = point.x
        dialog.y = point.y
    }
    function boundedOrigin(x, y) {
        const w = Math.max(0, boundsItem.width)
        const h = Math.max(0, boundsItem.height)
        // 通常整个弹窗留在边界内。超大弹窗允许底部/右侧溢出，但左端标题可达；
        // 不缩放内容、不强行改尺寸，避免破坏表单布局。
        const insetX = Math.min(edgeMargin, Math.max(0, (w - dialog.width) / 2))
        const insetY = Math.min(edgeMargin, Math.max(0, (h - dialog.height) / 2))
        const minX = dialog.width <= w ? insetX : Math.min(0, w - dialog.width)
        const maxX = dialog.width <= w ? Math.max(minX, w - dialog.width - insetX)
                                       : Math.max(0, w - Math.min(dialog.width, 96))
        const minY = dialog.height <= h ? insetY : 0
        const maxY = dialog.height <= h ? Math.max(minY, h - dialog.height - insetY)
                                        : Math.max(0, h - Math.min(titleHeight, h))
        return Qt.point(Math.max(minX, Math.min(maxX, x)), Math.max(minY, Math.min(maxY, y)))
    }
    function clampPosition() {
        if (!dialog || !dialog.visible || !dialog.parent || !boundsItem) return
        const origin = originInBounds()
        const bounded = boundedOrigin(origin.x, origin.y)
        setOrigin(bounded.x, bounded.y)
    }
    function resetPosition() {
        if (!dialog || !dialog.parent || !boundsItem) return
        dragging = false
        let x = (boundsItem.width - dialog.width) / 2
        let y = (boundsItem.height - dialog.height) / 2
        if (horizontalAlignment === Qt.AlignRight) x = boundsItem.width - dialog.width - rightMargin
        if (horizontalAlignment === Qt.AlignLeft) x = edgeMargin
        if (verticalAlignment === Qt.AlignTop) y = topMargin
        const bounded = boundedOrigin(x, y)
        setOrigin(bounded.x, bounded.y)
    }
    function beginDrag(source, x, y) {
        if (!dialog || !boundsItem || !dialog.parent) return
        pressPoint = source.mapToItem(boundsItem, x, y)
        pressOrigin = originInBounds()
        dragging = true
    }
    function moveDrag(source, x, y) {
        if (!dragging || !boundsItem) return
        const current = source.mapToItem(boundsItem, x, y)
        const bounded = boundedOrigin(pressOrigin.x + current.x - pressPoint.x,
                                      pressOrigin.y + current.y - pressPoint.y)
        setOrigin(bounded.x, bounded.y)
    }
    function endDrag() { dragging = false }

    property Connections dialogConnections: Connections {
        target: root.dialog
        function onAboutToShow() { root.resetPosition() }
        function onOpened() { root.clampPosition() }
        function onClosed() { root.endDrag() }
        function onWidthChanged() { root.clampPosition() }
        function onHeightChanged() { root.clampPosition() }
    }
    property Connections boundsConnections: Connections {
        target: root.boundsItem
        function onWidthChanged() { root.clampPosition(); Qt.callLater(root.clampPosition) }
        function onHeightChanged() { root.clampPosition(); Qt.callLater(root.clampPosition) }
    }
    property Connections parentConnections: Connections {
        target: root.dialog ? root.dialog.parent : null
        function onXChanged() { root.clampPosition() }
        function onYChanged() { root.clampPosition() }
        function onScaleChanged() { root.clampPosition() }
    }
}
