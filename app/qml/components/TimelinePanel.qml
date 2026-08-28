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

    /// 时间轴事件（timing.list 结果；升序）。value 语义：bpm=数值；
    /// stop=原始计数 n（1/192 全音符单位，BMS #STOPxx 原义；毫秒仅是显示换算）。
    property var bpmEvents: []
    property var stopEvents: []
    /// BPM/STOP 定义表（session.samples 的 bpm/stop；[{id(文本), value, refs}]）。
    property var bpmDefs: []
    property var stopDefs: []
    readonly property var defs: kind === "bpm" ? bpmDefs : stopDefs
    /// 定义表折叠区是否展开。
    property bool expandDefs: false
    /// 编辑（添加/改值）→ Main 走 timing.put。pos 以 num/den 分开传（避免对象绑定歧义）。
    /// ref = 手动绑定的 #BPMxx/#STOPxx id 文本（空 = codec 自动派生）。
    signal timingEditRequested(string kind, int measure, int num, int den, double value, string ref)
    /// 删除 → Main 走 timing.delete。
    signal timingDeleteRequested(string kind, int measure, int num, int den)
    /// 添加/覆盖一个 BPM/STOP 定义（id → 值文本）→ Main 走 sample.setValue。
    signal timingDefAddRequested(string kind, string id, string value)
    /// 删除一个 BPM/STOP 定义 → Main 走 sample.delete(kind)。
    signal timingDefDeleteRequested(string kind, string id)

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

    // ---- #BPM/#STOP 定义表（折叠；镜像左 Dock BGA 面板：明确区分「创建 id+绑定值」与「时间轴使用 id」） ----
    Rectangle { Layout.fillWidth: true; height: 1; color: Theme.border }
    BbToolButton {
        text: (root.expandDefs ? "▾ " : "▸ ") + qsTr("#%1 定义（%2）")
              .arg(root.kind === "stop" ? "STOP" : "BPM").arg(root.defs.length)
        flatStyle: true
        active: root.expandDefs
        onClicked: root.expandDefs = !root.expandDefs
        font.pixelSize: Theme.fsSmall
        Layout.fillWidth: true
        ToolTip.visible: hovered
        ToolTip.text: qsTr("%1 定义表（#%2xx → 数值）；事件在时间轴引用这些 id")
                      .arg(root.kind === "stop" ? "STOP" : "BPM").arg(root.kind === "stop" ? "STOP" : "BPM")
    }
    ColumnLayout {
        visible: root.expandDefs
        Layout.fillWidth: true
        spacing: 4

        // 添加行：id + 值 + 添加按钮（空 id → 自动分配新 id）
        RowLayout {
            Layout.fillWidth: true
            spacing: 4
            BbTextField {
                id: addDefId
                Layout.fillWidth: true
                Layout.minimumWidth: 40   // 隐式宽撑宽 dock → 允许收缩
                placeholderText: qsTr("id（如 01/ZZ）")
                font.family: Theme.fontMono
                font.pixelSize: Theme.fsSmall
            }
            BbTextField {
                id: addDefValue
                Layout.fillWidth: true
                Layout.minimumWidth: 40
                placeholderText: root.kind === "stop" ? qsTr("计数") : qsTr("BPM 值")
                font.family: Theme.fontMono
                font.pixelSize: Theme.fsSmall
            }
            BbToolButton {
                text: qsTr("添加")
                Layout.preferredWidth: 40
                Layout.preferredHeight: 24
                onClicked: {
                    const id = addDefId.text.trim()
                    const value = addDefValue.text.trim()
                    if (id === "") { addDefId.forceActiveFocus(); return }
                    if (value === "") { addDefValue.forceActiveFocus(); return }
                    root.timingDefAddRequested(root.kind, id, value)
                    addDefId.text = ""; addDefValue.text = ""
                }
                ToolTip.visible: hovered
                ToolTip.text: qsTr("添加/覆盖一个 #%1 定义（显式填 id；「+」事件添加才是自动派生新 id）")
                              .arg(root.kind === "stop" ? "STOP" : "BPM")
            }
        }

        // 定义列表（id + 值 + 引用数 + 删除；双击行 → 复用事件对话框改值，绑定该 id）
        Item {
            Layout.fillWidth: true
            Layout.preferredHeight: Math.min(160, root.defs.length * 24 + 4)
            Label {
                visible: root.defs.length === 0
                anchors.top: parent.top
                anchors.left: parent.left
                anchors.right: parent.right
                text: qsTr("（暂无 #%1 定义；上方 id+值 添加）").arg(root.kind === "stop" ? "STOP" : "BPM")
                color: Theme.textFaint
                font.pixelSize: Theme.fsTiny
                wrapMode: Text.WordWrap
            }
            ListView {
                anchors.fill: parent
                clip: true
                model: root.defs
                spacing: 2
                ScrollBar.vertical: ScrollBar { policy: ScrollBar.AsNeeded }
                delegate: Rectangle {
                    id: defRow
                    required property var modelData
                    /// 双击 → 内联编辑定义值（Enter/失焦保存，Esc 取消；与 WAV/BMP 文件名编辑同语义）。
                    property bool editing: false
                    function commitEdit() {
                        if (!defRow.editing) return
                        defRow.editing = false
                        const v = defValueEdit.text.trim()
                        if (v !== "" && v !== defRow.modelData.value) {
                            // 乐观更新（行内立即显示新值；随后 refreshTiming 从会话重取）
                            defRow.modelData.value = v
                            root.timingDefAddRequested(root.kind, defRow.modelData.id, v)
                        }
                    }
                    width: ListView.view.width
                    height: 24
                    radius: Theme.radiusSm
                    color: defMouse.containsMouse ? Theme.surface3 : "transparent"
                    RowLayout {
                        anchors.fill: parent
                        anchors.leftMargin: 6
                        anchors.rightMargin: 6
                        spacing: 6
                        Label {
                            text: "#" + (root.kind === "stop" ? "STOP" : "BPM") + defRow.modelData.id
                            color: Theme.textMuted
                            font.family: Theme.fontMono
                            font.pixelSize: Theme.fsTiny
                        }
                        Label {
                            text: defRow.modelData.value
                            visible: !defRow.editing
                            color: root.kind === "stop" ? Theme.warning : Theme.accent
                            font.family: Theme.fontMono
                            font.pixelSize: Theme.fsSmall
                            elide: Text.ElideRight
                            Layout.fillWidth: true
                        }
                        TextField {
                            id: defValueEdit
                            visible: defRow.editing
                            Layout.fillWidth: true
                            text: defRow.modelData.value
                            selectByMouse: true
                            color: Theme.text
                            font.family: Theme.fontMono
                            font.pixelSize: Theme.fsSmall
                            background: Rectangle {
                                radius: Theme.radiusSm
                                border.width: 1
                                border.color: defValueEdit.activeFocus ? Theme.primary : Theme.borderStrong
                                color: Theme.surface2
                            }
                            onAccepted: defRow.commitEdit()
                            onActiveFocusChanged: if (!activeFocus && defRow.editing) defRow.commitEdit()
                            Keys.onEscapePressed: defRow.editing = false
                        }
                        Label {
                            text: defRow.modelData.refs > 0 ? "×" + defRow.modelData.refs : ""
                            color: Theme.textFaint
                            font.family: Theme.fontMono
                            font.pixelSize: Theme.fsTiny
                        }
                        BbToolButton {
                            text: "×"
                            Layout.preferredWidth: 20
                            Layout.preferredHeight: 18
                            flatStyle: true
                            onClicked: root.timingDefDeleteRequested(root.kind, defRow.modelData.id)
                            ToolTip.visible: hovered
                            ToolTip.text: qsTr("删除该 #%1 定义（引用保留 id，写回自动派生；Undo 可恢复）")
                                          .arg(root.kind === "stop" ? "STOP" : "BPM")
                        }
                    }
                    MouseArea {
                        id: defMouse
                        z: -1
                        anchors.fill: parent
                        hoverEnabled: true
                        // 双击定义行 → 内联编辑该定义的值（改定义值 = 所有引用者交叉同步）
                        onDoubleClicked: {
                            if (!defRow.editing) {
                                defRow.editing = true
                                defValueEdit.forceActiveFocus()
                            }
                        }
                    }
                }
            }
        }
    }

    // ---- 添加 / 编辑对话框 ----
    Dialog {
        id: dialog
        modal: true
        // 2026-09 用户：「无论 dock 什么宽度都可以显示」——右对齐到**窗口右缘**
        //（模态 Popup 的 parent = Overlay = 窗口内容），不再受右 dock 宽度约束；
        // 宽度封顶 260 避免占满窗口。纵向居中。
        width: 260
        x: (parent !== null ? parent.width - width - 12 : 0)
        y: (parent !== null ? Math.max(8, (parent.height - height) / 2) : 0)
        title: qsTr("时间轴事件")
        standardButtons: Dialog.Ok | Dialog.Cancel

        property string kind: "bpm"
        property bool editing: false
        property int baseMeasure: 0
        property int baseNum: 0
        property int baseDen: 1
        property double baseValue: 0
        property string baseRef: ""  // 当前绑定的 #BPMxx/#STOPxx id

        function openForAdd(k) {
            kind = k
            editing = false
            baseMeasure = 0
            baseNum = 0
            baseDen = 1
            baseValue = (k === "bpm") ? 130 : stopFromDisplay("96")  // STOP 默认 96
            baseRef = ""
            applyFields()
            open()
        }
        /// 事件编辑：改「小节的这个位置放多少值」+ 可选修改绑定的 id。
        function openForEdit(k, measure, num, den, value, ref) {
            kind = k
            editing = true
            baseMeasure = measure
            baseNum = num
            baseDen = den
            baseValue = value
            baseRef = ref || ""
            applyFields()
            open()
        }
        function applyFields() {
            measureSpin.value = baseMeasure
            numSpin.value = baseNum
            denSpin.value = Math.max(1, baseDen)
            // 编辑模式：值字段留空（不填 = 维持原值）；添加模式：填默认值
            valueField.text = editing ? "" : (dialog.kind === "bpm" ? String(baseValue)
                                                                    : stopToDisplay(baseValue))
            refField.text = baseRef
        }

        onOpened: valueField.forceActiveFocus()

        ColumnLayout {
            anchors.fill: parent
            spacing: 4
            // 小节 2 : 0 / 1（冒号分隔；紧凑一行，默认 dock 宽可容纳）
            RowLayout {
                spacing: 4
                Layout.fillWidth: true
                Label { text: qsTr("小节"); color: Theme.textMuted; font.pixelSize: Theme.fsTiny }
                BbSpinBox {
                    id: measureSpin
                    from: 0; to: 999; editable: true
                    Layout.preferredWidth: 64
                    Layout.minimumWidth: 52   // 覆写 implicitWidth 72 → 窄 dock 不越界
                }
                Label { text: ":"; color: Theme.textMuted; font.pixelSize: Theme.fsTiny }
                BbSpinBox { id: numSpin; from: 0; to: 999; editable: true
                            Layout.preferredWidth: 48; Layout.minimumWidth: 36 }
                Label { text: "/"; color: Theme.textMuted; font.pixelSize: Theme.fsTiny }
                BbSpinBox { id: denSpin; from: 1; to: 999; editable: true
                            Layout.preferredWidth: 48; Layout.minimumWidth: 36 }
                Item { Layout.fillWidth: true }
            }
            RowLayout {
                spacing: 4
                Layout.fillWidth: true
                Label {
                    text: dialog.kind === "bpm" ? qsTr("值(BPM)") : qsTr("值(%1)").arg(stopUnitLabel())
                    color: Theme.textMuted; font.pixelSize: Theme.fsTiny
                }
                BbTextField {
                    id: valueField
                    Layout.fillWidth: true
                    Layout.minimumWidth: 70   // 覆写隐式宽 → 值输入框不越界
                    Layout.maximumWidth: 150  // 限制「值(BPM)」列长度（2026-09 用户）
                    placeholderText: dialog.editing
                                     ? qsTr("不填=维持 %1").arg(dialog.baseValue)
                                     : (dialog.kind === "bpm" ? qsTr("130") : qsTr("96"))
                    validator: DoubleValidator { bottom: 0; top: 999999 }
                    onAccepted: refField.forceActiveFocus()
                    escapeHandler: function() { dialog.reject() }
                }
                Item { Layout.fillWidth: true }
            }
            // 绑定的 #BPMxx/#STOPxx id（可手动修改）
            RowLayout {
                spacing: 4
                Layout.fillWidth: true
                Label {
                    text: "#" + (dialog.kind === "bpm" ? "BPM" : "STOP") + " id"
                    color: Theme.textMuted; font.pixelSize: Theme.fsTiny
                }
                BbTextField {
                    id: refField
                    Layout.fillWidth: true
                    Layout.minimumWidth: 40
                    placeholderText: qsTr("留空=自动")
                    font.family: Theme.fontMono
                    font.pixelSize: Theme.fsSmall
                    onAccepted: dialog.accept()
                    escapeHandler: function() { dialog.reject() }
                }
                Item { Layout.fillWidth: true }
            }
            Label {
                text: dialog.kind === "bpm"
                      ? (refField.text.trim() === "" ? qsTr("值自动派生 #BPMxx") : qsTr("绑定 #%1xx").arg("BPM"))
                      : (refField.text.trim() === ""
                         ? (stopUnit === 0 ? qsTr("值自动派生 #STOPxx") : qsTr("值自动派生 #STOPxx"))
                         : qsTr("绑定 #%1xx").arg("STOP"))
                color: Theme.textFaint
                font.pixelSize: Theme.fsTiny
            }
        }
        onAccepted: {
            const valueText = valueField.text.trim()
            const ref = refField.text.trim()
            let value = baseValue
            // 值字段为空 = 根据新 id 决定值
            if (valueText === "") {
                if (ref !== "" && ref !== baseRef) {
                    // 改绑到新 id：查找新 id 的原值，保持不变
                    const newDef = root.defs.find(d => d.id === ref)
                    if (newDef) {
                        // 新 id 已有定义：保持其原值
                        value = root.kind === "bpm" ? parseFloat(newDef.value)
                                                    : root.stopFromDisplay(newDef.value)
                    }
                    // 新 id 无定义：保持原值（baseValue）
                }
                // ref 未变或为空：保持原值
            } else {
                // 用户填了值：用新值
                value = root.kind === "bpm" ? parseFloat(valueText)
                                            : root.stopFromDisplay(valueText)
            }
            if (!isFinite(value)) value = baseValue
            root.timingEditRequested(dialog.kind, measureSpin.value,
                                     numSpin.value, denSpin.value, value, ref)
        }
    }

    // STOP 单位显示名（对话框 label 用）
    function stopUnitLabel() {
        return stopUnit === 0 ? qsTr("unit") : qsTr("ms")
    }
}
