// SPDX-License-Identifier: GPL-3.0-only
// 属性检查器（右 Dock「属性」标签页，M2/M3 边界）：显示当前选中 note 集合的详情。
// 纯表现 + 只读（数据 = EditPage.selection，由 Main 的 selectionRefs 回填，与 ChartView 同源）；
// 「修改采样…」按钮 = M3 编辑入口（转发 noteEditRequested → Main 弹 note.setSample 对话框）。
// 双语言纪律（doc/08 §2）：id/通道号语义在 C++（chartSession.idTextOf/laneChannel），
// QML 只做展示与分发；不直接 dispatch 编辑命令。
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import "../components"

ColumnLayout {
    id: root

    /// 选中 note 集合（NoteRef 语义，与 ChartView.selection 同源）。
    property var selection: []
    /// 编辑入口信号（单选用）：→ EditPage.noteEditRequested → Main 弹 note.setSample 对话框。
    signal noteEditRequested(var ref)

    readonly property int _count: selection ? selection.length : 0
    readonly property var _single: _count === 1 ? selection[0] : null

    spacing: 8

    // ---- 选中计数徽标 + 状态短语 ----
    RowLayout {
        Layout.fillWidth: true
        spacing: 6
        Label {
            text: qsTr("选中 note")
            color: Theme.textMuted
            font.pixelSize: Theme.fsSmall
        }
        Rectangle {
            width: 20
            height: 16
            radius: 3
            color: _count > 0 ? Theme.primarySoft : Theme.surface3
            Label {
                anchors.centerIn: parent
                text: root._count
                color: _count > 0 ? Theme.primary : Theme.textFaint
                font.family: Theme.fontMono
                font.pixelSize: Theme.fsTiny
            }
        }
        Item { Layout.fillWidth: true }
        Label {
            text: _count === 0 ? qsTr("未选中")
                 : _count === 1 ? qsTr("单选")
                 : qsTr("多选 %1 个").arg(_count)
            color: _count > 0 ? Theme.accent : Theme.textFaint
            font.pixelSize: Theme.fsTiny
        }
    }

    // 分隔线
    Rectangle { Layout.fillWidth: true; height: 1; color: Theme.border }

    // ---- 空态 ----
    Label {
        visible: _count === 0
        Layout.fillWidth: true
        text: qsTr("选择工具下点击 note，或 Shift+拖拽框选，查看其属性。")
        color: Theme.textMuted
        font.pixelSize: Theme.fsTiny
        wrapMode: Text.WordWrap
    }

    // ---- 多选摘要 ----
    ColumnLayout {
        visible: _count > 1
        Layout.fillWidth: true
        spacing: 4
        Label {
            text: qsTr("已选中 %1 个 note（可整体删除 / 平移 / 量化 / 变换）").arg(_count)
            color: Theme.text
            font.pixelSize: Theme.fsSmall
            wrapMode: Text.WordWrap
        }
        Label {
            text: qsTr("Del 删除 · 拖拽平移 · 量化/镜像/旋转见工具条")
            color: Theme.textFaint
            font.pixelSize: Theme.fsTiny
            wrapMode: Text.WordWrap
        }
    }

    // ---- 单选详情 ----
    ColumnLayout {
        visible: _count === 1
        Layout.fillWidth: true
        spacing: 6

        // 表头：通道名（粗体）
        Label {
            Layout.fillWidth: true
            text: root._single ? root.laneName(root._single) : ""
            color: Theme.primary
            font.bold: true
            font.family: Theme.fontSans
            font.pixelSize: Theme.fsBase
            elide: Text.ElideRight
        }
        // LN 配对激活提示（lnSelectMode 下 noteAt 返回配对段）
        Label {
            visible: !!root._single && !!root._single.lnPartner
            Layout.fillWidth: true
            text: qsTr("LN 配对段已选（整体移动/删除）")
            color: Theme.accent
            font.pixelSize: Theme.fsTiny
        }

        // 通道（显示名 + 实际 BMS 通道号，如 "键 3 · ch13"）
        RowLayout {
            Layout.fillWidth: true
            spacing: 6
            Label { text: qsTr("通道"); color: Theme.textMuted; font.pixelSize: Theme.fsTiny; Layout.preferredWidth: 40 }
            Label {
                Layout.fillWidth: true
                text: root._single
                      ? root.laneName(root._single) + root.laneCh(root._single)
                      : ""
                color: Theme.text
                font.family: Theme.fontMono
                font.pixelSize: Theme.fsSmall
                elide: Text.ElideRight
            }
        }
        // 位置（小节 · m/n）
        RowLayout {
            Layout.fillWidth: true
            spacing: 6
            Label { text: qsTr("位置"); color: Theme.textMuted; font.pixelSize: Theme.fsTiny; Layout.preferredWidth: 40 }
            Label {
                Layout.fillWidth: true
                text: root._single ? root.posText(root._single) : ""
                color: Theme.text
                font.family: Theme.fontMono
                font.pixelSize: Theme.fsSmall
                elide: Text.ElideRight
            }
        }
        // 采样（#WAVid；注意：noteAt 返回数值 id → chartSession.idTextOf 按 id_base 转文本）
        RowLayout {
            Layout.fillWidth: true
            spacing: 6
            Label { text: qsTr("采样"); color: Theme.textMuted; font.pixelSize: Theme.fsTiny; Layout.preferredWidth: 40 }
            Label {
                Layout.fillWidth: true
                text: root._single ? "#WAV" + chartSession.idTextOf(root._single.sample) : ""
                color: Theme.accent
                font.family: Theme.fontMono
                font.pixelSize: Theme.fsSmall
                elide: Text.ElideRight
            }
        }
        // 类型（轨道家族语义）
        RowLayout {
            Layout.fillWidth: true
            spacing: 6
            Label { text: qsTr("类型"); color: Theme.textMuted; font.pixelSize: Theme.fsTiny; Layout.preferredWidth: 40 }
            Label {
                Layout.fillWidth: true
                text: root._single ? root.laneType(root._single) : ""
                color: Theme.text
                font.pixelSize: Theme.fsSmall
                elide: Text.ElideRight
            }
        }

        // M3 编辑入口：改引用采样 id（与双击 note 同一路径 note.setSample）
        BbToolButton {
            text: qsTr("修改采样…")
            Layout.fillWidth: true
            onClicked: if (root._single) root.noteEditRequested(root._single)
            ToolTip.visible: hovered
            ToolTip.text: qsTr("修改选中 note 引用的 #WAV id（切音手工版）")
        }
    }

    // ---- 展示辅助（双语言纪律：只读语义，无命令） ----
    function laneName(ref) {
        const l = ref.lane
        const prefix = l.player === 1 ? "2P·" : ""
        if (l.kind === "key") return prefix + qsTr("键") + l.index
        if (l.kind === "scratch") return prefix + qsTr("皿")
        if (l.kind === "pedal") return prefix + qsTr("踏板")
        if (l.kind === "bgm")
            return qsTr("BGM") + (ref.bgm_line !== undefined && ref.bgm_line >= 0
                                  ? "·" + (ref.bgm_line + 1) : "")
        return prefix + l.kind
    }
    function laneCh(ref) {
        const ch = chartSession.laneChannel(ref.lane.player, ref.lane.kind, ref.lane.index)
        return ch !== "" ? " · ch" + ch : ""
    }
    function laneType(ref) {
        if (ref.lane.kind === "key") return qsTr("键音")
        if (ref.lane.kind === "scratch") return qsTr("皿")
        if (ref.lane.kind === "pedal") return qsTr("踏板")
        if (ref.lane.kind === "bgm") return qsTr("背景音")
        return qsTr("其他")
    }
    function posText(ref) {
        return qsTr("小节 %1").arg(ref.measure) +
               qsTr(" · %1/%2").arg(ref.pos.num).arg(ref.pos.den)
    }
}
