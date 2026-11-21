// SPDX-License-Identifier: GPL-3.0-only
import QtQuick
import QtQuick.Controls

Label {
    id: root
    property bool paneActive: false
    leftPadding: 4
    rightPadding: 4
    topPadding: 3
    bottomPadding: 3
    font.family: Theme.fontMono
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
