// SPDX-License-Identifier: GPL-3.0-only
// Presentation only: discovery, validation and immutable preview plans live in the controller.
// Empty controller targets mean ALL; checked-only with no charts must never reach start().
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Dialogs as NativeDialogs

BbDialog {
    id: root
    objectName: "resourceRelocationDialog"
    required property var controller
    required property string projectRoot
    required property var targetPaths
    property string onlyResource: ""
    property bool checkedOnly: false
    property string localMessage: ""
    property bool previewCurrent: false
    property alias searchFolder: folderInput.text
    property alias recursive: recursiveBox.checked
    property alias oldDirectory: oldInput.text
    property alias newDirectory: newInput.text
    readonly property var preview: controller ? controller.preview : ({})
    readonly property bool scopeAllowed: !checkedOnly || targetPaths.length > 0
    readonly property bool canApply: scopeAllowed && previewCurrent && !!preview.ok
                                     && preview.changedCount > 0 && !controller.busy
    signal applied(string message)
    title: qsTr("修复缺失资源")
    width: Math.max(0, Math.min(1040, (parent ? parent.width : 1064) - 24))
    height: Math.max(0, Math.min(760, (parent ? parent.height : 784) - 24))

    function chartLabel(path) {
        const prefix = projectRoot + "/"
        return path.indexOf(prefix) === 0 ? path.slice(prefix.length) : path
    }
    function referenceLabel(key) {
        // Only the trailing kind / ID / field are presentation data; never show the path identity.
        const parts = key.split("|")
        if (parts.length < 4) return qsTr("资源引用")
        const field = parts[parts.length - 1]
        return "#" + (field || parts[parts.length - 3].toUpperCase() + parts[parts.length - 2])
    }
    function stateLabel(state) {
        switch (state) {
        case "searching": return qsTr("正在搜索候选")
        case "ambiguous": return qsTr("多个候选，请手动选择")
        case "ready": return qsTr("已选择候选")
        case "limited": return qsTr("搜索未完成，请缩小目录或手动选择")
        default: return qsTr("未找到候选，可手动选择文件")
        }
    }
    function invalidatePreview() { previewCurrent = false }
    function restart() {
        invalidatePreview()
        if (!controller || !visible) return
        controller.cancel()
        // Change handlers can run before the derived scopeAllowed binding reevaluates.
        // Read the changed inputs directly: passing [] to start() would mean ALL.
        if (checkedOnly && targetPaths.length === 0) {
            localMessage = qsTr("仅勾选谱面：请先勾选至少一个谱面；不会自动改为全部。")
            return
        }
        const result = controller.start(onlyResource, checkedOnly ? targetPaths : [])
        localMessage = result.ok ? "" : result.error
    }
    function openFor(resourcePath) {
        onlyResource = resourcePath
        checkedOnly = false
        open()
    }
    function search() {
        if (!scopeAllowed || controller.busy) return
        invalidatePreview()
        localMessage = ""
        controller.search(folderInput.text, recursiveBox.checked)
    }
    function mapDirectory() {
        if (!scopeAllowed || controller.busy) return
        invalidatePreview()
        const result = controller.mapDirectory(oldInput.text, newInput.text)
        localMessage = result.ok ? "" : result.error
    }
    function selectUses(selected, onlyResolved) {
        if (!scopeAllowed || controller.busy) return
        invalidatePreview()
        controller.setAllSelected(selected, onlyResolved)
    }
    function chooseCandidate(key, path) {
        invalidatePreview()
        const result = controller.chooseCandidate(key, path)
        localMessage = result.ok ? "" : result.error
    }
    function prepare() {
        if (!scopeAllowed || controller.busy) return
        const result = controller.prepare()
        previewCurrent = !!result.ok
        localMessage = result.ok ? "" : result.error
    }
    function applyPreview() {
        if (!canApply) return
        const result = controller.commit(preview.planId)
        invalidatePreview()
        if (!result.ok) { localMessage = result.error; return }
        applied(qsTr("资源引用已在内存更新，待保存；每谱可独立撤销。不复制文件、不自动保存。"))
        close()
    }
    onOpened: restart()
    onClosed: { invalidatePreview(); if (controller) controller.cancel() }
    onCheckedOnlyChanged: restart()
    onTargetPathsChanged: restart()
    onProjectRootChanged: { if (visible) close() }

    NativeDialogs.FolderDialog {
        id: folderPicker
        title: qsTr("选择搜索文件夹（仅在指定目录搜索）")
        onAccepted: folderInput.text = window.urlToPath(selectedFolder)
    }
    NativeDialogs.FileDialog {
        id: filePicker
        property string resourceKey: ""
        title: qsTr("手动选择替代文件（项目内）")
        fileMode: NativeDialogs.FileDialog.OpenFile
        onAccepted: root.chooseCandidate(resourceKey, window.urlToPath(selectedFile))
    }

    ScrollView {
        id: bodyScroll
        objectName: "resourceRelocationScroll"
        Layout.fillWidth: true
        Layout.fillHeight: true
        Layout.minimumHeight: 0
        clip: true
        contentWidth: availableWidth
        contentHeight: body.height
        ColumnLayout {
            id: body
            width: bodyScroll.availableWidth
            height: implicitHeight
            spacing: 10
            Label {
                Layout.fillWidth: true
                text: qsTr("仅更新已有资源引用；候选文件必须位于项目内。不复制文件、不自动保存。先选择候选与受影响引用，再预览旧 → 新引用并应用。")
                wrapMode: Text.Wrap
                color: Theme.textMuted
                font.pixelSize: Theme.fsSmall
            }
            Label {
                Layout.fillWidth: true
                text: root.onlyResource !== "" ? qsTr("仅修复：%1").arg(root.onlyResource) : qsTr("修复项目内缺失资源")
                wrapMode: Text.WrapAnywhere
                color: Theme.text
                font.pixelSize: Theme.fsSmall
            }
            BbCheckBox {
                objectName: "resourceRelocationCheckedOnly"
                text: qsTr("仅勾选谱面（%1）").arg(root.targetPaths.length)
                checked: root.checkedOnly
                onToggled: root.checkedOnly = checked
                enabled: !root.controller.busy
            }
            Label {
                Layout.fillWidth: true
                text: root.checkedOnly ? qsTr("范围：仅勾选谱面的受影响引用；未勾选谱面不改动。")
                                       : qsTr("范围：所有受影响谱面的引用（与项目页勾选无关）。")
                wrapMode: Text.Wrap
                color: root.scopeAllowed ? Theme.textMuted : Theme.warning
                font.pixelSize: Theme.fsSmall
            }
            BbTextField {
                id: folderInput
                objectName: "resourceRelocationSearchFolder"
                Layout.fillWidth: true
                Layout.minimumWidth: 0
                placeholderText: qsTr("指定搜索文件夹（不扫描整个磁盘）")
                onTextChanged: root.invalidatePreview()
            }
            Flow {
                Layout.fillWidth: true
                Layout.preferredHeight: implicitHeight
                spacing: 8
                BbToolButton { text: qsTr("选搜索文件夹…"); onClicked: folderPicker.open() }
                BbCheckBox {
                    id: recursiveBox
                    text: qsTr("递归子目录")
                    checked: true
                    onToggled: root.invalidatePreview()
                }
                BbToolButton {
                    objectName: "resourceRelocationSearchButton"
                    text: qsTr("搜索候选")
                    enabled: root.scopeAllowed && !root.controller.busy && folderInput.text.trim() !== ""
                    onClicked: root.search()
                }
            }
            Label { text: qsTr("旧目录 → 新目录映射"); color: Theme.text; font.pixelSize: Theme.fsSmall }
            BbTextField {
                id: oldInput
                objectName: "resourceRelocationOldDirectory"
                Layout.fillWidth: true
                Layout.minimumWidth: 0
                placeholderText: qsTr("旧目录（项目相对路径或绝对路径）")
                onTextChanged: root.invalidatePreview()
            }
            BbTextField {
                id: newInput
                objectName: "resourceRelocationNewDirectory"
                Layout.fillWidth: true
                Layout.minimumWidth: 0
                placeholderText: qsTr("新目录（项目内，不移动文件）")
                onTextChanged: root.invalidatePreview()
            }
            BbToolButton {
                objectName: "resourceRelocationMapButton"
                text: qsTr("匹配目录映射")
                enabled: root.scopeAllowed && !root.controller.busy && oldInput.text.trim() !== "" && newInput.text.trim() !== ""
                onClicked: root.mapDirectory()
            }
            Label {
                Layout.fillWidth: true
                objectName: "resourceRelocationMessage"
                text: root.localMessage || root.controller.message
                wrapMode: Text.WrapAnywhere
                color: Theme.warning
                font.pixelSize: Theme.fsSmall
            }
            Label { text: root.controller.busy ? qsTr("正在搜索…") : qsTr("缺失资源 → 候选文件 / 受影响引用"); color: Theme.text; font.pixelSize: Theme.fsSmall }
            Flow {
                Layout.fillWidth: true
                Layout.preferredHeight: implicitHeight
                spacing: 8
                BbToolButton {
                    objectName: "resourceRelocationSelectResolvedButton"
                    text: qsTr("勾选可修复项")
                    enabled: root.scopeAllowed && !root.controller.busy
                    onClicked: root.selectUses(true, true)
                }
                BbToolButton {
                    objectName: "resourceRelocationClearSelectionButton"
                    text: qsTr("清空勾选")
                    enabled: root.scopeAllowed && !root.controller.busy
                    onClicked: root.selectUses(false, false)
                }
            }
            ListView {
                objectName: "resourceRelocationGroups"
                Layout.fillWidth: true
                Layout.preferredHeight: 360
                Layout.minimumHeight: 0
                clip: true
                ScrollBar.vertical: ScrollBar {}
                model: root.scopeAllowed ? root.controller.rows : []
                delegate: ColumnLayout {
                    id: resourceRow
                    required property var modelData
                    width: ListView.view.width
                    height: implicitHeight
                    spacing: 5
                    Label {
                        Layout.fillWidth: true
                        text: resourceRow.modelData.oldPath + " → " + (resourceRow.modelData.replacement || qsTr("尚未选择"))
                        wrapMode: Text.WrapAnywhere
                        color: Theme.text
                        font.pixelSize: Theme.fsSmall
                    }
                    Label {
                        Layout.fillWidth: true
                        text: root.stateLabel(resourceRow.modelData.state)
                        wrapMode: Text.WrapAnywhere
                        color: Theme.textMuted
                        font.pixelSize: Theme.fsSmall
                    }
                    BbComboBox {
                        Layout.fillWidth: true
                        Layout.minimumWidth: 0
                        model: resourceRow.modelData.candidates
                        displayText: qsTr("选择候选（%1）").arg(count)
                        enabled: !root.controller.busy
                        onActivated: root.chooseCandidate(resourceRow.modelData.key, currentText)
                    }
                    BbTextField {
                        id: manualInput
                        Layout.fillWidth: true
                        Layout.minimumWidth: 0
                        placeholderText: qsTr("手动输入替代文件绝对路径")
                        onTextChanged: root.invalidatePreview()
                    }
                    Flow {
                        Layout.fillWidth: true
                        Layout.preferredHeight: implicitHeight
                        spacing: 8
                        BbToolButton {
                            text: qsTr("使用此文件")
                            enabled: !root.controller.busy && manualInput.text.trim() !== ""
                            onClicked: root.chooseCandidate(resourceRow.modelData.key, manualInput.text)
                        }
                        BbToolButton {
                            text: qsTr("选择文件…")
                            enabled: !root.controller.busy
                            onClicked: { filePicker.resourceKey = resourceRow.modelData.key; filePicker.open() }
                        }
                    }
                    ListView {
                        objectName: "resourceRelocationUses:" + resourceRow.modelData.key
                        Layout.fillWidth: true
                        Layout.preferredHeight: Math.min(140, resourceRow.modelData.uses.length * 36)
                        Layout.minimumHeight: 0
                        clip: true
                        model: resourceRow.modelData.uses
                        ScrollBar.vertical: ScrollBar {}
                        delegate: BbCheckBox {
                            id: useBox
                            required property var modelData
                            width: ListView.view.width
                            height: implicitHeight
                            objectName: "resourceRelocationUse:" + modelData.key
                            text: modelData.chart + "  " + modelData.kind + " " + (modelData.field || modelData.id)
                            checked: modelData.selected
                            enabled: !root.controller.busy && (!root.checkedOnly || root.targetPaths.indexOf(modelData.chartPath) >= 0)
                            onToggled: {
                                root.invalidatePreview()
                                root.controller.setUseSelected(resourceRow.modelData.key, modelData.key, checked)
                            }
                            contentItem: Label {
                                text: useBox.text
                                leftPadding: useBox.indicator.width + useBox.spacing
                                wrapMode: Text.WrapAnywhere
                                color: Theme.text
                                font.pixelSize: Theme.fsSmall
                            }
                        }
                    }
                    Rectangle { Layout.fillWidth: true; implicitHeight: 1; color: Theme.border }
                }
            }
            BbToolButton {
                objectName: "resourceRelocationPreviewButton"
                text: qsTr("预览引用更新")
                enabled: root.scopeAllowed && !root.controller.busy && root.controller.rows.length > 0
                onClicked: root.prepare()
            }
            Label {
                Layout.fillWidth: true
                objectName: "resourceRelocationPreviewSummary"
                text: root.previewCurrent && root.preview.ok
                      ? qsTr("受影响 %1 谱 · 更新 %2 项引用（内存，待保存）").arg(root.preview.targetCount).arg(root.preview.changedCount)
                      : qsTr("尚无有效预览；选择变更后请重新预览。")
                wrapMode: Text.Wrap
                color: Theme.text
                font.pixelSize: Theme.fsSmall
            }
            ListView {
                objectName: "resourceRelocationPreviewList"
                Layout.fillWidth: true
                Layout.preferredHeight: 180
                Layout.minimumHeight: 0
                clip: true
                model: root.previewCurrent && root.preview.ok ? root.preview.rows : []
                ScrollBar.vertical: ScrollBar {}
                delegate: Label {
                    required property var modelData
                    width: ListView.view.width
                    text: root.chartLabel(modelData.chart) + "  " + root.referenceLabel(modelData.key)
                          + "\n" + modelData.old + " → " + modelData.new
                    HoverHandler { id: previewHover }
                    ToolTip.visible: previewHover.hovered
                    ToolTip.delay: 500
                    ToolTip.text: modelData.chart + "\n" + root.referenceLabel(modelData.key)
                                  + "  " + modelData.old + " → " + modelData.new
                    wrapMode: Text.WrapAnywhere
                    color: Theme.text
                    font.pixelSize: Theme.fsSmall
                }
            }
        }
    }
    footer: Rectangle {
        width: root.width
        height: 42
        color: Theme.surface2
        RowLayout {
            anchors.fill: parent
            anchors.margins: 5
            Item { Layout.fillWidth: true }
            BbToolButton {
                objectName: "resourceRelocationApplyButton"
                text: qsTr("应用引用更新")
                enabled: root.canApply
                onClicked: root.applyPreview()
            }
            BbToolButton { text: qsTr("取消"); onClicked: root.reject() }
        }
    }
}
