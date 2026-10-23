// SPDX-License-Identifier: GPL-3.0-only
// 切音页状态栏摘要（2026-09）：视口 / 光标 / 参考音频 / 播放 / 切片 / MIDI。
// 由 Main.qml 底部全局状态栏在 currentPage === 1 时显示（与编辑页状态互斥）；
// 只读展示，不含业务逻辑（doc/skin-system.md 二、 双语言纪律）。
// 数据源：sliceWorkspace / audioEngine（模块上下文属性）+ 切音页只读摘要（page.*）。
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

RowLayout {
    id: root
    /// 切音页实例（Main 注入；提供 viewportLabel / playheadSec / selectedBeatSec / 计数）。
    property var page: null
    spacing: 8

    readonly property var ws: (typeof sliceWorkspace !== "undefined") ? sliceWorkspace : null
    readonly property var ae: (typeof audioEngine !== "undefined") ? audioEngine : null
    readonly property bool playing: ae ? ae.playing : false
    readonly property bool hasAudio: ws ? ws.hasAudio : false

    /// 秒 → m:ss.s（<0 = "—"）。
    function fmtSec(v) {
        if (!(v >= 0)) return "—"
        var m = Math.floor(v / 60)
        var s = v - m * 60
        return m + ":" + (s < 10 ? "0" : "") + s.toFixed(1)
    }
    /// 路径 → 文件名（状态栏只显示文件名，完整路径在工具提示/页面里）。
    function baseName(p) {
        if (!p) return ""
        var i = Math.max(p.lastIndexOf("/"), p.lastIndexOf("\\"))
        return i >= 0 ? p.substring(i + 1) : p
    }

    // 主消息：导入/切分/导出状态（与切音页中央状态行同源）
    Label {
        text: root.ws ? root.ws.statusText : ""
        color: Theme.textMuted
        elide: Text.ElideRight
        font.family: Theme.fontMono
        font.pixelSize: Theme.fsSmall
        Layout.fillWidth: true
        Layout.minimumWidth: 60
    }
    // 参考音频：文件名 · 时长 · 采样率 · 偏移（未导入时给空态提示）
    Label {
        visible: root.hasAudio
        text: {
            if (!root.ws) return ""
            var t = root.baseName(root.ws.audioPath) + " · " + root.fmtSec(root.ws.audioDurationSec)
                    + " · " + (root.ws.audioSampleRate / 1000).toFixed(1) + "kHz"
            if (Math.abs(root.ws.offsetSec) >= 0.0005)
                t += " · " + qsTr("偏移") + " " + (root.ws.offsetSec >= 0 ? "+" : "")
                     + Math.round(root.ws.offsetSec * 1000) + "ms"
            return t
        }
        color: Theme.textFaint
        elide: Text.ElideMiddle
        font.family: Theme.fontMono
        font.pixelSize: Theme.fsSmall
        Layout.preferredWidth: 250
        Layout.minimumWidth: 70
        Layout.maximumWidth: 300
    }
    Label {
        visible: !root.hasAudio
        text: qsTr("未导入参考音频")
        color: Theme.textFaint
        font.pixelSize: Theme.fsSmall
    }
    // 播放状态 + 播放头（秒）。用文字而非 ▶/⏸：emoji 字体回退会把符号渲染成彩色方块。
    Label {
        visible: root.hasAudio
        text: (root.playing ? qsTr("播放中") : qsTr("已暂停")) + " "
              + root.fmtSec(root.page ? root.page.playheadSec : -1)
        color: root.playing ? Theme.accent : Theme.textFaint
        font.family: Theme.fontMono
        font.pixelSize: Theme.fsSmall
    }
    // 视口：行 N/M · 时间范围 · 档位
    Label {
        visible: root.page !== null
        text: root.page ? root.page.viewportLabel : ""
        color: Theme.textFaint
        elide: Text.ElideRight
        font.family: Theme.fontMono
        font.pixelSize: Theme.fsSmall
        Layout.preferredWidth: 210
        Layout.minimumWidth: 80
    }
    // 光标（选中拍；未选时不占位）
    Label {
        visible: root.page !== null && root.page.selectedBeatSec >= 0
        text: qsTr("光标") + " " + root.fmtSec(root.page ? root.page.selectedBeatSec : -1)
        color: Theme.textFaint
        font.family: Theme.fontMono
        font.pixelSize: Theme.fsSmall
    }
    // 切片：总数 · 启用数
    Label {
        visible: root.ws ? root.ws.hasSlices : false
        text: root.page
              ? qsTr("切片 %1 · 启用 %2").arg(root.page.sliceCount).arg(root.page.enabledSliceCount)
              : ""
        color: Theme.textFaint
        font.family: Theme.fontMono
        font.pixelSize: Theme.fsSmall
    }
    // MIDI：音符数（未导入不占位）
    Label {
        visible: root.ws ? root.ws.hasMidi : false
        text: root.ws ? qsTr("MIDI %1").arg(root.ws.midiNotes.length) : ""
        color: Theme.textFaint
        font.family: Theme.fontMono
        font.pixelSize: Theme.fsSmall
    }
}
