// SPDX-License-Identifier: GPL-3.0-only
// 元信息面板（左 Dock）：显示 + 编辑 Chart.meta 字段 + 「扩展代码」（原始控制行，格式兜底）。
// 2026-09 用户迭代：
// - 加大 DPI（fsBase + 8px 间距 + BbTextField/可编辑 ComboBox）；标题↔字段间距收窄；
// - **去掉独立「保存」**——元信息修改交「整个文件保存」（Ctrl+S 时先 meta.edit + meta.rawEdit 再保存）；
//   「重置」= 放弃面板改动、重载；
// - 常用字段（TITLE/ARTIST/GENRE/BPM 等）恒显；其余（BACKBMP/STAGEFILE…）收进「更多字段」折叠组；
// - **PLAYER / DIFFICULTY / RANK 用下拉（可编辑 ComboBox，允许手填）**，参照 iBMSC（只参考信息）；
// - **底部「扩展代码」大文本框**（对照 iBMSC 右下角）：显示 chart.raw_lines（#RANDOM/#IF/#SWITCH
//   块、未知控制指令；写回原样输出），供用户自行编辑，作格式兜底。
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

ColumnLayout {
    id: root
    property var meta: null
    property string chartPath: ""
    /// 状态提示（Main 置状态栏）
    signal metaMessage(string msg)

    /// 编辑字段状态（orig = 载入原始值；value = 当前编辑值；脏 = value!==orig）
    property var fields: []
    /// 原始控制行（扩展代码；\n 连接）
    property string rawText: ""
    /// 编辑 tick（文本变化 +1；「重置」使能 + 保存时判定是否有改动）
    property int dirtyTick: 0
    readonly property var primaryKeys: ["TITLE", "SUBTITLE", "ARTIST", "GENRE", "BPM",
        "PLAYER", "PLAYLEVEL", "RANK", "TOTAL", "DIFFICULTY"]
    property bool expandSecondary: false
    property bool expandRaw: false
    readonly property var primaryFields: root.fields.filter(f => root.isPrimary(f.key))
    readonly property var secondaryFields: root.fields.filter(f => !root.isPrimary(f.key))
    function isPrimary(k) { return root.primaryKeys.indexOf(k) >= 0 }
    /// 有下拉选项的字段（PLAYER/DIFFICULTY/RANK；可编辑 ComboBox，允许手填）
    function comboOptions(key) {
        if (key === "PLAYER") return ["1", "2", "3", "4"]
        if (key === "DIFFICULTY") return ["1", "2", "3", "4", "5", "6", "7", "8", "9", "10", "11", "12"]
        if (key === "RANK") return ["1", "2", "3"]
        return null
    }

    /// 载入元数据 + 扩展代码（Main 打开谱面后调用；无谱面则清空并提示）
    function reload() {
        if (!root.meta) { root.fields = []; root.rawText = ""; return }
        const r = JSON.parse(beatbench.dispatch(JSON.stringify({ command: "meta.list", args: {} })))
        if (!r.ok) { root.fields = []; root.metaMessage(r.error.code + ": " + r.error.message); return }
        const arr = []
        for (const k in r.result.meta)
            arr.push({ key: k, value: r.result.meta[k] || "", orig: r.result.meta[k] || "" })
        root.fields = arr
        const rr = JSON.parse(beatbench.dispatch(JSON.stringify({ command: "meta.raw", args: {} })))
        root.rawText = rr.ok ? rr.result.lines.join("\n") : ""
        root.dirtyTick = 0
    }
    /// 放弃修改：重新载入
    function reset() { root.reload() }
    /// 脏字段编辑集（保存用；value 空串=删除）
    function collectEdits() {
        const edits = []
        for (let i = 0; i < root.fields.length; i++) {
            const f = root.fields[i]
            if (f.value !== f.orig) edits.push({ key: f.key, value: f.value })
        }
        return edits
    }
    function rawEdits() {
        const lines = root.rawText.split("\n")
        // 去掉末尾空行（split 会把结尾 \n 分成一个空串）
        while (lines.length && lines[lines.length - 1] === "") lines.pop()
        return lines
    }
    /// 是否「扩展代码」有改动（与载入原值比较）
    function rawDirty() {
        const lines = root.rawEdits()
        const cur = root.fields.length ? root._rawOrig : null
        return cur !== null && cur.join("\n") !== lines.join("\n")
    }
    property var _rawOrig: []

    /// 保存扩展代码（Main 在文件保存前调用：先 meta.rawEdit 再 session.save）
    function applyRawEdits() {
        if (!root.meta) return false
        const lines = root.rawEdits()
        const cur = root._rawOrig
        if (cur !== null && cur.join("\n") === lines.join("\n")) return false
        const resp = beatbench.dispatch(JSON.stringify({ command: "meta.rawEdit", args: { lines: lines } }))
        const r = JSON.parse(resp)
        if (r.ok) { root._rawOrig = lines.slice(); root.expandRaw = false; return true }
        root.metaMessage(r.error.code + ": " + r.error.message)
        return false
    }

    spacing: 2

    RowLayout {
        spacing: 8
        Label { text: qsTr("元信息"); font.bold: true; color: Theme.text; font.pixelSize: Theme.fsBase }
        Item { Layout.fillWidth: true }
        BbToolButton {
            // 2026-09：元信息修改交整个文件保存，不再单独「保存」；「重置」= 放弃改动
            text: qsTr("重置")
            enabled: root.meta !== null && root.dirtyTick > 0
            onClicked: root.reset()
            font.pixelSize: Theme.fsSmall
            ToolTip.visible: hovered
            ToolTip.text: qsTr("放弃元信息面板改动（重新载入）")
        }
    }

    Label {
        visible: root.meta === null
        text: qsTr("打开谱面开始编辑（Ctrl+O）")
        color: Theme.textFaint
        font.pixelSize: Theme.fsSmall
    }

    ScrollView {
        Layout.fillWidth: true
        Layout.fillHeight: true
        clip: true
        visible: root.meta !== null
        ColumnLayout {
            width: parent.width
            spacing: 6
            Repeater {
                model: root.primaryFields
                delegate: RowLayout {
                    Layout.fillWidth: true
                    spacing: 6
                    Label {
                        text: modelData.key + ":"
                        color: Theme.textMuted
                        Layout.preferredWidth: 76
                        font.family: Theme.fontMono
                        font.pixelSize: Theme.fsBase
                    }
                    BbTextField {
                        visible: !root.comboOptions(modelData.key)
                        Layout.fillWidth: true
                        text: modelData.value
                        onTextChanged: { modelData.value = text; root.dirtyTick++ }
                    }
                    ComboBox {
                        visible: !!root.comboOptions(modelData.key)
                        Layout.fillWidth: true
                        model: root.comboOptions(modelData.key)
                        editable: true
                        font.pixelSize: Theme.fsBase
                        // currentIndex 绑定到字段值（无匹配 = -1 手填）；二者择一写回
                        currentIndex: root.comboOptions(modelData.key).indexOf(modelData.value)
                        onActivated: { modelData.value = currentText; root.dirtyTick++ }
                        onEditTextChanged: { if (modelData.value !== editText) { modelData.value = editText; root.dirtyTick++ } }
                    }
                }
            }
            BbToolButton {
                text: (root.expandSecondary ? "▾ " : "▸ ") +
                      qsTr("更多字段（%1）").arg(root.secondaryFields.length)
                flatStyle: true
                active: root.expandSecondary
                enabled: root.secondaryFields.length > 0
                onClicked: root.expandSecondary = !root.expandSecondary
                font.pixelSize: Theme.fsSmall
                Layout.fillWidth: true
            }
            ColumnLayout {
                visible: root.expandSecondary
                spacing: 6
                Repeater {
                    model: root.secondaryFields
                    delegate: RowLayout {
                        Layout.fillWidth: true
                        spacing: 6
                        Label {
                            text: modelData.key + ":"
                            color: Theme.textMuted
                            Layout.preferredWidth: 76
                            font.family: Theme.fontMono
                            font.pixelSize: Theme.fsBase
                        }
                        BbTextField {
                            visible: !root.comboOptions(modelData.key)
                            Layout.fillWidth: true
                            text: modelData.value
                            onTextChanged: { modelData.value = text; root.dirtyTick++ }
                        }
                        ComboBox {
                            visible: !!root.comboOptions(modelData.key)
                            Layout.fillWidth: true
                            model: root.comboOptions(modelData.key)
                            editable: true
                            font.pixelSize: Theme.fsBase
                            currentIndex: root.comboOptions(modelData.key).indexOf(modelData.value)
                            onActivated: { modelData.value = currentText; root.dirtyTick++ }
                            onEditTextChanged: { if (modelData.value !== editText) { modelData.value = editText; root.dirtyTick++ } }
                        }
                    }
                }
            }
            // 「扩展代码」原始控制行（格式兜底；对照 iBMSC 右下角）
            BbToolButton {
                text: (root.expandRaw ? "▾ " : "▸ ") + qsTr("扩展代码")
                flatStyle: true
                active: root.expandRaw
                onClicked: root.expandRaw = !root.expandRaw
                font.pixelSize: Theme.fsSmall
                Layout.fillWidth: true
                ToolTip.visible: hovered
                ToolTip.text: qsTr("未结构化表示的原始控制行（#RANDOM/#IF/未知指令等）；保存时写回，作格式兜底")
            }
            TextArea {
                id: rawArea
                visible: root.expandRaw
                Layout.fillWidth: true
                Layout.preferredHeight: 120
                text: root.rawText
                wrapMode: TextEdit.NoWrap
                font.family: Theme.fontMono
                font.pixelSize: Theme.fsSmall
                onTextChanged: { root.rawText = text; root.dirtyTick++ }
                placeholderText: qsTr("（无原始控制行…）")
                background: Rectangle {
                    radius: Theme.radiusSm
                    border.width: 1
                    border.color: rawArea.activeFocus ? Theme.primary : Theme.borderStrong
                    color: Theme.surface2
                }
            }
        }
    }
}
