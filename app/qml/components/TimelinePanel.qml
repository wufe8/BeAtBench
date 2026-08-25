// SPDX-License-Identifier: GPL-3.0-only
// 时间轴管理面板（右 Dock「时间轴」标签页，M2/M3 边界）：BPM / STOP 事件列表。
// 数据源 = Main.timingBpm/timingStop（timing.list 结果：[{measure, pos:{num,den}, value}]，升序）。
// 编辑经信号 → Main 包 timing.put/delete（一个 undo 步），成功后 Main 重取 timing.list 回填。
// 双语言纪律（doc/08 §2）：本组件纯展示 + 事件分发，不直接 dispatch。
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQml
import "../components"

ColumnLayout {
    id: root

    /// 时间轴事件（timing.list 结果；升序）。value 语义：bpm=数值，stop=微秒。
    property var bpmEvents: []
    property var stopEvents: []
    /// 编辑（添加/改值）→ Main 走 timing.put。pos 以 num/den 分开传（避免对象绑定歧义）。
    signal timingEditRequested(string kind, int measure, int num, int den, double value)
    /// 删除 → Main 走 timing.delete。
    signal timingDeleteRequested(string kind, int measure, int num, int den)

    property string kind: "bpm"   // bpm / stop
    readonly property var events: kind === "bpm" ? bpmEvents : stopEvents

    spacing: 6

    // ---- 顶栏：BPM/STOP 切换 + 添加按钮 ----
    RowLayout {
        Layout.fillWidth: true
        spacing: 6
        BbTabStrip {
            id: kindTabs
            Layout.fillWidth: true
            Layout.preferredHeight: 28
            model: [qsTr("BPM"), qsTr("STOP")]
            currentIndex: root.kind === "stop" ? 1 : 0
            onIndexRequested: (i) => root.kind = i === 0 ? "bpm" : "stop"
        }
        BbToolButton {
            text: qsTr("+")
            Layout.preferredWidth: 30
            Layout.preferredHeight: 28
            onClicked: dialog.openForAdd(root.kind)
            ToolTip.visible: hovered
            ToolTip.text: qsTr("在该位置添加一个 %1 事件").arg(root.kind.toUpperCase())
        }
    }

    // ---- 事件列表 ----
    Item {
        Layout.fillWidth: true
        Layout.fillHeight: true

        // 空态
        Label {
            visible: root.events.length === 0
            anchors.top: parent.top
            anchors.left: parent.left
            anchors.right: parent.right
            text: root.kind === "bpm"
                  ? qsTr("无 BPM 变化事件（初始 BPM 在元信息）")
                  : qsTr("无 STOP 事件")
            color: Theme.textFaint
            font.pixelSize: Theme.fsTiny
        }

        ListView {
            anchors.fill: parent
            clip: true
            model: root.events
            spacing: 2
            ScrollBar.vertical: ScrollBar { policy: ScrollBar.AsNeeded }
            // event = {measure, pos:{num,den}, value}
            delegate: Rectangle {
                id: row
                required property var modelData
                width: ListView.view.width
                height: 26
                radius: Theme.radiusSm
                color: rowMouse.containsMouse ? Theme.surface3 : "transparent"
                RowLayout {
                    anchors.fill: parent
                    anchors.leftMargin: 6
                    anchors.rightMargin: 6
                    spacing: 6
                    Label {
                        text: qsTr("小节 %1").arg(row.modelData.measure) +
                              qsTr(" · %1/%2").arg(row.modelData.pos.num).arg(row.modelData.pos.den)
                        color: Theme.textMuted
                        font.family: Theme.fontMono
                        font.pixelSize: Theme.fsTiny
                        elide: Text.ElideRight
                    }
                    Item { Layout.fillWidth: true }
                    Label {
                        text: root.kind === "bpm"
                              ? qsTr("%1").arg(row.modelData.value)
                              : valueText(row.modelData.value)
                        color: root.kind === "bpm" ? Theme.accent : Theme.warning
                        font.family: Theme.fontMono
                        font.pixelSize: Theme.fsSmall
                    }
                    BbToolButton {
                        text: "×"
                        Layout.preferredWidth: 22
                        Layout.preferredHeight: 20
                        flatStyle: true
                        onClicked: root.timingDeleteRequested(
                                       root.kind, row.modelData.measure,
                                       row.modelData.pos.num, row.modelData.pos.den)
                        ToolTip.visible: hovered
                        ToolTip.text: qsTr("删除该 %1 事件").arg(root.kind.toUpperCase())
                    }
                }
                MouseArea {
                    id: rowMouse
                    anchors.fill: parent
                    hoverEnabled: true
                    // 右键删除 / 双击编辑值
                    onDoubleClicked: dialog.openForEdit(
                                         root.kind, row.modelData.measure,
                                         row.modelData.pos.num, row.modelData.pos.den,
                                         row.modelData.value)
                    acceptedButtons: Qt.LeftButton | Qt.RightButton
                    onClicked: (mouse) => {
                        if (mouse.button === Qt.RightButton)
                            root.timingDeleteRequested(
                                root.kind, row.modelData.measure,
                                row.modelData.pos.num, row.modelData.pos.den)
                    }
                }
            }
        }
    }

    // ---- 添加 / 编辑对话框 ----
    Dialog {
        id: dialog
        modal: true
        anchors.centerIn: parent
        width: 260
        title: qsTr("时间轴事件")
        standardButtons: Dialog.Ok | Dialog.Cancel

        property string kind: "bpm"
        property bool editing: false
        property int baseMeasure: 0
        property int baseNum: 0
        property int baseDen: 1
        property double baseValue: 0

        function openForAdd(k) {
            kind = k
            editing = false
            baseMeasure = 0
            baseNum = 0
            baseDen = 1
            baseValue = (k === "bpm") ? 130 : 5000
            applyFields()
            open()
        }
        function openForEdit(k, measure, num, den, value) {
            kind = k
            editing = true
            baseMeasure = measure
            baseNum = num
            baseDen = den
            baseValue = value
            applyFields()
            open()
        }
        function applyFields() {
            measureSpin.value = baseMeasure
            numSpin.value = baseNum
            denSpin.value = Math.max(1, baseDen)
            valueField.text = String(baseValue)
        }

        onOpened: valueField.forceActiveFocus()

        ColumnLayout {
            anchors.fill: parent
            spacing: 6
            RowLayout {
                spacing: 6
                Label { text: qsTr("小节"); color: Theme.textMuted; font.pixelSize: Theme.fsTiny }
                BbSpinBox {
                    id: measureSpin
                    from: 0; to: 999; editable: true
                    Layout.fillWidth: true
                }
            }
            RowLayout {
                spacing: 6
                Label { text: qsTr("位置"); color: Theme.textMuted; font.pixelSize: Theme.fsTiny }
                BbSpinBox { id: numSpin; from: 0; to: 999; editable: true; Layout.fillWidth: true }
                Label { text: "/"; color: Theme.textMuted; font.pixelSize: Theme.fsTiny }
                BbSpinBox { id: denSpin; from: 1; to: 999; editable: true; Layout.fillWidth: true }
            }
            RowLayout {
                spacing: 6
                Label {
                    text: dialog.kind === "bpm" ? qsTr("值(BPM)") : qsTr("值(μs)")
                    color: Theme.textMuted; font.pixelSize: Theme.fsTiny
                }
                BbTextField {
                    id: valueField
                    Layout.fillWidth: true
                    placeholderText: dialog.kind === "bpm" ? qsTr("如 130") : qsTr("如 5000")
                    validator: DoubleValidator { bottom: 0; top: 999999 }
                    onAccepted: dialog.accept()
                }
            }
            Label {
                text: dialog.kind === "bpm"
                      ? qsTr("· BPM 数值")
                      : qsTr("· 微秒（1000 μs = 1 ms）")
                color: Theme.textFaint
                font.pixelSize: Theme.fsTiny
            }
        }
        onAccepted: {
            const value = parseFloat(valueField.text)
            if (!isFinite(value)) { root.timingEditRequested(dialog.kind, measureSpin.value, numSpin.value, denSpin.value, 0); return }
            root.timingEditRequested(dialog.kind, measureSpin.value,
                                     numSpin.value, denSpin.value, value)
        }
    }

    // STOP 微秒 → 可读文本（≥1000μs → ms；≥1000000μs → s）
    function valueText(v) {
        if (v >= 1000000) return (v / 1e6).toFixed(3) + "s"
        return (v / 1000).toFixed(1) + "ms"
    }
}
