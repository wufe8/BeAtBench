// SPDX-License-Identifier: GPL-3.0-only
import QtQuick
import QtQuick.Layouts

BbDialog {
    id: root
    objectName: "chartHistoryDialog"
    required property var controller
    property alias selectedSnapshotId: panel.selectedSnapshotId
    readonly property real hostWidth: parent ? parent.width : 684
    readonly property real hostHeight: parent ? parent.height : 524
    title: qsTr("本地谱面历史")
    width: Math.max(0, Math.min(660, hostWidth - 24))
    height: Math.max(0, Math.min(500, hostHeight - 24))
    showCancel: false
    Connections {
        target: root.controller
        function onShowRequested() { root.open() }
    }
    ChartHistoryPanel {
        id: panel
        controller: root.controller
        onCloseRequested: root.close()
    }
}
