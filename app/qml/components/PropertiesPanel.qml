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
    /// 选中 BGA/BPM/STOP 对象集合（meta 语义；与 selection 互斥，点击后另一侧清空）。
    property var metaSelection: []
    /// STOP 值显示单位（0=1/192 全音符，1=毫秒；与窗口/时间轴面板一致，供 meta 详情展示）。
    property int stopUnit: 0
    /// 毫秒换算参考 BPM（秒 = n×1.25/bpm）。
    property real stopBpm: 130
    /// 编辑入口信号（单选用 note）：→ EditPage.noteEditRequested → Main 弹 note.setSample 对话框。
    signal noteEditRequested(var ref)
    /// 编辑入口信号（单选用 meta 对象）：→ EditPage.metaEditRequested → Main 弹 meta 编辑对话框。
    signal metaEditRequested(var obj)

    readonly property int _count: selection ? selection.length : 0
    readonly property var _single: _count === 1 ? selection[0] : null
    readonly property var _singleMeta: metaSelection && metaSelection.length === 1 ? metaSelection[0] : null

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
        Layout.minimumWidth: 0
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
            Layout.fillWidth: true
            Layout.minimumWidth: 0   // 隐式宽=单行文本宽 → 会撑宽 dock；限制为可收缩
        }
        Label {
            text: qsTr("Del 删除 · 拖拽平移 · 量化/镜像/旋转见工具条")
            color: Theme.textFaint
            font.pixelSize: Theme.fsTiny
            wrapMode: Text.WordWrap
            Layout.fillWidth: true
            Layout.minimumWidth: 0
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
            Layout.minimumWidth: 0
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
                Layout.minimumWidth: 0   // 隐式宽=文本宽 → 撑宽 dock；限制可收缩（elide 处理显示）
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
                Layout.minimumWidth: 0   // 隐式宽=文本宽 → 撑宽 dock；限制可收缩（elide 处理显示）
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
                Layout.minimumWidth: 0   // 隐式宽=文本宽 → 撑宽 dock；限制可收缩（elide 处理显示）
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
                Layout.minimumWidth: 0   // 隐式宽=文本宽 → 撑宽 dock；限制可收缩（elide 处理显示）
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

    // ---- 单选 meta 对象（BGA/BPM/STOP）详情：统一查询 objectAt 提供的 kind 相关字段 ----
    ColumnLayout {
        visible: _count === 0 && !!root._singleMeta
        Layout.fillWidth: true
        spacing: 6

        // 表头：对象类别
        Label {
            Layout.fillWidth: true
            text: root.metaKindName(root._singleMeta)
            color: Theme.primary
            font.bold: true
            font.family: Theme.fontSans
            font.pixelSize: Theme.fsBase
            elide: Text.ElideRight
            Layout.minimumWidth: 0
        }
        // 位置（所有 kind 共有）
        RowLayout {
            Layout.fillWidth: true
            spacing: 6
            Label { text: qsTr("位置"); color: Theme.textMuted; font.pixelSize: Theme.fsTiny; Layout.preferredWidth: 40 }
            Label {
                Layout.fillWidth: true
                text: root.metaPosText(root._singleMeta)
                color: Theme.text
                font.family: Theme.fontMono
                font.pixelSize: Theme.fsSmall
                elide: Text.ElideRight
                Layout.minimumWidth: 0   // 隐式宽=文本宽 → 撑宽 dock；限制可收缩（elide 处理显示）
            }
        }
        // BGA：图层 + 图像 #BMPxx
        RowLayout {
            visible: root._singleMeta && root._singleMeta.kind === "bga"
            Layout.fillWidth: true
            spacing: 6
            Label { text: qsTr("图层"); color: Theme.textMuted; font.pixelSize: Theme.fsTiny; Layout.preferredWidth: 40 }
            Label {
                Layout.fillWidth: true
                text: root._singleMeta ? root.bgaLayerName(root._singleMeta.layer) : ""
                color: Theme.text
                font.family: Theme.fontMono
                font.pixelSize: Theme.fsSmall
                elide: Text.ElideRight
                Layout.minimumWidth: 0   // 隐式宽=文本宽 → 撑宽 dock；限制可收缩（elide 处理显示）
            }
        }
        RowLayout {
            visible: root._singleMeta && root._singleMeta.kind === "bga"
            Layout.fillWidth: true
            spacing: 6
            Label { text: qsTr("图像"); color: Theme.textMuted; font.pixelSize: Theme.fsTiny; Layout.preferredWidth: 40 }
            Label {
                Layout.fillWidth: true
                text: root._singleMeta ? "#BMP" + chartSession.idTextOf(root._singleMeta.sample) : ""
                color: Theme.accent
                font.family: Theme.fontMono
                font.pixelSize: Theme.fsSmall
                elide: Text.ElideRight
                Layout.minimumWidth: 0   // 隐式宽=文本宽 → 撑宽 dock；限制可收缩（elide 处理显示）
            }
        }
        // BPM：数值 + 引用 id
        RowLayout {
            visible: root._singleMeta && root._singleMeta.kind === "bpm"
            Layout.fillWidth: true
            spacing: 6
            Label { text: qsTr("值"); color: Theme.textMuted; font.pixelSize: Theme.fsTiny; Layout.preferredWidth: 40 }
            Label {
                Layout.fillWidth: true
                text: root._singleMeta ? qsTr("%1 BPM").arg(root._singleMeta.value) : ""
                color: Theme.accent
                font.family: Theme.fontMono
                font.pixelSize: Theme.fsSmall
                elide: Text.ElideRight
                Layout.minimumWidth: 0   // 隐式宽=文本宽 → 撑宽 dock；限制可收缩（elide 处理显示）
            }
        }
        RowLayout {
            visible: root._singleMeta && root._singleMeta.kind === "bpm"
            Layout.fillWidth: true
            spacing: 6
            Label { text: qsTr("id"); color: Theme.textMuted; font.pixelSize: Theme.fsTiny; Layout.preferredWidth: 40 }
            Label {
                Layout.fillWidth: true
                text: root._singleMeta ? root.metaRefText(root._singleMeta, "BPM") : ""
                color: root._singleMeta && root._singleMeta.ref_id !== undefined ? Theme.textMuted : Theme.textFaint
                font.family: Theme.fontMono
                font.pixelSize: Theme.fsSmall
                elide: Text.ElideRight
                Layout.minimumWidth: 0   // 隐式宽=文本宽 → 撑宽 dock；限制可收缩（elide 处理显示）
            }
        }
        // STOP：数值（按单位换算）+ 实际时长（count×1.25/生效BPM；用户 2026-09 需要）
        RowLayout {
            visible: root._singleMeta && root._singleMeta.kind === "stop"
            Layout.fillWidth: true
            spacing: 6
            Label { text: qsTr("值"); color: Theme.textMuted; font.pixelSize: Theme.fsTiny; Layout.preferredWidth: 40 }
            Label {
                Layout.fillWidth: true
                text: root._singleMeta ? qsTr("%1 %2").arg(root.stopToDisplay(root._singleMeta.value)).arg(root._stopUnitLabel) : ""
                color: Theme.warning
                font.family: Theme.fontMono
                font.pixelSize: Theme.fsSmall
                elide: Text.ElideRight
                Layout.minimumWidth: 0   // 隐式宽=文本宽 → 撑宽 dock；限制可收缩（elide 处理显示）
            }
        }
        // 实际时长（括号：秒）；⚠️ 换算 BPM 用 stopBpm（窗口头部 BPM）——实际应为
        // STOP 拍位生效 BPM（变速后 STOP 时长不同）；精确值由 ChartView 轨显示
        //（bpm_at 已接入），此处为快捷参考（2026-09 用户要求加括号实际时间）。
        RowLayout {
            visible: root._singleMeta && root._singleMeta.kind === "stop"
            Layout.fillWidth: true
            spacing: 6
            Label { text: qsTr("时长"); color: Theme.textMuted; font.pixelSize: Theme.fsTiny; Layout.preferredWidth: 40 }
            Label {
                Layout.fillWidth: true
                text: root._singleMeta ? "(" + (root.stopBpm > 0
                        ? (root._singleMeta.value * 1.25 / root.stopBpm).toFixed(2) + "s"
                        : "?") + ")" : ""
                color: Theme.textMuted
                font.family: Theme.fontMono
                font.pixelSize: Theme.fsSmall
                elide: Text.ElideRight
                Layout.minimumWidth: 0
            }
        }
        RowLayout {
            visible: root._singleMeta && root._singleMeta.kind === "stop"
            Layout.fillWidth: true
            spacing: 6
            Label { text: qsTr("id"); color: Theme.textMuted; font.pixelSize: Theme.fsTiny; Layout.preferredWidth: 40 }
            Label {
                Layout.fillWidth: true
                text: root._singleMeta ? root.metaRefText(root._singleMeta, "STOP") : ""
                color: root._singleMeta && root._singleMeta.ref_id !== undefined ? Theme.textMuted : Theme.textFaint
                font.family: Theme.fontMono
                font.pixelSize: Theme.fsSmall
                elide: Text.ElideRight
                Layout.minimumWidth: 0   // 隐式宽=文本宽 → 撑宽 dock；限制可收缩（elide 处理显示）
            }
        }

        // 编辑入口：双击/按钮直达 meta 编辑对话框（与视口双击 meta 对象同路径）
        BbToolButton {
            text: qsTr("编辑事件…")
            Layout.fillWidth: true
            onClicked: if (root._singleMeta) root.metaEditRequested(root._singleMeta)
            ToolTip.visible: hovered
            ToolTip.text: qsTr("编辑该 BGA/BPM/STOP 事件（meta 编辑对话框）")
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
            return qsTr("BGM") + (ref.sub_line !== undefined && ref.sub_line >= 0
                                  ? "·" + (ref.sub_line + 1) : "")
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
    // ---- meta 对象（BGA/BPM/STOP）展示辅助，与 ChartView.objectAt 的 kind 相关字段对应 ----
    function metaKindName(o) {
        if (!o) return ""
        if (o.kind === "bga") return qsTr("BGA 对象")
        if (o.kind === "bpm") return qsTr("BPM 事件")
        if (o.kind === "stop") return qsTr("STOP 事件")
        return o.kind
    }
    function metaPosText(o) {
        if (!o) return ""
        return qsTr("小节 %1").arg(o.measure) +
               qsTr(" · %1/%2").arg(o.pos.num).arg(o.pos.den)
    }
    /// 引用 id 文本（#BPMxx/#STOPxx；ref_id 未定义 = codec 自动派生 → "(auto)"）。
    function metaRefText(o, prefix) {
        if (!o) return ""
        return o.ref_id !== undefined && o.ref_id !== 0
              ? "#" + prefix + chartSession.idTextOf(o.ref_id)
              : qsTr("(auto)")
    }
    function bgaLayerName(l) {
        if (l === 1) return qsTr("poor")
        if (l === 2) return qsTr("layer")
        if (l === 3) return qsTr("layer2")
        return qsTr("base")
    }
    readonly property string _stopUnitLabel: stopUnit === 0 ? qsTr("unit") : qsTr("ms")
    /// STOP 计数 → 单位显示文本。
    function stopToDisplay(v) {
        if (stopUnit === 0) return String(Math.round(v))
        const bpm = (stopBpm > 0) ? stopBpm : 130
        return String(Math.round(v * 1250 / bpm))
    }
}
