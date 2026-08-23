// SPDX-License-Identifier: GPL-3.0-only
// BeatBench 主窗口（M2 页面式工作区外壳，doc/05 v0.2）。
// 结构：固定 chrome（菜单栏 + 页面工具条 + 页面条 + 状态栏）包裹页面内容区；
// 页面内容 = EditPage / SlicePage / TestPage（切换只换视图，不换命令引擎）。
// 颜色/字体一律走 Theme token（doc/07 §4，禁硬编码）；皮肤系统 = 内置默认皮肤骨架（doc/08 §3.4）。
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
    title: qsTr("BeAtBench")
    visible: true
    font.family: Theme.fontSans
    font.pixelSize: Theme.fsBase

    // ---------- 会话状态（不入 undo，doc/05 §1.2） ----------
    property int currentPage: 0          // 0 编辑 / 1 切音 / 2 测试
    property var chartMeta: null         // dispatch(info) 的 result.meta
    property string chartPath: ""        // 当前谱面路径（info 返回的规范化路径）
    property string statusText: qsTr("就绪")
    property string currentSampleId: ""  // 当前采样（会话状态，M3 放置落点）
    // 轨道列头显示实际 BMS 通道 id（debug 用；Ctrl 临时切换在 ChartView 内处理）
    property bool showChannelIds: false
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
    // --delete-selection：点击后自动 Del（验收删除链）
    property bool debugDeleteSelection: false
    onDebugDeleteSelectionChanged: if (debugDeleteSelection) debugDeleteTimer.restart()
    Timer {
        id: debugDeleteTimer
        interval: 400   // 点击（150ms）之后
        onTriggered: deleteSelection()
    }
    onDebugBgmExpandChanged: if (debugBgmExpand && editPage) editPage.bgmExpanded = true
    onDebugShowChannelIdsChanged: if (debugShowChannelIds) window.showChannelIds = true
    onDebugNoteSampleModeChanged: if (debugNoteSampleMode > 0) window.noteSampleMode = debugNoteSampleMode
    onDebugShowExtrasChanged: if (debugShowExtras) window.showExtras = true
    // 当前编辑工具（互斥单选，会话状态；M3 接输入/放置，note 类型（普通/LN/地雷）届时
    // 作为正交维度另设「放置类型」组，不并入本组——doc/05 §5 交互）
    property string editorTool: "select"
    // 剪贴板（BMS 原始行；clipboard.copy 输出 → paste 输入；会话状态）
    property var clipboardLines: []
    // 选中 note 集合（NoteRef；框选后存 + 回填高亮；Ctrl+C 复制）
    property var selectionRefs: []
    // 吸附（放置用）：snapNum/snapDen 小节（分子分母皆可调；1/16 = 每小节 16 槽，3/16 = 3/16 步长）
    property int snapNum: 1
    property int snapDen: 16
    // 平移模式（checkbox 开关，默认开）：拖拽选中 note = 时间轴移动（不改轨道）
    property bool moveMode: true
    /// 文本输入焦点（工具快捷键让行，避免输入时误触）
    readonly property bool textInputFocused:
        window.activeFocusItem && typeof window.activeFocusItem.text === "string"

    // ---------- 全局快捷键（QML MenuItem 无 shortcut 属性，用 Shortcut 类型） ----------
    Shortcut { sequence: "Ctrl+O"; onActivated: fileDialog.open() }
    Shortcut { sequence: "Ctrl+S"; onActivated: saveChart() }
    Shortcut { sequence: "Ctrl+Shift+S"; onActivated: saveAsDialog.open() }
    Shortcut { sequence: "Ctrl+Z"; onActivated: undoEdit() }
    Shortcut { sequence: "Ctrl+Y"; onActivated: redoEdit() }
    Shortcut { sequence: "Ctrl+C"; onActivated: copySelection() }
    Shortcut { sequence: "Ctrl+V"; onActivated: pasteClipboard() }
    Shortcut { sequence: "Del"; enabled: chartMeta !== null && currentPage === 0
                onActivated: deleteSelection() }
    // 编辑工具快捷键（数字 1-5：1=拖拽 2=选择 3=放置 4=LN 5=地雷；文本输入焦点时让行）
    Shortcut { sequence: "1"; enabled: currentPage === 0 && !window.textInputFocused
                onActivated: window.editorTool = "pan" }
    Shortcut { sequence: "2"; enabled: currentPage === 0 && !window.textInputFocused
                onActivated: window.editorTool = "select" }
    Shortcut { sequence: "3"; enabled: currentPage === 0 && !window.textInputFocused
                onActivated: window.editorTool = "note" }
    Shortcut { sequence: "4"; enabled: currentPage === 0 && !window.textInputFocused
                onActivated: window.editorTool = "ln" }
    Shortcut { sequence: "5"; enabled: currentPage === 0 && !window.textInputFocused
                onActivated: window.editorTool = "mine" }
    Shortcut { sequence: "Ctrl+Q"; onActivated: window.close() }

    // ---------- 菜单栏（固定全局） ----------
    menuBar: MenuBar {
        Menu {
            title: qsTr("文件")
            MenuItem {
                text: qsTr("打开谱面…")
                onTriggered: fileDialog.open()
            }
            MenuItem {
                text: qsTr("保存")
                enabled: chartMeta !== null
                onTriggered: saveChart()
            }
            MenuItem {
                text: qsTr("另存为…")
                enabled: chartMeta !== null
                onTriggered: saveAsDialog.open()
            }
            MenuSeparator {}
            MenuItem {
                text: qsTr("退出")
                onTriggered: window.close()
            }
        }
        Menu {
            title: qsTr("编辑")
            enabled: chartMeta !== null
            MenuItem { text: qsTr("撤销"); onTriggered: undoEdit() }
            MenuItem { text: qsTr("重做"); onTriggered: redoEdit() }
            MenuSeparator {}
            MenuItem { text: qsTr("复制"); onTriggered: copySelection() }
            MenuItem { text: qsTr("粘贴"); onTriggered: pasteClipboard() }
            MenuSeparator {}
            MenuItem { text: qsTr("元信息编辑（M3）"); enabled: false }
        }
        Menu {
            title: qsTr("视图")
            MenuItem { text: qsTr("皮肤（L1/L2，M2 后）"); enabled: false }
        }
        Menu {
            title: qsTr("工作区")
            MenuItem { text: qsTr("编辑页"); checkable: true; checked: currentPage === 0
                       onTriggered: currentPage = 0 }
            MenuItem { text: qsTr("切音页"); checkable: true; checked: currentPage === 1
                       onTriggered: currentPage = 1 }
            MenuItem { text: qsTr("测试页"); checkable: true; checked: currentPage === 2
                       onTriggered: currentPage = 2 }
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

        // 页面工具条（随页面变化；工具条 = 皮肤 L2 可声明区，doc/08 §3.1）
        ToolBar {
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
                    implicitWidth: 56
                    onValueModified: window.snapNum = Math.max(1, value)
                    ToolTip.visible: hovered
                    ToolTip.text: qsTr("snap 分子（槽数步长 = snapNum/snapDen 小节）")
                }
                Label { text: "/"; color: Theme.textMuted; font.pixelSize: Theme.fsSmall }
                BbSpinBox {
                    from: 1; to: 192
                    value: window.snapDen
                    editable: true
                    implicitWidth: 56
                    onValueModified: window.snapDen = Math.max(1, value)
                    ToolTip.visible: hovered
                    ToolTip.text: qsTr("snap 分母（每小节槽数；吸附 + 槽位线，>64 不画弱线）")
                }
                BbToolButton { text: qsTr("量化"); enabled: chartMeta !== null }
                BbToolButton { text: qsTr("网格"); enabled: chartMeta !== null }
                // 更多轨道：BGA 图层通道列（04/06/07/0A，游玩轨与背景轨之间，iBMSC 式）
                BbCheckBox {
                    id: extrasCheck
                    text: qsTr("更多轨道")
                    checked: window.showExtras
                    onToggled: window.showExtras = checked
                    ToolTip.visible: hovered
                    ToolTip.text: qsTr("在游玩轨与背景轨之间显示 BGA 图层通道（BGA/LAYER/POOR/LAYER2 = 04/06/07/0A）")
                }
                BbToolButton { text: qsTr("缩放 %1%").arg(editPage.zoomPercent)
                               enabled: chartMeta !== null
                               onClicked: { editPage.resetZoom(); setStatus(qsTr("缩放已重置")) }
                               ToolTip.visible: hovered
                               ToolTip.text: qsTr("当前缩放（点击恢复 100% = 小节高度 96px）；Ctrl+滚轮缩放") }
                Item { Layout.fillWidth: true }
                BbToolButton { text: qsTr("▶ 试听（Phase B）"); enabled: false }
                Label { text: "SP7K"; color: Theme.accent; font.family: Theme.fontMono
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
                // 顺序：1=拖拽（默认） 2=选择 3=放置 4=LN 5=地雷；快捷键同序。
                BbToolButton { text: "1 拖拽"; active: window.editorTool === "pan"; flatStyle: true
                               onClicked: window.editorTool = "pan" }
                BbToolButton { text: "2 选择"; active: window.editorTool === "select"; flatStyle: true
                               onClicked: window.editorTool = "select" }
                BbToolButton { text: "3 放置"; active: window.editorTool === "note"; flatStyle: true
                               onClicked: window.editorTool = "note" }
                BbToolButton { text: "4 LN"; active: window.editorTool === "ln"; flatStyle: true
                               onClicked: window.editorTool = "ln" }
                BbToolButton { text: "5 地雷"; active: window.editorTool === "mine"; flatStyle: true
                               onClicked: window.editorTool = "mine" }
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
                Item { Layout.fillWidth: true }
                // 轨道名 → 实际通道 id（皿=16、键1=11、BGM=01…；Ctrl 临时切换，Adobe 式）
                BbCheckBox {
                    id: channelIdCheck
                    text: qsTr("通道 ID")
                    checked: window.showChannelIds
                    onToggled: window.showChannelIds = checked
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
                // 当前采样（M3 放置落点；检索/选择在左 Dock 采样面板）
                Label {
                    text: sampleModel.currentSampleText
                    color: Theme.accent
                    font.family: Theme.fontMono
                    font.pixelSize: Theme.fsSmall
                    elide: Text.ElideMiddle
                    visible: sampleModel.currentSampleText !== ""
                    Layout.maximumWidth: 220
                }
                Label { text: chartPath ? chartPath : qsTr("未打开谱面"); color: Theme.textFaint
                        elide: Text.ElideMiddle; font.pixelSize: Theme.fsSmall }
            }
        }

        // 页面内容区（切页 = 切视图，doc/05 §2）
        StackLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            currentIndex: window.currentPage
            EditPage {
                id: editPage
                chartMeta: window.chartMeta
                chartPath: window.chartPath
                showChannelIds: window.showChannelIds
                noteSampleMode: window.noteSampleMode
                showExtras: window.showExtras
                editorTool: window.editorTool
                moveMode: window.moveMode
                sampleId: chartSession.sampleValueOf(window.currentSampleId)
                sampleText: sampleModel.currentSampleText
                selection: window.selectionRefs
                snapNum: window.snapNum
                snapDen: window.snapDen
                perfLog: window.debugPerfLog
                onSamplePicked: (id, file) => {
                    // 会话状态：当前采样（M3 放置落点；不入 undo，doc/05 §1.2）
                    window.currentSampleId = id
                    setStatus(qsTr("当前采样：#WAV%1 %2").arg(id, file))
                }
                onHitPlaceRequested: (hit) => placeNote(hit)
                onSelectionFinished: (refs) => onSelectionMade(refs)
                onNoteClicked: (ref, ctrl) => window.onNoteClicked(ref, ctrl)
                onCanvasClicked: () => window.onCanvasClicked()
                onNoteRightDeleted: (ref) => deleteNoteAt(ref)
                onMoveSelectionRequested: (deltaF, targetLane) => moveSelection(deltaF, targetLane)
                onToolNotReady: (tool) => {
                    setStatus(tool === "ln"
                              ? qsTr("LN 放置：M3 编辑命令尚未接 kind（当前仅普通 note）")
                              : qsTr("地雷放置：M3 编辑命令尚未接 kind（当前仅普通 note）"))
                }
            }
            SlicePage {}
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
            RowLayout {
                anchors.fill: parent
                anchors.leftMargin: 10
                anchors.rightMargin: 10
                // 单条消息：悬停信息（临时，优先）> 状态消息（瞬态：更新后 ~8s 自动回「就绪」）
                Label {
                    text: editPage.hoverText !== "" ? editPage.hoverText : window.statusText
                    color: editPage.hoverText !== "" ? Theme.accent : Theme.textMuted
                    elide: Text.ElideMiddle
                    Layout.fillWidth: true
                    font.family: Theme.fontMono
                    font.pixelSize: Theme.fsSmall
                }
                Label {
                    text: chartMeta ? "SP7K · " + (chartMeta.PLAYER !== undefined ? chartMeta.PLAYER : "") : ""
                    color: Theme.textFaint; font.family: Theme.fontMono; font.pixelSize: Theme.fsSmall
                }
            }
        }
    }

    // ---------- 状态消息（瞬态：setStatus 后 ~8s 自动回「就绪」，避免常驻噪声） ----------
    Timer {
        id: statusClearTimer
        interval: 8000
        onTriggered: window.statusText = qsTr("就绪")
    }
    function setStatus(msg) {
        window.statusText = msg
        statusClearTimer.restart()
    }

    // ---------- 第一条真链路：打开谱面 → dispatch(info) → 元信息 ----------
    FileDialog {
        id: fileDialog
        title: qsTr("打开 BMS 谱面")
        nameFilters: [qsTr("BMS 谱面 (*.bms)"), qsTr("所有文件 (*)")]
        onAccepted: openChart(urlToPath(selectedFile))
        onRejected: { /* 用户取消 */ }
    }

    // --open 调试参数（main.cpp 注入）：走与 Ctrl+O 相同的调用路径
    property string debugOpenPath: ""
    onDebugOpenPathChanged: if (debugOpenPath !== "") openChart(debugOpenPath)
    // --tool / --click 调试参数（配 --screenshot 验收点击链）：工具 + 一次模拟点击
    property string debugTool: ""
    property double debugClickX: -1
    property double debugClickY: -1
    onDebugToolChanged: if (debugTool !== "") window.editorTool = debugTool
    onDebugClickXChanged: debugMaybeClick()
    onDebugClickYChanged: debugMaybeClick()
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

    // 另存为（文件 → 另存为… / Ctrl+Shift+S）
    FileDialog {
        id: saveAsDialog
        title: qsTr("另存为 BMS 谱面")
        fileMode: FileDialog.SaveFile
        nameFilters: [qsTr("BMS 谱面 (*.bms)"), qsTr("所有文件 (*)")]
        onAccepted: saveChartAs(urlToPath(selectedFile))
    }

    function openChart(path) {
        var req = JSON.stringify({ command: "info", args: { path: path } })
        var resp = beatbench.dispatch(req)
        var r = JSON.parse(resp)
        if (r.ok) {
            window.chartMeta = r.result.meta
            window.chartPath = r.result.path
            // M2 第 5 步：时间轴真数据（ChartSession + TimingEngine，与 info 同源解析）
            chartSession.openChart(path)
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
        } else {
            window.chartMeta = null
            window.statusText = qsTr("打开失败：%1 %2").arg(r.error.code, r.error.message)
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

    // ---------- 编辑命令封装（M3 协议：note.put/move/delete、session.*、clipboard.*） ----------
    // 统一 dispatch + 成功即 chartSession.refresh()（指纹判定文档/内容变化，视图自动刷新）。
    function sessionCmd(name, args) {
        var req = JSON.stringify({ command: name, args: args || {} })
        var resp = JSON.parse(beatbench.dispatch(req))
        if (!resp.ok) {
            setStatus(resp.error.code + ": " + resp.error.message)
            return null
        }
        chartSession.refresh()
        return resp.result
    }
    /// 只 dispatch 不 refresh（批删除循环里用；调用方完成后统一 refresh 一次）。
    function dispatchCmd(name, args) {
        var req = JSON.stringify({ command: name, args: args || {} })
        var resp = JSON.parse(beatbench.dispatch(req))
        if (!resp.ok) {
            setStatus(resp.error.code + ": " + resp.error.message)
            return null
        }
        return resp.result
    }
    function deleteNoteAt(ref) {
        var r = sessionCmd("note.delete", {
            measure: ref.measure, pos: ref.pos, lane: ref.lane, sample: ref.sample
        })
        if (r) setStatus(qsTr("已删除（可撤销）"))
    }
    function deleteSelection() {
        if (!window.selectionRefs || window.selectionRefs.length === 0) {
            setStatus(qsTr("没有选中（点击 note 选中 / Shift+框选）"))
            return
        }
        var refs = window.selectionRefs.slice()
        var done = 0
        for (var i = 0; i < refs.length; i++) {
            var r = dispatchCmd("note.delete", {
                measure: refs[i].measure, pos: refs[i].pos,
                lane: refs[i].lane, sample: refs[i].sample
            })
            if (r) done++
        }
        if (done > 0) {
            chartSession.refresh()
            window.selectionRefs = []
            setStatus(qsTr("已删除 %1 个 note（Undo 可恢复）").arg(done))
        }
    }
    function placeNote(hit) {
        // kind 语义（M3 note.put 已支持）：note→normal / ln→LN 自动配对 / mine→地雷。
        var kind = "normal"
        if (window.editorTool === "ln") kind = "ln"
        else if (window.editorTool === "mine") kind = "mine"
        // BGM 展开列带 sampleHint（该列固定 #WAV id）→ 直接用；否则取当前采样
        if (hit.sampleHint !== undefined && hit.sampleHint >= 0) {
            var r0 = sessionCmd("note.put", {
                measure: hit.measure,
                pos: { num: hit.num, den: hit.den },
                lane: { player: hit.lanePlayer, kind: hit.laneKind, index: hit.laneIndex },
                sample: hit.sampleHint,
                kind: kind
            })
            if (r0)
                setStatus(kind === "ln"
                          ? qsTr("放置 LN #WAV%1（BGM 列）· 小节 %2").arg(hit.sampleHint).arg(hit.measure)
                          : kind === "mine"
                                ? qsTr("放置地雷 #WAV%1（BGM 列）· 小节 %2").arg(hit.sampleHint).arg(hit.measure)
                                : qsTr("放置 #WAV%1（BGM 列）· 小节 %2").arg(hit.sampleHint).arg(hit.measure))
            return
        }
        if (window.currentSampleId === "") {
            setStatus(qsTr("先选择采样：左 Dock「采样」面板点击 #WAVxx"))
            return
        }
        var v = chartSession.sampleValueOf(window.currentSampleId)
        if (v < 0) {
            setStatus(qsTr("当前采样 #WAV%1 不在定义表中").arg(window.currentSampleId))
            return
        }
        var r = sessionCmd("note.put", {
            measure: hit.measure,
            pos: { num: hit.num, den: hit.den },
            lane: { player: hit.lanePlayer, kind: hit.laneKind, index: hit.laneIndex },
            sample: v,
            kind: kind
        })
        if (r) {
            var st = kind === "ln"
                     ? qsTr("放置 LN #WAV%1 · 小节 %2 · %3/%4")
                     : kind === "mine"
                           ? qsTr("放置地雷 #WAV%1 · 小节 %2 · %3/%4")
                           : qsTr("放置 #WAV%1 · 小节 %2 · %3/%4")
            setStatus(st.arg(window.currentSampleId).arg(hit.measure).arg(hit.num).arg(hit.den))
        }
    }
    function onSelectionMade(refs) {
        window.selectionRefs = refs
        setStatus(qsTr("已选中 %1 个 note（Ctrl+C 复制）").arg(refs.length))
    }
    function refEquals(a, b) {
        return a && b && a.measure === b.measure && a.sample === b.sample &&
               a.lane.kind === b.lane.kind && a.lane.index === b.lane.index &&
               a.lane.player === b.lane.player &&
               a.pos.num === b.pos.num && a.pos.den === b.pos.den
    }
    function onNoteClicked(ref, ctrl) {
        if (ctrl) {
            // 多选切换（Ctrl+点击，文件管理器逻辑）：已选中 → 移除；否则追加
            var arr = window.selectionRefs.slice()
            var idx = -1
            for (var i = 0; i < arr.length; i++)
                if (refEquals(arr[i], ref)) { idx = i; break }
            if (idx >= 0) arr.splice(idx, 1)
            else arr.push(ref)
            window.selectionRefs = arr
            setStatus(qsTr("已选中 %1 个 note").arg(arr.length))
            return
        }
        window.selectionRefs = [ref]
        setStatus(qsTr("选中 #WAV%1（Del 删除 / 右键删除 / 拖拽平移）").arg(ref.sample))
    }
    function onCanvasClicked() {
        if (window.selectionRefs.length > 0) window.selectionRefs = []
    }
    /// 平移选中 note（统一位移：拖拽/框选整段/多选）。
    /// deltaF = 连续拍位位移（可负可跨小节）；targetLane = 横向目标列（laneAtX；null=纯时间）。
    /// ⚠️ 用 note.moveRegion：selection + 统一 delta（{measure,pos}+可选 to_lane）→ 一个 undo 步。
    ///   相对 M3 之前的「delete+put 兜底」（撤销 2 步、丢 LN），此为单命令、保 LN。
    ///   note.move 的 moves 数组留给「逐项不同目标」场景（非均匀拖动），本函数不用。
    function moveSelection(deltaF, targetLane) {
        if (!window.selectionRefs || window.selectionRefs.length === 0) {
            setStatus(qsTr("先选中 note（选择工具点击/框选）再移动"))
            return
        }
        // delta snap：把连续拍位位移吸附到当前槽（snapNum/snapDen 小节），再拆成
        // {measure(int 小节分量), pos(节内分数分量)}。BMS 槽位为离散步长，吸附后对齐网格。
        var num = window.snapNum, den = window.snapDen
        var slots = Math.max(1, Math.floor(den / num))
        var snappedF = Math.round(deltaF * slots) / slots  // 吸附到整槽
        var m = Math.floor(snappedF)
        var frac = snappedF - m
        var deltaPos = { num: Math.round(frac * den), den: den }
        var delta = { measure: m, pos: deltaPos }
        var args = { selection: window.selectionRefs.slice(), delta: delta }
        if (targetLane && targetLane.valid) {
            args.to_lane = { player: targetLane.lanePlayer, kind: targetLane.laneKind,
                             index: targetLane.laneIndex }
        }
        var r = sessionCmd("note.moveRegion", args)
        if (r) {
            window.selectionRefs = []
            setStatus(targetLane && targetLane.valid
                      ? qsTr("已移动 %1 个 note（改通道）").arg(r.notes)
                      : qsTr("已移动 %1 个 note（+%2 拍）").arg(r.notes).arg(deltaF.toFixed(3)))
        }
    }
    function copySelection() {
        if (!window.selectionRefs || window.selectionRefs.length === 0) {
            setStatus(qsTr("没有选中（选择工具下 Shift+拖拽框选）"))
            return
        }
        var r = sessionCmd("clipboard.copy", { selection: window.selectionRefs })
        if (r) {
            window.clipboardLines = r.lines
            setStatus(qsTr("已复制 %1 个 note（%2 行）").arg(r.count).arg(r.lines.length))
        }
    }
    function pasteClipboard() {
        if (!window.clipboardLines || window.clipboardLines.length === 0) {
            setStatus(qsTr("剪贴板为空（先框选 Ctrl+C）"))
            return
        }
        var target = 0
        if (typeof editPage !== "undefined" && editPage)
            target = Math.floor(editPage.centerMeasure() || 0)
        var r = sessionCmd("clipboard.paste", {
            lines: window.clipboardLines,
            target_measure: Math.max(0, target)
        })
        if (r)
            setStatus(qsTr("已粘贴 %1 个 note 到小节 %2").arg(r.notes).arg(r.target_measure))
    }
    function undoEdit() {
        var r = sessionCmd("session.undo")
        if (r) {
            if (r.ok)
                setStatus(qsTr("已撤销（可重做 %1 步）").arg(r.redo_depth))
            else
                setStatus(qsTr("无可撤销"))
        }
    }
    function redoEdit() {
        var r = sessionCmd("session.redo")
        if (r) {
            if (r.ok)
                setStatus(qsTr("已重做（可撤销 %1 步）").arg(r.undo_depth))
            else
                setStatus(qsTr("无可重做"))
        }
    }
    function saveChart() {
        var r = sessionCmd("session.save", { overwrite: true })
        if (r) {
            window.chartPath = r.output
            setStatus(qsTr("已保存：%1（%2 字节）").arg(r.output).arg(r.bytes))
        }
    }
    function saveChartAs(path) {
        var r = sessionCmd("session.save", { path: path, overwrite: true })
        if (r) {
            window.chartPath = r.output
            setStatus(qsTr("已另存为：%1").arg(r.output))
        }
    }

    // ---------- 关于对话框 ----------
    Dialog {
        id: aboutDialog
        title: qsTr("关于 BeAtBench")
        standardButtons: Dialog.Ok
        modal: true
        anchors.centerIn: parent
        width: 360
        ColumnLayout {
            anchors.fill: parent
            spacing: 6
            Label { text: "BeAtBench " + qsTr("0.1.0（M2）"); font.bold: true }
            Label { text: qsTr("BMS 谱面编辑器 · Qt Quick/QML · GPL-3.0") }
            Label { text: qsTr("协议：命令即接口（doc/06 §3）"); color: Theme.textMuted }
        }
    }
}
