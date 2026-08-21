// SPDX-License-Identifier: GPL-3.0-only
// 元信息面板（左 Dock）：显示 dispatch(info) 返回的 Chart.meta 常用字段。
// M2 第一条真链路的展示端；编辑（meta.edit 命令）归 M3。
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

ColumnLayout {
    id: root
    property var meta: null
    property string chartPath: ""

    spacing: 4

    Label {
        text: qsTr("元信息")
        font.bold: true
        color: "#c8cdd6"
    }

    // 常用头部字段（显示顺序）
    readonly property var fields: ["TITLE", "SUBTITLE", "ARTIST", "GENRE", "BPM", "PLAYER",
        "PLAYLEVEL", "RANK", "TOTAL", "DIFFICULTY"]

    ScrollView {
        Layout.fillWidth: true
        Layout.fillHeight: true
        clip: true
        ColumnLayout {
            width: parent.width
            spacing: 2
            Repeater {
                model: root.fields
                RowLayout {
                    Layout.fillWidth: true
                    spacing: 6
                    Label {
                        text: modelData + ":"
                        color: "#6b7484"
                        Layout.preferredWidth: 86
                        font.family: "Consolas, monospace"
                        font.pixelSize: 11
                    }
                    Label {
                        text: root.meta ? (root.meta[modelData] !== undefined ? String(root.meta[modelData]) : "—") : "—"
                        color: root.meta && root.meta[modelData] !== undefined ? "#c8cdd6" : "#5b6472"
                        elide: Text.ElideRight
                        Layout.fillWidth: true
                        font.pixelSize: 11
                    }
                }
            }
            // 其他字段（meta 里还有但不在常用列表的）
            Item { height: 6 }
            Label {
                visible: root.meta !== null
                text: qsTr("路径：%1").arg(root.chartPath)
                color: "#5b6472"
                font.pixelSize: 10
                elide: Text.ElideMiddle
                Layout.fillWidth: true
            }
        }
    }
}
