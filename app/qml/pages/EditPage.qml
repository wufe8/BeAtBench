// SPDX-License-Identifier: GPL-3.0-only
// 编辑页（M2 核心）：三栏模板 = 空模板的默认布局（doc/05 §4.3）。
// 左 Dock（面板容器：元信息/采样/lint/BGA）+ 中央视口占位 + 右 Dock（属性）。
// 分栏 = QML 原生 SplitView（可拖拽调宽；不用 QDockWidget，doc/07 §3）。
// ⚠️ Qt 6.11 SplitView 附属性是 SplitView.*（preferredWidth/fillWidth/minimumWidth），不是 Layout.*。
// 面板内容由本页装配；页面只做布局与展示，编辑命令接入归 M3。
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import "../components"  // BbTabButton（页面工具条等，默认皮肤组件库）

Item {
    id: root
    property var chartMeta: null
    property string chartPath: ""
    /// 游玩模式显示名（Main 提供：SP7K/DP/PMS/…，随谱面实际模式）。
    property string modeLabel: "SP7K"
    /// 轨道列头显示实际 BMS 通道 id（Main 工具条勾选；Alt 临时在 ChartView 内处理）
    property bool showChannelIds: false
    /// BGM 轨展开（列头点击；--bgm-expand 调试参数）
    property bool bgmExpanded: false
    /// note 采样标签：0 隐藏 / 1 id / 2 文件名
    property int noteSampleMode: 0
    /// LN 选取模式（默认关）：点 LN 任一段自动选配对两端（整体移动/删除）
    property bool lnSelectMode: false
    /// 更多轨道（BGA 图层通道列，游玩轨与背景轨之间）
    property bool showExtras: false
    /// 槽位弱线显示开关（「网格」按钮；默认开）。吸附不依赖此开关。
    property bool showGrid: true
    /// M5.2 播放头跟随（默认开——用户拍板；**单向绑定**：Main checkbox ↔ 本属性；
    /// 用户滚动 ChartView 内部 root.followPlayhead=false → 本属性自动跟随（绑定）
    /// ——无需回写，避免 QML 绑定循环断绑（2026-09 实测：双向回写断绑定 → 默认关+无效果））。
    /// ⚠️ 写入口 = setFollowPlayhead（checkbox onToggled 走它；直接赋值会断绑定）。
    property bool followPlayhead: chartView ? chartView.followPlayhead : true
    function setFollowPlayhead(v) {
        if (chartView) chartView.followPlayhead = v
    }
    /// 编辑工具（select/note/ln/mine/pan；Main 会话状态）
    property string editorTool: "select"
    /// 平移开关（拖拽选中 note；默认关=自由 2D，勾选=轴锁定）
    property bool moveMode: false
    /// 放置用采样数值 id（chartSession.sampleValueOf；-1 = 未选）
    property int sampleId: -1
    /// 当前采样展示文本（提示用）
    property string sampleText: ""
    /// 选中 note 集合（NoteRef；框选后回填 → 高亮）
    property var selection: []
    /// 选中 BGA/BPM/STOP 对象集合（{kind, measure, pos, ...}；视口回填 → 高亮）
    property var metaSelection: []
    /// 时间轴事件（timing.list 结果；Main 在打开谱面/编辑后重取回填，供右 Dock 时间轴面板）
    property var timingBpm: []
    property var timingStop: []
    /// 02 通道小节长度事件（timing.list kind="measure"）。
    property var timingMeasure: []
    property var timingBpmDefs: []
    property var timingStopDefs: []
    /// 吸附粒度（放置用：snapNum/snapDen 槽/小节；Main snap 分子分母）
    property int snapNum: 1
    property int snapDen: 16
    /// 缩放锚点模式（2026-09 用户）：true=鼠标位置缩放（默认开）
    property bool zoomToCursor: true
    /// paint 帧耗时采样（--perf-log）
    property bool perfLog: false
    /// 状态栏：鼠标位置 + note 信息（ChartViewItem.hoverText）
    readonly property string hoverText: chartView ? chartView.hoverText : ""
    /// M5.2 视口光标（红线）读数：秒 + 拍位文本（转发 ChartView；Main 状态栏用，
    /// 红线=视口光标——滚动内容滚过红线，值随视口变；2026-09 用户）。
    readonly property real cursorSec: chartView ? chartView.cursorSec : 0
    readonly property string cursorPosText: chartView ? chartView.cursorPosText : ""
    /// 暴露采样面板（Main 在文件编辑后 scrollTo 定位，避免列表回到顶部）
    readonly property var samplePanelObj: samplePanel
    /// 暴露 BGA 面板（Main 打开谱面/编辑后调 reloadBga）
    readonly property var bgaPanelObj: bgaPanel

    /// 采样被选中（面板点击/键盘确认）→ Main 记录为当前采样（M3 放置落点）
    signal samplePicked(string id, string file)
    /// 采样槽位文件名编辑请求（双击采样行）→ Main 走 sample.setFile 并刷新面板
    signal sampleFileRequested(string id, string file)
    /// 添加 #WAV 定义（id + 文件名）→ Main 走 sample.addWav
    signal sampleAddRequested(string id, string file)
    /// note 工具点击（hitTest 结果）→ Main 走 note.put
    signal hitPlaceRequested(var hit)
    /// 框选完成 → Main 存 selection + 复制到剪贴板
    signal selectionFinished(var refs)
    /// select 点击命中 note（选中；ctrl = 多选切换）
    signal noteClicked(var ref, bool ctrl)
    /// 点击 note（按下→释放无拖动）→ 播放该采样（M4.3 前端；拖动移动不播）
    signal playNoteSample(var ref)
    /// select 点击空白（清空选中）
    signal canvasClicked()
    /// 右键命中 note（删除）
    signal noteRightDeleted(var ref)
    /// 双击命中 note（切音手工版：改引用采样 id）
    signal noteEditRequested(var ref)
    /// 时间轴事件编辑（添加/改值）→ Main 走 timing.put。ref = 手动绑定 id（空=auto）。
    signal timingEditRequested(string kind, int measure, int num, int den, double value, string ref)
    /// 时间轴事件删除 → Main 走 timing.delete
    signal timingDeleteRequested(string kind, int measure, int num, int den)
    signal timingDefAddRequested(string kind, string id, string value)
    signal timingDefDeleteRequested(string kind, string id)
    /// 平移：deltaF = 时间轴位移（拍位小数）；targetLane = 横向目标列（laneAtX；null=纯时间）；
    /// sourceLane = 拖起 note 所在轨（{player,kind,index}；跨通道多选只移此轨 note）；
    /// sourceBgmLine = 拖起 note 的 BGM 子通道行号（-1=非 BGM；BGM 相对平移对齐基准）
    signal moveSelectionRequested(real deltaF, var targetLane, var sourceLane, int sourceBgmLine)
    /// BGA/BPM/STOP 点选（选中 + 可移动）
    signal metaObjectClicked(var obj, bool ctrl)
    /// BGA/BPM/STOP 移动（kind + 对象 + 时间位移 + 横向目标列）
    signal metaMoveRequested(string kind, var obj, real deltaF, var targetLane)
    /// BGA/BPM/STOP 右键删除
    signal metaRightDeleted(var obj)
    /// BGA/BPM/STOP 双击编辑
    signal metaEditRequested(var obj)
    /// 元信息操作状态提示 → Main 置状态栏
    signal metaMessage(string msg)
    /// 元信息面板「保存」→ Main 只保存元信息（应用 meta.edit + meta.rawEdit，不写文件）
    /// 元信息「保存」按钮 → Main：应用元信息编辑 + 扩展代码到会话（不写文件）。
    signal metaSaveRequested()
    /// 实时模式字段（LNTYPE/LNOBJ）变更 → Main 立即 meta.edit（LN 放置模式实时生效）。
    signal modeEditRequested(string key, string value)
    /// 元信息面板修改（保存前须先应用：CollectMetaEdits / applyRawEdits）
    signal metaDirty()
    /// 当前 #BMP（视口放置用；Main 回填 → BgaPanel 高亮）。
    property string currentBmpId: ""
    /// 选择当前 #BMP（视口 BGA 列放置用）→ Main 设 currentBmpId
    signal bmpSelected(string id)
    /// 编辑区任意按下 → Main 释放文本框焦点
    signal editAreaPressed()
    /// BGA 事件编辑（添加/改值）→ Main 走 bga.put（bmpId 为文本 id）
    signal bgaEditRequested(int layer, int measure, int num, int den, string bmpId)
    /// BGA 事件删除 → Main 走 bga.delete
    signal bgaDeleteRequested(int layer, int measure, int num, int den)
    /// 添加 #BMP 定义 → Main 走 sample.setFile(kind=bmp)
    signal bmpAddRequested(string id, string file)
    /// 设置 #BMP 文件 → Main 走 sample.setFile(kind=bmp)
    signal bmpSetFileRequested(string id, string file)
    /// 重命名 #BMP id → Main 走 sample.rename(kind=bmp)
    signal bmpRenameRequested(string fromId, string toId)
    /// 删除 #BMP 定义 → Main 走 sample.delete(kind=bmp)
    signal bmpDeleteRequested(string id)

    /// STOP 值显示/填入单位（0=1/192全音符 1=ms；Main 会话状态 → 时间轴面板）
    property int stopUnit: 0
    /// STOP 毫秒换算参考 BPM（时间轴事件小节生效 BPM；Main 由 chartMeta.BPM 提供）
    property real stopBpm: 130
    /// 视口中心小节（粘贴 target_measure 用；转发 ChartView）。
    function centerMeasure() {
        return chartView ? chartView.centerMeasure() : 0
    }

    /// 秒 → 视口滚动（波形总览 seek；--seek 调试走同一路径）。
    function seekToSeconds(sec) {
        if (chartView) chartView.seekToSeconds(sec)
    }

    /// 元信息载入（Main 打开谱面后调用 → metaPanel.reload()）。
    function reloadMeta() {
        if (metaPanel) metaPanel.reload()
    }
    /// BGA 面板载入（Main 打开谱面 / BGA 编辑后调用 → 重读 #BMP 定义 + 当前层事件）。
    function reloadBga() {
        if (bgaPanel) bgaPanel.reload()
    }
    /// 元信息重置（放弃改动）→ Main 调用。
    function resetMeta() {
        if (metaPanel) metaPanel.reset()
    }
    /// 元信息脏字段编辑集（Main 在保存前调用；空数组 = 无改动）。
    function collectMetaEdits() {
        return metaPanel ? metaPanel.collectEdits() : []
    }
    /// 应用「扩展代码」原始行改动（Main 在保存前调用；返回是否应用）。
    function applyRawEdits() {
        return metaPanel ? metaPanel.applyRawEdits() : false
    }
    /// 保存后把当前编辑值设为基线（orig=value）并清脏（元信息「保存」按钮成功后在 Main 调用）。
    function commitMeta() {
        if (metaPanel) metaPanel.commit()
    }

    /// 诊断探针（--probe）：返回 ChartViewItem（含 probe(x,y)），定位选中/移动命中问题。
    function locateChartView() {
        return chartView
    }

    /// 调试入口（--click）：ChartView 局部坐标 → 同一手势分发路径。
    /// ⚠️ 坐标语义 = ChartView 局部（probe columns 的 x/w + contentsH 推算；非窗口像素）。
    function clickLocal(x, y) {
        if (chartView) chartView.clickAt(x, y)
    }

    /// 调试入口（--drag）：ChartView 局部坐标（按下→移动→释放）。
    function dragLocal(x1, y1, x2, y2) {
        if (chartView) chartView.dragAt(x1, y1, x2, y2)
    }

    /// 诊断探针（--probe）：ChartView 局部坐标 → probe。
    function probeLocal(x, y) {
        if (!chartView) return null
        return chartView.probe(x, y)
    }

    /// 缩放重置（工具条「缩放」按钮）。
    function resetZoom() {
        if (chartView) chartView.resetZoom()
    }

    /// 2026-09「加一小节」：透传到 ChartView → ChartViewItem.extendMeasures()。
    function extendMeasures() {
        if (chartView) chartView.extendMeasures()
    }

    /// 2026-09 新建谱面：设置编辑态有效小节数下限（seed 到 ChartView → ChartViewItem）。
    function setEditableMeasures(n) {
        if (chartView) chartView.setEditableMeasures(n)
    }

    /// 当前缩放百分比（工具条显示）。
    readonly property int zoomPercent: chartView ? chartView.zoomPercent : 100

    /// 调试入口（--click，旧）：本页局部坐标 → ChartView 局部坐标 → 同一手势分发路径。
    function clickAt(x, y) {
        if (!chartView) return
        const p = chartView.mapFromItem(root, x, y)
        chartView.clickAt(p.x, p.y)
    }

    SplitView {
        anchors.fill: parent
        orientation: Qt.Horizontal

        // 分隔条（拖拽调宽；hover 高亮提示可拖）
        handle: Rectangle {
            color: Theme.border
            implicitWidth: 4
            Rectangle {
                anchors.fill: parent
                color: SplitView.hovered ? Theme.accent : "transparent"
                opacity: 0.35
            }
        }

        // ---------- 左 Dock（面板容器） ----------
        Rectangle {
            SplitView.preferredWidth: 240
            SplitView.minimumWidth: 180
            color: Theme.surface
            border.color: Theme.border

            ColumnLayout {
                anchors.fill: parent
                anchors.margins: 8
                spacing: 8
                // 栏目条（可横向滚动；BGA 等更多栏目加入时不截断）
                BbTabStrip {
                    id: leftTabs
                    objectName: "leftTabs"  // 调试 --tab N 用（main.cpp findChild）
                    Layout.fillWidth: true
                    Layout.preferredHeight: 30
                    model: [qsTr("元信息"), qsTr("采样"), qsTr("lint"), qsTr("BGA")]
                    onIndexRequested: (index) => leftTabs.currentIndex = index
                }
                StackLayout {
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    currentIndex: leftTabs.currentIndex
                    MetaPanel {
                        id: metaPanel
                        meta: root.chartMeta
                        chartPath: root.chartPath
                        onMetaMessage: (msg) => root.metaMessage(msg)
                        onSaveRequested: root.metaSaveRequested()
                        onModeEditRequested: (key, value) => root.modeEditRequested(key, value)
                    }
                    SamplePanel { id: samplePanel; onSamplePicked: (id, file) => root.samplePicked(id, file);
                                  onSampleFileRequested: (id, file) => root.sampleFileRequested(id, file)
                                  onSampleAddRequested: (id, file) => root.sampleAddRequested(id, file) }
                    LintPanel {
                        onIssuePicked: (id) => {
                            // lint → 采样 双向往返：切到采样标签并定位该行
                            leftTabs.currentIndex = 1
                            samplePanel.requireId(id)
                        }
                    }
                    BgaPanel {
                        id: bgaPanel
                        objectName: "bgaPanel"  // Main 打开谱面/编辑后调 reload()
                        currentBmpId: root.currentBmpId
                        onBgaEditRequested: (layer, measure, num, den, bmpId) =>
                            root.bgaEditRequested(layer, measure, num, den, bmpId)
                        onBgaDeleteRequested: (layer, measure, num, den) =>
                            root.bgaDeleteRequested(layer, measure, num, den)
                        onBmpAddRequested: (id, file) => root.bmpAddRequested(id, file)
                        onBmpSetFileRequested: (id, file) => root.bmpSetFileRequested(id, file)
                        onBmpRenameRequested: (fromId, toId) => root.bmpRenameRequested(fromId, toId)
                        onBmpDeleteRequested: (id) => root.bmpDeleteRequested(id)
                        onBmpSelected: (id) => root.bmpSelected(id)
                    }
                }
            }
        }
        Rectangle {
            SplitView.fillWidth: true
            SplitView.minimumWidth: 320
            color: Theme.bg
            border.color: Theme.border

            ColumnLayout {
                anchors.fill: parent
                spacing: 0

                // 视口头（对照 preview.html .viewport-head）
                Rectangle {
                    Layout.fillWidth: true
                    Layout.preferredHeight: 30
                    color: Theme.surface
                    RowLayout {
                        anchors.fill: parent
                        anchors.leftMargin: 12
                        anchors.rightMargin: 12
                        spacing: 12
                        Label {
                            text: qsTr("编辑工作区 · <b>%1</b> · %2/%3 snap").arg(root.modeLabel).arg(root.snapNum).arg(root.snapDen)
                            color: Theme.textMuted
                            font.family: Theme.fontMono
                            font.pixelSize: Theme.fsSmall
                            textFormat: Text.RichText
                        }
                        Item { Layout.fillWidth: true }
                        Label {
                            text: root.chartMeta
                                  ? qsTr("BPM %1").arg(root.chartMeta.BPM !== undefined ? root.chartMeta.BPM : "—")
                                  : qsTr("打开谱面开始编辑（Ctrl+O）")
                            color: root.chartMeta ? Theme.accent : Theme.textFaint
                            font.family: Theme.fontMono
                            font.pixelSize: Theme.fsSmall
                        }
                    }
                    Rectangle {  // 下边框
                        anchors.left: parent.left
                        anchors.right: parent.right
                        anchors.bottom: parent.bottom
                        height: 1
                        color: Theme.border
                    }
                }

                // 视口主体（M2 第 5 步：竖向时间轴，真数据 ChartSession）
                ChartView {
                    id: chartView
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    showChannelIds: root.showChannelIds
                    bgmExpanded: root.bgmExpanded
                    noteSampleMode: root.noteSampleMode
                    lnSelectMode: root.lnSelectMode
                    showExtras: root.showExtras
                    showGrid: root.showGrid
                    editorTool: root.editorTool
                    moveMode: root.moveMode
                    // M5.2 播放头跟随：单向绑定（root.followPlayhead = chartView 的值；
                    // 用户滚动 ChartView 内部改——此处不设值，只让 root 读）
                    followPlayhead: root.followPlayhead
                    sampleId: root.sampleId
                    sampleText: root.sampleText
                    selection: root.selection
                    metaSelection: root.metaSelection
                    snapNum: root.snapNum
                    snapDen: root.snapDen
                    zoomToCursor: root.zoomToCursor
                    perfLog: root.perfLog
                    onHitPlaceRequested: (hit) => root.hitPlaceRequested(hit)
                    onSelectionFinished: (refs) => root.selectionFinished(refs)
                    onNoteClicked: (ref, ctrl) => root.noteClicked(ref, ctrl)
                    onPlayNoteSample: (ref) => root.playNoteSample(ref)
                    onCanvasClicked: () => root.canvasClicked()
                    onNoteRightDeleted: (ref) => root.noteRightDeleted(ref)
                    onNoteEditRequested: (ref) => root.noteEditRequested(ref)
                    onMoveSelectionRequested: (deltaF, targetLane, sourceLane, sourceBgmLine) => root.moveSelectionRequested(deltaF, targetLane, sourceLane, sourceBgmLine)
                    onMetaObjectClicked: (obj, ctrl) => root.metaObjectClicked(obj, ctrl)
                    onMetaMoveRequested: (kind, obj, deltaF, targetLane) =>
                        root.metaMoveRequested(kind, obj, deltaF, targetLane)
                    onMetaRightDeleted: (obj) => root.metaRightDeleted(obj)
                    onMetaEditRequested: (obj) => root.metaEditRequested(obj)
                    onEditAreaPressed: root.editAreaPressed()
                }
            }
        }

        // ---------- 右 Dock（属性检查器 / 时间轴 标签页） ----------
        Rectangle {
            SplitView.preferredWidth: 230
            SplitView.minimumWidth: 160
            color: Theme.surface
            border.color: Theme.border

            ColumnLayout {
                anchors.fill: parent
                anchors.margins: 8
                spacing: 6
                // 标签页（可横向滚动；窄 dock 不截断）
                BbTabStrip {
                    id: rightTabs
                    objectName: "rightTabs"  // 调试 --rtab N 用（main.cpp 已接）
                    Layout.fillWidth: true
                    Layout.preferredHeight: 30
                    model: [qsTr("属性"), qsTr("时间轴")]
                    onIndexRequested: (index) => rightTabs.currentIndex = index
                }
                StackLayout {
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    currentIndex: rightTabs.currentIndex
                    PropertiesPanel {
                        id: propPanel
                        selection: root.selection
                        metaSelection: root.metaSelection
                        stopUnit: root.stopUnit
                        stopBpm: root.stopBpm
                        onNoteEditRequested: (ref) => root.noteEditRequested(ref)
                        onMetaEditRequested: (obj) => root.metaEditRequested(obj)
                    }
                    TimelinePanel {
                        id: timelinePanel
                        bpmEvents: root.timingBpm
                        stopEvents: root.timingStop
                        measureEvents: root.timingMeasure
                        bpmDefs: root.timingBpmDefs
                        stopDefs: root.timingStopDefs
                        stopUnit: root.stopUnit
                        stopBpm: root.stopBpm
                        onTimingEditRequested: (kind, measure, num, den, value, ref) =>
                            root.timingEditRequested(kind, measure, num, den, value, ref)
                        onTimingDeleteRequested: (kind, measure, num, den) =>
                            root.timingDeleteRequested(kind, measure, num, den)
                        onTimingDefAddRequested: (kind, id, value) =>
                            root.timingDefAddRequested(kind, id, value)
                        onTimingDefDeleteRequested: (kind, id) =>
                            root.timingDefDeleteRequested(kind, id)
                    }
                }
            }
        }
    }
}
