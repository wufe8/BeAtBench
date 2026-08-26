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
    /// ref = 手动绑定的 #BPMxx/#STOPxx id 文本（空 = codec 自动派生）。
    signal timingEditRequested(string kind, int measure, int num, int den, double value, string ref)
    /// 删除 → Main 走 timing.delete。
    signal timingDeleteRequested(string kind, int measure, int num, int den)

    property string kind: "bpm"   // bpm / stop
    readonly property var events: kind === "bpm" ? bpmEvents : stopEvents
    /// STOP 值显示/填入单位：0 = 1/192 全音符（BMS 默认）；1 = 毫秒。值与 Main.window.stopUnit 同步。
    property int stopUnit: 0
    /// 毫秒换算参考 BPM（时间轴事件所属小节生效的 BPM；Main 由 TimingEngine 提供）。
    /// STOP 秒 = n×1.25/bpm，毫秒 = n×1250/bpm。缺省 130。
    property real stopBpm: 130
    /// STOP 计数 n → 选中单位文本。毫秒需换算 BPM（秒 = n×1.25/bpm → ms = n×1250/bpm）。
    function stopToDisplay(v) {
        if (stopUnit === 0) return String(Math.round(v))
        const bpm = (stopBpm > 0) ? stopBpm : 130
        return String(Math.round(v * 1250 / bpm))
    }
    /// 选中单位文本 → 计数 n。
    function stopFromDisplay(t) {
        const val = parseFloat(t)
        if (!isFinite(val)) return NaN
        return stopUnit === 0 ? val : val * (stopBpm > 0 ? stopBpm : 130) / 1250
    }

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
                              : root.stopToDisplay(row.modelData.value)
                        color: root.kind === "bpm" ? Theme.accent : Theme.warning
                        font.family: Theme.fontMono
                        font.pixelSize: Theme.fsSmall
                    }
                    Label {
                        // 绑定 id：#BPMxx/#STOPxx（空 = codec 自动派生 id）
                        text: row.modelData.ref && row.modelData.ref !== ""
                              ? (root.kind === "bpm" ? "#BPM" : "#STOP") + row.modelData.ref
                              : qsTr("(auto)")
                        color: row.modelData.ref && row.modelData.ref !== "" ? Theme.textMuted : Theme.textFaint
                        font.family: Theme.fontMono
                        font.pixelSize: Theme.fsTiny
                        visible: root.kind !== "measure"
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
                    // ⚠️ 必须放在行内容（含「×」删除按钮）之下：本 MouseArea 声明在 RowLayout
                    // 之后，若默认 z，它会盖住行内容 → 左侧「×」点击被它拦截（其 onClicked 只处理
                    // 右键删除，左键点了无反应——用户反馈）。置 z:-1 让按钮在顶层接收点击，
                    // 而本 MouseArea 仍能收到行体（空白/标签处）的悬停、双击编辑与右键删除。
                    z: -1
                    anchors.fill: parent
                    hoverEnabled: true
                    // 右键删除 / 双击编辑值
                    onDoubleClicked: dialog.openForEdit(
                                         root.kind, row.modelData.measure,
                                         row.modelData.pos.num, row.modelData.pos.den,
                                         row.modelData.value, row.modelData.ref || "")
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
        // 以本面板（右 Dock 内容区）为视口中心。宽度收窄到面板内（避免超出 dock→「出界」，
        // 用户报告原 260 宽在 230 dock 内溢出）；anchors.centerIn 让其在面板内居中并随
        // dock 缩放重算。
        width: Math.min(root.width - 4, 260)
        anchors.centerIn: parent
        title: qsTr("时间轴事件")
        standardButtons: Dialog.Ok | Dialog.Cancel

        property string kind: "bpm"
        property bool editing: false
        property int baseMeasure: 0
        property int baseNum: 0
        property int baseDen: 1
        property double baseValue: 0
        property string baseRef: ""   // 手动绑定 #BPMxx/#STOPxx id（空 = auto 派生）

        function openForAdd(k) {
            kind = k
            editing = false
            baseMeasure = 0
            baseNum = 0
            baseDen = 1
            baseValue = (k === "bpm") ? 130 : stopFromDisplay("96")  // STOP 默认 96（1/192 全音符 ×96 = 半全音符）
            baseRef = ""
            applyFields()
            open()
        }
        function openForEdit(k, measure, num, den, value, ref) {
            kind = k
            editing = true
            baseMeasure = measure
            baseNum = num
            baseDen = den
            baseValue = value
            baseRef = (ref && ref !== "") ? ref : ""
            applyFields()
            open()
        }
        function applyFields() {
            measureSpin.value = baseMeasure
            numSpin.value = baseNum
            denSpin.value = Math.max(1, baseDen)
            valueField.text = dialog.kind === "bpm" ? String(baseValue)
                                                    : stopToDisplay(baseValue)
            refField.text = baseRef
            refRow.visible = dialog.kind !== "measure"
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
                    text: dialog.kind === "bpm" ? qsTr("值(BPM)") : qsTr("值(%1)").arg(stopUnitLabel())
                    color: Theme.textMuted; font.pixelSize: Theme.fsTiny
                }
                BbTextField {
                    id: valueField
                    Layout.fillWidth: true
                    placeholderText: dialog.kind === "bpm" ? qsTr("如 130") : qsTr("如 96")
                    validator: DoubleValidator { bottom: 0; top: 999999 }
                    onAccepted: dialog.accept()
                    // 一次 Esc 即关对话框（否则 BbTextField 释放焦点 → 需再按一次才到 Dialog）
                    escapeHandler: function() { dialog.reject() }
                }
            }
            Label {
                text: dialog.kind === "bpm"
                      ? qsTr("· BPM 数值")
                      : (stopUnit === 0 ? qsTr("· 单位 = 1/192 全音符（随该拍位 BPM 换算）") : qsTr("· 毫秒（按当前 BPM 换算）"))
                color: Theme.textFaint
                font.pixelSize: Theme.fsTiny
            }
            RowLayout {
                id: refRow
                spacing: 6
                Label {
                    text: dialog.kind === "bpm" ? qsTr("id") : qsTr("id")
                    color: Theme.textMuted; font.pixelSize: Theme.fsTiny
                }
                BbTextField {
                    id: refField
                    Layout.fillWidth: true
                    placeholderText: dialog.kind === "bpm" ? qsTr("#BPMxx（空=auto）") : qsTr("#STOPxx（空=auto）")
                    font.family: Theme.fontMono
                    font.pixelSize: Theme.fsTiny
                    onAccepted: dialog.accept()
                    escapeHandler: function() { dialog.reject() }
                }
            }
            Label {
                text: qsTr("· 绑定 id（#BPMxx/#STOPxx 引用；留空 = 写回时自动分配 id）")
                color: Theme.textFaint
                font.pixelSize: Theme.fsTiny
            }
        }
        onAccepted: {
            const value = dialog.kind === "bpm"
                          ? parseFloat(valueField.text)
                          : stopFromDisplay(valueField.text)
            const ref = refField.text.trim()
            if (!isFinite(value)) { root.timingEditRequested(dialog.kind, measureSpin.value, numSpin.value, denSpin.value, 0, ref); return }
            root.timingEditRequested(dialog.kind, measureSpin.value,
                                     numSpin.value, denSpin.value, value, ref)
        }
    }

    // STOP 单位显示名（对话框 label 用）
    function stopUnitLabel() {
        return stopUnit === 0 ? qsTr("unit") : qsTr("ms")
    }
}
