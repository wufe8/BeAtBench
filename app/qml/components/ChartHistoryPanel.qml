// SPDX-License-Identifier: GPL-3.0-only
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// Shared by the menu dialog and the project history tab. No disk logic here.
ScrollView {
        id: root
        required property var controller
        property bool saveAllowed: true
        property string selectedSnapshotId: ""
        signal closeRequested()
        Connections {
            target: root.controller
            function onEntriesChanged() { root.selectedSnapshotId = "" }
            function onSourcePathChanged() { root.selectedSnapshotId = ""; noteField.clear(); if (root.contentItem) root.contentItem.contentY = 0 }
        }
        objectName: "chartHistoryScroll"
        Layout.fillWidth: true
        Layout.fillHeight: true
        Layout.minimumWidth: 0
        Layout.minimumHeight: 0
        clip: true
        contentWidth: availableWidth
        ScrollBar.horizontal.policy: ScrollBar.AlwaysOff

        ColumnLayout {
            width: root.availableWidth
            spacing: 8

            Label {
                Layout.fillWidth: true
                text: root.controller.documentLabel || qsTr("未选择谱面")
                textFormat: Text.PlainText
                wrapMode: Text.WrapAnywhere
                color: Theme.text
                font.family: Theme.fontSans
                font.pixelSize: Theme.fsBase
                font.bold: true
            }
            BbCheckBox {
                id: enableHistory
                Layout.fillWidth: true
                Layout.minimumWidth: 0
                checked: root.controller.enabled
                enabled: !root.controller.busy
                text: qsTr("启用本地谱面历史（可选）")
                onToggled: root.controller.enabled = checked
                contentItem: Label {
                    text: enableHistory.text
                    leftPadding: enableHistory.indicator.width + enableHistory.spacing
                    color: Theme.text
                    font.family: Theme.fontSans
                    font.pixelSize: Theme.fsSmall
                    wrapMode: Text.Wrap
                }
            }
            Label {
                Layout.fillWidth: true
                text: qsTr("仅保存谱面文件，不包含 WAV、图片等素材；本地历史不替代备份。禁用不会删除已有历史，启用后可查看。")
                color: Theme.textFaint
                font.family: Theme.fontSans
                font.pixelSize: Theme.fsSmall
                wrapMode: Text.Wrap
            }
            BbCheckBox {
                id: exitSnapshots
                Layout.fillWidth: true
                Layout.minimumWidth: 0
                checked: root.controller.createOnClose
                enabled: root.controller.enabled && !root.controller.busy
                text: qsTr("退出时为已保存谱面创建快照")
                onToggled: root.controller.createOnClose = checked
                contentItem: Label {
                    text: exitSnapshots.text
                    leftPadding: exitSnapshots.indicator.width + exitSnapshots.spacing
                    color: Theme.text
                    font.family: Theme.fontSans
                    font.pixelSize: Theme.fsSmall
                    wrapMode: Text.Wrap
                }
            }
            Label {
                Layout.fillWidth: true
                text: qsTr("仅记录已命名且已保存的谱面；未保存或放弃的更改不会记录。")
                color: Theme.textFaint
                font.family: Theme.fontSans
                font.pixelSize: Theme.fsSmall
                wrapMode: Text.Wrap
            }
            BbTextField {
                id: noteField
                Layout.fillWidth: true
                Layout.minimumWidth: 0
                placeholderText: qsTr("备注（可选）")
                maximumLength: 65536
                enabled: root.controller.enabled && !root.controller.busy
                escapeHandler: function() { root.closeRequested() }
            }
            BbToolButton {
                Layout.maximumWidth: root.availableWidth
                text: qsTr("保存并创建快照")
                enabled: root.saveAllowed && root.controller.enabled && !root.controller.busy
                onClicked: root.controller.requestSavedSnapshot(noteField.text)
            }
            Label {
                Layout.fillWidth: true
                visible: !root.saveAllowed
                text: qsTr("当前所选谱面仅查看历史和恢复副本；如需保存新快照，请先在 A 中编辑此谱面。未命名谱面须先另存为。")
                wrapMode: Text.Wrap
                color: Theme.textMuted
                font.pixelSize: Theme.fsSmall
            }
            Label {
                Layout.fillWidth: true
                text: root.controller.busy ? qsTr("正在处理，请稍候…") : qsTr("历史快照（本机时间）")
                color: Theme.text
                font.family: Theme.fontSans
                font.pixelSize: Theme.fsSmall
                wrapMode: Text.Wrap
            }
            Rectangle {
                Layout.fillWidth: true
                Layout.preferredHeight: 145
                color: Theme.surface2
                border.color: Theme.borderStrong
                radius: Theme.boxRadius
                Label {
                    anchors.fill: parent
                    anchors.margins: 12
                    visible: root.controller.enabled && !root.controller.busy && root.controller.entries.length === 0
                    text: qsTr("还没有快照，点击上方按钮创建")
                    color: Theme.textFaint
                    font.family: Theme.fontSans
                    font.pixelSize: Theme.fsSmall
                    wrapMode: Text.Wrap
                    horizontalAlignment: Text.AlignHCenter
                    verticalAlignment: Text.AlignVCenter
                }
                ListView {
                    id: historyList
                    anchors.fill: parent
                    anchors.margins: 1
                    clip: true
                    model: root.controller.entries
                    enabled: root.controller.enabled && !root.controller.busy
                    ScrollBar.vertical: ScrollBar {}
                    delegate: ItemDelegate {
                        id: entryDelegate
                        required property var modelData
                        width: historyList.width
                        implicitHeight: entryColumn.implicitHeight + 12
                        highlighted: root.selectedSnapshotId === modelData.id
                        onClicked: root.selectedSnapshotId = modelData.id
                        background: Rectangle {
                            color: entryDelegate.highlighted ? Theme.primary : (entryDelegate.hovered ? Theme.surface : Theme.surface2)
                        }
                        contentItem: ColumnLayout {
                            id: entryColumn
                            spacing: 3
                            Label {
                                Layout.fillWidth: true
                                text: entryDelegate.modelData.displayTime
                                textFormat: Text.PlainText
                                color: entryDelegate.highlighted ? Theme.onAccent : Theme.text
                                font.family: Theme.fontSans
                                font.pixelSize: Theme.fsSmall
                                wrapMode: Text.WrapAnywhere
                            }
                            Label {
                                Layout.fillWidth: true
                                text: entryDelegate.modelData.note || qsTr("（无备注）")
                                textFormat: Text.PlainText
                                color: entryDelegate.highlighted ? Theme.onAccent : Theme.text
                                font.family: Theme.fontSans
                                font.pixelSize: Theme.fsSmall
                                wrapMode: Text.WrapAnywhere
                            }
                        }
                    }
                }
            }
            BbToolButton {
                Layout.maximumWidth: root.availableWidth
                text: qsTr("恢复为副本")
                enabled: root.controller.enabled && !root.controller.busy && root.selectedSnapshotId !== ""
                onClicked: root.controller.restoreCopy(root.selectedSnapshotId)
            }
            Label {
                Layout.fillWidth: true
                text: qsTr("恢复会在原目录创建唯一的新副本，不覆盖原文件，也不会自动打开。")
                color: Theme.textFaint
                font.family: Theme.fontSans
                font.pixelSize: Theme.fsSmall
                wrapMode: Text.Wrap
            }
            TextArea {
                Layout.fillWidth: true
                Layout.minimumWidth: 0
                Layout.preferredHeight: Math.max(60, implicitHeight)
                text: root.controller.message
                readOnly: true
                selectByMouse: true
                textFormat: TextEdit.PlainText
                wrapMode: TextEdit.WrapAnywhere
                color: Theme.text
                selectionColor: Theme.primary
                selectedTextColor: Theme.onAccent
                font.family: Theme.fontSans
                font.pixelSize: Theme.fsSmall
                background: Rectangle {
                    color: Theme.surface2
                    border.color: Theme.borderStrong
                    radius: Theme.boxRadius
                }
            }
            Label {
                Layout.fillWidth: true
                text: qsTr("历史存储：%1").arg(root.controller.storagePath)
                textFormat: Text.PlainText
                color: Theme.textFaint
                font.family: Theme.fontSans
                font.pixelSize: Theme.fsSmall
                wrapMode: Text.WrapAnywhere
            }
        }
    }
