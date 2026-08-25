// SPDX-License-Identifier: GPL-3.0-only
// 元信息面板（左 Dock）：显示 + 编辑 Chart.meta 字段 + 「扩展代码」（原始控制行，格式兜底）。
// 2026-09 用户迭代：
// - 加大 DPI（fsBase + 8px 间距 + BbTextField/可编辑 ComboBox）；标题↔字段间距收窄；
// - **「保存」按钮 = 只保存元信息**（把面板改动应用到内存会话，写文件仍走整体保存，
//   Ctrl+S / 另存为时一并落盘）；「重置」= 放弃面板改动、重载；
// - 字段**排序**：歌曲信息（TITLE/SUBTITLE/ARTIST/GENRE）→ 谱面信息（BPM/PLAYER/…）→
//   次要字段收进「更多字段」折叠组（原先是源文件顺序，无序）；
// - **PLAYER / DIFFICULTY / RANK 用可编辑下拉（ComboBox，允许手填）**；选项显示注释式标签
//   「3 - Easy」（参照 iBMSC），**实际写入仍是原始值 "3"**（label/value 分离）；
// - **下拉弹出主题化**（暗底 + Theme.text，替掉系统默认浅色黑字）+ 自定义 V 形箭头；
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
    /// 「保存」按钮 → Main 只保存元信息（应用 meta.edit + meta.rawEdit，不写文件）
    signal saveRequested()

    /// 编辑字段状态（orig = 载入原始值；value = 当前编辑值；脏 = value!==orig）
    property var fields: []
    /// 原始控制行（扩展代码；\n 连接）
    property string rawText: ""
    /// 编辑 tick（文本变化 +1；「重置/保存」使能 + 保存时判定是否有改动）
    property int dirtyTick: 0
    /// 字段排序（歌曲信息 → 谱面信息；其余次要字段收进折叠组，按名字母序）。
    readonly property var primaryKeys: ["TITLE", "SUBTITLE", "ARTIST", "GENRE",
        "BPM", "PLAYER", "PLAYLEVEL", "RANK", "TOTAL", "DIFFICULTY"]
    property bool expandSecondary: false
    property bool expandRaw: false
    readonly property var primaryFields: root.fields.filter(f => root.isPrimary(f.key))
    readonly property var secondaryFields: root.fields.filter(f => !root.isPrimary(f.key))
    function isPrimary(k) { return root.primaryKeys.indexOf(k) >= 0 }

    /// 有下拉字段的基础选项（label=显示注释，value=实际写回值；参照 iBMSC）。
    /// DIFFICULTY 注释 1-5（Beginner..Insane），6-12 只显数字（扩展谱面常见）；
    /// RANK 注释 1-5（Beginner/Normal/Easy/Hard/Very Hard）；PLAYER 注释 1-4。
    function _comboBase(key) {
        if (key === "PLAYER") return [
            { label: "1 - Single Play", value: "1" },
            { label: "2 - Double Play", value: "2" },
            { label: "3 - Couple Play", value: "3" },
            { label: "4 - Battle", value: "4" }
        ]
        if (key === "DIFFICULTY") {
            const names = ["", "Beginner", "Normal", "Hyper", "Another", "Insane"]
            const arr = []
            for (let i = 1; i <= 12; i++)
                arr.push({ label: i <= 5 ? (i + " - " + names[i]) : String(i), value: String(i) })
            return arr
        }
        if (key === "RANK") {
            const names = ["", "Beginner", "Normal", "Easy", "Hard", "Very Hard"]
            const arr = []
            for (let i = 1; i <= 5; i++)
                arr.push({ label: i + " - " + names[i], value: String(i) })
            return arr
        }
        return null
    }
    /// 有下拉选项的字段（PLAYER/DIFFICULTY/RANK）。返回 [{label,value}]；
    /// 若当前值不在基础选项里，追加为一项（保证 reload 后非标准值也能显示原始值）。
    function comboOptions(key, val) {
        let arr = root._comboBase(key)
        if (!arr) return null
        const v = String(val || "")
        if (v !== "") {
            const has = arr.some(function (it) { return it.value === v })
            if (!has) arr = arr.concat([{ label: v, value: v }])
        }
        return arr
    }
    /// 当前字段值在选项中的下标（无匹配 = -1 手填）。
    /// 用 comboOptions（含追加的当前值），保证 reload 后非标准值也能选中显示。
    function comboIndexOf(key, val) {
        const arr = root.comboOptions(key, val)
        if (!arr) return -1
        for (let i = 0; i < arr.length; i++)
            if (arr[i].value === val) return i
        return -1
    }
    /// 手填文本 → 写入原始值：命中某个注释标签则取该标签的 value，否则用文本本身。
    function resolveComboValue(key, text) {
        const arr = root._comboBase(key)
        if (!arr) return text
        for (let i = 0; i < arr.length; i++)
            if (arr[i].label === text) return arr[i].value
        return text
    }
    /// 字段排序比较器：主字段按 primaryKeys 序（歌曲→谱面），次要字段按 key 字母序。
    function sortMeta(a, b) {
        const oa = root.primaryKeys.indexOf(a.key)
        const ob = root.primaryKeys.indexOf(b.key)
        if (oa < 0 && ob < 0) return a.key.localeCompare(b.key)
        if (oa < 0) return 1
        if (ob < 0) return -1
        return oa - ob
    }

    /// 载入元数据 + 扩展代码（Main 打开谱面后调用；无谱面则清空并提示）
    function reload() {
        if (!root.meta) { root.fields = []; root.rawText = ""; return }
        const r = JSON.parse(beatbench.dispatch(JSON.stringify({ command: "meta.list", args: {} })))
        if (!r.ok) { root.fields = []; root.metaMessage(r.error.code + ": " + r.error.message); return }
        const arr = []
        for (const k in r.result.meta)
            arr.push({ key: k, value: r.result.meta[k] || "", orig: r.result.meta[k] || "" })
        arr.sort(root.sortMeta)
        root.fields = arr
        const rr = JSON.parse(beatbench.dispatch(JSON.stringify({ command: "meta.raw", args: {} })))
        root.rawText = rr.ok ? rr.result.lines.join("\n") : ""
        root.dirtyTick = 0
    }
    /// 放弃修改：重新载入
    function reset() { root.reload() }
    /// 保存后提交当前值为基线（orig=value；raw 基线由 applyRawEdits 更新）；脏清零。
    function commit() {
        for (let i = 0; i < root.fields.length; i++)
            root.fields[i].orig = root.fields[i].value
        root.dirtyTick = 0
    }
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
        const cur = root._rawOrig
        return cur !== null && cur.join("\n") !== lines.join("\n")
    }
    property var _rawOrig: []

    /// 保存扩展代码（Main 在保存前调用：先 meta.rawEdit 再 session.save）
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
            text: qsTr("保存")
            enabled: root.meta !== null && root.dirtyTick > 0
            onClicked: root.saveRequested()
            font.pixelSize: Theme.fsSmall
            ToolTip.visible: hovered
            ToolTip.text: qsTr("只应用元信息改动（含扩展代码）到当前编辑会话；不写文件，整体保存时落盘")
        }
        BbToolButton {
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
                        Layout.preferredWidth: 72
                        font.family: Theme.fontMono
                        font.pixelSize: Theme.fsBase
                    }
                    BbTextField {
                        visible: !root.comboOptions(modelData.key, modelData.value)
                        Layout.fillWidth: true
                        text: modelData.value
                        onTextChanged: { modelData.value = text; root.dirtyTick++ }
                    }
                    ComboBox {
                        id: combo
                        visible: !!root.comboOptions(modelData.key, modelData.value)
                        model: root.comboOptions(modelData.key, modelData.value)
                        textRole: "label"
                        valueRole: "value"
                        // 非可编辑：contentItem 用 Label（点击整块/箭头即开下拉，不再与文本框
                        // 重叠抢点击）。下拉始终含当前值（comboOptions 追补非标准值）。
                        contentItem: Label {
                            text: combo.displayText
                            color: Theme.text
                            font: combo.font
                            verticalAlignment: Text.AlignVCenter
                            elide: Text.ElideRight   // 文字随可用宽度截断省略，箭头始终有空间
                            leftPadding: 8
                            rightPadding: 22   // 预留右侧箭头区，点击区清楚易点
                        }
                        Layout.fillWidth: true
                        font.pixelSize: Theme.fsBase
                        font.family: Theme.fontSans
                        // currentIndex 不用绑定：QML 绑定对 delegate 的 JS 字段写（onActivated/输入）
                        // 不触发重算，且内联编辑会断绑定 → 重置后显示不更新。
                        // 改为显式管理：字段变化（重置/载入，modelData 上下文属性重绑）→ source 重绑
                        // → onSourceChanged 重算 currentIndex；点选/输入 → 各 handler 直接更新。
                        property var source: modelData
                        onSourceChanged: combo.syncFromField()
                        Component.onCompleted: combo.syncFromField()
                        function syncFromField() {
                            currentIndex = root.comboIndexOf(source.key, source.value)
                        }
                        onCurrentIndexChanged: {
                            const idx = currentIndex
                            if (idx >= 0) {
                                const v = root.comboOptions(source.key, source.value)[idx].value
                                if (source.value !== v) { source.value = v; root.dirtyTick++ }
                            }
                        }
                        indicator: Item {
                            anchors.right: parent.right
                            anchors.rightMargin: 8
                            anchors.verticalCenter: parent.verticalCenter
                            width: 14; height: 8
                            Canvas {
                                anchors.fill: parent
                                onPaint: {
                                    const ctx = getContext("2d")
                                    ctx.reset()
                                    ctx.fillStyle = Theme.textMuted
                                    ctx.beginPath()
                                    ctx.moveTo(0, 0); ctx.lineTo(7, 8); ctx.lineTo(14, 0)
                                    ctx.closePath(); ctx.fill()
                                }
                            }
                        }
                        background: Rectangle {
                            implicitHeight: 26
                            radius: Theme.radiusSm
                            border.width: 1
                            border.color: combo.hovered ? Theme.accent : Theme.borderStrong
                            color: Theme.surface2
                            opacity: combo.enabled ? 1.0 : 0.45
                        }
                        popup: Popup {
                            y: combo.height
                            width: combo.width
                            implicitHeight: contentItem.implicitHeight + 8
                            padding: 4
                            background: Rectangle {
                                radius: Theme.radiusSm
                                border.width: 1
                                border.color: Theme.borderStrong
                                color: Theme.surface
                            }
                            contentItem: ListView {
                                implicitHeight: Math.min(contentHeight + 4, 220)
                                clip: true
                                model: combo.model
                                currentIndex: combo.highlightedIndex
                                delegate: ItemDelegate {
                                    width: combo.popup ? combo.popup.width - 8 : combo.width
                                    required property int index
                                    contentItem: Label {
                                        text: combo.model[index].label
                                        color: Theme.text
                                        font: combo.font
                                        verticalAlignment: Text.AlignVCenter
                                        leftPadding: 8
                                    }
                                    background: Rectangle {
                                        radius: Theme.radiusSm
                                        color: ListView.isCurrentItem ? Theme.surface3 : "transparent"
                                    }
                                    onClicked: {
                                        combo.currentIndex = index
                                        combo.popup.close()
                                    }
                                }
                            }
                        }
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
                            Layout.preferredWidth: 72
                            font.family: Theme.fontMono
                            font.pixelSize: Theme.fsBase
                        }
                        BbTextField {
                            visible: !root.comboOptions(modelData.key, modelData.value)
                            Layout.fillWidth: true
                            text: modelData.value
                            onTextChanged: { modelData.value = text; root.dirtyTick++ }
                        }
                        ComboBox {
                            id: combo
                            visible: !!root.comboOptions(modelData.key, modelData.value)
                            model: root.comboOptions(modelData.key, modelData.value)
                            textRole: "label"
                            valueRole: "value"
                            // 非可编辑（同主字段 combo）：Label contentItem，整块/箭头可点。
                            contentItem: Label {
                                text: combo.displayText
                                color: Theme.text
                                font: combo.font
                                verticalAlignment: Text.AlignVCenter
                                elide: Text.ElideRight
                                leftPadding: 8
                                rightPadding: 22
                            }
                            Layout.fillWidth: true
                            font.pixelSize: Theme.fsBase
                            font.family: Theme.fontSans
                            // currentIndex 显式管理（见主字段 combo 注释）：字段变化 → source 重绑
                            // → onSourceChanged 重算；点选/输入 → 各 handler 直接更新。
                            property var source: modelData
                            onSourceChanged: combo.syncFromField()
                            Component.onCompleted: combo.syncFromField()
                            function syncFromField() {
                                currentIndex = root.comboIndexOf(source.key, source.value)
                            }
                            onCurrentIndexChanged: {
                                const idx = currentIndex
                                if (idx >= 0) {
                                    const v = root.comboOptions(source.key, source.value)[idx].value
                                    if (source.value !== v) { source.value = v; root.dirtyTick++ }
                                }
                            }
                            indicator: Item {
                                anchors.right: parent.right
                                anchors.rightMargin: 8
                                anchors.verticalCenter: parent.verticalCenter
                                width: 14; height: 8
                                Canvas {
                                    anchors.fill: parent
                                    onPaint: {
                                        const ctx = getContext("2d")
                                        ctx.reset()
                                        ctx.fillStyle = Theme.textMuted
                                        ctx.beginPath()
                                        ctx.moveTo(0, 0); ctx.lineTo(7, 8); ctx.lineTo(14, 0)
                                        ctx.closePath(); ctx.fill()
                                    }
                                }
                            }
                            background: Rectangle {
                                implicitHeight: 26
                                radius: Theme.radiusSm
                                border.width: 1
                                border.color: combo.hovered ? Theme.accent : Theme.borderStrong
                                color: Theme.surface2
                                opacity: combo.enabled ? 1.0 : 0.45
                            }
                            popup: Popup {
                                y: combo.height
                                width: combo.width
                                implicitHeight: contentItem.implicitHeight + 8
                                padding: 4
                                background: Rectangle {
                                    radius: Theme.radiusSm
                                    border.width: 1
                                    border.color: Theme.borderStrong
                                    color: Theme.surface
                                }
                                contentItem: ListView {
                                    implicitHeight: Math.min(contentHeight + 4, 220)
                                    clip: true
                                    model: combo.model
                                    currentIndex: combo.highlightedIndex
                                    delegate: ItemDelegate {
                                        width: combo.popup ? combo.popup.width - 8 : combo.width
                                        required property int index
                                        contentItem: Label {
                                            text: combo.model[index].label
                                            color: Theme.text
                                            font: combo.font
                                            verticalAlignment: Text.AlignVCenter
                                            leftPadding: 8
                                        }
                                        background: Rectangle {
                                            radius: Theme.radiusSm
                                            color: ListView.isCurrentItem ? Theme.surface3 : "transparent"
                                        }
                                        onClicked: {
                                            combo.currentIndex = index
                                            combo.popup.close()
                                        }
                                    }
                                }
                            }
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
                color: Theme.text
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
