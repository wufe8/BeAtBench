// SPDX-License-Identifier: GPL-3.0-only
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

BbDialog {
    id: root
    objectName: "resourceBindingDialog"
    required property var controller
    required property string projectRoot
    required property var targetPaths
    property alias rawText: input.text
    property var preview: ({})
    property string message: ""
    signal applied(string message)
    readonly property real hostWidth: parent ? parent.width : 824
    readonly property real hostHeight: parent ? parent.height : 624
    title: qsTr("批量更新自定义谱面内容")
    width: Math.max(0, Math.min(800, hostWidth - 24))
    height: Math.max(0, Math.min(600, hostHeight - 24))

    function invalidatePreview() {
        if (controller) controller.discardPlan()
        preview = ({})
        if (overwriteConsent) overwriteConsent.checked = false
    }
    function prepare() {
        invalidatePreview()
        preview = controller.prepareCustomContent(input.text, projectRoot, targetPaths)
        message = preview.ok ? qsTr("目标 %1 谱 · 修改 %2 项 · 覆盖 %3 项")
                  .arg(preview.targetCount).arg(preview.changedCount).arg(preview.collisionCount)
                  + (preview.warnings && preview.warnings.length ? "\n" + preview.warnings.join("\n") : "") : preview.error
    }
    function applyPreview() {
        if (!preview.ok || !preview.planId || preview.changedCount === 0) return
        if (preview.collisionCount > 0 && !overwriteConsent.checked) return
        const result = controller.commitBindings(preview.planId, overwriteConsent.checked)
        if (!result.ok) {
            invalidatePreview()
            message = result.error
            return
        }
        applied(qsTr("自定义内容已更新；每谱可独立撤销，请检查后保存。"))
        close()
    }
    Component.onCompleted: invalidatePreview()
    onProjectRootChanged: { if (controller) invalidatePreview() }
    onTargetPathsChanged: { if (controller) invalidatePreview() }
    onOpened: { invalidatePreview(); message = "" }
    onClosed: invalidatePreview()

    ScrollView {
        id: bodyScroll
        Layout.fillWidth: true
        Layout.fillHeight: true
        clip: true
        contentWidth: availableWidth
        contentHeight: bodyColumn.height
        ColumnLayout {
            id: bodyColumn
            width: bodyScroll.availableWidth
            height: implicitHeight
            spacing: 10
    Label {
        Layout.fillWidth: true
        text: qsTr("按对应字段、ID 或小节/通道更新内容，未涉及的内容保留；保存时由谱面格式写出器安排顺序，不按原文本行号插入。不复制文件、不自动保存。资源相对路径以项目根为基准。")
        wrapMode: Text.Wrap
        color: Theme.textMuted
        font.pixelSize: Theme.fsSmall
    }
    Label {
        objectName: "customContentRiskWarning"
        Layout.fillWidth: true
        text: qsTr("不受支持或无法识别的内容，可能无法插入到预期位置，保存后的位置和表现也可能变化。建议主要用于批量更新元数据或 WAV ID 定义；应用前请检查预览。随机/条件控制块及改变进制等不安全操作会被拒绝。")
        wrapMode: Text.Wrap
        color: Theme.warning
        font.pixelSize: Theme.fsSmall
    }
    Label {
        Layout.fillWidth: true
        text: qsTr("勾选 %1 谱 · 路径基准：%2").arg(root.targetPaths.length).arg(root.projectRoot)
        elide: Text.ElideMiddle
        color: Theme.text
        font.pixelSize: Theme.fsSmall
    }
    ScrollView {
        Layout.fillWidth: true
        Layout.preferredHeight: 110
        Layout.minimumHeight: 55
        clip: true
        TextArea {
            id: input
            objectName: "resourceBindingInput"
            placeholderText: "#TITLE New title\n#WAV01 audio/kick.wav"
            placeholderTextColor: Theme.textMuted
            color: Theme.text
            selectionColor: Theme.primarySoft
            font.pixelSize: Theme.fsSmall
            wrapMode: TextEdit.Wrap
            background: Rectangle { color: Theme.bg; border.color: Theme.border }
            onTextChanged: { root.invalidatePreview(); root.message = "" }
        }
    }
    BbToolButton {
        objectName: "resourceBindingPreviewButton"
        text: qsTr("预览内容更新")
        enabled: root.targetPaths.length > 0 && input.text.trim() !== ""
        onClicked: root.prepare()
    }
    Label {
        Layout.fillWidth: true
        text: root.message
        wrapMode: Text.Wrap
        color: root.preview.ok ? Theme.text : Theme.warning
        font.pixelSize: Theme.fsSmall
    }
    ListView {
        objectName: "resourceBindingPreviewList"
        Layout.fillWidth: true
        Layout.preferredHeight: Math.max(90, root.height - 380)
        Layout.minimumHeight: 35
        clip: true
        model: root.preview.ok ? root.preview.rows : []
        ScrollBar.vertical: ScrollBar {}
        delegate: Label {
            required property var modelData
            width: ListView.view.width
            text: modelData.chart + "  "
                  + (modelData.kind === "DATA" ? qsTr("数据组 ") + modelData.id
                     : modelData.kind === "RAW" ? qsTr("原始内容 ") + modelData.id
                     : "#" + modelData.kind + (modelData.id === modelData.kind ? "" : modelData.id))
                  + "  " + (modelData.overwrite ? qsTr("【覆盖】") : "")
                  + (modelData.oldFile || qsTr("未定义")) + " → " + modelData.newFile
                  + (modelData.kind === "DATA" ? "\n" + modelData.summary : "")
            wrapMode: Text.WrapAnywhere
            color: modelData.overwrite ? Theme.warning : Theme.text
            font.pixelSize: Theme.fsSmall
        }
    }
        }
    }
    BbCheckBox {
        id: overwriteConsent
        objectName: "resourceBindingOverwriteConsent"
        Layout.fillWidth: true
        visible: !!root.preview.ok && root.preview.collisionCount > 0
        text: qsTr("确认覆盖上列已有字段、定义或数据组（现有音符/BGA 引用已定义 ID 时也会使用新资源）")
        contentItem: Label {
            text: overwriteConsent.text
            color: Theme.text
            font.pixelSize: Theme.fsSmall
            font.family: Theme.fontSans
            wrapMode: Text.Wrap
            leftPadding: overwriteConsent.indicator.width + overwriteConsent.spacing
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
                objectName: "resourceBindingApplyButton"
                text: qsTr("应用内容更新")
                enabled: !!root.preview.ok && root.preview.changedCount > 0 &&
                         (root.preview.collisionCount === 0 || overwriteConsent.checked)
                onClicked: root.applyPreview()
            }
            BbToolButton { text: qsTr("取消"); onClicked: root.reject() }
        }
    }
}
