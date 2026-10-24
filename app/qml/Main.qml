// SPDX-License-Identifier: GPL-3.0-only
// BeatBench 主窗口（M2 页面式工作区外壳，doc/ui-design.md v0.2）。
// 结构：固定 chrome（菜单栏 + 页面工具条 + 页面条 + 状态栏）包裹页面内容区；
// 页面内容 = EditPage / SlicePage / TestPage（切换只换视图，不换命令引擎）。
// 颜色/字体一律走 Theme token（doc/skin-system.md，禁硬编码）；皮肤系统 = 内置默认皮肤骨架（doc/skin-system.md 三、4.）。
// 第一条真链路：文件 → 打开谱面 → dispatch(info) → 元信息面板（EditPage 左 Dock）。
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Dialogs
import "components"
import "pages"

ApplicationWindow {
    id: window
    width: 1280
    height: 800
    title: {
        var name = qsTr("BeAtBench")
        if (chartPath && chartPath.length > 0) {
            var slash = Math.max(chartPath.lastIndexOf("/"), chartPath.lastIndexOf("\\"))
            name = (slash >= 0 ? chartPath.slice(slash + 1) : chartPath) + " — BeAtBench"
        } else if (chartMeta !== null) {
            name = qsTr("未命名") + " — BeAtBench"
        }
        return (chartSession && chartSession.dirty) || metaDraftDirty ? "*" + name : name
    }
    visible: true
    color: Theme.bg  // 窗口底 = 主题 token（启动即深色；不依赖平台 palette 继承）
    font.family: Theme.fontSans
    font.pixelSize: Theme.fsBase

    // ---------- 会话状态（不入 undo，doc/ui-design.md 一、.2） ----------
    property int currentPage: 0          // 0 编辑 / 1 切音 / 2 测试
    property var chartMeta: null         // dispatch(info) 的 result.meta
    property string chartPath: ""        // 当前谱面路径（info 返回的规范化路径）
    /// 元信息面板草稿（未点面板「保存」、也未 Ctrl+S）也算未保存。
    property bool metaDraftDirty: false
    /// 菜单/工具条 enabled 绑定重算（uiActions.stateChanged +1）。Shortcut.sequence 直接读 shortcut()。
    property int uiStateTick: 0
    property string statusText: qsTr("就绪")
    property string currentSampleId: ""  // 当前采样（会话状态，M3 放置落点）
    property string currentBmpId: ""     // 当前 #BMP（视口 BGA 列放置用；BGA 面板行点击设置）
    // 当前 BPM/STOP 值（路线 A：工具栏值放置用；选中逻辑方案2 = 独立于左 Dock 采样选中——
    // 在什么时序轨放置就用哪种值，互不冲突）。打开谱面时用元信息 BPM 兜底。
    property real currentBpmValue: 130
    // STOP 值存原始计数 n（1/192 全音符单位；BMS #STOPxx n）。默认 96 = 半个全音符（任意 BPM 下固定 n）。
    property real currentStopValue: 96
    // STOP 值显示/填入单位：0 = BMS 单位（1/192 全音符 n，默认）；1 = 毫秒。
    // 毫秒换算依赖触发时 BPM：STOP 秒 = n×240/(192×bpm) = n×1.25/bpm → ms = n×1250/bpm。
    property int stopUnit: 0
    /// currentStopValue（计数 n）→ 选中单位的显示文本。
    function stopToDisplay(v) {
        if (window.stopUnit === 0) return String(Math.round(v))
        const bpm = (window.currentBpmValue > 0) ? window.currentBpmValue : 130
        return String(Math.round(v * 1250 / bpm))
    }
    /// 选中单位的填入文本 → 计数 n。
    function stopFromDisplay(t) {
        const v = parseFloat(t)
        if (!isFinite(v)) return NaN
        return window.stopUnit === 0 ? v : v * bpmFromMs() / 1250
    }
    function bpmFromMs() {
        return (window.currentBpmValue > 0) ? window.currentBpmValue : 130
    }
    /// STOP 单位显示名（对话框 label 用）。
    function stopUnitLabel() {
        return window.stopUnit === 0 ? qsTr("unit") : qsTr("ms")
    }
    // 文件格式/编码（底部状态栏右下角显示；info/check 返回）
    property string chartFormat: ""
    property string chartEncoding: ""
    /// 游玩模式（info 的 mode；"" = 未打开/未知，显示 SP 7K 缺省）。
    property string chartMode: ""
    readonly property bool isPmsChart: chartMode === "pms9k" || chartMode === "pms9k-bmesp"
    function modeLabel() {
        const m = window.chartMode
        if (m === "sp7k" || m === "") return "SP7K"
        if (m === "dp") return "DP7K"
        if (m === "pms") return "PMS"
        if (m === "bms14") return "BMS14"
        // BME-SP 型变体（读侧探测产物）与 pms9k 同呈现，标签不区分
        if (m === "pms9k-bmesp") return "PMS9K"
        return m.toUpperCase()
    }
    // 轨道列头显示实际 BMS 通道 id（debug 用；Ctrl 临时切换经下方 Connections 同路径翻转）
    property bool showChannelIds: false
    /// Ctrl 临时勾选状态机（同 SlicePage MIDI 线）：按下 → 直接翻转 window.showChannelIds
    /// （与点击同一条状态链路：checkbox 经 uiActions 注册表跟随显示——⚠️ 不要用
    /// checkbox.toggle()：实测 Qt 6.11 只改内部 checked 不发 toggled，外部状态不更新）；
    /// 松开 → 仍处翻转态则还原；按住期间用户点过 = 以其为准。
    property bool _channelIdCtrlActive: false
    property bool _channelIdCtrlSave: false
    Connections {
        target: keyMonitor
        function onCtrlHeldChanged() {
            if (keyMonitor.ctrlHeld && !window._channelIdCtrlActive && channelIdCheck.enabled) {
                window._channelIdCtrlActive = true
                window._channelIdCtrlSave = window.showChannelIds
                window.showChannelIds = !window.showChannelIds
            } else if (!keyMonitor.ctrlHeld && window._channelIdCtrlActive) {
                window._channelIdCtrlActive = false
                if (window.showChannelIds === !window._channelIdCtrlSave)
                    window.showChannelIds = window._channelIdCtrlSave
            }
        }
    }
    // note 上显示所用采样：0 隐藏 / 1 id / 2 文件名
    property int noteSampleMode: 0
    // 更多轨道（BGA 图层通道列，iBMSC 式；游玩轨与背景轨之间）
    property bool showExtras: false
    // 调试参数注入（--bgm-expand / --channel-ids / --note-labels / --show-extras，配 --screenshot 验收）
    property bool debugBgmExpand: false
    property bool debugShowChannelIds: false
    property int debugNoteSampleMode: 0
    property bool debugShowExtras: false
    property bool debugPerfLog: false
    // --zoom-at <y> <factor>：调试缩放锚点（验证 zoomToCursor；不等同真实滚轮事件）
    property real debugZoomY: -1
    property real debugZoomFactor: 1
    onDebugZoomFactorChanged: if (debugZoomFactor > 0 && debugZoomY >= 0) debugZoomTimer.restart()
    Timer {
        id: debugZoomTimer
        interval: 400
        onTriggered: {
            if (typeof editPage !== "undefined" && editPage && editPage.locateChartView())
                editPage.locateChartView().zoomAt(debugZoomY, debugZoomFactor)
        }
    }
    // --seek <秒>：调试跳转（波形总览 seekRequested 同一路径；配 --wait-render 验收）
    property real debugSeekSeconds: -1
    onDebugSeekSecondsChanged: if (debugSeekSeconds >= 0) debugSeekTimer.restart()
    Timer {
        id: debugSeekTimer
        interval: 400
        onTriggered: {
            if (typeof editPage !== "undefined" && editPage && editPage.locateChartView())
                editPage.locateChartView().seekTo(debugSeekSeconds)
        }
    }
    // --delete-selection：点击后自动 Del（验收删除链）
    property bool debugDeleteSelection: false
    onDebugDeleteSelectionChanged: if (debugDeleteSelection) debugDeleteTimer.restart()
    Timer {
        id: debugDeleteTimer
        interval: 400   // 点击（150ms）之后
        onTriggered: deleteSelection()
    }
    // --paste-text <文本>：写入系统剪贴板后触发编辑页粘贴（验收「BMS 原始行 → 谱面」链路；
    // 与真实 Ctrl+V 同一路径——pasteClipboard 读 clipboard.text()）
    property string debugPasteText: ""
    onDebugPasteTextChanged: if (debugPasteText.length > 0) debugPasteTimer.restart()
    Timer {
        id: debugPasteTimer
        interval: 500   // 等谱面加载（--open）完成
        onTriggered: {
            if (typeof clipboard !== "undefined" && clipboard) clipboard.setText(debugPasteText)
            window.pasteClipboard()
        }
    }
    onDebugBgmExpandChanged: if (debugBgmExpand && editPage) editPage.bgmExpanded = true
    onDebugShowChannelIdsChanged: if (debugShowChannelIds) window.showChannelIds = true
    onDebugNoteSampleModeChanged: if (debugNoteSampleMode > 0) window.noteSampleMode = debugNoteSampleMode
    onDebugShowExtrasChanged: if (debugShowExtras) window.showExtras = true
    // 当前编辑工具（互斥单选，会话状态；M3 接输入/放置，note 类型（普通/LN/地雷）届时
    // 作为正交维度另设「放置类型」组，不并入本组——doc/ui-design.md 五、 交互）
    // 2026-09 用户：默认工具 = 「1 拖拽」(pan)；拖拽工具下点 note 只选中/试听（不移动）。
    property string editorTool: "pan"
    // 工具切换：进入 LN 放置 → 显示状态栏说明（持久）；离开 → 取消未完成 LN 头 + 恢复就绪
    onEditorToolChanged: {
        if (window.editorTool === "ln") {
            activateLnHint()
        } else {
            if (window.pendingLnHead) cancelPendingLn()
            statusClearTimer.stop()
            window.statusText = qsTr("就绪")
        }
    }
    // 剪贴板（BMS 原始行；clipboard.copy 输出 → paste 输入；会话状态）
    property var clipboardLines: []
    // 选中 note 集合（NoteRef；框选后存 + 回填高亮；Ctrl+C 复制）
    property var selectionRefs: []
    // 选中 BGA/BPM/STOP 对象集合（{kind, measure, pos, ...}；视口点选/拖拽/删除/编辑用）
    property var metaSelection: []
    // 时间轴事件（timing.list 结果；打开谱面/编辑后 refreshTiming 重取，供右 Dock 时间轴面板）
    property var timingBpm: []
    property var timingStop: []
    /// 02 通道小节长度（timing.list kind="measure" 结果：{measure,pos:{num,den},value(拍数)}）。
    property var timingMeasure: []
    // 时间轴定义表（session.samples 的 bpm/stop；供「#BPM/#STOP 定义」区）
    property var timingBpmDefs: []
    property var timingStopDefs: []
    // 吸附（放置用）：snapNum/snapDen 小节（分子分母皆可调；1/16 = 每小节 16 槽，3/16 = 3/16 步长）
    property int snapNum: 1
    property int snapDen: 16
    /// 音频设备 API 筛选（首选项音频页；0 = 全部，1.. = audioEngine.devices 去重后的 api 序）
    property int apiFilterIndex: 0
    // 缩放锚点（2026-09 用户：鼠标滚轮缩放时往鼠标位置放大；默认开）
    property bool zoomToCursor: true
    // 平移模式（checkbox 开关，默认关）：拖拽选中 note = 移动；勾选=按方向轴锁定
    // （纵向→时间/通道不变；横向→通道/时间不变）。未勾=自由 2D（时间+通道都动）。
    property bool moveMode: false
    property bool showNoteWaveforms: true
    // LN 选取模式（默认关）：开启后点选 LN 任一段自动选中配对两端（整体移动/删除）
    property bool lnSelectMode: false
    // 槽位弱线显示开关（「网格」按钮；默认开）。吸附计算不依赖此开关，仅控制画不画槽位线。
    property bool showGrid: true
    /// LNTYPE 2（#LNOBJ）LN 放置的「待定头」（NoteRef 形状：measure/pos/lane/sample）。
    /// 会话状态不入 undo；同轨点尾 / 异轨重放头 / Esc 取消（2026-09 用户）。
    property var pendingLnHead: null
    /// 文本输入焦点（工具快捷键让行，避免输入时误触）。
    /// ⚠️ 不能用「有 text 属性」判定：Label/Button 都有 text → activeFocusItem 落在
    /// 那些 item 上时恒 true，工具快捷键全部禁用（用户反馈 2026-09 快捷键失效根因）。
    /// 改用 TextInput/TextField 特有的 inputMethodHints 属性判断。
    readonly property bool textInputFocused:
        window.activeFocusItem && "inputMethodHints" in window.activeFocusItem

    /// 释放文本框焦点（2026-09）：点击编辑区/空白时，把焦点从文本框移走，避免快捷键被文本框吞掉。
    function clearTextFocus() {
        if (window.activeFocusItem && "inputMethodHints" in window.activeFocusItem)
            window.contentItem.forceActiveFocus()
    }

    // ---------- 全局快捷键（从 uiActions 注册表查表生成，doc/action-registry.md） ----------
    // 2026-09：handler 已迁入 C++ 注册表（invoke = 唯一入口）；QML Shortcut 只负责
    // 序列 + enabled 条件（含文本焦点让行等注册表不建模的细节）。

    // 文件动作
    Shortcut { sequence: uiActions.shortcutRevision >= 0 ? uiActions.shortcut("file.new") : ""
               onActivated: uiActions.invoke("file.new") }
    Shortcut { sequence: uiActions.shortcutRevision >= 0 ? uiActions.shortcut("file.open") : ""
               onActivated: uiActions.invoke("file.open") }
    Shortcut { sequence: uiActions.shortcutRevision >= 0 ? uiActions.shortcut("file.save") : ""
               enabled: chartMeta !== null
               onActivated: uiActions.invoke("file.save") }
    Shortcut { sequence: uiActions.shortcutRevision >= 0 ? uiActions.shortcut("file.saveAs") : ""
               enabled: chartMeta !== null
               onActivated: uiActions.invoke("file.saveAs") }
    Shortcut { sequence: uiActions.shortcutRevision >= 0 ? uiActions.shortcut("file.exit") : ""
               onActivated: uiActions.invoke("file.exit") }

    // 编辑动作
    Shortcut { sequence: uiActions.shortcutRevision >= 0 ? uiActions.shortcut("edit.undo") : ""
               enabled: !window.textInputFocused &&
                        ((currentPage === 0 && chartMeta !== null) ||
                         (currentPage === 1 && sliceWorkspace.canUndoSlice))
               onActivated: uiActions.invoke("edit.undo") }
    Shortcut { sequence: uiActions.shortcutRevision >= 0 ? uiActions.shortcut("edit.redo") : ""
               enabled: !window.textInputFocused &&
                        ((currentPage === 0 && chartMeta !== null) ||
                         (currentPage === 1 && sliceWorkspace.canRedoSlice))
               onActivated: uiActions.invoke("edit.redo") }
    Shortcut { sequence: uiActions.shortcutRevision >= 0 ? uiActions.shortcut("edit.copy") : ""
               enabled: chartMeta !== null && currentPage === 0 &&
                        !window.textInputFocused && window.selectionRefs.length > 0
               onActivated: uiActions.invoke("edit.copy") }
    Shortcut { sequence: uiActions.shortcutRevision >= 0 ? uiActions.shortcut("edit.paste") : ""
               enabled: chartMeta !== null && currentPage === 0 && !window.textInputFocused
               onActivated: uiActions.invoke("edit.paste") }
    Shortcut { sequence: uiActions.shortcutRevision >= 0 ? uiActions.shortcut("edit.delete") : ""
               enabled: chartMeta !== null && currentPage === 0
               onActivated: uiActions.invoke("edit.delete") }

    // 工具动作（数字 1-5：文本输入焦点时让行）
    Shortcut { sequence: uiActions.shortcutRevision >= 0 ? uiActions.shortcut("tool.pan") : ""
               enabled: currentPage === 0 && !window.textInputFocused
               onActivated: uiActions.invoke("tool.pan") }
    Shortcut { sequence: uiActions.shortcutRevision >= 0 ? uiActions.shortcut("tool.select") : ""
               enabled: currentPage === 0 && !window.textInputFocused
               onActivated: uiActions.invoke("tool.select") }
    Shortcut { sequence: uiActions.shortcutRevision >= 0 ? uiActions.shortcut("tool.note") : ""
               enabled: currentPage === 0 && !window.textInputFocused
               onActivated: uiActions.invoke("tool.note") }
    Shortcut { sequence: uiActions.shortcutRevision >= 0 ? uiActions.shortcut("tool.ln") : ""
               enabled: currentPage === 0 && !window.textInputFocused
               onActivated: uiActions.invoke("tool.ln") }
    Shortcut { sequence: uiActions.shortcutRevision >= 0 ? uiActions.shortcut("tool.mine") : ""
               enabled: currentPage === 0 && !window.textInputFocused
               onActivated: uiActions.invoke("tool.mine") }

    // 缩放（编辑页 + 切音页共用 - / =；文本框让行）
    Shortcut { sequence: uiActions.shortcutRevision >= 0 ? uiActions.shortcut("view.zoomIn") : ""
               enabled: !window.textInputFocused && (currentPage === 0 || currentPage === 1)
               onActivated: uiActions.invoke("view.zoomIn") }
    Shortcut { sequence: uiActions.shortcutRevision >= 0 ? uiActions.shortcut("view.zoomOut") : ""
               enabled: !window.textInputFocused && (currentPage === 0 || currentPage === 1)
               onActivated: uiActions.invoke("view.zoomOut") }

    // Esc：取消未完成的 LN 头（LNTYPE 2 放置；文本输入焦点时让行）
    // 2026-09 快捷键收尾：进注册表（edit.cancel，scope=edit）→ 设置页可改绑。
    Shortcut {
        sequence: uiActions.shortcutRevision >= 0 ? uiActions.shortcut("edit.cancel") : ""
        enabled: currentPage === 0 && editorTool === "ln" && !window.textInputFocused
                 && chartMeta !== null
        onActivated: uiActions.invoke("edit.cancel")
    }

    // 首选项（M4.2 设置页；菜单「设置→首选项…」同源）
    // 2026-09：进注册表（app.settings，全局）→ 设置页可改绑（Ctrl+, 现可录键）。
    Shortcut {
        sequence: uiActions.shortcutRevision >= 0 ? uiActions.shortcut("app.settings") : ""
        onActivated: uiActions.invoke("app.settings")
    }

    // M5：Space = 播放/暂停（渲染代理 PCM；无渲染时提示/等待渲染）。Ctrl+R = 手动渲染。
    // 文本输入焦点时让行；只在编辑页 + 已打开谱面时可用。
    // 2026-09 快捷键收尾：进注册表（edit.playPause / edit.render，scope=edit）；
    // 与切音页 slice.playPause 的 Space 作用域隔离（各自页面门控，互不抢键）。
    Shortcut {
        sequence: uiActions.shortcutRevision >= 0 ? uiActions.shortcut("edit.playPause") : ""
        enabled: currentPage === 0 && chartMeta !== null && !window.textInputFocused
        onActivated: uiActions.invoke("edit.playPause")
    }
    Shortcut {
        sequence: uiActions.shortcutRevision >= 0 ? uiActions.shortcut("edit.render") : ""
        enabled: currentPage === 0 && chartMeta !== null && !window.textInputFocused
        onActivated: uiActions.invoke("edit.render")
    }

    // ---------- 菜单栏（固定全局；doc/action-registry.md：从 uiActions 注册表查表） ----------
    menuBar: MenuBar {
        // 菜单栏底 = 主题 token（启动即深色；Fusion 默认/平台 palette 兜底不可靠）。
        // ⚠️ 字体色：Fusion MenuBarItem 标题取 **palette.text**（非 windowText——上轮设错角色
        // 导致深灰字体）；palette.text 同步作用于弹出的菜单项文字。
        background: Rectangle { color: Theme.surface2 }
        palette.text: Theme.text
        palette.windowText: Theme.text
        palette.buttonText: Theme.text
        Menu {
            title: qsTr("文件")
            // 枚举渲染（doc/action-registry.md 九、验收 2）：文件菜单动作按注册表 idsByCategory("file") 生成，
            // 分隔线由注册表 isSeparator(id) 判定（addSeparator 建模）。
            // ⚠️ 单 Repeater + delegate 统一 MenuItem（分隔线 = MenuItem 视觉模拟横线）：
            // Repeater delegate 只能是单一组件类型（MenuSeparator/MenuItem 混排需 Loader；
            // Loader wrapper 破坏菜单导航 → hover 高亮失效，2026-09 用户实测两轮）。
            // 分隔线 MenuItem：无文字、背景画横线、不可点击。
            Repeater {
                model: uiActions.idsByCategory("file")
                delegate: MenuItem {
                    id: fileItem
                    required property string modelData
                    readonly property bool isSep: uiActions.isSeparator(modelData)
                    text: isSep ? "" : uiActions.label(modelData) + "    " + (uiActions.shortcutRevision >= 0 ? uiActions.shortcut(modelData) : "")
                    enabled: !isSep && window.uiStateTick >= 0 && uiActions.enabled(modelData)
                    visible: true
                    onTriggered: if (!isSep) uiActions.invoke(modelData)
                    // ⚠️ 分隔线 + hover 高亮 = **系统 QPalette Highlight**（2026-09 用户实测：
                    // 菜单 hover 色来自 Windows 系统主题色（QPalette::Highlight），不是皮肤
                    // token——工作区菜单（QQC2 默认）hover = 系统紫；文件菜单曾用
                    // Theme.primary（皮肤靛蓝）→ 换 Windows 主题色/换肤时分叉）。统一：
                    // 用 Palette.highlight（Qt 调色板 = QPalette::Highlight，main.cpp 已把
                    // apply 到 qApp）+ 文字默认（highlighted 时 MenuItem 内容转 HighlightedText）。
                    // 这使文件/编辑/工作区菜单 hover 全走同一系统色，与 Windows 主题一致。
                    background: Rectangle {
                        color: fileItem.highlighted
                              ? (window.palette ? window.palette.highlight : Theme.primary)
                              : "transparent"
                        Rectangle {
                            anchors.left: parent.left; anchors.right: parent.right
                            anchors.leftMargin: 8; anchors.rightMargin: 8
                            anchors.verticalCenter: parent.verticalCenter
                            height: 1
                            visible: fileItem.isSep && !fileItem.highlighted
                            color: Theme.border
                        }
                    }
                }
            }
            // 「新建为模式」子菜单（2026-10 阶段一 issue #26）：直建指定模式（跳过对话框），
            // 未保存确认仍走。模式列表同源 codecModes()，与新建对话框一致。
            Menu {
                title: qsTr("新建为模式")
                Repeater {
                    model: chartSession.codecModes()
                    delegate: MenuItem {
                        required property string modelData
                        text: window.modeDisplayName(modelData)
                        onTriggered: window.newChartAs(modelData)
                    }
                }
            }
            Menu {
                title: qsTr("切换为模式")
                enabled: chartMeta !== null
                Repeater {
                    model: chartSession.codecModes()
                    delegate: MenuItem {
                        required property string modelData
                        text: window.modeDisplayName(modelData)
                        enabled: chartMeta !== null
                        onTriggered: window.requestModeSwitch(modelData)
                    }
                }
            }
        }
        Menu {
            title: qsTr("编辑")
            enabled: chartMeta !== null
            Repeater {
                model: uiActions.idsByCategory("edit")
                delegate: MenuItem {
                    id: editItem
                    required property string modelData
                    readonly property bool isSep: uiActions.isSeparator(modelData)
                    text: isSep ? "" : uiActions.label(modelData) + "    " + (uiActions.shortcutRevision >= 0 ? uiActions.shortcut(modelData) : "")
                    enabled: !isSep && window.uiStateTick >= 0 && uiActions.enabled(modelData)
                    onTriggered: if (!isSep) uiActions.invoke(modelData)
                    background: Rectangle {
                        color: editItem.highlighted
                              ? (window.palette ? window.palette.highlight : Theme.primary)
                              : "transparent"
                        Rectangle {
                            anchors.left: parent.left; anchors.right: parent.right
                            anchors.leftMargin: 8; anchors.rightMargin: 8
                            anchors.verticalCenter: parent.verticalCenter
                            height: 1
                            visible: editItem.isSep && !editItem.highlighted
                            color: Theme.border
                        }
                    }
                }
            }
        }
        Menu {
            title: qsTr("视图")
            // 皮肤（doc/skin-system.md 三、3. 运行时换肤）：L1 token 覆写 + 运行时切换。
            // 列出「默认」+ 内置皮肤，当前生效项打勾；点击即运行时切换（Theme.applySkinByName）。
            // ⚠️ 运行时切肤同时应用该皮肤目录的 keymap.json（doc/dev-workflow.md 四、 待改进）——默认皮肤清除覆写。
            Menu {
                title: qsTr("皮肤")
                MenuItem {
                    text: qsTr("默认")
                    checkable: true
                    checked: Theme.activeSkin === ""
                    onTriggered: window.applySkinByName("默认")
                }
                Repeater {
                    model: Theme.skinNames()
                    delegate: MenuItem {
                        required property string modelData
                        text: modelData
                        checkable: true
                        checked: Theme.activeSkin === Theme.skinDir(modelData)
                        onTriggered: window.applySkinByName(modelData)
                    }
                }
            }
        }
        Menu {
            title: qsTr("工作区")
            MenuItem { text: uiActions.label("view.page.edit"); checkable: true
                       checked: window.uiStateTick >= 0 && uiActions.checked("view.page.edit")
                       onTriggered: uiActions.invoke("view.page.edit") }
            MenuItem { text: uiActions.label("view.page.slice"); checkable: true
                       checked: window.uiStateTick >= 0 && uiActions.checked("view.page.slice")
                       onTriggered: uiActions.invoke("view.page.slice") }
            MenuItem { text: uiActions.label("view.page.test"); checkable: true
                       checked: window.uiStateTick >= 0 && uiActions.checked("view.page.test")
                       onTriggered: uiActions.invoke("view.page.test") }
        }
        Menu {
            title: qsTr("设置")
            MenuItem {
                // 2026-09：走注册表（app.settings）——改绑后菜单提示同步（与 Ctrl+, 同源）。
                text: qsTr("首选项…") + (uiActions.shortcutRevision >= 0 && uiActions.shortcut("app.settings")
                                         ? "    " + uiActions.shortcut("app.settings") : "")
                onTriggered: uiActions.invoke("app.settings")
            }
        }
        Menu {
            title: qsTr("帮助")
            MenuItem {
                text: qsTr("关于")
                onTriggered: aboutDialog.open()
            }
        }
    }

    // ---------- 主体（Column：工具条 / 内容 / 页面条 / 状态栏） ----------
    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        // 页面工具条（随页面变化；工具条 = 皮肤 L2 可声明区，doc/skin-system.md 三、1.）
        // M6.1：编辑专属（snap/变换/A-B/谱面播放）；切音/测试页各用自有工具条 → 仅编辑页显示
        ToolBar {
            visible: currentPage === 0
            Layout.fillWidth: true
            background: Rectangle {
                color: Theme.surface
                Rectangle { anchors.left: parent.left; anchors.right: parent.right
                             anchors.bottom: parent.bottom; height: 1; color: Theme.border }
            }
            RowLayout {
                anchors.fill: parent
                anchors.leftMargin: 8
                anchors.rightMargin: 8
                spacing: 6
                // snap 粒度（snapNum/snapDen 小节）：槽位弱线显示 + 放置吸附（同源；分子分母手填）
                Label { text: qsTr("snap"); color: Theme.textMuted
                        font.pixelSize: Theme.fsSmall; padding: 2 }
                BbSpinBox {
                    from: 1; to: 999
                    value: window.snapNum
                    editable: true
                    stepFactor: 2   // 2026-09：上下按钮 ×2/÷2（音乐拍子）；手填仍 1/3、1/5
                    implicitWidth: 56
                    // 2026-09 修复：BbSpinBox 的上下按钮/回车都是**程序化**设 root.value，
                    // 而 SpinBox「用户修改」信号 valueModified 对程序化赋值不触发（已实测）→
                    // 改用 onValueChanged（任何赋值都触发），否则 window.snapNum 不更新、
                    // 编辑区拍子线纹丝不动。
                    onValueChanged: window.snapNum = Math.max(1, value)
                    ToolTip.visible: hovered
                    ToolTip.text: qsTr("snap 分子（槽数步长 = snapNum/snapDen 小节；上下按钮 ×2/÷2）")
                }
                Label { text: "/"; color: Theme.textMuted; font.pixelSize: Theme.fsSmall }
                BbSpinBox {
                    from: 1; to: 192
                    value: window.snapDen
                    editable: true
                    stepFactor: 2   // 同上：×2/÷2
                    implicitWidth: 56
                    onValueChanged: window.snapDen = Math.max(1, value)
                    ToolTip.visible: hovered
                    ToolTip.text: qsTr("snap 分母（每小节槽数；吸附 + 槽位线，>64 不画弱线；上下按钮 ×2/÷2）")
                }
                BbToolButton {
                    text: uiActions.label("view.toggleGrid")
                    // 外部态驱动高亮（外部激活而非 checkable 自翻，避免断绑定，doc/development.md 五、）
                    active: window.showGrid
                    enabled: window.uiStateTick >= 0 && uiActions.enabled("view.toggleGrid")
                    onClicked: uiActions.invoke("view.toggleGrid")
                    ToolTip.visible: hovered
                    ToolTip.text: qsTr("开/关槽位弱线（网格显示开关；吸附不依赖此开关）")
                }
                // 变换动作（doc/action-registry.md 九、 工具条注册化）：按注册表 transform 组枚举渲染，
                // 皮肤/注册表新增变换动作即自动出现在页面工具条（无需改 QML）。
                Repeater {
                    model: uiActions.idsByToolbar("transform")
                    delegate: BbToolButton {
                        property string actId: modelData
                        text: uiActions.label(actId)
                        enabled: window.uiStateTick >= 0 && uiActions.enabled(actId)
                        onClicked: uiActions.invoke(actId)
                        ToolTip.visible: hovered
                        ToolTip.text: uiActions.tooltip(actId)
                    }
                }
                // 更多轨道：BGA 图层通道列（04/06/07/0A，游玩轨与背景轨之间，iBMSC 式）
                BbCheckBox {
                    id: extrasCheck
                    text: uiActions.label("view.toggleExtras")
                    checked: window.uiStateTick >= 0 && uiActions.checked("view.toggleExtras")
                    onToggled: uiActions.invoke("view.toggleExtras")
                    ToolTip.visible: hovered
                    ToolTip.text: qsTr("在游玩轨与背景轨之间显示 BGA 图层通道（BGA/LAYER/POOR/LAYER2 = 04/06/07/0A）")
                }
                BbToolButton { text: qsTr("缩放 %1%").arg(editPage.zoomPercent)
                               enabled: chartMeta !== null
                               onClicked: { editPage.resetZoom(); setStatus(qsTr("缩放已重置")) }
                               ToolTip.visible: hovered
                               ToolTip.text: qsTr("当前缩放（点击恢复 100% = 小节高度 96px）；-/= 或 Ctrl+滚轮缩放（最大 500%）") }
                BbCheckBox {
                    text: qsTr("光标缩放")
                    checked: window.zoomToCursor
                    onToggled: window.zoomToCursor = checked
                    ToolTip.visible: hovered
                    ToolTip.text: qsTr("勾选=Ctrl+滚轮缩放时往鼠标位置放大（锚点=光标拍位）；"
                                       + "未勾=固定往视口中心放大") }
                Item { Layout.fillWidth: true }
                // ---- M5 播放控制（工具条右区）：跟随 / A/B 循环 / 播放 / 暂停 ----
                BbCheckBox {
                    text: qsTr("跟随")
                    checked: editPage.followPlayhead
                    onToggled: editPage.setFollowPlayhead(checked)
                    visible: chartMeta !== null
                    ToolTip.visible: hovered
                    ToolTip.text: qsTr("播放时视口锁定跟随播放头（底部 20% 固定高度）；用户滚动自动关闭")
                }
                BbToolButton { text: qsTr("A")
                               enabled: chartMeta !== null
                               highlighted: audioEngine && audioEngine.loopA >= 0
                               onClicked: {
                                   var on = audioEngine.setLoopA(window.currentPosSec())
                                   setStatus(on ? qsTr("循环起点 A = %1 秒（再点解除）").arg(audioEngine.loopA.toFixed(2))
                                                : qsTr("循环起点 A 已解除"))
                               }
                               ToolTip.visible: hovered
                               ToolTip.text: qsTr("设循环起点 A（红线/播放头位置；再点解除）") }
                BbToolButton { text: qsTr("B")
                               enabled: chartMeta !== null
                               highlighted: audioEngine && audioEngine.loopB >= 0
                               onClicked: {
                                   var on = audioEngine.setLoopB(window.currentPosSec())
                                   setStatus(on ? qsTr("循环终点 B = %1 秒（再点解除）").arg(audioEngine.loopB.toFixed(2))
                                                : qsTr("循环终点 B 已解除"))
                               }
                               ToolTip.visible: hovered
                               ToolTip.text: qsTr("设循环终点 B（红线/播放头位置；再点解除）") }
                BbToolButton { text: audioEngine && audioEngine.playing ? qsTr("⏸") : qsTr("▶")
                               enabled: chartMeta !== null
                               onClicked: togglePlayback()
                               highlighted: audioEngine && audioEngine.playing
                               ToolTip.visible: hovered
                               ToolTip.text: qsTr("播放/暂停（Space）") }
                Label { text: window.modeLabel(); color: Theme.accent; font.family: Theme.fontMono
                        font.pixelSize: Theme.fsSmall; padding: 4 }
            }
        }

        // 编辑工具条（编辑页专属，flat 工具选择样式）
        ToolBar {
            visible: currentPage === 0
            Layout.fillWidth: true
            background: Rectangle {
                color: Theme.surface2
                Rectangle { anchors.left: parent.left; anchors.right: parent.right
                             anchors.bottom: parent.bottom; height: 1; color: Theme.border }
            }
            RowLayout {
                anchors.fill: parent
                anchors.leftMargin: 8
                anchors.rightMargin: 8
                spacing: 6
                Label { text: qsTr("工具"); color: Theme.textFaint
                        font.pixelSize: Theme.fsTiny; padding: 4 }
                // 互斥单选：active = 外部状态（editorTool），无 checkable 断绑残留问题。
                // 工具选择条按注册表 tool 组枚举渲染（doc/action-registry.md 九、）：prefix=快捷键前缀，
                // value=当前工具值（互斥 active 判定）；新增工具只需注册进组。
                Repeater {
                    model: uiActions.idsByToolbar("tool")
                    delegate: BbToolButton {
                        property string actId: modelData
                        text: uiActions.prefix(actId) + uiActions.label(actId)
                        active: window.editorTool === uiActions.value(actId)
                        flatStyle: true
                        onClicked: uiActions.invoke(actId)
                        ToolTip.visible: hovered
                        ToolTip.text: uiActions.tooltip(actId)
                    }
                }
                // 平移 = 轴锁定开关（不占工具位、非门控）：勾选后拖拽选中 note 按方向主轴
                // 移动——纵向=时间（note.move，通道不变）；横向=通道（delete+put，时间不变）。
                // 未勾选 = 自由 2D（时间+通道都动）。无论勾选与否，拖拽选中 note 都可移动。
                BbCheckBox {
                    id: moveModeCheck
                    text: qsTr("平移")
                    checked: window.moveMode
                    onToggled: window.moveMode = checked
                    ToolTip.visible: hovered
                    ToolTip.text: qsTr("勾选=按方向轴锁定移动（纵向→时间/通道不变，横向→通道/时间不变）；"
                                     + "未勾选=自由 2D 移动（时间+通道都动）。拖拽选中 note 恒可移动")
                }
                BbCheckBox {
                    text: qsTr("波形")
                    checked: window.showNoteWaveforms
                    onToggled: window.showNoteWaveforms = checked
                    ToolTip.visible: hovered
                    ToolTip.text: qsTr("选中 note 的采样波形（按采样时长展开；异步解码）")
                }
                Item { Layout.fillWidth: true }
                // LN 选取模式（默认关）：勾选后点选 LN 任一段，自动选中配对两端（可整体移动/删除）
                BbCheckBox {
                    id: lnSelectCheck
                    text: qsTr("LN 选取")
                    checked: window.lnSelectMode
                    onToggled: window.lnSelectMode = checked
                    ToolTip.visible: hovered
                    ToolTip.text: qsTr("勾选=点 LN 任一段自动选中配对两端（整体移动/删除）；未勾=LNs 当单 note")
                }
                // 单点 ↔ LN 转换（2026-09 用户）：选中游玩轨 note 一键转换（注册表动作，仅 LN 轨）
                BbToolButton {
                    text: uiActions.label("tool.toggleLn")
                    enabled: window.uiStateTick >= 0 && uiActions.enabled("tool.toggleLn")
                    onClicked: uiActions.invoke("tool.toggleLn")
                    ToolTip.visible: hovered
                    ToolTip.text: qsTr("按 LNTYPE 切换选中 note 的 LN 通道（LNTYPE 1：普通↔5x/6x）；"
                                       + "配对由同通道时间序交替自动组成（无向前查询）")
                }
                // 轨道名 → 实际通道 id（皿=16、键1=11、BGM=01…；Ctrl 临时切换，Adobe 式）
                BbCheckBox {
                    id: channelIdCheck
                    text: uiActions.label("view.toggleChannelIds")
                    checked: window.uiStateTick >= 0 && uiActions.checked("view.toggleChannelIds")
                    onToggled: uiActions.invoke("view.toggleChannelIds")
                    ToolTip.visible: hovered
                    ToolTip.text: qsTr("轨道列头显示 BMS 通道号（Ctrl 按住临时显示；Alt 会被菜单栏拦截）")
                }
                // note 上显示所用采样（隐藏 / 显示 id / 显示文件名）
                Label { text: qsTr("采样"); color: Theme.textFaint
                        font.pixelSize: Theme.fsTiny; padding: 4 }
                BbComboBox {
                    model: [qsTr("隐藏"), qsTr("显示 ID"), qsTr("显示文件名")]
                    currentIndex: window.noteSampleMode
                    onActivated: (idx) => window.noteSampleMode = idx
                    Layout.preferredWidth: 130
                }
                // 时序值（路线 A：BPM/STOP 值放置；选中逻辑方案2 = 独立于采样选中——
                // 在 BPM 轨放置用 BPM 值、STOP 轨用 STOP 值，互不冲突）
                Label { text: qsTr("时序"); color: Theme.textFaint
                        font.pixelSize: Theme.fsTiny; padding: 4 }
                BbTextField {
                    id: bpmValueField
                    Layout.preferredWidth: 52
                    text: String(window.currentBpmValue)
                    validator: DoubleValidator { bottom: 0.001; top: 9999; locale: "C" }
                    onEditingFinished: {
                        const v = parseFloat(text)
                        if (isFinite(v) && v > 0) window.currentBpmValue = v
                        else text = String(window.currentBpmValue)
                    }
                    ToolTip.visible: hovered
                    ToolTip.text: qsTr("当前 BPM 值（点击时间轴 BPM 列放置；Enter/失焦生效）")
                }
                BbTextField {
                    id: stopValueField
                    Layout.preferredWidth: 64
                    text: window.stopToDisplay(window.currentStopValue)
                    validator: DoubleValidator { bottom: 0; top: 99999999; locale: "C" }
                    onEditingFinished: {
                        const us = window.stopFromDisplay(text)
                        if (isFinite(us) && us >= 0) {
                            window.currentStopValue = us
                            text = window.stopToDisplay(window.currentStopValue)
                        } else text = window.stopToDisplay(window.currentStopValue)
                    }
                    ToolTip.visible: hovered
                    ToolTip.text: window.stopUnit === 0
                                  ? qsTr("当前 STOP 值（单位 = 1/192 全音符，随该拍位 BPM 换算；点击时间轴 STOP 列放置）")
                                  : qsTr("当前 STOP 值（毫秒，按当前 BPM 换算；点击时间轴 STOP 列放置）")
                }
                BbToolButton {
                    text: window.stopUnit === 0 ? qsTr("unit") : qsTr("ms")
                    Layout.preferredHeight: stopValueField.implicitHeight + 6
                    onClicked: {
                        window.stopUnit = window.stopUnit === 0 ? 1 : 0
                        stopValueField.text = window.stopToDisplay(window.currentStopValue)
                    }
                    ToolTip.visible: hovered
                    ToolTip.text: qsTr("STOP 值单位切换：unit = 1/192 全音符（BMS 原生，随当前 BPM）/ ms = 毫秒（按当前 BPM）")
                }
                // 当前采样（M3 放置落点；检索/选择在左 Dock 采样面板）+ 文件路径 → 底部状态栏右侧，
                // 不在工具条（2026-09：避免工具条长度随文件路径/采样名变化，1280 宽下占满抖区）
            }
        }

        // 页面内容区（切页 = 切视图，doc/ui-design.md 二、）
        StackLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            currentIndex: window.currentPage
            EditPage {
                id: editPage
                chartMeta: window.chartMeta
                chartPath: window.chartPath
                modeLabel: window.modeLabel()
                showChannelIds: window.showChannelIds
                noteSampleMode: window.noteSampleMode
                lnSelectMode: window.lnSelectMode
                showExtras: window.showExtras
                showGrid: window.showGrid
                editorTool: window.editorTool
                moveMode: window.moveMode
                showNoteWaveforms: window.showNoteWaveforms
                sampleId: chartSession.sampleValueOf(window.currentSampleId)
                sampleText: sampleModel.currentSampleText
                selection: window.selectionRefs
                metaSelection: window.metaSelection
                timingBpm: window.timingBpm
                timingStop: window.timingStop
                timingMeasure: window.timingMeasure
                timingBpmDefs: window.timingBpmDefs
                timingStopDefs: window.timingStopDefs
                snapNum: window.snapNum
                snapDen: window.snapDen
                stopUnit: window.stopUnit
                stopBpm: (chartMeta && chartMeta.BPM !== undefined) ? parseFloat(chartMeta.BPM) : 130
                zoomToCursor: window.zoomToCursor
                perfLog: window.debugPerfLog
                onMetaDraftDirtyChanged: window.metaDraftDirty = metaDraftDirty
                onSamplePicked: (id, file) => {
                    // 会话状态：当前采样（M3 放置落点；不入 undo，doc/ui-design.md 一、.2）
                    window.currentSampleId = id
                    setStatus(qsTr("当前采样：#WAV%1 %2").arg(id).arg(file))
                    // M4.1 试听：单击采样行 → 播放该采样文件（缺失/解码失败 → statusText）
                    if (file !== "" && typeof audioEngine !== "undefined" && audioEngine) {
                        // 只对「绑定文件」的行试听；未绑定槽位（file 空）不播（lint 已报）
                        audioEngine.playPreview(file)
                    }
                }
                onSampleFileRequested: (id, file) => setSampleFile(id, file)
                onSampleAddRequested: (id, file) => addWavSample(id, file)
                onHitPlaceRequested: (hit) => placeNote(hit)
                onSelectionFinished: (refs) => onSelectionMade(refs)
                onNoteClicked: (ref, ctrl) => window.onNoteClicked(ref, ctrl)
                onPlayNoteSample: (ref) => session.playNoteSample(ref)
                onCanvasClicked: () => window.onCanvasClicked()
                onNoteRightDeleted: (ref) => deleteNoteAt(ref)
                onNoteEditRequested: (ref) => editNoteSample(ref)
                onMoveSelectionRequested: (deltaF, targetLane, sourceLane, sourceBgmLine) => moveSelection(deltaF, targetLane, sourceLane, sourceBgmLine)
                onMetaObjectClicked: (obj, ctrl) => onMetaClicked(obj, ctrl)
                onMetaMoveRequested: (kind, obj, deltaF, targetLane) => moveMetaObject(kind, obj, deltaF, targetLane)
                onMetaRightDeleted: (obj) => deleteMetaObject(obj)
                onMetaEditRequested: (obj) => editMetaObject(obj)
                onMetaMessage: (msg) => setStatus(msg)
                onMetaSaveRequested: saveMetaEdits()
                onModeEditRequested: (key, value) => setModeMeta(key, value)
                onEditAreaPressed: clearTextFocus()
                onTimingEditRequested: (kind, measure, num, den, value, ref) =>
                    editTiming(kind, measure, num, den, value, ref)
                onTimingDeleteRequested: (kind, measure, num, den) =>
                    deleteTiming(kind, measure, num, den)
                onTimingDefAddRequested: (kind, id, value) =>
                    timingDefAdd(kind, id, value)
                onTimingDefDeleteRequested: (kind, id) =>
                    timingDefDelete(kind, id)
                onBgaEditRequested: (layer, measure, num, den, bmpId) =>
                    editBga(layer, measure, num, den, bmpId)
                onBgaDeleteRequested: (layer, measure, num, den) =>
                    deleteBga(layer, measure, num, den)
                onBmpAddRequested: (id, file) => bmpAdd(id, file)
                onBmpSetFileRequested: (id, file) => bmpSetFile(id, file)
                onBmpRenameRequested: (fromId, toId) => bmpRename(fromId, toId)
                onBmpDeleteRequested: (id) => bmpDelete(id)
                onBmpSelected: (id) => setCurrentBmp(id)
            }
            SlicePage {
                id: slicePage
                // M6.4f 快捷键门控（父注入：跨文档绑定里无法引用 window 的 id）：
                // 页面激活（currentPage）&& 无文本输入焦点（SpinBox 聚焦时 activeFocusItem
                // 是其 TextInput → textInputFocused=true → ↑↓ 调值不被快捷键抢）
                kbdPageActive: window.currentPage === 1 && !window.textInputFocused
            }
            TestPage {}
        }

        // 底部页面条（固定，Resolve 式页面切换）
        PageSwitcher {
            Layout.fillWidth: true
            currentPage: window.currentPage
            onPageRequested: (index) => window.currentPage = index
        }

        // 状态栏（固定全局）
        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: 28
            color: Theme.surface2
            border.color: Theme.border
            // 编辑页/其它页状态（现状）；切音页改下方专用摘要（currentPage === 1 互斥）。
            RowLayout {
                anchors.fill: parent
                anchors.leftMargin: 10
                anchors.rightMargin: 10
                spacing: 8
                visible: window.currentPage !== 1
                // 主消息（可变长，占满剩余空间，右截断）：悬停信息 > 状态消息。
                Label {
                    text: editPage.hoverText !== "" ? editPage.hoverText : window.statusText
                    color: editPage.hoverText !== "" ? Theme.accent : Theme.textMuted
                    elide: Text.ElideRight
                    Layout.fillWidth: true
                    Layout.minimumWidth: 60
                    font.family: Theme.fontMono
                    font.pixelSize: Theme.fsSmall
                }
                // —— 可变长内容（右区左段，窄窗收缩+截断）：采样名、文件路径 ——
                Label {
                    text: sampleModel.currentSampleText
                    color: Theme.accent
                    font.family: Theme.fontMono
                    font.pixelSize: Theme.fsSmall
                    elide: Text.ElideMiddle
                    visible: sampleModel.currentSampleText !== ""
                    Layout.preferredWidth: 170
                    Layout.minimumWidth: 60
                    Layout.maximumWidth: 220
                }
                Label {
                    text: chartPath ? chartPath : qsTr("未打开谱面")
                    color: Theme.textFaint
                    elide: Text.ElideMiddle
                    font.pixelSize: Theme.fsSmall
                    Layout.preferredWidth: 200
                    Layout.minimumWidth: 90
                    Layout.maximumWidth: 320
                }
                // —— 固定长度令牌（右区右段，right-aligned，自然宽度不截断）：SP7K/格式/编码 ——
                // M5 收尾 2026-09：有播放头（已渲染/已播放，playheadSec>=0）→ 时间/小节 = **播放头**
                // （随播放推进；暂停停在播放位置，无视"跟随"开关）；未渲染（playheadSec<0）→ 视口光标。
                Label {
                    visible: typeof chartSession !== "undefined" && chartSession && chartSession.hasChart
                    text: "小节 " + (editPage.playheadPosText !== ""
                                  ? editPage.playheadPosText
                                  : (editPage.cursorPosText !== "" ? editPage.cursorPosText : "—")) +
                          " ｜ " + window.fmtTime(editPage.playheadSec >= 0
                                  ? editPage.playheadSec : editPage.cursorSec) +
                          (typeof audioEngine !== "undefined" && audioEngine && audioEngine.hasPcm
                                ? " / " + window.fmtTime(audioEngine.durationSec) : "")
                    color: audioEngine.playing ? Theme.accent : Theme.textFaint
                    font.family: Theme.fontMono
                    font.pixelSize: Theme.fsSmall
                    Layout.rightMargin: 2
                }
                Label {
                    text: chartMeta ? window.modeLabel() + "·" + (chartMeta.PLAYER !== undefined ? chartMeta.PLAYER : "") : ""
                    color: Theme.textFaint; font.family: Theme.fontMono; font.pixelSize: Theme.fsSmall
                    Layout.rightMargin: 2
                }
                Label {
                    text: window.chartFormat !== "" ? window.chartFormat.toUpperCase() : ""
                    color: Theme.textFaint; font.family: Theme.fontMono; font.pixelSize: Theme.fsSmall
                    visible: window.chartFormat !== ""
                }
                Label {
                    text: window.chartEncoding !== "" ? window.chartEncoding : ""
                    color: Theme.textFaint; font.family: Theme.fontMono; font.pixelSize: Theme.fsSmall
                    visible: window.chartEncoding !== ""
                }
            }
            // 切音页专用摘要（视口/光标/参考音频/播放/切片/MIDI；与上方编辑页状态互斥）
            SliceStatusBar {
                anchors.fill: parent
                anchors.leftMargin: 10
                anchors.rightMargin: 10
                visible: window.currentPage === 1
                page: slicePage
            }
        }
    }

    // ---------- 状态消息（瞬态：setStatus 后 ~8s 自动回「就绪/工具提示」，避免常驻噪声） ----------
    Timer {
        id: statusClearTimer
        interval: 8000
        onTriggered: window.statusText = window.lnHintActive() ? window.lnHint() : qsTr("就绪")
    }
    function setStatus(msg) {
        window.statusText = msg
        statusClearTimer.restart()
    }
    Connections {
        target: typeof chartSession !== "undefined" ? chartSession : null
        function onAuditionFinished(ok, error) {
            if (ok) {
                if (typeof audioEngine !== "undefined" && audioEngine) audioEngine.auditionPlay()
                setStatus(qsTr("整轨试听：选中 %1 个 note").arg(window.selectionRefs.length))
            } else {
                setStatus(qsTr("整轨试听失败：%1").arg(error))
            }
        }
    }
    /// LN 放置提示是否该显示（当前为 LN 工具 + 已打开谱面）。
    function lnHintActive() { return window.editorTool === "ln" && window.chartMeta !== null }
    /// LN 放置提示文本（LNTYPE 2 = 头尾状态机；其余 = 连点两次自动配对）。
    /// 附当前 LNTYPE/LNOBJ 解析值——用户可据此确认会话里 #LNTYPE 是否真的为 2
    ///（元信息改了须点「保存」才写入会话；否则仍走 LNTYPE 1）。
    function lnHint() {
        const t = (typeof chartSession !== "undefined" && chartSession) ? chartSession.lnType() : 1
        const lj = (typeof chartSession !== "undefined" && chartSession) ? chartSession.lnobjSample() : -1
        const mode = t === 2 ? "LNTYPE2" : (t === 0 ? "LNTYPE0" : "LNTYPE1")
        const ljText = t === 2 ? (lj >= 0 ? " · LNOBJ #WAV" + chartSession.idTextOf(lj) : " · LNOBJ 未定义") : ""
        return (t === 2 ? qsTr("同轨道左键放置Ln尾，不同轨道左键重新放置Ln头，按esc键取消")
                        : qsTr("同一轨道连点两次：先头后尾（自动配对）"))
               + qsTr("〔%1%2〕").arg(mode).arg(ljText)
    }
    /// 进入 LN 工具：停掉自动清空，直接显示放置说明（持久直到切工具）。
    function activateLnHint() {
        statusClearTimer.stop()
        window.statusText = window.lnHint()
    }

    // ---------- 第一条真链路：打开谱面 → dispatch(info) → 元信息 ----------
    FileDialog {
        id: fileDialog
        title: qsTr("打开 BMS 谱面")
        nameFilters: [qsTr("BMS 谱面 (*.bms *.bml *.bme *.pms)"), qsTr("所有文件 (*)")]
        onAccepted: doOpenChart(urlToPath(selectedFile))
        onRejected: { /* 用户取消 */ }
    }

    // --open 调试参数（main.cpp 注入）：走与 Ctrl+O 相同的调用路径
    property string debugOpenPath: ""
    onDebugOpenPathChanged: if (debugOpenPath !== "") doOpenChart(debugOpenPath)
    // 外部文件（双击文件关联 / 把文件拖到 exe 图标：Explorer 把路径作为 argv[1] 传入）：
    // 与「拖拽入窗口」共用同一路由函数，按后缀分流（谱面 → 编辑页；音频/MIDI → 切音工作台）。
    property string externalFilePath: ""
    onExternalFilePathChanged: if (externalFilePath !== "") handleExternalFile(externalFilePath)
    // M6.1 调试参数：--slice-audio / --slice-midi（切音页导入；配 --page 1 --screenshot）
    property string debugSliceAudio: ""
    property string debugSliceMidi: ""
    onDebugSliceAudioChanged: if (debugSliceAudio !== "") sliceWorkspace.loadAudioFile(debugSliceAudio)
    onDebugSliceMidiChanged: if (debugSliceMidi !== "") sliceWorkspace.loadMidiFile(debugSliceMidi)
    // M6.2 调试参数：--slice-detect grid|midi（解码完成后自动生成切片；配 --page 1 --screenshot）
    property string debugSliceDetect: ""
    onDebugSliceDetectChanged: if (debugSliceDetect !== "") doDebugSliceDetect()
    // 解码为异步（QThreadPool），音频就绪时间不定 → repeat 直到 hasAudio（不再只试一次）
    Timer { id: sliceDetectRetry; interval: 300; repeat: true; onTriggered: doDebugSliceDetect() }
    function doDebugSliceDetect() {
        if (!sliceWorkspace.hasAudio) {
            sliceDetectRetry.start()
            return
        }
        sliceDetectRetry.stop()
        const src = (debugSliceDetect === "midi") ? "midi" : "grid"
        slicePage.debugSetSource(src)   // 与 UI 同态（切片源 + MIDI 线默认）
        sliceWorkspace.detectSlices(src, 120.0, 4, sliceWorkspace.audioDurationSec,
                                    slicePage.midiExtendNext)
    }
    // M6.2 调试参数：--slice-extend <0|1>（MIDI 「到下一起点」开关；--slice-detect midi 前注入）
    property int debugSliceExtend: -1
    onDebugSliceExtendChanged: if (debugSliceExtend >= 0) slicePage.midiExtendNext = (debugSliceExtend !== 0)
    // M6.2 调试参数：--slice-offset <ms>（可复现 offset 实时效果：网格/MIDI 刻度随 offset 平移）
    property real debugSliceOffset: -1
    onDebugSliceOffsetChanged: if (debugSliceOffset >= 0) sliceWorkspace.setOffsetSec(debugSliceOffset / 1000.0)
    // M6.3 调试参数：--slice-export <起始id>（切片就绪后自动导出分片 + 输出可复制 raw；配 --slice-detect）
    property int debugSliceExport: -1
    onDebugSliceExportChanged: if (debugSliceExport >= 0) doDebugSliceExport()
    // M6.3 调试参数：--slice-place <0|1>（导出时同时铺入编辑区谱面；配 --slice-export）
    property int debugSlicePlace: -1
    onDebugSlicePlaceChanged: if (debugSlicePlace >= 0) slicePage.placeToChart = (debugSlicePlace !== 0)
    // M6.4f 切音动作：生产注册表直接调用正式入口；--slice-act 仍使用下方调试队列。
    function dispatchSliceAction(act) {
        slicePage.dispatchSliceAction(act)
    }
    property string debugSliceAct: ""
    property var _debugActQueue: []
    // M6.4f 焦点区域高亮验收：--focus-region <editLeft|editCenter|editRight|sliceLeft|sliceCenter|sliceRight>
    property string debugFocusRegion: ""
    onDebugFocusRegionChanged: if (debugFocusRegion.length > 0) {
        if (typeof editPage !== "undefined" && editPage)
            editPage.focusRegionId = debugFocusRegion
        if (typeof slicePage !== "undefined" && slicePage)
            slicePage.focusRegionId = debugFocusRegion
    }
    Timer {
        id: debugActFlush
        interval: 200
        repeat: true
        onTriggered: {
            if (_debugActQueue.length === 0) { stop(); return }
            if (!sliceWorkspace.hasSlices && _debugActQueue[0] !== "playPause" &&
                _debugActQueue[0] !== "rowUp" && _debugActQueue[0] !== "rowDown" &&
                _debugActQueue[0] !== "beatLeft" && _debugActQueue[0] !== "beatRight")
                return  // 等切片就绪（解码/检测异步）
            slicePage.dispatchSliceAction(_debugActQueue.shift())
        }
    }
    onDebugSliceActChanged: if (debugSliceAct.length > 0) {
        _debugActQueue.push(debugSliceAct)
        debugSliceAct = ""
        debugActFlush.start()
    }
    // 2026-09「同时铺入编辑区」成功 → 刷新左 dock 采样列表（#WAV 定义新入谱面；
    // contentChanged 只刷新时间轴/波形，定义表须显式重取 session.samples）。
    Connections {
        target: sliceWorkspace
        function onSamplesPlaced() { session.refreshSamples() }
        function onSliceHistoryChanged() { updateActionStates() }
        function onSlicesChanged() { updateActionStates() }
        function onAudioChanged() { updateActionStates() }
    }
    Timer { id: sliceExportRetry; interval: 300; repeat: true; onTriggered: doDebugSliceExport() }
    function doDebugSliceExport() {
        if (!sliceWorkspace.hasAudio || !sliceWorkspace.hasSlices) { sliceExportRetry.start(); return }
        sliceExportRetry.stop()
        // 走页面同一路径（导出 + raw 进右 dock + 状态行）；结果打日志备查
        slicePage.debugExport(debugSliceExport)
        const r = slicePage.exportResult
        console.log("debug slice export: ok=" + r.ok + " count=" + r.count +
                    (r.error ? " err=" + r.error : "") + "\n" + r.raw)
    }
    // M6.3c 调试参数：--slice-tab N（切音页左 dock 页签验收）
    property int debugSliceDockTab: -1
    onDebugSliceDockTabChanged: if (debugSliceDockTab >= 0) slicePage.dockTab = debugSliceDockTab
    // 2026-09 BbDialog 主题化验收：--open-dialog <about|note|meta>
    property string debugOpenDialog: ""
    onDebugOpenDialogChanged: if (debugOpenDialog.length > 0) {
        if (debugOpenDialog === "about") aboutDialog.open()
        else if (debugOpenDialog === "note") noteSampleDialog.open()
        else if (debugOpenDialog === "meta") metaEditDialog.open()
    }
    // M6.4 调试参数：--slice-zoom N / --slice-row N（换行视口验收）
    property int debugSliceZoom: -1
    onDebugSliceZoomChanged: if (debugSliceZoom >= 0)
        slicePage.zoomIndex = Math.max(0, Math.min(slicePage.zoomLevels.length - 1, debugSliceZoom))
    property int debugSliceRow: -1
    onDebugSliceRowChanged: if (debugSliceRow >= 0) slicePage.scrollRow = debugSliceRow
    // M6.5 调试参数：--slice-edit <index>（打开切片编辑对话框）/ --slice-edit-apply <i:startMs:durMs>
    property int debugSliceEdit: -1
    onDebugSliceEditChanged: if (debugSliceEdit >= 0) doDebugSliceEdit()
    Timer { id: sliceEditRetry; interval: 300; repeat: true; onTriggered: doDebugSliceEdit() }
    function doDebugSliceEdit() {
        if (!sliceWorkspace.hasAudio || !sliceWorkspace.hasSlices) { sliceEditRetry.start(); return }
        sliceEditRetry.stop()
        slicePage.openSliceEdit(debugSliceEdit)
    }
    property string debugSliceEditApply: ""
    onDebugSliceEditApplyChanged: if (debugSliceEditApply.length > 0) doDebugSliceEditApply()
    Timer { id: sliceEditApplyRetry; interval: 300; repeat: true; onTriggered: doDebugSliceEditApply() }
    function doDebugSliceEditApply() {
        if (!sliceWorkspace.hasAudio || !sliceWorkspace.hasSlices) { sliceEditApplyRetry.start(); return }
        sliceEditApplyRetry.stop()
        const p = debugSliceEditApply.split(":")
        if (p.length < 3) return
        sliceWorkspace.setSliceBounds(parseInt(p[0], 10),
                                      parseFloat(p[1]) / 1000.0,
                                      parseFloat(p[2]) / 1000.0)
    }
    // M6.4c 调试参数：--slice-toggle-point <秒>（手动切分点切换验收；等待音频就绪——
    // 纯手动模式无切片也可加点）
    property real debugSliceTogglePoint: -1
    onDebugSliceTogglePointChanged: if (debugSliceTogglePoint >= 0) doDebugTogglePoint()
    Timer { id: sliceToggleRetry; interval: 300; repeat: true; onTriggered: doDebugTogglePoint() }
    function doDebugTogglePoint() {
        if (!sliceWorkspace.hasAudio) { sliceToggleRetry.start(); return }
        sliceToggleRetry.stop()
        sliceWorkspace.toggleManualPoint(debugSliceTogglePoint)
    }
    // M6.4d 调试参数：--slice-select-beat <秒>（选中拍子光标验收）
    property real debugSliceSelectBeat: -1
    onDebugSliceSelectBeatChanged: if (debugSliceSelectBeat >= 0)
        slicePage.debugSelectBeat(debugSliceSelectBeat)
    // --wait-render 截图等待标志（main.cpp 轮询；波形验收用）
    property bool debugRenderDone: false
    // 渲染完成次数（--wait-render 增量验收：等待全量 + 增量都完成）
    property int debugRenderCount: 0
    // --settings 调试参数：启动即打开首选项（M4.2 设置页验收）
    property bool debugOpenSettings: false
    onDebugOpenSettingsChanged: if (debugOpenSettings) settingsDialog.open()
    // --settings-section <id>：打开首选项并切到指定分节（display/audio/editor/shortcut）
    property string debugSettingsSection: ""
    onDebugSettingsSectionChanged: if (debugSettingsSection.length > 0) {
        settingsDialog.currentSection = debugSettingsSection
        settingsDialog.open()
    }
    // --tool / --click / --probe 调试参数（配 --screenshot 验收点击链）：工具 + 一次模拟点击
    property string debugTool: ""
    property double debugClickX: -1
    property double debugClickY: -1
    property double debugProbeX: -1
    property double debugProbeY: -1
    onDebugToolChanged: if (debugTool !== "") window.editorTool = debugTool
    onDebugClickXChanged: debugMaybeClick()
    onDebugClickYChanged: debugMaybeClick()
    onDebugProbeXChanged: debugMaybeProbe()
    onDebugProbeYChanged: debugMaybeProbe()
    // --probe：等首帧渲染后调 ChartView.probe(x,y)，把命中结果打到 QML 日志（诊断定位用）
    function debugMaybeProbe() {
        if (debugProbeX < 0 || debugProbeY < 0) return
        probeTimer.restart()
    }
    Timer {
        id: probeTimer
        interval: 150
        onTriggered: {
            if (typeof editPage !== "undefined" && editPage && editPage.locateChartView) {
                const r = editPage.probeLocal(debugProbeX, debugProbeY)
                console.log("PROBE " + JSON.stringify(r))
            }
        }
    }
    function debugMaybeClick() {
        if (debugClickX < 0 || debugClickY < 0) return
        // 等 openChart + 首帧渲染（hitTest 依赖 paint 后的 m_colRects）
        debugClickTimer.restart()
    }
    Timer {
        id: debugClickTimer
        interval: 150
        onTriggered: editPage.clickLocal(debugClickX, debugClickY)
    }
    // --drag x1 y1 x2 y2：模拟拖拽（按下→移动→释放；同一分发路径，复现移动交互问题）
    property double debugDragX1: -1
    property double debugDragY1: -1
    property double debugDragX2: -1
    property double debugDragY2: -1
    /// --click-after-render <x> <y>：渲染完成后再模拟点击（增量重渲染验收：
    /// 先全量渲染 → 编辑 note → 触发 refresh → 自动增量渲染 → 波形更新）。
    property double debugClickAfterRenderX: -1
    property double debugClickAfterRenderY: -1
    onDebugClickAfterRenderXChanged: debugMaybeClickAfterRender()
    onDebugClickAfterRenderYChanged: debugMaybeClickAfterRender()
    function debugMaybeClickAfterRender() {
        if (debugClickAfterRenderX < 0 || debugClickAfterRenderY < 0) return
        debugClickAfterRenderTimer.restart()
    }
    Timer {
        id: debugClickAfterRenderTimer
        interval: 150
        onTriggered: {
            // 等 renderFinished（waveform.visible 置 true）后再点击
            if (!window.debugRenderDone) { debugClickAfterRenderTimer.restart(); return }
            editPage.clickLocal(debugClickAfterRenderX, debugClickAfterRenderY)
        }
    }
    /// --render <out.wav>：调试——启动后自动 renderChartToFile（复现 Space 渲染；M4.3b）
    property string debugRenderPath: ""
    onDebugRenderPathChanged: debugRenderTimer.restart()
    Timer {
        id: debugRenderTimer
        interval: 800  // 等 openChart + 首帧（渲染路径依赖 chartPath/chartSession）
        onTriggered: {
            if (debugRenderPath !== "") renderChartToFile()
        }
    }
    Timer {
        id: debugDragTimer
        interval: 150
        onTriggered: {
            if (typeof editPage !== "undefined" && editPage && editPage.locateChartView)
                editPage.locateChartView().dragAt(debugDragX1, debugDragY1, debugDragX2, debugDragY2)
        }
    }
    function debugMaybeDrag() {
        if (debugDragX1 < 0 || debugDragY1 < 0 || debugDragX2 < 0 || debugDragY2 < 0) return
        debugDragTimer.restart()
    }
    onDebugDragX1Changed: debugMaybeDrag()
    onDebugDragY1Changed: debugMaybeDrag()
    onDebugDragX2Changed: debugMaybeDrag()
    onDebugDragY2Changed: debugMaybeDrag()

    // 另存为（文件 → 另存为… / Ctrl+Shift+S）
    FileDialog {
        id: saveAsDialog
        title: qsTr("另存为 BMS 谱面")
        fileMode: FileDialog.SaveFile
        nameFilters: [qsTr("BMS 谱面 (*.bms *.bml *.bme *.pms)"), qsTr("所有文件 (*)")]
        // 所有 PMS 写出 profile 均需 .pms 后缀，重开才能自动识别 9key。
        defaultSuffix: window.isPmsChart ? "pms" : "bms"
        onAccepted: window.handleSaveAsAccepted(urlToPath(selectedFile))
        onRejected: {
            window.saveAsDocumentId = ""
            if (window.pendingDocAction !== "") cancelPendingDocumentAction()
        }
    }

    // 原生文件框先冻结文档身份；接受后等待模态确认不能被当成保存失败。
    property string saveAsDocumentId: ""
    property string pendingPmsSaveDocumentId: ""
    property string pendingPmsSavePath: ""
    property bool pendingPmsSaveAs: false
    property int pmsSaveRequestToken: 0
    readonly property bool pmsSaveConfirmationPending: pendingPmsSaveDocumentId !== ""
    function openSaveAsDialog() {
        if (window.pmsSaveConfirmationPending || window.saveAsDocumentId !== "" || !chartSession.hasChart) return
        window.saveAsDocumentId = chartSession.activeSessionId()
        saveAsDialog.open()
    }
    function handleSaveAsAccepted(path) {
        const id = window.saveAsDocumentId
        window.saveAsDocumentId = "" // 一次性消费，重复/过期 accepted 不会写入另一文档。
        if (id === "" || id !== chartSession.activeSessionId()) {
            setStatus(qsTr("文档已切换，另存为已取消；请重新选择保存目标"))
            if (id !== "") cancelPendingDocumentAction()
            return false
        }
        const result = saveChartAs(path)
        completeSaveAttempt(result)
        return result
    }
    function completeSaveAttempt(result) {
        if (result === undefined || window.pendingDocAction === "") return
        if (result) {
            finishPendingDocumentAction()
        } else {
            setStatus(qsTr("保存失败，未关闭/替换当前谱面"))
            cancelPendingDocumentAction()
        }
    }
    function requestPmsSuffixConfirmation(path, saveAs) {
        if (window.pmsSaveConfirmationPending) return
        window.pendingPmsSaveDocumentId = chartSession.activeSessionId()
        window.pendingPmsSavePath = path
        window.pendingPmsSaveAs = saveAs
        const token = ++window.pmsSaveRequestToken
        // 让原生FileDialog的accepted回调先退栈、释放原生模态层，再显示QML确认。
        Qt.callLater(function() {
            if (token === window.pmsSaveRequestToken && window.pmsSaveConfirmationPending)
                pmsSuffixConfirmDialog.open()
        })
    }
    function clearPmsSuffixConfirmation() {
        ++window.pmsSaveRequestToken
        window.pendingPmsSaveDocumentId = ""
        window.pendingPmsSavePath = ""
        window.pendingPmsSaveAs = false
    }
    function cancelPmsSuffixSave() {
        clearPmsSuffixConfirmation()
        if (window.pendingDocAction !== "") cancelPendingDocumentAction()
        setStatus(qsTr("已取消保存；请使用 .pms 后缀，当前更改和面板草稿仍保留"))
    }
    function confirmPmsSuffixSave() {
        const id = window.pendingPmsSaveDocumentId
        const path = window.pendingPmsSavePath
        const saveAs = window.pendingPmsSaveAs
        clearPmsSuffixConfirmation() // 本次授权仅消费一次，不能留下全局“已允许”状态。
        if (id === "" || id !== chartSession.activeSessionId() ||
                !chartSession.hasChart || (!saveAs && path !== window.chartPath)) {
            setStatus(qsTr("文档或目标已变化，保存已取消；请重新确认"))
            if (window.pendingDocAction !== "") cancelPendingDocumentAction()
            return false
        }
        const result = saveAs ? session.saveChartAs(path) : session.saveChart()
        completeSaveAttempt(result)
        return result
    }
    BbDialog {
        id: pmsSuffixConfirmDialog
        objectName: "pmsSuffixConfirmDialog"
        title: qsTr("PMS 保存后缀警告")
        width: 540
        height: 320
        showCancel: false
        closePolicy: Popup.CloseOnEscape
        onOpened: cancelPmsSaveButton.forceActiveFocus()
        onAccepted: window.confirmPmsSuffixSave()
        onRejected: window.cancelPmsSuffixSave()
        ScrollView {
            id: pmsSuffixTextScroll
            Layout.fillWidth: true
            Layout.fillHeight: true
            contentWidth: availableWidth
            ScrollBar.horizontal.policy: ScrollBar.AlwaysOff
            clip: true
            Label {
                width: pmsSuffixTextScroll.availableWidth
                text: qsTr("当前文档是 PMS 9key，但目标不是 .pms：\n%1\n\n仍将按当前 PMS 通道约定写出，不会转换成 SP/DP。关闭后重新打开将无法自动恢复 PMS 9key，音符可能被解释到错误轨道。\n\n建议取消并改用 .pms 后缀。是否仍按此后缀保存？").arg(window.pendingPmsSavePath)
                color: Theme.text
                wrapMode: Text.WrapAnywhere
                font.pixelSize: Theme.fsSmall
            }
        }
        footer: Rectangle {
            width: pmsSuffixConfirmDialog.width
            height: 42
            color: Theme.surface2
            RowLayout {
                anchors.fill: parent
                anchors.margins: 5
                Item { Layout.fillWidth: true }
                BbToolButton {
                    text: qsTr("仍按此后缀保存")
                    onClicked: pmsSuffixConfirmDialog.accept()
                }
                BbToolButton {
                    id: cancelPmsSaveButton
                    text: qsTr("取消保存")
                    onClicked: pmsSuffixConfirmDialog.reject()
                }
            }
        }
    }

    /// 关闭/新建/打开前的未保存确认。切音工作区是独立会话，不并入谱面脏。
    property string pendingDocAction: ""   // "" | "close" | "new" | "open"
    property string pendingOpenPath: ""
    /// 待建谱面的模式（2026-10 阶段一 issue #26）："" = 未定（确认未保存后弹新建对话框）；
    /// 非空 = 子菜单「新建为模式」直建该模式（跳过对话框）。
    property string pendingNewMode: ""
    property bool allowClose: false

    function documentIsDirty() {
        if (chartSession && chartSession.dirty) return true
        if (window.metaDraftDirty) return true
        return false
    }
    function documentDirtyLabel() {
        if (window.chartPath && window.chartPath.length > 0) {
            var slash = Math.max(window.chartPath.lastIndexOf("/"), window.chartPath.lastIndexOf("\\"))
            return slash >= 0 ? window.chartPath.slice(slash + 1) : window.chartPath
        }
        return qsTr("未命名谱面")
    }
    function requestDocumentAction(action, path) {
        if (window.pmsSaveConfirmationPending || window.saveAsDocumentId !== "" || unsavedConfirmDialog.visible) return
        window.pendingDocAction = action
        window.pendingOpenPath = path || ""
        if (!documentIsDirty()) {
            finishPendingDocumentAction()
            return
        }
        unsavedConfirmDialog.open()
    }
    function finishPendingDocumentAction() {
        const action = window.pendingDocAction
        const path = window.pendingOpenPath
        window.pendingDocAction = ""
        window.pendingOpenPath = ""
        if (action === "close") {
            window.allowClose = true
            window.close()
        } else if (action === "new") {
            // 模式已定（子菜单「新建为模式」直建）；未定（Ctrl+N）先弹新建对话框选模式
            if (window.pendingNewMode !== "") {
                const m = window.pendingNewMode
                window.pendingNewMode = ""
                performNewChart(m)
            } else {
                newChartDialog.open()
            }
        } else if (action === "open") {
            if (path && path.length > 0) doOpenChart(path)
            else fileDialog.open()
        }
    }
    function cancelPendingDocumentAction() {
        window.pendingDocAction = ""
        window.pendingOpenPath = ""
        window.pendingNewMode = ""
        window.allowClose = false
    }
    function saveThenContinue() {
        const saved = saveChart()
        if (saved === undefined) return // 等待文件选择/后缀确认，不失败、不放行后续操作。
        completeSaveAttempt(saved)
    }

    onClosing: function(close) {
        if (window.pmsSaveConfirmationPending || window.saveAsDocumentId !== "") {
            close.accepted = false
            return
        }
        if (window.allowClose || !documentIsDirty()) {
            window.allowClose = false
            return
        }
        close.accepted = false
        requestDocumentAction("close")
    }

    BbDialog {
        id: unsavedConfirmDialog
        title: qsTr("未保存的更改")
        width: 420
        height: 200
        showCancel: false
        Label {
            Layout.fillWidth: true
            text: qsTr("「%1」有未保存的更改。保存后继续，放弃更改，或取消。切音工作区不随谱面保存。").arg(window.documentDirtyLabel())
            color: Theme.text
            wrapMode: Text.WordWrap
            font.pixelSize: Theme.fsSmall
        }
        footer: Rectangle {
            width: unsavedConfirmDialog.width
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
                    text: qsTr("保存")
                    onClicked: {
                        unsavedConfirmDialog.close()
                        window.saveThenContinue()
                    }
                }
                BbToolButton {
                    text: qsTr("不保存")
                    onClicked: {
                        unsavedConfirmDialog.close()
                        window.finishPendingDocumentAction()
                    }
                }
                BbToolButton {
                    text: qsTr("取消")
                    onClicked: unsavedConfirmDialog.reject()
                }
            }
        }
        onRejected: window.cancelPendingDocumentAction()
    }

    // Base62 粘贴确认（2026-09 数据安全）：目标谱面 Base36 + 粘贴文本含 Base62 id（#BASE 62
    // 声明或小写 id）→ 先确认。两位 id 在两种进制下数值不同（如 10：Base62=62 / Base36=36），
    // 可能按错误 id 覆盖 #WAV 定义。取消可先去元信息设置 #BASE 62 或重新导出。
    BbDialog {
        id: modeSwitchDialog
        title: qsTr("切换谱面模式")
        width: 460
        height: 190
        property string pendingMode: ""
        property int pendingNotes: 0
        Label {
            Layout.fillWidth: true
            text: qsTr("切换为 %1？\n\n当前 %2 个 note 都能在新模式下表示。保存后通道号将按新表改写；此操作可撤销。")
                  .arg(window.modeDisplayName(modeSwitchDialog.pendingMode))
                  .arg(modeSwitchDialog.pendingNotes)
            color: Theme.text
            wrapMode: Text.WordWrap
            font.pixelSize: Theme.fsSmall
        }
        onAccepted: window.applyModeSwitch()
    }

    BbDialog {
        id: base62PasteDialog
        title: qsTr("Base62 粘贴提醒")
        width: 470
        height: 224
        showCancel: false
        property string pendingText: ""
        property int pendingTarget: 0
        Label {
            Layout.fillWidth: true
            text: qsTr("粘贴内容看起来是 Base62（含 #BASE 62 声明或小写 id），但当前谱面未声明 #BASE 62。\n\nBase36 会把两位 id 解释成不同数值（如 10：Base62=62，Base36=36），可能覆盖已有 #WAV 定义。\n\n建议取消，先在元信息里设置 #BASE 62，或用 Base36 重新导出。仍要继续吗？")
            color: Theme.text
            wrapMode: Text.WordWrap
            font.pixelSize: Theme.fsSmall
        }
        footer: Rectangle {
            width: base62PasteDialog.width
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
                    text: qsTr("仍要粘贴")
                    onClicked: {
                        const t = base62PasteDialog.pendingText
                        const m = base62PasteDialog.pendingTarget
                        base62PasteDialog.close()
                        session.pasteRawText(t, m)
                    }
                }
                BbToolButton {
                    text: qsTr("取消")
                    onClicked: base62PasteDialog.reject()
                }
            }
        }
    }

    // 新建谱面对话框（2026-10 阶段一 issue #26）：Ctrl+N 未保存确认后弹出，选游玩模式再建。
    // 模式列表同源 chartSession.codecModes()（Codec::modes / capabilities）；默认第一项 sp7k。
    BbDialog {
        id: newChartDialog
        title: qsTr("新建谱面")
        width: 400
        height: 200
        ColumnLayout {
            anchors.fill: parent
            anchors.margins: 12
            spacing: 8
            Label {
                text: qsTr("选择游玩模式（决定轨道布局与保存通道表）：")
                color: Theme.text
                font.pixelSize: Theme.fsSmall
            }
            BbComboBox {
                id: newModeCombo
                Layout.preferredWidth: 240
                Layout.fillWidth: true
                textRole: "label"
                model: {
                    var arr = []
                    const modes = chartSession.codecModes()
                    for (var i = 0; i < modes.length; ++i)
                        arr.push({ label: modeDisplayName(modes[i]), value: modes[i] })
                    return arr
                }
                currentIndex: 0
            }
            Item { Layout.fillHeight: true }
        }
        footer: Rectangle {
            width: newChartDialog.width
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
                    text: qsTr("新建")
                    onClicked: {
                        const m = newModeCombo.model[newModeCombo.currentIndex].value
                        newChartDialog.close()
                        performNewChart(m)
                    }
                }
                BbToolButton {
                    text: qsTr("取消")
                    onClicked: newChartDialog.reject()
                }
            }
        }
    }

    function doOpenChart(path) { openChart(path) }

    /// 外部文件路由（拖拽入窗口 / 拖到 exe 图标 / 双击关联）：分类 → 打开并切到对应页。
    /// 后缀分类在 EditUtils（C++ 纯函数，单测覆盖），QML 只做分流，避免后缀表两处维护。
    /// 返回是否已接受该文件（供 DropArea 决定 acceptProposedAction）。
    function handleExternalFile(path) {
        if (!path || path.length === 0) return false
        const kind = editUtils.classifyExternalFile(path)
        if (kind === "chart") {
            window.currentPage = 0
            doOpenChart(path)
            return true
        }
        if (kind === "audio") {
            window.currentPage = 1
            sliceWorkspace.loadAudioFile(path)   // 并发解码；成功/失败均经 statusText 反馈
            return true
        }
        if (kind === "midi") {
            window.currentPage = 1
            sliceWorkspace.loadMidiFile(path)    // 同步解析；成功/失败均经 statusText 反馈
            return true
        }
        setStatus(qsTr("不支持的文件类型：%1").arg(path))
        return false
    }

    function openChart(path) {
        var req = JSON.stringify({ command: "info", args: { path: path } })
        var resp = beatbench.dispatch(req)
        var r = JSON.parse(resp)
        if (r.ok) {
            window.chartMeta = r.result.meta
            // 当前 BPM 值兜底 = 元信息 BPM（路线 A 值放置默认）
            if (window.chartMeta && window.chartMeta.BPM !== undefined) {
                const b = parseFloat(window.chartMeta.BPM)
                if (isFinite(b) && b > 0) window.currentBpmValue = b
            }
            window.chartPath = r.result.path
            window.chartFormat = r.result.format !== undefined ? r.result.format : ""
            window.chartMode = r.result.mode !== undefined ? r.result.mode : ""
            // 编码：从 info/check 的 diagnostics（"encoding: UTF-8 (path)"）干净提取令牌
            var enc = ""
            if (r.result.diagnostics) {
                for (var di = 0; di < r.result.diagnostics.length; di++) {
                    var dm = r.result.diagnostics[di].message
                    if (dm && dm.indexOf("encoding:") === 0) {
                        var m = dm.match(/^encoding:\s*(\S+)/)
                        if (m) enc = m[1]
                        break
                    }
                }
            }
            window.chartEncoding = enc
            // M2 第 5 步：时间轴真数据（ChartSession + TimingEngine，与 info 同源解析）
            chartSession.openChart(path)
            window.pendingLnHead = null  // 换谱面：清除可能的未完成 LN 头
            window.selectionRefs = []
            window.metaSelection = []
            refreshTiming()  // 时间轴面板（BPM/STOP）数据源
            // 元信息可编辑表单载入（meta.list；session 已 load，此后编辑保存走 meta.edit）
            if (typeof editPage !== "undefined" && editPage) editPage.reloadMeta()
            // BGA 面板载入（#BMP 定义 + 当前层事件；bga.list/session.samples）
            if (typeof editPage !== "undefined" && editPage) editPage.reloadBga()
            // 当前 #BMP 默认 = 首个定义（视口 BGA 列放置用）；若无 #BMP 保持空（放置时提示）
            const sb = JSON.parse(beatbench.dispatch(JSON.stringify({ command: "session.samples", args: {} })))
            if (sb.ok && sb.result.samples && sb.result.samples.bmp && sb.result.samples.bmp.length > 0
                    && (window.currentBmpId === "" ||
                        !sb.result.samples.bmp.some(function (d) { return d.id === window.currentBmpId }))) {
                window.currentBmpId = sb.result.samples.bmp[0].id
                if (typeof editPage !== "undefined" && editPage) editPage.currentBmpId = window.currentBmpId
            }
            // ⚠️ 避免 multi-arg String.arg（QML 引擎会抛 Invalid arguments）：
            // 预计算 + 链式单参 .arg（经典稳妥形式）
            var wavCount = r.result.samples && r.result.samples.wav
                           ? r.result.samples.wav.length : 0
            window.statusText = qsTr("已打开：%1（%2 个采样）").arg(r.result.path).arg(wavCount)
            statusClearTimer.restart()
            // 采样面板 + lint 面板：info 的 wav 定义表 + check 的缺失/诊断（M2 第 4 步）
            sampleModel.loadFromInfo(resp)
            var checkResp = beatbench.dispatch(JSON.stringify({ command: "check", args: { path: path } }))
            sampleModel.loadFromCheck(checkResp)
            lintModel.loadFromCheck(checkResp)
            // M4.1 音频：设置采样相对路径解析基准（谱面目录）。setChartPath 内部
            // 用 QFileInfo 解析（兼容 / 与 \）——试听点击在 onSamplePicked 触发时
            // 经 audioEngine.playPreview(file) 解析。
            if (typeof audioEngine !== "undefined" && audioEngine) {
                audioEngine.setChartPath(r.result.path)
            }
            window.metaDraftDirty = false
        } else {
            window.chartMeta = null
            window.statusText = qsTr("打开失败：%1 %2").arg(r.error.code).arg(r.error.message)
            statusClearTimer.restart()
        }
    }

    // file:///C:/x → C:/x（Windows 本地路径）
    function urlToPath(url) {
        var s = url.toString()
        s = s.replace(/^file:\/\//, "")
        if (s.charAt(0) === "/" && /^\/[A-Za-z]:/.test(s))
            s = s.slice(1)
        return decodeURIComponent(s)
    }

    // M5 播放时间格式化（m:ss.cc；与秒标尺一致——秒以下十进制）
    function fmtTime(sec) {
        if (typeof sec !== "number" || !isFinite(sec) || sec < 0) sec = 0
        var m = Math.floor(sec / 60)
        var s = sec - m * 60
        var whole = Math.floor(s)
        var cs = Math.floor((s - whole) * 100)
        return m + ":" + (whole < 10 ? "0" : "") + whole + "." + (cs < 10 ? "0" : "") + cs
    }

    // M5 收尾 2026-09：当前播放位置（红线）秒 = **播放头**（已渲染/已播放用，随播放推进·暂停停原位）
    // 否则视口光标（未渲染回退）。播放起点/循环 A/B 都用它——保证「从播放线播放/设循环」跟红线一致。
    function currentPosSec() {
        return editPage.playheadSec >= 0 ? editPage.playheadSec : editPage.cursorSec
    }

    // 首选项入口（app.settings 注册表 handler；菜单/快捷键同源）
    function openSettings() { settingsDialog.open() }

    // ---------- M5：Space = 播放/暂停（渲染代理 PCM）；Ctrl+R = 手动渲染 ----------
    // ⚠️ 播放起点 = 循环 A（若设）否则当前播放位置（红线=播放头）——红线是当前编辑/播放位置（用户 2026-09：
    // 「没有 A 点则从播放线开始播放」）。点击 A/B 的 setLoopA/B 也传入当前红线位置（编辑处即循环点）。
    function togglePlayback() {
        if (typeof audioEngine === "undefined" || !audioEngine) return
        // #50：有选中 → 试听选中集；无选中 → 现有谱面播放。
        if (window.selectionRefs && window.selectionRefs.length > 0) {
            if (audioEngine.auditionPlaying && window.selectionRefs.length === 1)
                audioEngine.auditionStop()
            if (window.selectionRefs.length === 1) {
                if (audioEngine.playing) audioEngine.pause()
                session.playNoteSample(window.selectionRefs[0])
                return
            }
            if (audioEngine.auditionPlaying) {
                audioEngine.auditionStop()
                setStatus(qsTr("已停止整轨试听"))
                return
            }
            if (audioEngine.playing) audioEngine.pause()
            if (typeof chartSession !== "undefined" && chartSession &&
                    chartSession.renderAudition(window.selectionRefs, window.currentPosSec(), 44100.0)) {
                setStatus(qsTr("整轨试听：渲染中（选中 %1 个 note）").arg(window.selectionRefs.length))
            } else {
                setStatus(qsTr("整轨试听失败：选中 note 都在红线之前或无可试听内容"))
            }
            return
        }
        if (audioEngine.auditionPlaying) audioEngine.auditionStop()
        if (audioEngine.playing) { audioEngine.pause(); return }
        var start = (audioEngine.loopA >= 0) ? audioEngine.loopA : window.currentPosSec()
        if (audioEngine.hasPcm) audioEngine.seekSeconds(start)
        audioEngine.play()
    }
    // ---------- M4.3c+：后台异步渲染当前谱面 → 混音 WAV（Ctrl+R 手动；M5 载入自动内存化） ----------
    // 多线程（用户 2026-09 拍板）：renderAsync 提交 QThreadPool 后台解码+混音，
    // UI 不卡；完成时 renderFinished 信号 → 状态栏提示。Ctrl+R 输出 = 谱面同目录
    // <basename>.render.wav（验证产物）；载入自动/增量渲染 = 内存 only（不写盘）。
    function renderChartToFile() {
        if (typeof chartSession === "undefined" || !chartSession || !chartSession.hasChart) return
        if (chartPath === "") return
        var ok = chartSession.renderAsync(44100.0, true)
        if (!ok) {
            setStatus(qsTr("渲染进行中…（请稍候；完成时提示）"))
        } else {
            setStatus(qsTr("渲染中…（后台线程，可继续编辑）"))
        }
    }
    // 后台渲染完成（ChartSession.renderFinished 转发）→ 状态栏
    Connections {
        target: typeof chartSession !== "undefined" ? chartSession : null
        function onRenderFinished(ok, outPath, durationSec) {
            // 调试（--wait-render）：截图等待标志（main.cpp 轮询窗口属性）
            window.debugRenderDone = true
            // 渲染完成计数（--wait-render 增量验收：等全量+增量都完成）
            window.debugRenderCount += 1
            if (ok) {
                // M5：自动/增量渲染不写盘（outPath 仅 Ctrl+R 手动写）；文案统一「已完成」
                setStatus(qsTr("渲染完成：%1 秒").arg(durationSec.toFixed(1)))
                // --play：渲染完成后自动播放（M5 播放验收）
                if (window.debugPlayAfterRender && typeof audioEngine !== "undefined") {
                    audioEngine.play()
                    // 播放启动后才开始倒计时（避免属性设置时机早于播放 → 过早停止）
                    window.debugPlayStart()
                }
            } else {
                setStatus(qsTr("渲染失败：%1").arg(chartSession.errorMessage()))
            }
        }
    }
    // --play-duration <秒>：播放指定时长后停止（配合 --play/--screenshot 验收）
    property bool debugPlayAfterRender: false
    property var debugPlayDuration: 0
    Timer {
        id: playAutoStop
        interval: 3000
        onTriggered: {
            if (typeof audioEngine !== "undefined") audioEngine.stopPlay()
        }
    }
    // 播放启动后开始倒计时（--play 播放验收；属性设置时机早于播放 → 不能在那里 restart）
    function debugPlayStart() {
        if (debugPlayDuration > 0) {
            playAutoStop.interval = Math.max(100, Math.round(debugPlayDuration * 1000))
            playAutoStop.restart()
        }
    }

    // ---------- 会话控制器委托（业务逻辑抽离到 SessionController，2026-09） ----------
    // 每个逻辑函数在此一行委托给 session.*；chrome（菜单/工具条/快捷键/对话框）调用不变。
    // ⚠️ 委托保留原签名（QML 不支持 `...args` 转发），逐参透传。
    SessionController {
        id: session
        window: window
        editPage: editPage
    }

    function addPosDelta(ref, delta) { return session.addPosDelta(ref, delta) }
    function bgaLayerName(l) { return session.bgaLayerName(l) }
    function bmpAdd(id, file) { return session.bmpAdd(id, file) }
    function bmpDelete(id) { return session.bmpDelete(id) }
    function bmpRename(fromId, toId) { return session.bmpRename(fromId, toId) }
    function bmpSetFile(id, file) { return session.bmpSetFile(id, file) }
    function addWavSample(id, file) { return session.addWavSample(id, file) }
    function cancelPendingLn() { return session.cancelPendingLn() }
    function copySelection() { return session.copySelection() }
    function deleteBga(layer, measure, num, den) { return session.deleteBga(layer, measure, num, den) }
    function deleteMetaObject(obj) { return session.deleteMetaObject(obj) }
    function deleteMetaSelection() { return session.deleteMetaSelection() }
    function deleteNoteAt(ref) { return session.deleteNoteAt(ref) }
    function deleteSelection() { return session.deleteSelection() }
    function deleteTiming(kind, measure, num, den) { return session.deleteTiming(kind, measure, num, den) }
    function dispatchCmd(name, args) { return session.dispatchCmd(name, args) }
    function editBga(layer, measure, num, den, bmpId) { return session.editBga(layer, measure, num, den, bmpId) }
    function editTiming(kind, measure, num, den, value, ref) { return session.editTiming(kind, measure, num, den, value, ref) }
    /// 2026-09「加一小节」：编辑态有效小节数下限 +1（可放内容的新小节；用不到的保存时被 BMS 丢弃）。
    function addMeasure() { editPage.extendMeasures(); setStatus(qsTr("已加一小节（保存时未用到的空小节会被舍弃）")) }
    /// 2026-09 新建空谱面：未保存确认 → 选模式（对话框）→ chartSession.newChart(mode)。
    /// 2026-10 阶段一（issue #26）：Ctrl+N 不再直建 sp7k，先选模式（新谳 PMS/DP 不再受阻）。
    function newChart() {
        if (window.pmsSaveConfirmationPending || window.saveAsDocumentId !== "") return
        window.pendingNewMode = ""
        requestDocumentAction("new")
    }
    /// 运行中切换模式：先 dry-run 统计；有不可表示 note 直接拒绝（raw 降级尚未实现）。
    function requestModeSwitch(mode) {
        if (!chartMeta) return
        var r = sessionCmd("chart.set_mode", { mode: mode, dry_run: true })
        if (!r) return
        if (r.mode === r.current) { setStatus(qsTr("已经是该模式")); return }
        if (r.unsupported > 0) {
            setStatus(qsTr("切换失败：有 %1 个 note 无法在 %2 下表示（raw 降级未实现）")
                      .arg(r.unsupported).arg(window.modeDisplayName(mode)))
            return
        }
        modeSwitchDialog.pendingMode = mode
        modeSwitchDialog.pendingNotes = r.notes
        modeSwitchDialog.open()
    }
    function applyModeSwitch() {
        var mode = modeSwitchDialog.pendingMode
        modeSwitchDialog.close()
        var r = sessionCmd("chart.set_mode", { mode: mode })
        if (!r) return
        window.chartMode = r.mode || mode
        refreshTiming()
        setStatus(qsTr("已切换为 %1（保存后按新表写通道）").arg(window.modeDisplayName(mode)))
    }
    /// 「新建为模式」子菜单直建：跳过模式对话框（模式已定），未保存确认仍走。
    function newChartAs(mode) {
        if (window.pmsSaveConfirmationPending || window.saveAsDocumentId !== "") return
        window.pendingNewMode = mode
        requestDocumentAction("new")
    }
    /// 模式 id → 中文展示名（与 chart_mode_by_id 配置表及 codecModes() 同源；仅展示层映射）。
    function modeDisplayName(mode) {
        if (mode === "sp7k") return qsTr("SP 7键")
        if (mode === "dp") return qsTr("DP 7键")
        if (mode === "battle") return qsTr("Battle")
        if (mode === "pms9k") return qsTr("PMS 9键")
        return mode
    }
    function performNewChart(mode) {
        if (!chartSession.newChart(mode)) {
            setStatus(qsTr("新建空谱面失败：%1").arg(chartSession.errorMessage()))
            statusClearTimer.restart()
            return
        }
        window.chartPath = ""
        window.chartFormat = "bms"
        window.chartMode = mode
        window.chartEncoding = "utf8"
        // 默认元信息（空谱面面板可编辑；TITLE 等留空让用户填）。
        // ⚠️ 不预设 PLAYER（2026-10 复核）：草稿回填 "1" 会覆盖模式推导——DP/Battle 新建
        // 保存后 PLAYER 由 writer 按 mode_id 推导（dp=3/battle=4），重开不再退回 SP。
        window.chartMeta = { "TITLE": "", "ARTIST": "", "GENRE": "", "SUBTITLE": "",
                             "BPM": "130", "PLAYLEVEL": "", "RANK": "",
                             "TOTAL": "", "DIFFICULTY": "" }
        window.currentBpmValue = 130
        window.pendingLnHead = null
        window.selectionRefs = []
        window.metaSelection = []
        refreshTiming()
        // 清空上一谱面的采样（新建空谱面无采样；lint 面板对空谱面显空/占位，暂不额外清）
        sampleModel.clear()
        if (typeof editPage !== "undefined" && editPage) {
            editPage.reloadMeta()
            editPage.reloadBga()
        }
        editPage.setEditableMeasures(1)  // seed：渲染/可放小节 0（放进 note 即长真实小节数）
        if (typeof audioEngine !== "undefined" && audioEngine) audioEngine.setChartPath("")
        window.metaDraftDirty = false
        window.statusText = qsTr("已新建空谱面（Ctrl+S 另存为）")
        statusClearTimer.restart()
    }
    function timingDefAdd(kind, id, value) { return session.timingDefAdd(kind, id, value) }
    function timingDefDelete(kind, id) { return session.timingDefDelete(kind, id) }
    function gcd(a, b) { return session.gcd(a, b) }
    function laneEquals(lane, kind, index, player) { return session.laneEquals(lane, kind, index, player) }
    function metaKey(o) { return session.metaKey(o) }
    function metaTargetPos(measure, p, delta) { return session.metaTargetPos(measure, p, delta) }
    function moveMetaObject(kind, obj, deltaF, targetLane) { return session.moveMetaObject(kind, obj, deltaF, targetLane) }
    function moveSelection(deltaF, targetLane, sourceLane, sourceBgmLine) { return session.moveSelection(deltaF, targetLane, sourceLane, sourceBgmLine) }
    function onCanvasClicked() { return session.onCanvasClicked() }
    function onMetaClicked(obj, ctrl) { return session.onMetaClicked(obj, ctrl) }
    function onNoteClicked(ref, ctrl) { return session.onNoteClicked(ref, ctrl) }
    function onSelectionMade(refs) { return session.onSelectionMade(refs) }
    function pasteClipboard() { return session.pasteClipboard() }
    /// Base62 粘贴确认（2026-09 数据安全）：SessionController.pasteClipboard 判定「目标谱面
    /// Base36 + 文本含 Base62 id」时调用；确认后走同一 pasteRawText 落点（一个 undo 步）。
    function requestBase62PasteConfirm(text, targetMeasure) {
        base62PasteDialog.pendingText = text
        base62PasteDialog.pendingTarget = targetMeasure
        base62PasteDialog.open()
    }
    function placeBgaAt(hit) { return session.placeBgaAt(hit) }
    function placeLnType2(hit) { return session.placeLnType2(hit) }
    function placeNote(hit) { return session.placeNote(hit) }
    function quantizeSelection() { return session.quantizeSelection() }
    function refEquals(a, b) { return session.refEquals(a, b) }
    function refreshLint() { return session.refreshLint() }
    function refreshSamples() { return session.refreshSamples() }
    function refreshTiming() { return session.refreshTiming() }
    function saveChart() {
        if (window.pmsSaveConfirmationPending || window.saveAsDocumentId !== "") return undefined
        if (!chartSession.hasChart) return false
        // 新建谱面（path 空）Ctrl+S → 另存为；undefined 表示等待，不是失败。
        if (window.chartPath === "") { openSaveAsDialog(); return undefined }
        if (editUtils.pmsSuffixNeedsConfirmation(window.chartMode, window.chartPath)) {
            requestPmsSuffixConfirmation(window.chartPath, false)
            return undefined
        }
        return session.saveChart()
    }
    function saveChartAs(path) {
        if (window.pmsSaveConfirmationPending || window.saveAsDocumentId !== "") return undefined
        if (!chartSession.hasChart) return false
        if (editUtils.pmsSuffixNeedsConfirmation(window.chartMode, path)) {
            requestPmsSuffixConfirmation(path, true)
            return undefined
        }
        return session.saveChartAs(path)
    }
    function saveMetaEdits() { return session.saveMetaEdits() }
    function setModeMeta(key, value) { return session.setModeMeta(key, value) }
    function sessionCmd(name, args) { return session.sessionCmd(name, args) }
    function setCurrentBmp(id) { return session.setCurrentBmp(id) }
    function setSampleFile(id, file) { return session.setSampleFile(id, file) }
    function toggleGrid() { return session.toggleGrid() }
    function toggleLnSelection() { return session.toggleLnSelection() }
    function transformSelection(mirror, rotate) { return session.transformSelection(mirror, rotate) }
    function undoEdit() {
        if (currentPage === 1) {
            sliceWorkspace.undoSliceEdit()
            return
        }
        return session.undoEdit()
    }
    function redoEdit() {
        if (currentPage === 1) {
            sliceWorkspace.redoSliceEdit()
            return
        }
        return session.redoEdit()
    }

    // ---------- UI 动作注册表（doc/action-registry.md）：invoke = 唯一入口；以下包装函数是 handler 落点 ----------
    // 迁移期机制：行为原点不变（原 chrome 的第 2 遍调用并入 handler）；invoke 失败 =
    // 方法不存在（C++ qWarning）或动作禁用（静默）。
    function uiActionOpen() { requestDocumentAction("open") }
    function uiActionSaveAs() { openSaveAsDialog() }
    function uiActionExit() { window.close() }
    /// 运行时换肤（doc/skin-system.md 三、3.）：应用皮肤 token（applySkinByName）+ 该皮肤目录 keymap.json。
    /// "默认" 清除 keymap 覆写（恢复内置默认快捷键）；皮肤目录携带 keymap.json 时同步应用（doc/dev-workflow.md 四、）。
    function applySkinByName(name) {
        Theme.applySkinByName(name)
        if (name === "默认") {
            uiActions.clearKeymap()              // 恢复内置默认快捷键
        } else {
            const p = Theme.skinDirResolved(name) + "/keymap.json"
            uiActions.applyKeymapFile(p)         // 皮肤无 keymap.json → 打开失败返回 -1，不阻塞
        }
    }
    function uiActionDelete() {
        if (window.metaSelection.length > 0) deleteMetaSelection()
        else deleteSelection()
    }
    function uiActionToggleChannelIds() { window.showChannelIds = !window.showChannelIds }
    function uiActionToggleExtras() { window.showExtras = !window.showExtras }
    function zoomInView() {
        if (currentPage === 1) slicePage.sliceAct("zoomIn")
        else if (currentPage === 0 && editPage) editPage.zoomStep(true)
    }
    function zoomOutView() {
        if (currentPage === 1) slicePage.sliceAct("zoomOut")
        else if (currentPage === 0 && editPage) editPage.zoomStep(false)
    }
    function uiActionMirror() { transformSelection(true, 0) }
    function uiActionRotate() { transformSelection(false, 1) }
    /// 注册表 enabled 状态同步（QML 状态变化 → setEnabled → stateChanged → 菜单/工具条重算）。
    /// 与 Shortcut 自带的 enabled 条件镜像（后者还多文本焦点等细节，保持原样）。
    /// ⚠️ load 期间（注册未完成）曾有状态 on*Changed 触发——用 exists 守卫跳过，免启动期误报；
    /// 注册完成后 main.cpp 补调一次 updateActionStates（正确值兜底）。
    function setActionEnabled(id, state) {
        if (uiActions.exists(id)) uiActions.setEnabled(id, state)
    }
    function updateActionStates() {
        setActionEnabled("file.save", chartMeta !== null)
        setActionEnabled("file.saveAs", chartMeta !== null)
        setActionEnabled("edit.undo",
                         (currentPage === 0 && chartMeta !== null) ||
                         (currentPage === 1 && sliceWorkspace.canUndoSlice))
        setActionEnabled("edit.redo",
                         (currentPage === 0 && chartMeta !== null) ||
                         (currentPage === 1 && sliceWorkspace.canRedoSlice))
        setActionEnabled("edit.copy", chartMeta !== null && selectionRefs.length > 0)
        setActionEnabled("edit.paste", chartMeta !== null && currentPage === 0)
        setActionEnabled("edit.delete", chartMeta !== null && currentPage === 0 &&
                                           (selectionRefs.length > 0 || metaSelection.length > 0))
        setActionEnabled("tool.quantize", selectionRefs.length > 0)
        setActionEnabled("tool.mirror", selectionRefs.length > 0)
        setActionEnabled("tool.rotate", selectionRefs.length > 0)
        setActionEnabled("tool.toggleLn", selectionRefs.length > 0)
        setActionEnabled("tool.pan", currentPage === 0)
        setActionEnabled("tool.select", currentPage === 0)
        setActionEnabled("tool.note", currentPage === 0)
        setActionEnabled("tool.ln", currentPage === 0)
        setActionEnabled("tool.mine", currentPage === 0)
        setActionEnabled("view.zoomIn", currentPage === 0 || currentPage === 1)
        setActionEnabled("view.zoomOut", currentPage === 0 || currentPage === 1)
        setActionEnabled("slice.detect", currentPage === 1)
        setActionEnabled("slice.clearSlices", currentPage === 1 && sliceWorkspace.hasSlices)
        setActionEnabled("slice.export", currentPage === 1 && sliceWorkspace.hasSlices && sliceWorkspace.hasAudio)
        setActionEnabled("slice.importAudio", currentPage === 1)
        setActionEnabled("slice.importMidi", currentPage === 1)
        setActionEnabled("view.toggleGrid", chartMeta !== null)
    }
    /// 勾选态同步（doc/action-registry.md 一、2 打通）：QML 会话状态 → 注册表 `setChecked`（checkable 动作）。
    /// 这样 `uiActions.checked(id)` 成为查询/皮肤读取的单一数据源；菜单/工具条/复选框的
    /// `checked:` 都改绑它（见上）。QML 状态是真相源（handler 翻转它），invoke 后经
    /// on*Changed → 本函数回填注册表 → stateChanged → UI 刷新。皮肤如需 programmatic 置位，
    /// 走 `setChecked(id,v)` + `invoke(id)`（invoke 会执行 QML handler 翻转实际状态）。
    function updateCheckedStates() {
        if (!uiActions.exists("view.toggleGrid")) return  // 注册未完成，跳过（load 期兜底由 main.cpp 补调）
        uiActions.setChecked("view.toggleGrid", window.showGrid)
        uiActions.setChecked("view.toggleChannelIds", window.showChannelIds)
        uiActions.setChecked("view.toggleExtras", window.showExtras)
        uiActions.setChecked("view.page.edit", currentPage === 0)
        uiActions.setChecked("view.page.slice", currentPage === 1)
        uiActions.setChecked("view.page.test", currentPage === 2)
    }
    onChartMetaChanged: { updateActionStates(); updateCheckedStates() }
    onSelectionRefsChanged: { updateActionStates(); updateCheckedStates() }
    onClipboardLinesChanged: { updateActionStates(); updateCheckedStates() }
    onCurrentPageChanged: {
        updateActionStates()
        updateCheckedStates()
        // M6.1 页切换音频协调：进切音页 → 谱面播放暂停（共用内核 kPcmSlot，互斥）；
        // 离开切音页 → 参考播放停止（退出切音页即静音）
        if (typeof audioEngine === "undefined" || !audioEngine) return
        if (currentPage === 1) {
            if (audioEngine.playing) audioEngine.pause()
        } else if (audioEngine.refPlaying) {
            audioEngine.refStop()
        }
    }
    onMetaSelectionChanged: { updateActionStates(); updateCheckedStates() }
    onShowGridChanged: updateCheckedStates()
    onShowChannelIdsChanged: updateCheckedStates()
    onShowExtrasChanged: updateCheckedStates()
    // 注意：不挂 Component.onCompleted——onCompleted 在 loadFromModule 期间执行，
    // 此时 C++ 注册尚未完成（setEnabled 会 unknown）；注册后由 main.cpp 补调 updateActionStates。
    // 菜单/工具条 enabled 绑定依赖的「重算触发器」：QML 绑定不会因函数返回值自动重算，
    // 绑定到 uiStateTick（stateChanged 信号 +1），表达式统一形如
    // `enabled: window.uiStateTick >= 0 && uiActions.enabled("id")`。
    Connections {
        target: uiActions
        function onStateChanged() { window.uiStateTick++ }
    }
    // 运行时换肤（doc/skin-system.md 三、3.）：Theme.token 是 NOTIFY 属性，tokensChanged → QML 绑定自动重算；
    // QPalette 由 main.cpp 的 tokensChanged → rebuildPalette 重建；这里只需强制视口重绘
    // （ChartViewItem 的 note 颜色在 paint 时读 Theme token，重绘后才反映新皮肤）。
    Connections {
        target: Theme
        function onTokensChanged() {
            if (typeof editPage !== "undefined" && editPage && editPage.locateChartView && editPage.locateChartView())
                editPage.locateChartView().refreshTheme()
        }
    }

    // ---------- 对话框耦合函数（委托需 window 作用域的对话框，故保留在 Main） ----------
    function editMetaObject(obj) {
        if (obj.kind === "bga") metaEditDialog.openBga(obj)
        else metaEditDialog.openTiming(obj.kind, obj)
    }

    function editNoteSample(ref) {
        if (!ref) return
        noteSampleDialog.ref = ref
        noteIdInput.text = chartSession.idTextOf(ref.sample)
        noteSampleDialog.open()
    }


    // ---------- 关于对话框 ----------
    // 首选项（M4.2）：音频设置 + 显示（皮肤）。皮肤切换经信号 → applySkinByName（与菜单同源）。
    SettingsDialog {
        id: settingsDialog
        keymapTick: window.uiStateTick
        onSkinRequested: (name) => window.applySkinByName(name)
    }

    BbDialog {
        id: aboutDialog
        title: qsTr("关于 BeAtBench")
        width: 360
        height: 148
        showCancel: false
        Label { text: "BeAtBench " + beatbench.versionString(); font.bold: true; color: Theme.text }
        Label { text: qsTr("BMS 谱面编辑器 · GPL-3.0"); font.pixelSize: Theme.fsSmall; color: Theme.text }
        Label { text: qsTr("© 2026 wufe8"); font.pixelSize: Theme.fsSmall; color: Theme.textMuted }
    }

    // 编辑区双击 note → 改引用采样 id（切音手工版；note.setSample）
    BbDialog {
        id: noteSampleDialog
        title: qsTr("编辑对象")
        width: 320
        height: 176
        property var ref: null
        readonly property string posText: ref ? qsTr("小节 %1 · %2/%3")
                                                 .arg(ref.measure).arg(ref.pos.num).arg(ref.pos.den) : ""
        readonly property string curText: ref ? qsTr("#WAV%1").arg(chartSession.idTextOf(ref.sample)) : ""
        onOpened: noteIdInput.forceActiveFocus()
        Label { text: qsTr("note 引用采样"); color: Theme.primary; font.pixelSize: Theme.fsSmall; font.bold: true }
        Label { text: noteSampleDialog.posText + qsTr("　当前 %1").arg(noteSampleDialog.curText);
                color: Theme.textFaint; font.family: Theme.fontMono; font.pixelSize: Theme.fsTiny }
        RowLayout {
            Layout.fillWidth: true
            spacing: 6
            Label { text: qsTr("#WAV id"); color: Theme.textMuted; font.pixelSize: Theme.fsTiny;
                    Layout.preferredWidth: 56 }
            BbTextField {
                id: noteIdInput
                Layout.fillWidth: true
                font.family: Theme.fontMono
                placeholderText: qsTr("如 1A")
                onAccepted: noteSampleDialog.accept()
                // 一次 Esc 即关对话框（否则 BbTextField 释放焦点 → 需再按一次才到 Dialog）
                escapeHandler: function() { noteSampleDialog.reject() }
            }
        }
        Label { text: qsTr("双击左 Dock 采样行可给该槽位绑定/改文件"); color: Theme.textFaint;
                font.pixelSize: Theme.fsTiny }
        onAccepted: {
            const r = noteSampleDialog.ref
            if (!r) return
            const to = noteIdInput.text.trim()
            if (to === "") { setStatus(qsTr("未填 #WAV id")); return }
            const args = {
                measure: r.measure,
                pos: { num: r.pos.num, den: r.pos.den },
                lane: r.lane,
                sample: r.sample,
                to: to
            }
            if (r.sub_line !== undefined) args.sub_line = r.sub_line
            const res = sessionCmd("note.setSample", args)
            if (res) setStatus(qsTr("note → #WAV%1（Undo 可恢复）").arg(to))
        }
    }

    // ---- 视口双击编辑 BGA/BPM/STOP 对象（2026-09）：编辑内容（BMP id / 值）；位置用拖拽移动） ----
    BbDialog {
        id: metaEditDialog
        title: qsTr("编辑对象")
        width: 300
        height: 190
        property string kind: ""   // bga / bpm / stop
        property var base: null    // 原对象（measure/pos/layer/sample/value）
        readonly property string layerText: base ? ["base","poor","layer","layer2"][base.layer] : ""

        function openBga(obj) { kind = "bga"; base = obj; applyFields(); open() }
        function openTiming(k, obj) { kind = k; base = obj; applyFields(); open() }
        function applyFields() {
            meLabel.text = kind === "bga" ? qsTr("BGA 对象")
                          : kind === "bpm" ? qsTr("BPM 对象") : qsTr("STOP 对象")
            mePosLabel.text = qsTr("小节 %1 · %2/%3").arg(base.measure).arg(base.pos.num).arg(base.pos.den)
            meLayerRow.visible = kind === "bga"
            if (kind === "bga") {
                meLayerLabel.text = qsTr("图层：%1").arg(metaEditDialog.layerText)
                meContentLabel.text = qsTr("#BMP id")
                meContentField.text = "#BMP" + chartSession.idTextOf(base.sample)
                meContentField.placeholderText = qsTr("01/ZZ")
            } else {
                meContentLabel.text = kind === "bpm" ? qsTr("值(BPM)") : qsTr("值(%1)").arg(stopUnitLabel())
                meContentField.text = kind === "bpm" ? String(base.value) : window.stopToDisplay(base.value)
                meContentField.placeholderText = kind === "bpm" ? qsTr("如 180") : (window.stopUnit === 0 ? qsTr("如 96") : qsTr("如 800"))
            }
        }
        onOpened: meContentField.forceActiveFocus()

        Label { id: meLabel; color: Theme.primary; font.pixelSize: Theme.fsSmall; font.bold: true }
        Label { id: mePosLabel; color: Theme.textFaint; font.family: Theme.fontMono;
                font.pixelSize: Theme.fsTiny }
        RowLayout { id: meLayerRow; spacing: 6; Layout.fillWidth: true
            Label { id: meLayerLabel; color: Theme.textMuted; font.pixelSize: Theme.fsTiny }
        }
        RowLayout {
            Layout.fillWidth: true
            spacing: 6
            Label { id: meContentLabel; color: Theme.textMuted; font.pixelSize: Theme.fsTiny; Layout.preferredWidth: 56 }
            BbTextField {
                id: meContentField
                Layout.fillWidth: true
                font.family: Theme.fontMono
                onAccepted: metaEditDialog.accept()
                escapeHandler: function() { metaEditDialog.reject() }
            }
        }
        onAccepted: {
            const s = metaEditDialog.base
            if (!s) return
            if (metaEditDialog.kind === "bga") {
                const id = meContentField.text.trim().indexOf("#BMP") === 0
                           ? meContentField.text.trim().substring(4) : meContentField.text.trim()
                const sample = chartSession.decodeId(id)
                if (sample < 0) { setStatus(qsTr("#BMP id 非法：%1").arg(id)); return }
                const r = sessionCmd("bga.put", { layer: s.layer, measure: s.measure, pos: s.pos, sample: sample })
                if (r) {
                    if (typeof editPage !== "undefined" && editPage) editPage.reloadBga()
                    setStatus(qsTr("BGA → #BMP%1").arg(id))
                }
            } else {
                const raw = parseFloat(meContentField.text)
                const value = metaEditDialog.kind === "bpm" ? raw : window.stopFromDisplay(meContentField.text)
                if (!isFinite(value)) { setStatus(qsTr("值非法")); return }
                const r = sessionCmd("timing.put", { kind: metaEditDialog.kind,
                                                     measure: s.measure, pos: s.pos, value: value })
                if (r) { refreshTiming(); setStatus(qsTr("已改 %1 值").arg(metaEditDialog.kind.toUpperCase())) }
            }
        }
    }

    // ---------- 拖拽入窗口（谱面 / 音频 / MIDI）----------
    // 覆盖整个内容区（工具条/状态栏除外，接受拖拽也够用）；落在窗口内任意位置均可。
    // DropArea 只处理拖放事件、不拦截普通鼠标事件，故不影响编辑区 pan / 框选等既有交互。
    DropArea {
        id: windowDropArea
        anchors.fill: parent
        onDropped: function (drop) {
            if (!drop.hasUrls || drop.urls.length === 0) return
            let accepted = false
            for (let i = 0; i < drop.urls.length; ++i) {
                const p = window.urlToPath(drop.urls[i])
                if (window.handleExternalFile(p)) accepted = true
            }
            if (accepted) drop.acceptProposedAction()
        }
        // 拖入提示：仅拖拽悬停期间可见
        Rectangle {
            anchors.fill: parent
            visible: windowDropArea.containsDrag
            color: Qt.rgba(Theme.primary.r, Theme.primary.g, Theme.primary.b, 0.12)
            border.color: Theme.primary
            border.width: 2
            z: 10000
            ColumnLayout {
                anchors.centerIn: parent
                spacing: 4
                Label {
                    Layout.alignment: Qt.AlignHCenter
                    text: qsTr("松开以打开")
                    color: Theme.text
                    font.pixelSize: Theme.fsBase + 4
                    font.bold: true
                }
                Label {
                    Layout.alignment: Qt.AlignHCenter
                    text: qsTr("谱面 / 音频 / MIDI")
                    color: Theme.textMuted
                    font.pixelSize: Theme.fsSmall
                }
            }
        }
    }
}

