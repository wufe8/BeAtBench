// SPDX-License-Identifier: GPL-3.0-only
// 只标题栏响应左键拖动；不抢输入焦点，不覆盖正文/页脚，也不创建桌面 Window。
import QtQuick
import QtQuick.Controls

Rectangle {
    id: root
    required property var dialog
    required property var positioner
    width: dialog.width
    height: 34
    color: Theme.surface
    border.color: Theme.borderStrong
    border.width: 1
    Label {
        anchors.left: parent.left
        anchors.leftMargin: 12
        anchors.right: parent.right
        anchors.rightMargin: 12
        anchors.verticalCenter: parent.verticalCenter
        text: root.dialog.title
        elide: Text.ElideRight
        color: Theme.text
        font.bold: true
        font.pixelSize: Theme.fsBase
    }
    MouseArea {
        anchors.fill: parent
        acceptedButtons: Qt.LeftButton
        preventStealing: true
        objectName: "dialogDragArea"
        cursorShape: !root.enabled || !enabled ? Qt.ArrowCursor : pressed ? Qt.ClosedHandCursor : Qt.OpenHandCursor
        onPressed: function(mouse) { root.positioner.beginDrag(this, mouse.x, mouse.y) }
        onPositionChanged: function(mouse) {
            if (pressed) root.positioner.moveDrag(this, mouse.x, mouse.y)
        }
        onReleased: root.positioner.endDrag()
        onCanceled: root.positioner.endDrag()
    }
}
