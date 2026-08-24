// SPDX-License-Identifier: GPL-3.0-only
// 元信息面板（左 Dock）：显示 + 编辑 dispatch(info)/meta.list 返回的 Chart.meta 字段。
// 2026-09 用户迭代：加大 DPI（fsBase + 8px 间距 + BbTextField）+ 可编辑表单（meta.list / meta.edit /
// 重置）+ **标题已收窄（spacing 2，去掉标题↔字段大间隙）** + **去掉底部路径**（右上角已显示路径）
// + **次要字段「更多字段」折叠组**（对照 iBMSC 文件头/扩展区，只做信息参考，不做其右 dock 布局）。
// 常用字段（TITLE/ARTIST/GENRE/BPM 等）恒显；BACKBMP/STAGEFILE 等其余字段收进可折叠组。
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

ColumnLayout {
    id: root
    property var meta: null
    property string chartPath: ""
    /// 保存成功（Main 据此 chartSession.refresh() + refreshLint()）
    signal metaSaved()
    /// 状态提示（Main 置状态栏）
    signal metaMessage(string msg)

    /// 编辑字段状态（orig = 载入时的原始值，用于脏判定；value = 当前编辑值）
    property var fields: []
    /// 常用字段（恒显）；其余收进「更多字段」折叠组（对照文档元信息清单）
    readonly property var primaryKeys: ["TITLE", "SUBTITLE", "ARTIST", "GENRE", "BPM",
        "PLAYER", "PLAYLEVEL", "RANK", "TOTAL", "DIFFICULTY"]
    property bool expandSecondary: false
    readonly property var primaryFields: root.fields.filter(f => root.isPrimary(f.key))
    readonly property var secondaryFields: root.fields.filter(f => !root.isPrimary(f.key))
    function isPrimary(k) { return root.primaryKeys.indexOf(k) >= 0 }

    /// 载入元数据（Main 在打开谱面后调用；无谱面则清空并提示）
    function reload() {
        if (!root.meta) { root.fields = []; return }
        const resp = beatbench.dispatch(JSON.stringify({ command: "meta.list", args: {} }))
        const r = JSON.parse(resp)
        if (!r.ok) { root.fields = []; root.metaMessage(r.error.code + ": " + r.error.message); return }
        const arr = []
        for (const k in r.result.meta)
            arr.push({ key: k, value: r.result.meta[k] || "", orig: r.result.meta[k] || "" })
        root.fields = arr
    }
    function hasDirty() {
        for (let i = 0; i < root.fields.length; i++)
            if (root.fields[i].value !== root.fields[i].orig) return true
        return false
    }
    /// 放弃修改：重新载入
    function reset() { root.reload() }
    /// 保存：收集脏字段 → meta.edit（空值删字段）→ 成功重载 + 发 metaSaved
    function save() {
        if (!root.meta) { root.metaMessage(qsTr("先打开谱面")); return }
        const edits = []
        for (let i = 0; i < root.fields.length; i++) {
            const f = root.fields[i]
            if (f.value !== f.orig) edits.push({ key: f.key, value: f.value })
        }
        if (edits.length === 0) { root.metaMessage(qsTr("元信息无修改")); return }
        const resp = beatbench.dispatch(JSON.stringify({ command: "meta.edit", args: { edits: edits } }))
        const r = JSON.parse(resp)
        if (r.ok) {
            root.reload()
            root.metaSaved()
            root.metaMessage(qsTr("元信息已保存 %1 处（可撤销）").arg(edits.length))
        } else {
            root.metaMessage(r.error.code + ": " + r.error.message)
        }
    }

    spacing: 2   // 标题↔字段收窄（去除大间隙）

    RowLayout {
        spacing: 8
        Label { text: qsTr("元信息"); font.bold: true; color: Theme.text; font.pixelSize: Theme.fsBase }
        Item { Layout.fillWidth: true }
        BbToolButton {
            text: qsTr("保存")
            enabled: root.meta !== null && root.hasDirty()
            onClicked: root.save()
            font.pixelSize: Theme.fsSmall
        }
        BbToolButton {
            text: qsTr("重置")
            enabled: root.meta !== null && root.hasDirty()
            onClicked: root.reset()
            font.pixelSize: Theme.fsSmall
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
            spacing: 8
            // 常用字段恒显
            Repeater {
                model: root.primaryFields
                delegate: RowLayout {
                    Layout.fillWidth: true
                    spacing: 8
                    Label {
                        text: modelData.key + ":"
                        color: Theme.textMuted
                        Layout.preferredWidth: 92
                        font.family: Theme.fontMono
                        font.pixelSize: Theme.fsBase
                    }
                    BbTextField {
                        text: modelData.value
                        Layout.fillWidth: true
                        onTextChanged: modelData.value = text
                    }
                }
            }
            // 次要字段折叠组（「更多字段」；对照文档元信息清单，其余字段收进这）
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
                spacing: 8
                Repeater {
                    model: root.secondaryFields
                    delegate: RowLayout {
                        Layout.fillWidth: true
                        spacing: 8
                        Label {
                            text: modelData.key + ":"
                            color: Theme.textMuted
                            Layout.preferredWidth: 92
                            font.family: Theme.fontMono
                            font.pixelSize: Theme.fsBase
                        }
                        BbTextField {
                            text: modelData.value
                            Layout.fillWidth: true
                            onTextChanged: modelData.value = text
                        }
                    }
                }
            }
            Item { height: 4 }
        }
    }
}
