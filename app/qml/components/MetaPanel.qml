// SPDX-License-Identifier: GPL-3.0-only
// 元信息面板（左 Dock）：显示 + 编辑 dispatch(info)/meta.list 返回的 Chart.meta 常用字段。
// 2026-09 用户：原只读、UI 密度过小不适合修改 → 加大 DPI（fsBase 字号 + 8px 间距 + BbTextField
// 控件）+ 改为可编辑表单（meta.list 载入 / meta.edit 保存 / 重置 / 脏标记）。
// 保存成功 → 发 metaSaved（Main 刷新视图/lint + 状态栏）；meta.edit 为 CompositeCommand 可撤销。
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
            Repeater {
                model: root.fields
                delegate: RowLayout {
                    Layout.fillWidth: true
                    spacing: 8
                    Label {
                        text: modelData.key + ":"
                        color: Theme.textMuted
                        Layout.preferredWidth: 108
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
            Item { height: 6 }
            Label {
                text: qsTr("路径：%1").arg(root.chartPath)
                color: Theme.textFaint
                font.pixelSize: Theme.fsSmall
                elide: Text.ElideMiddle
                Layout.fillWidth: true
            }
        }
    }
}
