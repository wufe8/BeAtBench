// SPDX-License-Identifier: GPL-3.0-only
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

RowLayout {
    id: root
    objectName: "projectInlineWavBinding"
    required property var controller
    required property string projectRoot
    required property var targetPaths
    property bool editingAllowed: true
    property var pending: ({})
    property string statusText: ""
    property alias wavId: wavIdField.text
    property alias fileName: fileField.text
    signal statusChanged(string message)
    spacing: 4

    function invalidate() {
        if (pending.planId && controller) controller.discardPlan()
        pending = ({})
        statusText = ""
    }
    function updateBinding() {
        if (!editingAllowed) {
            statusChanged(qsTr("请先提交或取消面板草稿，再更新 WAV 定义"))
            return
        }
        if (!pending.planId) {
            pending = controller.prepareWavBinding(wavIdField.text.trim(), fileField.text.trim(), projectRoot, targetPaths)
            if (!pending.ok) { statusChanged(pending.error); pending = ({}); return }
            if (pending.collisionCount > 0) {
                statusText = qsTr("将覆盖 %1 个已定义 WAV ID；现有音符将使用新资源。再次点击“确认覆盖”应用，或修改输入取消。").arg(pending.collisionCount)
                statusChanged(statusText)
                return
            }
        }
        const result = controller.commitBindings(pending.planId, pending.collisionCount > 0)
        pending = ({})
        statusText = ""
        statusChanged(result.ok ? qsTr("WAV 定义已更新，可逐谱撤销；尚未保存。") : result.error)
    }
    onProjectRootChanged: invalidate()
    onTargetPathsChanged: invalidate()
    onEditingAllowedChanged: { if (!editingAllowed) invalidate() }

    BbTextField {
        id: wavIdField
        objectName: "projectWavIdInput"
        Layout.preferredWidth: 58
        Layout.minimumWidth: 44
        placeholderText: qsTr("WAV ID")
        maximumLength: 2
        font.family: Theme.fontMono
        onTextChanged: root.invalidate()
    }
    BbTextField {
        id: fileField
        objectName: "projectWavFileInput"
        Layout.fillWidth: true
        Layout.minimumWidth: 40
        placeholderText: qsTr("文件名（相对项目根）")
        font.family: Theme.fontMono
        onTextChanged: root.invalidate()
        onAccepted: root.updateBinding()
    }
    BbToolButton {
        objectName: "projectWavUpdateButton"
        text: root.pending.planId ? qsTr("确认覆盖") : qsTr("更新 WAV")
        enabled: root.targetPaths.length > 0 && wavIdField.text.trim() !== "" && fileField.text.trim() !== ""
        onClicked: root.updateBinding()
        ToolTip.visible: hovered
        ToolTip.text: qsTr("将这一条 WAV 定义应用到项目页勾选谱面；已有定义须确认覆盖，不自动保存。")
    }
}
