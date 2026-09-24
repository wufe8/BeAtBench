// SPDX-License-Identifier: GPL-3.0-only
// 切音页（M6.1 导入工作台）：参考音频（stem.wav）+ MIDI（notes.mid）导入、
// 波形预览 + 播放/seek + offset 微调（全局）。M6.2 起叠加切片线/列表。
// M6.3c UI 改良（用户 2026-09）：控件瘦身 + 左 dock（MIDI 音符/切片/网格三页签，
// 网格页留待手动切片）、raw 右 dock（IDE 式输出）、「MIDI 线」Ctrl 临时开关
// （网格模式参考 MIDI 线，为手动切片做准备）、导出加「起始小节」。
// 数据侧全部在 SliceWorkspace（C++）；本页只做装配与交互（doc/08 §2 双语言纪律）。
// woslicerII 参考（键盘 + 网格节拍 + 末端 fade + 無音切）→ M6.2 切分交互时实现。
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Dialogs
import BeatBench

Item {
    id: root

    /// 当前播放头（秒；<0 = 无）。播放中由下方 Timer 刷新（audioEngine 20Hz 信号在此聚合）。
    property real playheadSec: -1
    /// M6.3 导出结果（exportSlices 返回 map）+ 可复制 raw。
    property var exportResult: null
    property string rawText: ""
    /// 「MIDI 线」开关实际态（网格模式默认关；Ctrl 临时勾选 = 经 checkbox.toggle() 同路径翻
    /// 转，松开还原——与正常点击走同一条 onToggled 链路，checkbox 视觉同步真实状态）。
    property bool midiLinesOn: false
    /// MIDI 切片右边界（2026-09 用户）：true（默认）= 下一起始/音频末尾——与手动切片一致
    /// （只标起始；同起始和弦合并一片）；false = 按 note 结束切分（历史行为）。
    property bool midiExtendNext: true
    /// 「同时铺入编辑区」（M6.3 2026-09）：导出成功后把 raw 直接铺进当前谱面 ch01
    /// （子行接续：目标小节段已有最高子行 +1 起；一个撤销步）。
    property bool placeToChart: false
    property bool independentExport: false
    property bool _base62WarningConfirmed: false
    /// M6.4f 键盘快捷键门控（Main 注入：页面激活 && 无文本输入焦点）；
    /// 本页自持 = 页面可见（StackLayout 激活）&& 切片编辑对话框未开。
    property bool kbdPageActive: false
    readonly property bool kbdEnabled: root.kbdPageActive && root.visible
                                       && !sliceEditDialog.visible && !exportConflictDialog.visible
    /// 焦点区域高亮（PR 式 2026-09；同 EditPage）：页内最后点击区域 → 边缘高亮（sliceLeft/
    /// sliceCenter/sliceRight）；切页清除。页级 propagate MouseArea 记录（accept=false 不拦截）。
    property string focusRegionId: ""
    onVisibleChanged: if (!visible) focusRegionId = ""

    /// 页内点击 → 区域 id（三栏矩形命中；点中三栏以外 → ""）。
    function regionAt(mx, my) {
        var cands = [["sliceLeft", leftDockItem], ["sliceCenter", centerBox],
                     ["sliceRight", rawDockItem]]
        for (var i = 0; i < cands.length; ++i) {
            const r = cands[i][1]
            const p = r.mapFromItem(root, mx, my)
            if (p.x >= 0 && p.y >= 0 && p.x <= r.width && p.y <= r.height) return cands[i][0]
        }
        return ""
    }
    /// 全局点击记录（最顶层、不拦截 → 下层控件继续处理）
    MouseArea {
        anchors.fill: parent
        z: 100
        propagateComposedEvents: true
        onPressed: (mouse) => {
            focusRegionId = regionAt(mouse.x, mouse.y)
            mouse.accepted = false
        }
    }
    /// 左 dock 页签：0 = 切片（默认·放置开关/编辑）/ 1 = MIDI 音符。
    /// 2026-09 用户：网格页（纯引导文字，无实际效果）删除。
    property int dockTab: 0
    /// Ctrl 临时勾选状态机：保存按下前状态；松开仍处翻转态才还原（期间用户点过 = 以其为准）。
    property bool _midiCtrlActive: false
    property bool _midiCtrlSave: false

    function defaultOutDir() {
        var cPath = (typeof chartSession !== "undefined" && chartSession.path) ? chartSession.path : ""
        var base = cPath.length ? cPath : sliceWorkspace.audioPath
        var i = Math.max(base.lastIndexOf("/"), base.lastIndexOf("\\"))
        return i >= 0 ? base.substring(0, i) : ""
    }
    // #WAV id 文本：自动/显式 Base36 或 Base62，均显示两位。
    function idTextOf(v) {
        var base = sliceWorkspace.effectiveWavIdBase(exportBaseBox ? exportBaseBox.currentIndex : 0)
        var alphabet = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz"
        var n = Math.max(0, parseInt(v, 10) || 0)
        var s = ""
        do { s = alphabet.charAt(n % base) + s; n = Math.floor(n / base) } while (n > 0)
        while (s.length < 2) s = "0" + s
        return s
    }
    function idValueOf(text) {
        var base = sliceWorkspace.effectiveWavIdBase(exportBaseBox ? exportBaseBox.currentIndex : 0)
        var alphabet = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz"
        var t = ("" + text).trim()
        var v = 0
        if (!t.length) return 1
        for (var i = 0; i < t.length; ++i) {
            var d = alphabet.indexOf(t.charAt(i))
            if (d < 0 || d >= base) return -1
            v = v * base + d
        }
        var max = base === 62 ? 3843 : 1295
        return Math.max(1, Math.min(max, v))
    }
    function doExport(policy) {
        if (policy === undefined && exportBaseBox.currentIndex === 2 &&
            !root.independentExport && root.placeToChart &&
            (typeof chartSession !== "undefined" && chartSession && chartSession.hasChart) &&
            sliceWorkspace.effectiveWavIdBase(0) !== 62 &&
            !root._base62WarningConfirmed) {
            base62PlacementWarning.open()
            return
        }
        var dir = defaultOutDir()
        var prefix = prefixBox.text.length ? prefixBox.text : "slice"
        if (policy === undefined || policy === null || policy === "") {
            var preview = sliceWorkspace.previewExportFiles(dir, prefix)
            if (!preview || !preview.ok) {
                exportResult = preview
                rawText = ""
                return
            }
            if (preview.collisions && preview.collisions.length > 0) {
                exportConflictDialog.outDir = preview.outDir || dir
                exportConflictDialog.prefix = preview.prefix || prefix
                exportConflictDialog.collisions = preview.collisions
                exportConflictDialog.nextContinueIndex = preview.nextContinueIndex || 0
                exportConflictDialog.open()
                return
            }
            policy = "overwrite"
        }
        var r = sliceWorkspace.exportSlices(bpmBox.value, subBox.value, 4,
                                            exportIdBox.value, startMeasureBox.value,
                                            dir, prefix, 1.0,
                                            root.placeToChart && !root.independentExport, policy,
                                            exportBaseBox.currentIndex, root.independentExport)
        exportResult = r
        rawText = (typeof r.raw === "string") ? r.raw : ""
        if (r.ok) {
            // 连续导入导出：起始 id = 本次分配的最大 id + 1（跳过已占用；无谱面也是连续）
            exportIdBox.value = (typeof r.nextStartId === "number" && r.nextStartId >= 1)
                                ? r.nextStartId : sliceWorkspace.nextFreeWavId()
        }
    }
    // ---- 调试（main.cpp --slice-detect/--slice-export 同路径）----
    /// 切换切片源 UI（MIDI/网格；连带 MIDI 线默认态），供 --slice-detect 注入。
    function debugSetSource(source) {
        sliceSourceBox.currentIndex = (source === "midi") ? 1 : 0
    }
    /// 以指定起始 #WAV id 导出（与页面「导出分片」同一路径；raw 进右 dock）。
    function debugExport(startId) {
        exportIdBox.value = startId
        // 调试路径禁止弹窗，也禁止静默覆盖：有重名则 error 零落盘。
        doExport("error")
    }
    /// 选中拍子（--slice-select-beat <秒>；手动切分选中态验收）。
    function debugSelectBeat(t) {
        sliceWaveform.selectedBeatSec = t
    }

    // ---- M6.4f 切音动作正式入口（生产注册表/调试队列共用业务分派） ----
    function dispatchSliceAction(act) {
        sliceAct(act)
    }

    // 兼容页面内部按钮和旧调试调用；生产 UiActionRegistry 不再经过 debugSliceAct。
    function sliceAct(act) {
        switch (act) {
        case "playPause": audioEngine.refTogglePlay(); break
        case "beatLeft": moveSelectedBeat(-1); break
        case "beatRight": moveSelectedBeat(1); break
        case "rowUp": moveSelectedRow(-1); break
        case "rowDown": moveSelectedRow(1); break
        case "togglePoint": {
            var t = sliceWaveform.selectedBeatSec
            if (t < 0) t = Math.max(0, root.playheadSec)
            sliceWorkspace.toggleManualPoint(t)
            break
        }
        case "clearPoints": sliceWorkspace.clearManualPoints(); break
        case "copyPoints": sliceWorkspace.copyManualPoints(); break
        case "pastePoints": sliceWorkspace.pasteManualPoints(); break
        case "detect":
            if (sliceWorkspace.detectSlices(
                    sliceSourceBox.currentIndex === 0 ? "grid" : "midi",
                    bpmBox.value, subBox.value, sliceWorkspace.audioDurationSec,
                    root.midiExtendNext))
                root.dockTab = 0
            break
        case "clearSlices": sliceWorkspace.clearSlices(); break
        case "export": root.doExport(); break
        case "importAudio": audioFileDialog.open(); break
        case "importMidi": midiFileDialog.open(); break
        case "zoomIn":
            root.zoomIndex = Math.min(root.zoomLevels.length - 1, root.zoomIndex + 1)
            break
        case "zoomOut":
            root.zoomIndex = Math.max(0, root.zoomIndex - 1)
            break
        }
    }
    /// 光标所在行（按当前行时长取整；音频末尾恰在边界时归入最后一行）。
    function selectedRowOf(t) {
        var total = root.totalRowCount()
        if (total <= 1) return 0
        return Math.max(0, Math.min(total - 1, Math.floor(Math.max(0, t) / root.rowSecOf())))
    }
    /// 让光标保持在可见范围内；正常视口上下各预留一行，显示不超过两行时取消预留。
    function ensureSelectedRowVisible(row) {
        var total = root.totalRowCount()
        var rows = root.visibleRows
        if (total <= rows) {
            root.scrollRow = 0
            return
        }
        var margin = rows > 2 ? 1 : 0
        var top = root.scrollRow + margin
        var bottom = root.scrollRow + rows - 1 - margin
        if (row < top) root.scrollRow = row - margin
        else if (row > bottom) root.scrollRow = row - (rows - 1 - margin)
        root.clampScroll()
    }
    /// 选中拍子按网格步长移动（←→），并按行维护视口安全区。
    function moveSelectedBeat(dir) {
        var t = sliceWaveform.selectedBeatSec
        if (t < 0) t = Math.max(0, root.playheadSec)
        t = Math.max(0, Math.min(sliceWorkspace.audioDurationSec, t + dir * root.gridStepSec()))
        sliceWaveform.selectedBeatSec = t
        ensureSelectedRowVisible(selectedRowOf(t))
    }
    /// 上下方向键移动光标一整行，保留行内相对位置；视口上下各留一行。
    function moveSelectedRow(dir) {
        var duration = sliceWorkspace.audioDurationSec
        var rowSec = root.rowSecOf()
        var t = sliceWaveform.selectedBeatSec
        if (t < 0) t = Math.max(0, root.playheadSec)
        var row = selectedRowOf(t)
        var targetRow = Math.max(0, Math.min(root.totalRowCount() - 1, row + dir))
        var inRow = Math.max(0, t - row * rowSec)
        var targetStart = targetRow * rowSec
        var targetEnd = Math.min(duration, targetStart + rowSec)
        var target = Math.min(targetEnd, targetStart + inRow)
        sliceWaveform.selectedBeatSec = Math.max(0, target)
        ensureSelectedRowVisible(targetRow)
    }
    /// 网格步长（秒）：一拍 = 60/BPM，再 ÷「细分/拍」（与波形网格参考线同源参数）。
    function gridStepSec() {
        var bpm = bpmBox.value > 0 ? bpmBox.value : 120
        var sub = subBox.value > 0 ? subBox.value : 1
        return 60 / bpm / sub
    }
    /// 快捷键序列 → 显示文本（Space→空格、Left→←…；按钮文本/tooltip 提示用，读注册表——
    /// 将来改绑自动更新显示）。
    function prettyShortcut(seq) {
        if (!seq) return ""
        var m = { Space: "空格", Left: "←", Right: "→", Up: "↑", Down: "↓" }
        var parts = String(seq).split("+")
        for (var i = 0; i < parts.length; ++i)
            if (m[parts[i]]) parts[i] = m[parts[i]]
        return parts.join("+")
    }

    // ---- M6.4f 键盘快捷键（woslicer 系 2026-09；序列来自 uiActions 注册表——将来设置页
    // 改绑（setShortcut/keymap.json）自动生效；enabled 门控 = 页面激活 + 无文本输入 + 无对话框；
    // ↑↓/←→/空格 与编辑页快捷键按 currentPage 互斥（编辑页 Space 有 currentPage===0 门控）） ----
    Shortcut { sequence: uiActions.shortcutRevision >= 0 ? uiActions.shortcut("slice.playPause") : "";  enabled: root.kbdEnabled; onActivated: root.sliceAct("playPause") }
    Shortcut { sequence: uiActions.shortcutRevision >= 0 ? uiActions.shortcut("slice.beatLeft") : "";   enabled: root.kbdEnabled; onActivated: root.sliceAct("beatLeft") }
    Shortcut { sequence: uiActions.shortcutRevision >= 0 ? uiActions.shortcut("slice.beatRight") : "";  enabled: root.kbdEnabled; onActivated: root.sliceAct("beatRight") }
    Shortcut { sequence: uiActions.shortcutRevision >= 0 ? uiActions.shortcut("slice.rowUp") : "";      enabled: root.kbdEnabled; onActivated: root.sliceAct("rowUp") }
    Shortcut { sequence: uiActions.shortcutRevision >= 0 ? uiActions.shortcut("slice.rowDown") : "";    enabled: root.kbdEnabled; onActivated: root.sliceAct("rowDown") }
    Shortcut { sequence: uiActions.shortcutRevision >= 0 ? uiActions.shortcut("slice.togglePoint") : ""; enabled: root.kbdEnabled; onActivated: root.sliceAct("togglePoint") }
    Shortcut { sequence: uiActions.shortcutRevision >= 0 ? uiActions.shortcut("slice.clearPoints") : ""; enabled: root.kbdEnabled; onActivated: root.sliceAct("clearPoints") }
    Shortcut { sequence: uiActions.shortcutRevision >= 0 ? uiActions.shortcut("slice.copyPoints") : "";  enabled: root.kbdEnabled; onActivated: root.sliceAct("copyPoints") }
    Shortcut { sequence: uiActions.shortcutRevision >= 0 ? uiActions.shortcut("slice.pastePoints") : ""; enabled: root.kbdEnabled; onActivated: root.sliceAct("pastePoints") }
    Shortcut { sequence: uiActions.shortcutRevision >= 0 ? uiActions.shortcut("slice.detect") : ""; enabled: root.kbdEnabled; onActivated: root.sliceAct("detect") }
    Shortcut { sequence: uiActions.shortcutRevision >= 0 ? uiActions.shortcut("slice.clearSlices") : ""; enabled: root.kbdEnabled; onActivated: root.sliceAct("clearSlices") }
    Shortcut { sequence: uiActions.shortcutRevision >= 0 ? uiActions.shortcut("slice.export") : ""; enabled: root.kbdEnabled; onActivated: root.sliceAct("export") }
    Shortcut { sequence: uiActions.shortcutRevision >= 0 ? uiActions.shortcut("slice.importAudio") : ""; enabled: root.kbdEnabled; onActivated: root.sliceAct("importAudio") }
    Shortcut { sequence: uiActions.shortcutRevision >= 0 ? uiActions.shortcut("slice.importMidi") : ""; enabled: root.kbdEnabled; onActivated: root.sliceAct("importMidi") }

    Timer {
        interval: 100
        running: audioEngine.refPlaying
        repeat: true
        onTriggered: root.playheadSec = audioEngine.refPositionSec
    }

    function urlToPath(url) {
        var s = url.toString()
        s = s.replace(/^file:\/\//, "")
        if (s.charAt(0) === "/" && /^\/[A-Za-z]:/.test(s))
            s = s.slice(1)
        return decodeURIComponent(s)
    }

    function fmtTime(sec) {
        if (typeof sec !== "number" || !isFinite(sec) || sec < 0) sec = 0
        var m = Math.floor(sec / 60)
        var s = sec - m * 60
        var ss = s < 10 ? "0" + s.toFixed(2) : s.toFixed(2)
        return m + ":" + ss
    }

    // ---- M6.4 换行视口（行=每行时长 rowSec 的整数倍换行；滚轮/方向键整行滚动，Ctrl+滚轮缩放）----
    /// 缩放档位：0 = 全曲一行；>=1 = 每行时长（小节或拍换算：行秒 = measures × 4拍/小节 × 60/BPM）。
    /// 深档如 1/8 拍 @60BPM ≈ 0.125s/行 ≈ 4300px/s——能看到 440Hz 正弦波周期（此前最深 1 小节
    /// 只有 ~135px/s，正弦每像素 3+ 周期 → 渲染成实心方块；2026-09 用户）。
    property var zoomLevels: [
        { label: qsTr("全曲"), measures: 0 },
        { label: qsTr("16小节"), measures: 16 },
        { label: qsTr("8小节"), measures: 8 },
        { label: qsTr("4小节"), measures: 4 },
        { label: qsTr("2小节"), measures: 2 },
        { label: qsTr("1小节"), measures: 1 },
        { label: qsTr("2拍"), measures: 0.5 },
        { label: qsTr("1拍"), measures: 0.25 },
        { label: qsTr("1/2拍"), measures: 0.125 },
        { label: qsTr("1/4拍"), measures: 0.0625 },
        { label: qsTr("1/8拍"), measures: 0.03125 } ]
    /// 当前档位（默认 4 小节/行——BPM 自适应，拍距约 34px 可直接点击）
    property int zoomIndex: 3
    /// 同屏行数（默认 4；行高约 135px，长宽比正常）
    property int visibleRows: 4
    /// 首可见行号（波形 tooltip/滚动条/键盘共用）
    property int scrollRow: 0

    /// 每行时长（秒）：缩放档位 → 时间（BPM 行内固定；未知 120）。
    function rowSecOf() {
        var m = zoomLevels[zoomIndex].measures
        if (m <= 0) return sliceWorkspace.audioDurationSec > 0 ? sliceWorkspace.audioDurationSec : 8
        var bpm = bpmBox.value > 0 ? bpmBox.value : 120
        return m * 4 * 60 / bpm
    }
    function totalRowCount() {
        var dur = sliceWorkspace.audioDurationSec
        if (dur <= 0) return 1
        return Math.max(1, Math.ceil(dur / rowSecOf() - 1e-9))
    }
    /// 夹逼 scrollRow（档位/行数/时长变化后调用）。
    function clampScroll() {
        var maxRow = Math.max(0, root.totalRowCount() - root.visibleRows)
        if (root.scrollRow > maxRow) root.scrollRow = maxRow
        if (root.scrollRow < 0) root.scrollRow = 0
    }
    onZoomIndexChanged: root.clampScroll()
    onVisibleRowsChanged: root.clampScroll()
    onScrollRowChanged: root.clampScroll()
    Connections {
        target: sliceWorkspace
        function onAudioChanged() {
            root.scrollRow = 0   // 换音频 → 回到顶部
        }
    }
    /// 视图条状态文本：行 N/M · 时间范围 · 档位（范围夹逼到曲尾；深档 <1s/行 → 小数秒）
    readonly property string viewportLabel: {
        var total = root.totalRowCount()
        var rs = root.rowSecOf()
        var dur = sliceWorkspace.audioDurationSec
        var s0 = root.scrollRow * rs
        var visEnd = Math.min(root.scrollRow + root.visibleRows, total)
        var s1 = Math.min(visEnd * rs, dur)
        function pad(v) { return v < 10 ? "0" + v : "" + v }
        function fmt(v) {
            var m = Math.floor(v / 60), s = v - m * 60
            if (rs < 1) return m + ":" + pad(Math.floor(s)) + "." + Math.floor((s % 1) * 10)
            return m + ":" + pad(Math.floor(s))
        }
        return (root.scrollRow + 1) + "/" + total + " · " + fmt(s0) + "-" + fmt(s1)
               + " · " + zoomLevels[zoomIndex].label
    }

    /// ---- 全局状态栏摘要（2026-09；Main 底部状态栏在切音页显示；只读、不含业务） ----
    /// 选中拍子（秒；<0 = 无）。←→ 移动光标用；状态栏「光标」。
    readonly property real selectedBeatSec: sliceWaveform.selectedBeatSec
    /// 切片总数 / 启用（放置开关）数（SliceWorkspace.slices 每次访问构造列表，只在此处消费）。
    readonly property int sliceCount: sliceWorkspace.slices.length
    readonly property int enabledSliceCount: {
        var n = 0
        var list = sliceWorkspace.slices
        for (var i = 0; i < list.length; ++i) if (list[i].enabled) ++n
        return n
    }

    Component.onCompleted: {
        // 起始小节默认 = 下一空小节（当前谱面已用小节数 + 1；无谱面 = 1）
        startMeasureBox.value = sliceWorkspace.suggestedStartMeasure()
        // MIDI 线默认随切片源（MIDI = 显示；网格 = 隐藏）
        root.midiLinesOn = sliceSourceBox.currentIndex === 1
    }

    // Ctrl 临时勾选：按下 → 直接翻转外部状态 midiLinesOn（与用户点击同一条状态链路：
    // checkbox 经 `checked:` 绑定跟随显示；midiVisible 直读 midiLinesOn——⚠️ 不要用
    // checkbox.toggle()：实测 Qt 6.11 它只改内部 checked 不发 toggled，外部状态不更新）；
    // 松开 → 仍处翻转态则还原。按住期间用户点过 checkbox = 用户意图优先（松开不还原）。
    Connections {
        target: keyMonitor
        function onCtrlHeldChanged() {
            if (keyMonitor.ctrlHeld && !root._midiCtrlActive && midiLinesBox.enabled) {
                root._midiCtrlActive = true
                root._midiCtrlSave = root.midiLinesOn
                root.midiLinesOn = !root.midiLinesOn
            } else if (!keyMonitor.ctrlHeld && root._midiCtrlActive) {
                root._midiCtrlActive = false
                if (root.midiLinesOn === !root._midiCtrlSave)
                    root.midiLinesOn = root._midiCtrlSave
            }
        }
    }

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 10
        spacing: 8

        // ---- 工具条：导入 / 清除 / offset / 播放控制 ----
        RowLayout {
            Layout.fillWidth: true
            spacing: 6

            BbToolButton {
                text: qsTr("导入音频…")
                      + (uiActions.shortcut("slice.importAudio")
                         ? "   " + root.prettyShortcut(uiActions.shortcut("slice.importAudio")) : "")
                enabled: !sliceWorkspace.busy
                onClicked: audioFileDialog.open()
            }
            BbToolButton {
                text: qsTr("导入 MIDI…")
                      + (uiActions.shortcut("slice.importMidi")
                         ? "   " + root.prettyShortcut(uiActions.shortcut("slice.importMidi")) : "")
                onClicked: midiFileDialog.open()
            }
            BbToolButton {
                text: qsTr("清除")
                onClicked: {
                    sliceWorkspace.clearAll()
                    root.playheadSec = -1
                }
            }
            Rectangle {
                Layout.preferredWidth: 1
                Layout.preferredHeight: 20
                color: Theme.border
            }
            Label {
                text: qsTr("偏移(ms)")
                color: Theme.textMuted
            }
            BbSpinBox {
                id: offsetBox
                from: -5000
                to: 5000
                value: Math.round(sliceWorkspace.offsetSec * 1000)
                editable: true
                onValueModified: sliceWorkspace.setOffsetSec(value / 1000.0)
            }
            Item { Layout.fillWidth: true }
            BbToolButton {
                text: (audioEngine.refPlaying ? qsTr("暂停") : qsTr("播放"))
                      + (uiActions.shortcut("slice.playPause")
                         ? "   " + root.prettyShortcut(uiActions.shortcut("slice.playPause")) : "")
                enabled: audioEngine.refHasPcm
                onClicked: audioEngine.refTogglePlay()
                ToolTip.visible: hovered
                ToolTip.text: qsTr("播放/暂停参考音频（%1）")
                    .arg(root.prettyShortcut(uiActions.shortcut("slice.playPause")))
            }
            BbToolButton {
                text: qsTr("停止")
                onClicked: {
                    audioEngine.refStop()
                    root.playheadSec = audioEngine.refPositionSec
                }
            }
            Label {
                text: root.playheadSec >= 0
                      ? (fmtTime(root.playheadSec) + " / " + fmtTime(sliceWorkspace.audioDurationSec))
                      : fmtTime(sliceWorkspace.audioDurationSec)
                color: Theme.text
                font.family: Theme.fontMono
            }
        }

        // ---- M6.2 切片控制：位置源（网格/MIDI）+ BPM/细分 + 生成/清除 + MIDI 线开关 ----
        RowLayout {
            Layout.fillWidth: true
            spacing: 6

            Label { text: qsTr("切片源"); color: Theme.textMuted }
            BbComboBox {
                id: sliceSourceBox
                model: [qsTr("网格"), qsTr("MIDI")]
                implicitWidth: 84
                onCurrentIndexChanged: root.midiLinesOn = (currentIndex === 1)
            }
            Label {
                text: qsTr("BPM")
                color: Theme.textMuted
                visible: sliceSourceBox.currentIndex === 0
            }
            BbSpinBox {
                id: bpmBox
                from: 10
                to: 300
                value: Math.round(sliceWorkspace.midiTempoBpm)
                editable: true
                visible: sliceSourceBox.currentIndex === 0
                ToolTip.visible: hovered
                ToolTip.text: qsTr("网格 BPM（10-300；整轨慢速音轨可低至 10）")
            }
            Label {
                text: qsTr("细分/拍")
                color: Theme.textMuted
                visible: sliceSourceBox.currentIndex === 0
            }
            BbSpinBox {
                id: subBox
                from: 1
                to: 16
                value: 4
                editable: true
                visible: sliceSourceBox.currentIndex === 0
            }
            // M6.2 修正（2026-09 用户）：MIDI 右边界默认 = 下一起始/音频末尾（与手动切片一致）
            BbCheckBox {
                text: qsTr("到下一起点")
                checked: root.midiExtendNext
                visible: sliceSourceBox.currentIndex === 1
                ToolTip.visible: hovered
                ToolTip.text: qsTr("MIDI 切片右边界 = 下一起始/音频末尾（与手动切片一致，"
                                   + "和弦自动合并）；关闭 = 按音符结束切分")
                onToggled: root.midiExtendNext = checked
            }
            BbToolButton {
                text: qsTr("生成切片")
                      + (uiActions.shortcut("slice.detect")
                         ? "   " + root.prettyShortcut(uiActions.shortcut("slice.detect")) : "")
                onClicked: root.sliceAct("detect")
            }
            BbToolButton {
                text: qsTr("清除切片")
                      + (uiActions.shortcut("slice.clearSlices")
                         ? "   " + root.prettyShortcut(uiActions.shortcut("slice.clearSlices")) : "")
                enabled: sliceWorkspace.hasSlices
                onClicked: sliceWorkspace.clearSlices()
            }
            // M6.3c：网格模式下显示 MIDI 线参考（Ctrl 按住临时取反；同编辑页「通道序号」）
            BbCheckBox {
                id: midiLinesBox
                text: qsTr("MIDI 线")
                checked: root.midiLinesOn
                enabled: sliceWorkspace.hasMidi
                ToolTip.visible: hovered
                ToolTip.text: qsTr("显示 MIDI 音符线（Ctrl 按住临时切换；网格模式下手动切片参考）")
                onToggled: root.midiLinesOn = checked
            }
            Item { Layout.fillWidth: true }
            Label {
                text: sliceWorkspace.hasSlices
                      ? qsTr("切片 %1 个").arg(sliceWorkspace.slices.length)
                      : qsTr("（未生成切片——网格需 BPM/细分，MIDI 需已导入）")
                color: Theme.textMuted
                elide: Text.ElideRight
            }
        }

        // ---- 三栏：左 dock（MIDI 音符/切片/网格页签）｜中央波形｜右 dock（raw） ----
        SplitView {
            Layout.fillWidth: true
            Layout.fillHeight: true
            spacing: 6

            // handle 悬停态用 SplitHandle.hovered（SplitView.hovered 非官方 API，
            // Qt 6.11 起告警且高亮不生效；同 EditPage 的 handle 注释）
            handle: Rectangle {
                implicitWidth: 4
                color: SplitHandle.hovered ? Theme.accent : Theme.border
            }

            // ================= 左 dock =================
            Item {
                id: leftDockItem
                SplitView.preferredWidth: 250
                SplitView.minimumWidth: 180
                SplitView.maximumWidth: 360

                // 左 dock 高亮描边（焦点区域；透明不挡交互）
                Rectangle {
                    anchors.fill: parent
                    z: 10
                    color: "transparent"
                    border.width: root.focusRegionId === "sliceLeft" ? 2 : 1
                    border.color: root.focusRegionId === "sliceLeft" ? Theme.focusRing : Theme.border
                }

                ColumnLayout {
                    anchors.fill: parent
                    anchors.margins: 4      // 2026-09：内容距边 4px，避免被 focus 描边（2px）遮挡
                    spacing: 4

                    BbTabStrip {
                        id: dockTabs
                        Layout.fillWidth: true
                        model: [qsTr("切片"), qsTr("MIDI 音符")]
                        currentIndex: root.dockTab
                        onIndexRequested: (i) => root.dockTab = i
                    }

                    Rectangle {
                        Layout.fillWidth: true
                        Layout.fillHeight: true
                        color: Theme.surface
                        border.width: 1
                        border.color: Theme.border
                        radius: Theme.radiusSm
                        clip: true

                        // ---- 页签 1：MIDI 音符表（列标题 + 列表；无 MIDI → 提示） ----
                        ColumnLayout {
                            anchors.fill: parent
                            anchors.margins: 4
                            spacing: 2
                            visible: root.dockTab === 1

                            RowLayout {
                                Layout.fillWidth: true
                                spacing: 4
                                visible: sliceWorkspace.hasMidi
                                // ⚠️ RowLayout 子项直接 width: 会被布局忽略（QtQml 布局接管几何）
                                // → 列宽必须 Layout.preferredWidth + min/max 锁死，否则表头与行
                                // 随文本伸缩错位（2026-09 实测：序号 0-9 与 10+ 整行偏移）
                                Label { text: qsTr("序号"); Layout.preferredWidth: 28; Layout.minimumWidth: 28; Layout.maximumWidth: 28; color: Theme.textFaint; font.pixelSize: Theme.fsTiny }
                                Label { text: qsTr("音高"); Layout.preferredWidth: 32; Layout.minimumWidth: 32; Layout.maximumWidth: 32; color: Theme.textFaint; font.pixelSize: Theme.fsTiny }
                                Label { text: qsTr("通道"); Layout.preferredWidth: 26; Layout.minimumWidth: 26; Layout.maximumWidth: 26; color: Theme.textFaint; font.pixelSize: Theme.fsTiny }
                                Label { text: qsTr("轨"); Layout.preferredWidth: 26; Layout.minimumWidth: 26; Layout.maximumWidth: 26; color: Theme.textFaint; font.pixelSize: Theme.fsTiny }
                                Label { text: qsTr("起始"); Layout.preferredWidth: 62; Layout.minimumWidth: 62; Layout.maximumWidth: 62; color: Theme.textFaint; font.pixelSize: Theme.fsTiny }
                                Label {
                                    text: qsTr("持续")
                                    color: Theme.textFaint
                                    font.pixelSize: Theme.fsTiny
                                    Layout.fillWidth: true
                                }
                            }
                            Label {
                                Layout.fillWidth: true
                                Layout.fillHeight: true
                                visible: !sliceWorkspace.hasMidi
                                text: qsTr("（未导入 MIDI——点上方「导入 MIDI…」）")
                                color: Theme.textFaint
                                horizontalAlignment: Text.AlignHCenter
                                verticalAlignment: Text.AlignVCenter
                            }
                            ListView {
                                Layout.fillWidth: true
                                Layout.fillHeight: true
                                model: sliceWorkspace.midiNotes
                                clip: true
                                visible: sliceWorkspace.hasMidi
                                delegate: RowLayout {
                                    required property var modelData
                                    width: ListView.view.width
                                    spacing: 4
                                    Label { text: modelData.index; Layout.preferredWidth: 28; Layout.minimumWidth: 28; Layout.maximumWidth: 28; color: Theme.textMuted; font.family: Theme.fontMono; font.pixelSize: Theme.fsSmall }
                                    Label { text: modelData.pitch; Layout.preferredWidth: 32; Layout.minimumWidth: 32; Layout.maximumWidth: 32; color: Theme.text; font.family: Theme.fontMono; font.pixelSize: Theme.fsSmall }
                                    Label { text: modelData.channel + "ch"; Layout.preferredWidth: 26; Layout.minimumWidth: 26; Layout.maximumWidth: 26; color: Theme.textMuted; font.pixelSize: Theme.fsSmall }
                                    Label { text: "T" + modelData.track; Layout.preferredWidth: 26; Layout.minimumWidth: 26; Layout.maximumWidth: 26; color: Theme.textMuted; font.pixelSize: Theme.fsSmall }
                                    Label {
                                        text: (modelData.startSec + sliceWorkspace.offsetSec).toFixed(3)
                                        Layout.preferredWidth: 62; Layout.minimumWidth: 62; Layout.maximumWidth: 62
                                        color: Theme.accent2; font.family: Theme.fontMono; font.pixelSize: Theme.fsSmall
                                    }
                                    Label {
                                        text: (modelData.endSec - modelData.startSec).toFixed(3) + "s"
                                        color: Theme.textMuted; font.family: Theme.fontMono; font.pixelSize: Theme.fsSmall
                                    }
                                }
                            }
                        }

                        // ---- 页签 0：切片表（列标题 + 放置开关 + 双击编辑边界；无切片 → 提示） ----
                        ColumnLayout {
                            anchors.fill: parent
                            anchors.margins: 4
                            spacing: 2
                            visible: root.dockTab === 0

                            RowLayout {
                                Layout.fillWidth: true
                                spacing: 4
                                visible: sliceWorkspace.hasSlices
                                Item { Layout.preferredWidth: 22; Layout.minimumWidth: 22; Layout.maximumWidth: 22 }
                                Label { text: qsTr("序号"); Layout.preferredWidth: 28; Layout.minimumWidth: 28; Layout.maximumWidth: 28; color: Theme.textFaint; font.pixelSize: Theme.fsTiny }
                                Label { text: qsTr("起始"); Layout.preferredWidth: 62; Layout.minimumWidth: 62; Layout.maximumWidth: 62; color: Theme.textFaint; font.pixelSize: Theme.fsTiny }
                                Label { text: qsTr("持续"); Layout.preferredWidth: 50; Layout.minimumWidth: 50; Layout.maximumWidth: 50; color: Theme.textFaint; font.pixelSize: Theme.fsTiny }
                                Label {
                                    text: qsTr("来源")
                                    color: Theme.textFaint
                                    font.pixelSize: Theme.fsTiny
                                    Layout.fillWidth: true
                                }
                            }
                            Label {
                                Layout.fillWidth: true
                                Layout.fillHeight: true
                                visible: !sliceWorkspace.hasSlices
                                text: qsTr("（无切片——选「网格/MIDI」切片源后点「生成切片」；"
                                           + "或直接在波形上点击选中拍子（青色光标）→ 再击同一拍子 = "
                                           + "添加切分点，右键 = 删除。列表行双击 = 编辑边界）\n"
                                           + "键盘：空格=播放/暂停 · ←→=选中拍子移动 · ↑↓=视口滚动 · "
                                           + "Z=放置/消去切分点 · C=清除全部切分点 · V/B=复制/粘贴切分点 · "
                                            + "Ctrl+G=生成切片 · Ctrl+E=导出 · -/= 缩放")
                                color: Theme.textFaint
                                horizontalAlignment: Text.AlignHCenter
                                verticalAlignment: Text.AlignVCenter
                                wrapMode: Text.WordWrap
                            }
                            ListView {
                                Layout.fillWidth: true
                                Layout.fillHeight: true
                                model: sliceWorkspace.slices
                                clip: true
                                visible: sliceWorkspace.hasSlices
                                delegate: Item {
                                    required property var modelData
                                    width: ListView.view.width
                                    implicitHeight: row.implicitHeight
                                    // 2026-09 切片编辑：双击行 = 打开编辑对话框（起始/持续）。
                                    // CheckBox 在上层（后声明的兄弟叠上），点击仍走开关。
                                    MouseArea {
                                        anchors.fill: parent
                                        acceptedButtons: Qt.LeftButton
                                        onDoubleClicked: root.openSliceEdit(modelData.index)
                                    }
                                    RowLayout {
                                        id: row
                                        anchors.fill: parent
                                        spacing: 4
                                        CheckBox {
                                            checked: modelData.enabled
                                            onToggled: sliceWorkspace.setSliceEnabled(modelData.index, checked)
                                            implicitHeight: 20
                                            Layout.preferredWidth: 22; Layout.minimumWidth: 22; Layout.maximumWidth: 22
                                        }
                                        Label { text: modelData.index; Layout.preferredWidth: 28; Layout.minimumWidth: 28; Layout.maximumWidth: 28; color: Theme.textMuted; font.pixelSize: Theme.fsSmall }
                                        Label {
                                            text: modelData.startSec.toFixed(3)
                                            Layout.preferredWidth: 62; Layout.minimumWidth: 62; Layout.maximumWidth: 62
                                            color: Theme.accent2; font.family: Theme.fontMono; font.pixelSize: Theme.fsSmall
                                        }
                                        Label {
                                            text: modelData.durationSec.toFixed(3) + "s"
                                            Layout.preferredWidth: 50; Layout.minimumWidth: 50; Layout.maximumWidth: 50
                                            color: Theme.textMuted; font.family: Theme.fontMono; font.pixelSize: Theme.fsSmall
                                        }
                                        Label {
                                            text: modelData.kind === "midi"
                                                  ? (modelData.noteCount > 1
                                                     ? ("MIDI ×" + modelData.noteCount)
                                                     : ("MIDI " + modelData.note))
                                                  : (modelData.kind === "manual" ? qsTr("手动") : qsTr("网格"))
                                            color: Theme.text
                                            elide: Text.ElideRight
                                            font.pixelSize: Theme.fsSmall
                                        }
                                    }
                                }
                            }
                        }
                    }
                }
            }

            // ================= 中央：视图条 + 波形（换行视口） + 滚动条 =================
            Item {
                id: centerBox
                SplitView.fillWidth: true
                SplitView.minimumWidth: 320
                ColumnLayout {
                    anchors.fill: parent
                    anchors.margins: 4      // 2026-09：内容距边 4px，避免被 focus 描边遮挡
                    spacing: 6

                // ---- 视图条：行数 / 缩放档位 / 视口范围 ----
                RowLayout {
                    Layout.fillWidth: true
                    spacing: 6
                    Label { text: qsTr("行数"); color: Theme.textMuted }
                    BbComboBox {
                        id: rowsBox
                        model: [1, 2, 3, 4, 5, 6]
                        implicitWidth: 52
                        currentIndex: root.visibleRows - 1
                        onActivated: (idx) => root.visibleRows = idx + 1
                        ToolTip.visible: hovered
                        ToolTip.text: qsTr("同屏行数（1-6）")
                    }
                    Rectangle { Layout.preferredWidth: 1; Layout.preferredHeight: 20; color: Theme.border }
                    BbToolButton {
                        text: "−"
                        ToolTip.visible: hovered
                        ToolTip.text: qsTr("缩小（每行时长更长；%1）")
                            .arg(root.prettyShortcut(uiActions.shortcut("view.zoomOut")))
                        onClicked: root.sliceAct("zoomOut")
                    }
                    BbToolButton {
                        text: "+"
                        ToolTip.visible: hovered
                        ToolTip.text: qsTr("放大（每行时长更短；%1 / Ctrl+滚轮）")
                            .arg(root.prettyShortcut(uiActions.shortcut("view.zoomIn")))
                        onClicked: root.sliceAct("zoomIn")
                    }
                    BbToolButton {
                        text: qsTr("适应全曲")
                        onClicked: root.zoomIndex = 0
                    }
                    Item { Layout.fillWidth: true }
                    Label {
                        text: root.viewportLabel
                        color: Theme.textMuted
                        font.family: Theme.fontMono
                    }
                }

                RowLayout {
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    spacing: 6

                    // 波形 + note 刻度 + 播放头 + 切片线 + 实时拍子网格（换行视口）
                    SliceWaveformItem {
                        id: sliceWaveform
                        Layout.fillWidth: true
                        Layout.fillHeight: true
                        workspace: sliceWorkspace
                        theme: Theme
                        playheadSec: root.playheadSec
                        gridVisible: sliceSourceBox.currentIndex === 0
                        gridBpm: bpmBox.value
                        gridSubdivision: subBox.value
                        rowSec: root.rowSecOf()
                        visibleRows: root.visibleRows
                        scrollRow: root.scrollRow
                        // Ctrl 临时勾选已写入 midiLinesOn（checkbox.toggle 同路径），此处直连
                        midiVisible: root.midiLinesOn
                        onSeekRequested: {
                            audioEngine.refSeek(seconds)
                            root.playheadSec = audioEngine.refPositionSec
                        }
                        onScrollRequested: (dir) => root.scrollRow += dir
                        // ⚠️ 钳制必须在这里做（zoomIndex 是普通属性，负值/越界会拖住缩放节奏：
                        // 缩到全曲后继续缩小 → 索引变负，再放大要先补回 0 才有效果）
                        onZoomRequested: (dir) => root.zoomIndex =
                            Math.max(0, Math.min(root.zoomLevels.length - 1, root.zoomIndex + dir))
                        // M6.4c 手动切分：双击 = 添加/切换；右键 = 删除（已吸附拍子线）
                        onManualToggleRequested: (t) => sliceWorkspace.toggleManualPoint(t)
                        onManualDeleteRequested: (t) => sliceWorkspace.removeManualPoint(t)
                        // 键盘：↑↓ 走页面级 Shortcut（窗口级优先消费，此处不再重复处理）；
                        // Page/Home/End 走通用 onPressed
                        Keys.onPressed: {
                            if (event.key === Qt.Key_PageUp) { root.scrollRow -= root.visibleRows; event.accepted = true }
                            else if (event.key === Qt.Key_PageDown) { root.scrollRow += root.visibleRows; event.accepted = true }
                            else if (event.key === Qt.Key_Home) { root.scrollRow = 0; event.accepted = true }
                            else if (event.key === Qt.Key_End) { root.scrollRow = 9999; event.accepted = true }  // clampScroll 夹逼
                        }
                    }

                    // ---- 行滚动条（拖/点击跳行；位置 = scrollRow/(total-visible)） ----
                    Rectangle {
                        id: vBar
                        Layout.preferredWidth: 8
                        Layout.fillHeight: true
                        color: Theme.surface2
                        radius: Theme.radiusSm
                        border.width: 1
                        border.color: Theme.border
                        Rectangle {
                            id: vThumb
                            anchors.left: parent.left
                            anchors.right: parent.right
                            readonly property int total: root.totalRowCount()
                            readonly property int maxRow: Math.max(0, total - root.visibleRows)
                            height: Math.min(vBar.height - 2,
                                             Math.max(14, vBar.height * root.visibleRows / Math.max(1, total)))
                            y: maxRow > 0 ? (vBar.height - height) * (root.scrollRow / maxRow) : 0
                            radius: vBar.radius
                            color: Theme.primary
                            opacity: 0.75
                        }
                        MouseArea {
                            anchors.fill: parent
                            hoverEnabled: true
                            property real grabOffset: 0
                            onPressed: {
                                grabOffset = mouse.y - vThumb.y
                                jumpTo(mouse.y - grabOffset)
                            }
                            onPositionChanged: if (pressed) jumpTo(mouse.y - grabOffset)
                            function jumpTo(my) {
                                var maxRow = vThumb.maxRow
                                if (maxRow <= 0) return
                                var frac = Math.max(0, Math.min(1, my / vBar.height))
                                root.scrollRow = Math.round(frac * maxRow)
                            }
                            ToolTip.visible: containsMouse
                            ToolTip.delay: 600
                            ToolTip.text: qsTr("行滚动条：拖动/点击跳行；滚轮或方向键逐行")
                        }
                    }
                }

                // ---- 状态行 ----
                Label {
                    Layout.fillWidth: true
                    text: sliceWorkspace.statusText
                    color: Theme.textFaint
                    elide: Text.ElideRight
                }
                }
                // 中央区高亮描边（焦点区域；透明不挡交互）
                Rectangle {
                    anchors.fill: parent
                    z: 10
                    color: "transparent"
                    border.width: root.focusRegionId === "sliceCenter" ? 2 : 1
                    border.color: root.focusRegionId === "sliceCenter" ? Theme.focusRing : Theme.border
                }
            }

            // ================= 右 dock：WAV 定义 · ch01（raw） =================
            Item {
                id: rawDockItem
                SplitView.preferredWidth: 250
                SplitView.minimumWidth: 180
                SplitView.maximumWidth: 400

                // 右 dock 高亮描边（焦点区域；透明不挡交互）
                Rectangle {
                    anchors.fill: parent
                    z: 10
                    color: "transparent"
                    border.width: root.focusRegionId === "sliceRight" ? 2 : 1
                    border.color: root.focusRegionId === "sliceRight" ? Theme.focusRing : Theme.border
                }

                ColumnLayout {
                    anchors.fill: parent
                    anchors.margins: 4      // 2026-09：内容距边 4px，避免被 focus 描边遮挡
                    spacing: 4

                    RowLayout {
                        Layout.fillWidth: true
                        spacing: 6
                        Label {
                            text: qsTr("WAV 定义 · ch01")
                            color: Theme.textMuted
                        }
                        Item { Layout.fillWidth: true }
                        BbToolButton {
                            text: qsTr("复制 raw")
                            enabled: root.rawText.length > 0
                            onClicked: clipboard.setText(root.rawText)
                        }
                    }

                    Rectangle {
                        Layout.fillWidth: true
                        Layout.fillHeight: true
                        color: Theme.surface
                        border.width: 1
                        border.color: Theme.border
                        radius: Theme.radiusSm
                        clip: true

                        ScrollView {
                            anchors.fill: parent
                            anchors.margins: 2
                            visible: root.rawText.length > 0
                            clip: true
                            TextArea {
                                text: root.rawText
                                readOnly: true
                                wrapMode: TextEdit.NoWrap
                                font.family: Theme.fontMono
                                font.pixelSize: Theme.fsTiny
                                color: Theme.text
                                background: Rectangle { color: "transparent" }
                            }
                        }
                        Label {
                            anchors.fill: parent
                            anchors.margins: 10
                            visible: root.rawText.length === 0
                            text: qsTr("导出后此处显示 #WAV 定义 + ch01 铺放行（可复制到编辑区）")
                            color: Theme.textFaint
                            wrapMode: Text.WordWrap
                            horizontalAlignment: Text.AlignHCenter
                            verticalAlignment: Text.AlignVCenter
                        }
                    }
                }
            }
        }

        // ---- M6.3 导出：起始 #WAV id + 起始小节 + 前缀 + 导出分片（raw 在右 dock） ----
        RowLayout {
            Layout.fillWidth: true
            spacing: 6
            Label { text: qsTr("导出起始ID"); color: Theme.textMuted }
            BbSpinBox {
                id: exportIdBox
                from: 1
                to: exportBaseBox.currentIndex === 2 ? 3843 : 1295
                value: sliceWorkspace.nextFreeWavIdForBase(exportBaseBox.currentIndex, root.independentExport)
                editable: true
                // 36 进制 id 输入：默认 IntValidator 只放行数字 → 覆盖为字母可入（A0-ZZ/a0-zz）
                // ⚠️ Qt 6.11 起 RegExpValidator 已移除 → 用 RegularExpressionValidator
                validatorOverride: RegularExpressionValidator { regularExpression: /^[0-9A-Za-z]{0,3}$/ }
                textFromValue: function(value) { return root.idTextOf(value) }
                valueFromText: function(text, locale) { return root.idValueOf(text) }
                ToolTip.visible: hovered
                ToolTip.text: qsTr("起始 #WAV id（36 进制：01-99/A0-ZZ；可填字母）")
            }
            Label { text: qsTr("起始小节"); color: Theme.textMuted }
            BbSpinBox {
                id: startMeasureBox
                from: 1
                to: 999
                value: 1
                editable: true
                ToolTip.visible: hovered
                ToolTip.text: qsTr("ch01 起始小节（第 N 小节 = 文件 #(N-1)01:\n默认 = 下一空小节）")
            }
            Label { text: qsTr("进制"); color: Theme.textMuted }
            BbComboBox {
                id: exportBaseBox
                model: [qsTr("自动"), qsTr("Base36"), qsTr("Base62")]
                implicitWidth: 76
                onCurrentIndexChanged: exportIdBox.value = sliceWorkspace.nextFreeWavIdForBase(currentIndex, root.independentExport)
                ToolTip.visible: hovered
                ToolTip.text: qsTr("自动：跟随当前谱面；无谱面或未声明 #BASE 时使用 Base36")
            }
            Label { text: qsTr("前缀"); color: Theme.textMuted }
            BbTextField {
                id: prefixBox
                text: "slice"
                placeholderText: qsTr("slice 或 slices/slice")
                implicitWidth: 130
            }
            BbCheckBox {
                id: independentExportBox
                text: qsTr("独立导出")
                checked: root.independentExport
                onToggled: {
                    root.independentExport = checked
                    if (checked) root.placeToChart = false
                }
                ToolTip.visible: hovered
                ToolTip.text: qsTr("不避开当前谱面的 WAV ID；不能同时铺入编辑区")
            }
            BbCheckBox {
                id: placeToChartBox
                text: qsTr("同时铺入编辑区")
                checked: root.placeToChart
                enabled: !root.independentExport
                onToggled: root.placeToChart = checked
                ToolTip.visible: hovered
                ToolTip.text: qsTr("导出后直接把切片铺进当前谱面 ch01（一个撤销步）\n子行接续：从目标小节段已有子行之后开始，\n同 tick 不挤旧行、新内容跨小节同列")
            }
            BbToolButton {
                text: qsTr("导出分片")
                      + (uiActions.shortcut("slice.export")
                         ? "   " + root.prettyShortcut(uiActions.shortcut("slice.export")) : "")
                enabled: sliceWorkspace.hasSlices && sliceWorkspace.hasAudio
                onClicked: root.doExport()
            }
        }
        RowLayout {
            Layout.fillWidth: true
            spacing: 6
            Label {
                text: root.exportResult && root.exportResult.ok
                      ? (qsTr("已导出 %1 片").arg(root.exportResult.count)
                         + (root.exportResult.placed
                            ? qsTr(" · 已铺 %1 片").arg(root.exportResult.placedNotes)
                            : (root.exportResult.placeError
                               ? qsTr(" · 铺放失败：") + root.exportResult.placeError
                               : ""))
                         + (root.exportResult.placementText
                            ? (" · " + root.exportResult.placementText) : "")
                         + (root.exportResult.outDir
                            ? (" · " + root.exportResult.outDir) : ""))
                      : (root.exportResult
                         ? (qsTr("导出失败：") + root.exportResult.error)
                         : (sliceWorkspace.hasSlices ? "" : qsTr("（先生成切片）")))
                color: (root.exportResult && root.exportResult.ok) ? Theme.success : Theme.warning
                elide: Text.ElideRight
                Layout.fillWidth: true
                Layout.minimumWidth: 0
            }
        }
    }

    FileDialog {
        id: audioFileDialog
        title: qsTr("导入参考音频")
        nameFilters: [qsTr("音频文件 (*.wav *.ogg *.mp3 *.flac)"), qsTr("所有文件 (*)")]
        onAccepted: {
            sliceWorkspace.loadAudioFile(urlToPath(selectedFile))
            root.playheadSec = audioEngine.refPositionSec
        }
    }
    FileDialog {
        id: midiFileDialog
        title: qsTr("导入 MIDI")
        nameFilters: [qsTr("MIDI 文件 (*.mid *.midi)"), qsTr("所有文件 (*)")]
        onAccepted: sliceWorkspace.loadMidiFile(urlToPath(selectedFile))
    }

    // ---- 2026-09 切片编辑：切片表行双击 → 编辑起始/持续（参照编辑页 metaEditDialog 模式） ----
    function openSliceEdit(index) {
        const s = sliceWorkspace.slices[index]
        if (!s) return
        sliceEditDialog.editIndex = index
        startBox.value = Math.round(s.startSec * 1000)
        durBox.value = Math.round(s.durationSec * 1000)
        sliceEditDialog.open()
    }
    BbDialog {
        id: sliceEditDialog
        title: qsTr("编辑切片")
        width: 320
        height: 186   // 34(header) + ~110(content: 提示2行+两行输入) + 42(footer)
        property int editIndex: -1
        onOpened: startBox.forceActiveFocus()
        Label {
            Layout.fillWidth: true
            text: qsTr("起始 / 持续（ms）；终点自动夹逼到音频尾；改起始后切片表按时间重排。")
            color: Theme.textFaint
            font.pixelSize: Theme.fsTiny
            wrapMode: Text.WordWrap
        }
        RowLayout {
            Layout.fillWidth: true
            spacing: 8
            Label { text: qsTr("起始"); color: Theme.textMuted; font.pixelSize: Theme.fsSmall; Layout.preferredWidth: 48 }
            BbSpinBox {
                id: startBox
                from: 0
                to: 3600000
                editable: true
                Layout.fillWidth: true
                escapeHandler: function() { sliceEditDialog.reject() }
            }
        }
        RowLayout {
            Layout.fillWidth: true
            spacing: 8
            Label { text: qsTr("持续"); color: Theme.textMuted; font.pixelSize: Theme.fsSmall; Layout.preferredWidth: 48 }
            BbSpinBox {
                id: durBox
                from: 1
                to: 3600000
                editable: true
                Layout.fillWidth: true
                escapeHandler: function() { sliceEditDialog.reject() }
            }
        }
        onAccepted: {
            if (sliceEditDialog.editIndex < 0) return
            sliceWorkspace.setSliceBounds(sliceEditDialog.editIndex,
                                          startBox.value / 1000.0, durBox.value / 1000.0)
        }
    }

    BbDialog {
        id: base62PlacementWarning
        width: 520
        height: 250
        title: qsTr("Base62 导出提醒")
        Label {
            Layout.fillWidth: true
            text: qsTr("当前编辑区谱面未启用 #BASE 62，但导出进制选择为 Base62。\n\n若同时铺入编辑区，Base62 的 ID 可能按 Base36 解释并覆盖已有定义，这通常不是预期行为。建议改回“自动”，或先在编辑区谱面中设置 #BASE 62。\n\n仍要继续吗？")
            color: Theme.text
            wrapMode: Text.WordWrap
        }
        onAccepted: {
            root._base62WarningConfirmed = true
            root.doExport()
        }
    }

    // B2：同前缀已有 wav 时先选覆盖 / 续号 / 取消（文件序号与 #WAV id 独立）。
    Dialog {
        id: exportConflictDialog
        modal: true
        anchors.centerIn: parent
        width: 500
        height: 280
        padding: 0
        standardButtons: Dialog.NoButton
        title: qsTr("导出文件已存在")
        property string outDir: ""
        property string prefix: "slice"
        property var collisions: []
        property int nextContinueIndex: 0
        function collisionText() {
            var list = collisions
            if (!list || list.length === 0) return ""
            var shown = []
            var n = Math.min(list.length, 6)
            for (var i = 0; i < n; ++i) shown.push(list[i])
            var s = shown.join("\n")
            if (list.length > 6)
                s += "\n" + qsTr("…另有 %1 个").arg(list.length - 6)
            return s
        }
        function pad3(v) {
            var s = "" + v
            while (s.length < 3) s = "0" + s
            return s
        }
        background: Rectangle {
            color: Theme.surface
            border.color: Theme.borderStrong
            border.width: 1
            radius: Theme.boxRadius
        }
        header: Rectangle {
            width: exportConflictDialog.width
            height: 34
            color: Theme.surface
            border.color: Theme.borderStrong
            border.width: 1
            Label {
                anchors.left: parent.left
                anchors.leftMargin: 12
                anchors.verticalCenter: parent.verticalCenter
                text: exportConflictDialog.title
                color: Theme.text
                font.bold: true
                font.pixelSize: Theme.fsBase
            }
        }
        footer: Rectangle {
            width: exportConflictDialog.width
            height: 42
            color: Theme.surface2
            border.color: Theme.borderStrong
            border.width: 1
            RowLayout {
                anchors.fill: parent
                anchors.margins: 5
                anchors.rightMargin: 8
                spacing: 8
                Item { Layout.fillWidth: true }
                BbToolButton {
                    text: qsTr("覆盖重名文件")
                    onClicked: {
                        exportConflictDialog.close()
                        root.doExport("overwrite")
                    }
                }
                BbToolButton {
                    text: qsTr("接续后续编号")
                    onClicked: {
                        exportConflictDialog.close()
                        root.doExport("continue")
                    }
                }
                BbToolButton {
                    text: qsTr("取消")
                    onClicked: exportConflictDialog.reject()
                }
            }
        }
        contentItem: Item {
            ColumnLayout {
                anchors.fill: parent
                anchors.leftMargin: 12
                anchors.rightMargin: 12
                anchors.topMargin: 10
                anchors.bottomMargin: 10
                spacing: 8
            Label {
                Layout.fillWidth: true
                text: qsTr("目标目录已有同名前缀 wav。覆盖只替换本批计划文件；续号保留旧文件并从 %1 起编号。文件序号与 #WAV id 独立。").arg(exportConflictDialog.prefix + "_" + exportConflictDialog.pad3(exportConflictDialog.nextContinueIndex) + ".wav")
                color: Theme.text
                wrapMode: Text.WordWrap
                font.pixelSize: Theme.fsSmall
            }
            Label {
                Layout.fillWidth: true
                text: qsTr("输出目录：") + exportConflictDialog.outDir
                color: Theme.textMuted
                wrapMode: Text.WrapAnywhere
                font.pixelSize: Theme.fsTiny
                font.family: Theme.fontMono
            }
            Label {
                Layout.fillWidth: true
                Layout.preferredHeight: 72
                text: exportConflictDialog.collisionText()
                color: Theme.warning
                wrapMode: Text.WordWrap
                elide: Text.ElideRight
                font.pixelSize: Theme.fsTiny
                font.family: Theme.fontMono
            }
            }
        }
    }
}
