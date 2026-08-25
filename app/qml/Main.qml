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
    // 文件格式/编码（底部状态栏右下角显示；info/check 返回）
    property string chartFormat: ""
    property string chartEncoding: ""
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
    // 缩放锚点（2026-09 用户：鼠标滚轮缩放时往鼠标位置放大；默认开）
    property bool zoomToCursor: true
    // 平移模式（checkbox 开关，默认关）：拖拽选中 note = 移动；勾选=按方向轴锁定
    // （纵向→时间/通道不变；横向→通道/时间不变）。未勾=自由 2D（时间+通道都动）。
    property bool moveMode: false
    // LN 选取模式（默认关）：开启后点选 LN 任一段自动选中配对两端（整体移动/删除）
    property bool lnSelectMode: false
    // 槽位弱线显示开关（「网格」按钮；默认开）。吸附计算不依赖此开关，仅控制画不画槽位线。
    property bool showGrid: true
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
                    text: qsTr("网格")
                    // 外部态驱动高亮（外部激活而非 checkable 自翻，避免断绑定，doc/04 §5）
                    active: window.showGrid
                    enabled: chartMeta !== null
                    onClicked: toggleGrid()
                    ToolTip.visible: hovered
                    ToolTip.text: qsTr("开/关槽位弱线（网格显示开关；吸附不依赖此开关）")
                }
                BbToolButton {
                    text: qsTr("量化")
                    // 2026-09：变换类按钮需先选中 note 才点亮（量化/镜像/旋转一致）
                    enabled: chartMeta !== null && window.selectionRefs.length > 0
                    onClicked: quantizeSelection()
                    ToolTip.visible: hovered
                    ToolTip.text: qsTr("把选中 note 吸附到当前 snap 网格（一个 undo 步；先选中再点）")
                }
                BbToolButton {
                    text: qsTr("镜像")
                    enabled: chartMeta !== null && window.selectionRefs.length > 0
                    onClicked: transformSelection(true, 0)
                    ToolTip.visible: hovered
                    ToolTip.text: qsTr("左右镜像选中 note（key i ↔ key 8-i；一个 undo 步）")
                }
                BbToolButton {
                    text: qsTr("旋转")
                    enabled: chartMeta !== null && window.selectionRefs.length > 0
                    onClicked: transformSelection(false, 1)
                    ToolTip.visible: hovered
                    ToolTip.text: qsTr("循环右移一格 key 轨（1→2→…→7→1；一个 undo 步）")
                }
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
                               ToolTip.text: qsTr("当前缩放（点击恢复 100% = 小节高度 96px）；Ctrl+滚轮缩放（最大 500%）") }
                BbCheckBox {
                    text: qsTr("光标缩放")
                    checked: window.zoomToCursor
                    onToggled: window.zoomToCursor = checked
                    ToolTip.visible: hovered
                    ToolTip.text: qsTr("勾选=Ctrl+滚轮缩放时往鼠标位置放大（锚点=光标拍位）；"
                                       + "未勾=固定往视口中心放大") }
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
                // LN 选取模式（默认关）：勾选后点选 LN 任一段，自动选中配对两端（可整体移动/删除）
                BbCheckBox {
                    id: lnSelectCheck
                    text: qsTr("LN 选取")
                    checked: window.lnSelectMode
                    onToggled: window.lnSelectMode = checked
                    ToolTip.visible: hovered
                    ToolTip.text: qsTr("勾选=点 LN 任一段自动选中配对两端（整体移动/删除）；未勾=LNs 当单 note")
                }
                // 单点 ↔ LN 转换（2026-09 用户）：选中游玩轨 note 一键转换
                BbToolButton {
                    text: qsTr("单点/LN")
                    enabled: chartMeta !== null && window.selectionRefs.length > 0
                    onClicked: toggleLnSelection()
                    ToolTip.visible: hovered
                    ToolTip.text: qsTr("按 LNTYPE 切换选中 note 的 LN 通道（LNTYPE 1：普通↔5x/6x）；"
                                       + "配对由同通道时间序交替自动组成（无向前查询）")
                }
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
                lnSelectMode: window.lnSelectMode
                showExtras: window.showExtras
                showGrid: window.showGrid
                editorTool: window.editorTool
                moveMode: window.moveMode
                sampleId: chartSession.sampleValueOf(window.currentSampleId)
                sampleText: sampleModel.currentSampleText
                selection: window.selectionRefs
                snapNum: window.snapNum
                snapDen: window.snapDen
                zoomToCursor: window.zoomToCursor
                perfLog: window.debugPerfLog
                onSamplePicked: (id, file) => {
                    // 会话状态：当前采样（M3 放置落点；不入 undo，doc/05 §1.2）
                    window.currentSampleId = id
                    setStatus(qsTr("当前采样：#WAV%1 %2").arg(id, file))
                }
                onSampleFileRequested: (id, file) => setSampleFile(id, file)
                onHitPlaceRequested: (hit) => placeNote(hit)
                onSelectionFinished: (refs) => onSelectionMade(refs)
                onNoteClicked: (ref, ctrl) => window.onNoteClicked(ref, ctrl)
                onCanvasClicked: () => window.onCanvasClicked()
                onNoteRightDeleted: (ref) => deleteNoteAt(ref)
                onNoteEditRequested: (ref) => editNoteSample(ref)
                onMoveSelectionRequested: (deltaF, targetLane, sourceLane) => moveSelection(deltaF, targetLane, sourceLane)
                onMetaMessage: (msg) => setStatus(msg)
                onMetaSaveRequested: saveMetaEdits()
                onEditAreaPressed: clearTextFocus()
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
                // 文件格式/编码（问题2：底部右下角，类似文本编辑器；info/check 返回）
                Label {
                    text: window.chartFormat !== "" ? window.chartFormat.toUpperCase() +
                             (window.chartEncoding !== "" ? " · " + window.chartEncoding : "") : ""
                    color: Theme.textFaint; font.family: Theme.fontMono; font.pixelSize: Theme.fsSmall
                    visible: window.chartFormat !== ""
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
        nameFilters: [qsTr("BMS 谱面 (*.bms *.bml *.bme *.pms)"), qsTr("所有文件 (*)")]
        onAccepted: openChart(urlToPath(selectedFile))
        onRejected: { /* 用户取消 */ }
    }

    // --open 调试参数（main.cpp 注入）：走与 Ctrl+O 相同的调用路径
    property string debugOpenPath: ""
    onDebugOpenPathChanged: if (debugOpenPath !== "") openChart(debugOpenPath)
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
        onAccepted: saveChartAs(urlToPath(selectedFile))
    }

    function openChart(path) {
        var req = JSON.stringify({ command: "info", args: { path: path } })
        var resp = beatbench.dispatch(req)
        var r = JSON.parse(resp)
        if (r.ok) {
            window.chartMeta = r.result.meta
            window.chartPath = r.result.path
            window.chartFormat = r.result.format !== undefined ? r.result.format : ""
            // 编码：从 info/check 的 diagnostics（"encoding: UTF-8 (path)"）提取
            var enc = ""
            if (r.result.diagnostics) {
                for (var di = 0; di < r.result.diagnostics.length; di++) {
                    var dm = r.result.diagnostics[di].message
                    if (dm && dm.indexOf("encoding:") === 0) {
                        var colon = dm.indexOf(":", 9)
                        enc = dm.substring(9, colon > 9 ? colon : dm.length).trim()
                        break
                    }
                }
            }
            window.chartEncoding = enc
            // M2 第 5 步：时间轴真数据（ChartSession + TimingEngine，与 info 同源解析）
            chartSession.openChart(path)
            // 元信息可编辑表单载入（meta.list；session 已 load，此后编辑保存走 meta.edit）
            if (typeof editPage !== "undefined" && editPage) editPage.reloadMeta()
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
        refreshLint()  // 编辑后刷新 lint 面板（内存 lint：LN 未配对等）
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
    /// 内存 lint（session.lint）→ lintModel（编辑后「LN 通道 note 未组成完整 LN」等提示）。
    function refreshLint() {
        var req = JSON.stringify({ command: "session.lint", args: {} })
        var resp = beatbench.dispatch(req)
        lintModel.loadFromIssues(resp)
    }
    function deleteNoteAt(ref) {
        var args = {
            measure: ref.measure, pos: ref.pos, lane: ref.lane, sample: ref.sample
        }
        if (ref.bgm_line !== undefined) args.bgm_line = ref.bgm_line
        var r = sessionCmd("note.delete", args)
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
            var args = {
                measure: refs[i].measure, pos: refs[i].pos,
                lane: refs[i].lane, sample: refs[i].sample
            }
            if (refs[i].bgm_line !== undefined) args.bgm_line = refs[i].bgm_line
            var r = dispatchCmd("note.delete", args)
            if (r) done++
        }
        if (done > 0) {
            chartSession.refresh()
            refreshLint()  // 2026-09 用户：删除后 lint 也要刷新（之前只刷新视图没刷新 lint）
            window.selectionRefs = []
            setStatus(qsTr("已删除 %1 个 note（Undo 可恢复）").arg(done))
        }
    }
    function placeNote(hit) {
        // kind 语义（M3 note.put 已支持）：note→normal / ln→LN 自动配对 / mine→地雷。
        var kind = "normal"
        if (window.editorTool === "ln") kind = "ln"
        else if (window.editorTool === "mine") kind = "mine"
        // 2026-09 用户：LN 只能放在「游玩轨/LN 轨」——BMS 中 BGM（ch01）无 LN 通道表示
        //（映射层 bms_channel_for(Bgm,ln) 为空，写出会丢/降级）。前端先行阻止。
        if (kind === "ln" && hit.laneKind === "bgm") {
            setStatus(qsTr("BGM 轨不能放置 LN（格式无 LN 通道表示）；请用「3 放置」放普通背景音"))
            return
        }
        // BGM 展开列带 sampleHint（该列固定 #WAV id）→ 直接用；否则取当前采样
        if (hit.sampleHint !== undefined && hit.sampleHint >= 0) {
            var putArgs = {
                measure: hit.measure,
                pos: { num: hit.num, den: hit.den },
                lane: { player: hit.lanePlayer, kind: hit.laneKind, index: hit.laneIndex },
                sample: hit.sampleHint,
                kind: kind
            }
            if (hit.bgm_line !== undefined && hit.bgm_line >= 0)
                putArgs.bgm_line = hit.bgm_line
            var r0 = sessionCmd("note.put", putArgs)
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
        // ⚠️ 问题1（2026-09）：BGM 展开列（bgmLine>=0，无固定 sampleHint）放置必须传
        // bgm_line——否则 note.put 默认 bgm_line=0 落到 bgm1（虚拟子通道行号丢失）。
        var putArgs2 = {
            measure: hit.measure,
            pos: { num: hit.num, den: hit.den },
            lane: { player: hit.lanePlayer, kind: hit.laneKind, index: hit.laneIndex },
            sample: v,
            kind: kind
        }
        if (hit.bgm_line !== undefined && hit.bgm_line >= 0)
            putArgs2.bgm_line = hit.bgm_line
        var r = sessionCmd("note.put", putArgs2)
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
               a.pos.num === b.pos.num && a.pos.den === b.pos.den &&
               (a.bgm_line === undefined || b.bgm_line === undefined || a.bgm_line === b.bgm_line)
    }
    function onNoteClicked(ref, ctrl) {
        // LN 选取模式（默认关）：点 LN 任一段 → 自动纳入配对段。ref 由 noteAt 返回，
        // 命中 LN 时带 lnPartner（配对段的 NoteRef）。选中集可整体移动/删除。
        if (window.lnSelectMode && ref.lnPartner) {
            var picked = [ref.lnPartner, ref]
            if (!ctrl) { window.selectionRefs = picked }
            else {
                // Ctrl+点击：把两端整体加入（若已含则移除）——简单化：追加未含的段
                var arr = window.selectionRefs.slice()
                for (var i = 0; i < picked.length; i++) {
                    var exists = false
                    for (var j = 0; j < arr.length; j++)
                        if (refEquals(arr[j], picked[i])) { exists = true; break }
                    if (!exists) arr.push(picked[i])
                }
                window.selectionRefs = arr
            }
            setStatus(qsTr("已选中 LN（%1 段）").arg(window.selectionRefs.length))
            return
        }
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
    /// 2026-09 跨命名空间：targetLane 带 metaKind（bpm/stop）→ note.convert（id 不变）；
    /// BGA 图层列（bgaLayer >= 0）→ note.convert（bga_*）；其余 = note.moveRegion。
    /// 移动选中 note（统一位移：拖拽/框选整段/多选）。
    /// deltaF = 连续拍位位移（可负可跨小节）；targetLane = 横向目标列（laneAtX；null=纯时间）；
    /// sourceLane = 拖起 note 所在轨（{player,kind,index}）。跨命名空间（BPM/STOP/BGA）→ note.convert；
    /// 普通轨 → note.move（moves 数组，**跨通道只把「拖起轨 sourceLane 的 note」改到目标轨**，
    /// 其余 note 仅时间移动、保持原轨道——修复 2026-09 多选跨通道全部挤到松开通道的 bug）。
    /// ⚠️ note.move = CompositeCommand 一个 undo 步。移动后**保持选中**（selectionRefs 更新到新位置）。
    function moveSelection(deltaF, targetLane, sourceLane) {
        if (!window.selectionRefs || window.selectionRefs.length === 0) {
            setStatus(qsTr("先选中 note（选择工具点击/框选）再移动"))
            return
        }
        const refs = window.selectionRefs.slice()
        // delta snap：把连续拍位位移吸附到当前槽（snapNum/snapDen 小节），再拆成
        // {measure(int 小节分量), pos(节内分数分量)}。BMS 槽位为离散步长，吸附后对齐网格。
        const num = window.snapNum, den = window.snapDen
        const slots = Math.max(1, Math.floor(den / num))
        const snappedF = Math.round(deltaF * slots) / slots
        const m = Math.floor(snappedF)
        const frac = snappedF - m
        const delta = { measure: m, pos: { num: Math.round(frac * den), den: den } }
        if (targetLane && targetLane.valid) {
            // 跨命名空间：BPM/STOP 列（metaKind）→ note.convert；BGA 图层列 → note.convert（bga_*）
            if (targetLane.metaKind === "bpm" || targetLane.metaKind === "stop") {
                const r0 = sessionCmd("note.convert", { selection: refs, target: targetLane.metaKind, delta: delta })
                if (r0) {
                    window.selectionRefs = []
                    setStatus(qsTr("已转换 %1 个 note → %2（id 不变）").arg(r0.notes).arg(targetLane.metaKind.toUpperCase()))
                }
                return
            }
            if (targetLane.bgaLayer !== undefined && targetLane.bgaLayer >= 0) {
                const bgaTarget = targetLane.bgaLayer === 1 ? "bga_poor"
                                  : targetLane.bgaLayer === 2 ? "bga_layer"
                                  : targetLane.bgaLayer === 3 ? "bga_layer2" : "bga_base"
                const r1 = sessionCmd("note.convert", { selection: refs, target: bgaTarget, delta: delta })
                if (r1) {
                    window.selectionRefs = []
                    setStatus(qsTr("已转换 %1 个 note → BGA（id 不变）").arg(r1.notes))
                }
                return
            }
        }
        // 普通轨道移动：逐 note 计算 to（绝对位置 = 源 + delta，带进位）。
        // **通道「同距离偏移」**（2026-09 用户：拖动时每个选中 note 都移动相同距离）：
        // 拖起轨 key i → 目标轨 key j，偏移 = j-i；所有选中 key note 左/右移相同量（保相对位置），
        // 不再「全部挤到目标轨」或「只动拖起轨」。非 key note（皿/踏板/BGM）不变；
        // 跨 kind（如 key→皿）走 sourceLane 兜底（拖起轨 note 改到目标 kind）。
        let channelOffset = 0
        const keyToKey = sourceLane && sourceLane.kind === "key" &&
                         targetLane && targetLane.valid && targetLane.laneKind === "key"
        if (keyToKey) channelOffset = targetLane.laneIndex - sourceLane.index
        const moves = []
        const newRefs = []
        for (let i = 0; i < refs.length; i++) {
            const ref = refs[i]
            const to = addPosDelta(ref, delta)
            let changedLane = false
            if (channelOffset !== 0 && ref.lane.kind === "key") {
                // 同距离偏移（key 轨；钳到合法 key 范围 1..7）
                const ni = Math.max(1, Math.min(7, ref.lane.index + channelOffset))
                if (ni !== ref.lane.index) {
                    to.lane = { player: ref.lane.player, kind: "key", index: ni }
                    changedLane = true
                }
            } else if (targetLane && targetLane.valid && sourceLane &&
                    laneEquals(ref.lane, sourceLane.kind, sourceLane.index, sourceLane.player) &&
                    !laneEquals(ref.lane, targetLane.laneKind, targetLane.laneIndex, targetLane.lanePlayer)) {
                // 跨 kind 兜底：拖起轨 note 改到目标 kind（其余 keep）
                to.lane = { player: targetLane.lanePlayer, kind: targetLane.laneKind,
                            index: targetLane.laneIndex }
                if (targetLane.bgm_line !== undefined && targetLane.bgm_line >= 0)
                    to.bgm_line = targetLane.bgm_line
                changedLane = true
            }
            moves.push({ from: ref, to: to })
            // 移动后保持选中：selectionRefs 更新到新位置（measure/pos/lane/bgm_line）
            const nr = { measure: to.measure, pos: to.pos,
                         lane: changedLane ? to.lane : ref.lane,
                         sample: ref.sample }
            if (to.bgm_line !== undefined) nr.bgm_line = to.bgm_line
            else if (ref.bgm_line !== undefined) nr.bgm_line = ref.bgm_line
            newRefs.push(nr)
        }
        const r = sessionCmd("note.move", { moves: moves })
        if (r) {
            window.selectionRefs = newRefs   // 保持选中（新位置）
            const chan = targetLane && targetLane.valid && Math.abs(deltaF) < 0.0001
            setStatus(chan ? qsTr("已移动 %1 个 note（改通道）").arg(r.moved)
                           : qsTr("已移动 %1 个 note（+%2 拍）").arg(r.moved).arg(deltaF.toFixed(3)))
        }
    }
    /// 源位置 + 位移增量 → 绝对目标位置（带小节进位；分数约分，保证与 core Rational 一致）。
    function addPosDelta(ref, delta) {
        const rn = ref.pos.num, rd = ref.pos.den
        const dn = delta.pos.num, dd = delta.pos.den
        const newNum = rn * dd + dn * rd
        const newDen = rd * dd
        const carry = Math.floor(newNum / newDen)
        const g = gcd(newNum - carry * newDen, newDen)
        return { measure: ref.measure + delta.measure + carry,
                 pos: { num: (newNum - carry * newDen) / g, den: newDen / g } }
    }
    function gcd(a, b) {
        a = Math.abs(a); b = Math.abs(b)
        while (b) { const t = b; b = a % b; a = t }
        return a || 1
    }
    function laneEquals(lane, kind, index, player) {
        return lane && lane.kind === kind && lane.index === index && lane.player === player
    }
    /// 单点 ↔ LN 转换（工具栏「单点/LN」按钮；selection 批量一个 undo 步）。
    /// LNTYPE 翻转选中 note 的 LN 通道（LNTYPE 1：ln_channel ←→ 普通；配对由 rebuild 自动）。
    function toggleLnSelection() {
        if (!window.selectionRefs || window.selectionRefs.length === 0) {
            setStatus(qsTr("先选中 note（点击/框选）再转换单点/LN"))
            return
        }
        // 2026-09：BGM 轨无 LN 通道表示，过滤掉这类 note（其余继续转换）
        var playable = window.selectionRefs.filter(function (r) {
            return r.lane && r.lane.kind !== "bgm"
        })
        if (playable.length === 0) {
            setStatus(qsTr("选中的都是 BGM 轨 note（无 LN 通道表示），无法转换"))
            return
        }
        var r = sessionCmd("note.toggleLn", { selection: playable.slice() })
        if (r) {
            window.selectionRefs = []
            setStatus(qsTr("已转换 %1 个 note（单点↔LN）").arg(r.notes))
        }
    }
    /// 量化：把选中 note 的 pos 吸附到当前 snap 网格（note.quantize；一个 undo 步）。
    /// 量化后**保持选中**（selectionRefs 的 pos 更新到吸附值）。
    function quantizeSelection() {
        if (!window.selectionRefs || window.selectionRefs.length === 0) {
            setStatus(qsTr("先选中 note（点击/框选）再量化"))
            return
        }
        const sn = window.snapNum, sd = window.snapDen
        var r = sessionCmd("note.quantize", {
            selection: window.selectionRefs.slice(),
            snap: { num: sn, den: sd }
        })
        if (r) {
            // 保持选中：pos 更新到吸附值（k*snapNum/snapDen，约分）
            window.selectionRefs = window.selectionRefs.map(function(ref) {
                const p = ref.pos.num / ref.pos.den
                const k = Math.round(p * sd / sn)
                const nnum = k * sn, nden = sd
                const g = gcd(nnum, nden)
                return { measure: ref.measure, pos: { num: nnum / g, den: nden / g },
                         lane: ref.lane, sample: ref.sample,
                         bgm_line: ref.bgm_line }
            })
            setStatus(qsTr("已量化 %1 个 note（吸附到 %2/%3）").arg(r.notes).arg(sn).arg(sd))
        }
    }
    /// 变换：镜像（mirror=true）/ 旋转（rotate=±1）；note.transform；一个 undo 步。
    /// 变换后**保持选中**（selectionRefs 的 key 轨道更新为镜像/旋转后的下标；相对位置不变）。
    /// ⚠️ 只处理 key 轨（镜像/旋转不影响皿/踏板/BGM）；按 7key 映射（key i ↔ key 8-i）。
    function transformSelection(mirror, rotate) {
        if (!window.selectionRefs || window.selectionRefs.length === 0) {
            setStatus(qsTr("先选中 note（点击/框选）再变换"))
            return
        }
        var args = { selection: window.selectionRefs.slice() }
        if (mirror) args.mirror = true
        if (rotate !== 0) args.rotate = rotate
        var r = sessionCmd("note.transform", args)
        if (r) {
            window.selectionRefs = window.selectionRefs.map(function(ref) {
                if (ref.lane.kind !== "key") return ref   // 非 key 轨不变
                const idx = ref.lane.index
                const nidx = mirror ? (8 - idx) : (((idx - 1 + rotate) % 7 + 7) % 7 + 1)
                return { measure: ref.measure, pos: ref.pos,
                         lane: { player: ref.lane.player, kind: "key", index: nidx },
                         sample: ref.sample, bgm_line: ref.bgm_line }
            })
            setStatus(mirror ? qsTr("已镜像 %1 个 note").arg(r.notes)
                             : qsTr("已旋转 %1 个 note").arg(r.notes))
        }
    }
    /// 网格开关：折叠/展开槽位弱线显示（不影响吸附）。
    function toggleGrid() {
        window.showGrid = !window.showGrid
        setStatus(window.showGrid ? qsTr("网格显示：开") : qsTr("网格显示：关"))
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
    /// 设置采样槽位绑定的文件名（双击采样行编辑；sample.setFile 一个 undo 步）。成功后
    /// 从内存会话刷新采样面板（id/file/引用数；session.samples 与 info 同构，枚举全部槽位）。
    function setSampleFile(id, file) {
        if (id === undefined || id === null || id === "") return
        const r = sessionCmd("sample.setFile", { id: id, file: file })
        if (!r) return
        const sresp = beatbench.dispatch(JSON.stringify({ command: "session.samples", args: {} }))
        if (sresp) {
            const r2 = JSON.parse(sresp)
            if (r2.ok) sampleModel.loadFromInfo(JSON.stringify({ ok: true, result: r2.result }))
        }
        setStatus(qsTr("#WAV%1 → %2（Undo 可恢复）").arg(id, file))
    }

    /// 编辑区双击 note → 改其引用采样 id（切音手工版）。弹出对话框，收新 #WAV id → note.setSample。
    function editNoteSample(ref) {
        if (!ref) return
        noteSampleDialog.ref = ref
        noteIdInput.text = chartSession.idTextOf(ref.sample)
        noteSampleDialog.open()
    }

    function saveChart() {
        // 2026-09：元信息修改交整个文件保存——先应用元信息编辑 + 扩展代码，再 session.save。
        if (typeof editPage !== "undefined" && editPage) {
            const edits = editPage.collectMetaEdits()
            if (edits && edits.length > 0) {
                const em = sessionCmd("meta.edit", { edits: edits })
                if (em) setStatus(qsTr("元信息已保存 %1 处").arg(edits.length))
            }
            editPage.applyRawEdits()
            refreshLint()
        }
        var r = sessionCmd("session.save", { overwrite: true })
        if (r) {
            window.chartPath = r.output
            setStatus(qsTr("已保存：%1（%2 字节）").arg(r.output).arg(r.bytes))
        }
    }
    /// 元信息面板「保存」按钮：只应用元信息编辑 + 扩展代码到内存会话（不写文件）。
    /// 之后 Ctrl+S / 另存为会随整个文件一并落盘。成功后提交基线（orig=value）清脏。
    function saveMetaEdits() {
        if (typeof editPage === "undefined" || !editPage) return
        const edits = editPage.collectMetaEdits()
        let n = 0
        if (edits && edits.length > 0) {
            const em = sessionCmd("meta.edit", { edits: edits })
            if (!em) return   // 失败：sessionCmd 已置状态栏
            n = edits.length
        }
        if (editPage.applyRawEdits()) n++
        editPage.commitMeta()
        refreshLint()
        setStatus(n > 0
                  ? qsTr("元信息已保存 %1 处（写文件时随整体保存落盘）").arg(n)
                  : qsTr("元信息无改动"))
    }
    function saveChartAs(path) {
        if (typeof editPage !== "undefined" && editPage) {
            const edits = editPage.collectMetaEdits()
            if (edits && edits.length > 0) sessionCmd("meta.edit", { edits: edits })
            editPage.applyRawEdits()
        }
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

    // 编辑区双击 note → 改引用采样 id（切音手工版；note.setSample）
    Dialog {
        id: noteSampleDialog
        title: qsTr("修改 note 引用采样 #WAV id")
        modal: true
        anchors.centerIn: parent
        width: 320
        property var ref: null
        standardButtons: Dialog.Ok | Dialog.Cancel
        onOpened: noteIdInput.forceActiveFocus()
        ColumnLayout {
            anchors.fill: parent
            spacing: 6
            Label {
                text: noteSampleDialog.ref
                      ? qsTr("当前引用：#WAV%1").arg(chartSession.idTextOf(noteSampleDialog.ref.sample))
                      : ""
                color: Theme.textMuted
                font.family: Theme.fontMono
                font.pixelSize: Theme.fsSmall
            }
            BbTextField {
                id: noteIdInput
                Layout.preferredWidth: 150
                font.family: Theme.fontMono
                placeholderText: qsTr("#WAV id（如 1A）")
            }
            Label { text: qsTr("双击左 Dock 采样行可给该槽位绑定/改文件"); color: Theme.textFaint;
                    font.pixelSize: Theme.fsTiny }
        }
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
            if (r.bgm_line !== undefined) args.bgm_line = r.bgm_line
            const res = sessionCmd("note.setSample", args)
            if (res) setStatus(qsTr("note → #WAV%1（Undo 可恢复）").arg(to))
        }
    }
}
