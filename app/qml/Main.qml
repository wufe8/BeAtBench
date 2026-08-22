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
    // 拍子线 [num]/[den]（默认 [1]/[4] = 每 4 分音符；M3 note 吸附也会依赖）
    property int beatNum: 1
    property int beatDen: 4
    // note 上显示所用采样：0 隐藏 / 1 id / 2 文件名
    property int noteSampleMode: 0
    // 更多轨道（BGA 图层通道列，iBMSC 式；游玩轨与背景轨之间）
    property bool showExtras: false
    // 调试参数注入（--bgm-expand / --channel-ids / --note-labels / --show-extras，配 --screenshot 验收）
    property bool debugBgmExpand: false
    property bool debugShowChannelIds: false
    property int debugNoteSampleMode: 0
    property bool debugShowExtras: false
    onDebugBgmExpandChanged: if (debugBgmExpand && editPage) editPage.bgmExpanded = true
    onDebugShowChannelIdsChanged: if (debugShowChannelIds) window.showChannelIds = true
    onDebugNoteSampleModeChanged: if (debugNoteSampleMode > 0) window.noteSampleMode = debugNoteSampleMode
    onDebugShowExtrasChanged: if (debugShowExtras) window.showExtras = true
    // 当前编辑工具（互斥单选，会话状态；M3 接输入/放置，note 类型（普通/LN/地雷）届时
    // 作为正交维度另设「放置类型」组，不并入本组——doc/05 §5 交互）
    property string editorTool: "select"

    // ---------- 全局快捷键（QML MenuItem 无 shortcut 属性，用 Shortcut 类型） ----------
    Shortcut { sequence: "Ctrl+O"; onActivated: fileDialog.open() }
    Shortcut { sequence: "Ctrl+Q"; onActivated: window.close() }

    // ---------- 菜单栏（固定全局） ----------
    menuBar: MenuBar {
        Menu {
            title: qsTr("文件")
            MenuItem {
                text: qsTr("打开谱面…")
                onTriggered: fileDialog.open()
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
            MenuItem { text: qsTr("元信息编辑（M3）"); enabled: false }
            MenuItem { text: qsTr("撤销（M3）"); enabled: false }
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
                BbToolButton { text: "snap 1/16"; enabled: chartMeta !== null }
                BbToolButton { text: qsTr("量化"); enabled: chartMeta !== null }
                BbToolButton { text: qsTr("网格"); enabled: chartMeta !== null }
                // 拍子线显示调整（每 num/den 音符一条；M3 note 吸附复用此单位）
                Label { text: qsTr("拍子线"); color: Theme.textFaint
                        font.pixelSize: Theme.fsTiny; padding: 4 }
                SpinBox {
                    from: 1; to: 16
                    value: window.beatNum
                    editable: true
                    onValueModified: window.beatNum = value
                    font.pixelSize: Theme.fsSmall
                    implicitWidth: 54
                }
                Label { text: "/"; color: Theme.textMuted; font.pixelSize: Theme.fsSmall }
                SpinBox {
                    from: 1; to: 16
                    value: window.beatDen
                    editable: true
                    onValueModified: window.beatDen = value
                    font.pixelSize: Theme.fsSmall
                    implicitWidth: 54
                }
                // 更多轨道：BGA 图层通道列（04/06/07/0A，游玩轨与背景轨之间，iBMSC 式）
                CheckBox {
                    id: extrasCheck
                    text: qsTr("更多轨道")
                    checked: window.showExtras
                    onToggled: window.showExtras = checked
                    ToolTip.visible: hovered
                    ToolTip.text: qsTr("在游玩轨与背景轨之间显示 BGA 图层通道（BGA/LAYER/POOR/LAYER2 = 04/06/07/0A）")
                }
                BbToolButton { text: qsTr("缩放"); enabled: chartMeta !== null }
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
                // 互斥单选：active = 外部状态（editorTool），无 checkable 断绑残留问题
                BbToolButton { text: "V 选择"; active: window.editorTool === "select"; flatStyle: true
                               onClicked: window.editorTool = "select" }
                BbToolButton { text: "N 放置"; active: window.editorTool === "note"; flatStyle: true
                               onClicked: window.editorTool = "note" }
                BbToolButton { text: "L LN"; active: window.editorTool === "ln"; flatStyle: true
                               onClicked: window.editorTool = "ln" }
                BbToolButton { text: "M 地雷"; active: window.editorTool === "mine"; flatStyle: true
                               onClicked: window.editorTool = "mine" }
                BbToolButton { text: "H 平移"; active: window.editorTool === "pan"; flatStyle: true
                               onClicked: window.editorTool = "pan" }
                Item { Layout.fillWidth: true }
                // 轨道名 → 实际通道 id（皿=16、键1=11、BGM=01…；Ctrl 临时切换，Adobe 式）
                CheckBox {
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
                ComboBox {
                    model: [qsTr("隐藏"), qsTr("显示 ID"), qsTr("显示文件名")]
                    currentIndex: window.noteSampleMode
                    onActivated: (idx) => window.noteSampleMode = idx
                    font.pixelSize: Theme.fsSmall
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
                beatNum: window.beatNum
                beatDen: window.beatDen
                noteSampleMode: window.noteSampleMode
                showExtras: window.showExtras
                onSamplePicked: (id, file) => {
                    // 会话状态：当前采样（M3 放置落点；不入 undo，doc/05 §1.2）
                    window.currentSampleId = id
                    setStatus(qsTr("当前采样：#WAV%1 %2").arg(id, file))
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
