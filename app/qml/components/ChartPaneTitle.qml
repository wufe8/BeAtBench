// SPDX-License-Identifier: GPL-3.0-only
import QtQuick
import QtQuick.Controls

Label {
    id: root
    property bool paneActive: false
    leftPadding: 2
    rightPadding: 2
    topPadding: 1
    bottomPadding: 1
    font.family: Theme.fontSans
    font.pixelSize: Theme.fsSmall
    color: paneActive ? Theme.focusRing : Theme.textMuted
    background: Rectangle {
        visible: root.paneActive
        color: Theme.surface2
        radius: Theme.boxRadius
        Rectangle {
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.bottom: parent.bottom
            anchors.margins: 2
            height: 1
            color: Theme.focusRing
        }
    }
}
