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
    /// 吸附粒度（放置用：snapNum/snapDen 槽/小节；Main snap 分子分母）
    property int snapNum: 1
    property int snapDen: 16
    /// 缩放锚点模式（2026-09 用户）：true=鼠标位置缩放（默认开）
    property bool zoomToCursor: true
    /// paint 帧耗时采样（--perf-log）
    property bool perfLog: false
    /// 状态栏：鼠标位置 + note 信息（ChartViewItem.hoverText）
    readonly property string hoverText: chartView ? chartView.hoverText : ""

    /// 采样被选中（面板点击/键盘确认）→ Main 记录为当前采样（M3 放置落点）
    signal samplePicked(string id, string file)
    /// note 工具点击（hitTest 结果）→ Main 走 note.put
    signal hitPlaceRequested(var hit)
    /// 框选完成 → Main 存 selection + 复制到剪贴板
    signal selectionFinished(var refs)
    /// ln/mine 工具点击（命令未接）
    signal toolNotReady(string tool)
    /// select 点击命中 note（选中；ctrl = 多选切换）
    signal noteClicked(var ref, bool ctrl)
    /// select 点击空白（清空选中）
    signal canvasClicked()
    /// 右键命中 note（删除）
    signal noteRightDeleted(var ref)
    /// 平移：deltaF = 时间轴位移（拍位小数）；targetLane = 横向目标列（laneAtX；null=纯时间）
    signal moveSelectionRequested(real deltaF, var targetLane)

    /// 视口中心小节（粘贴 target_measure 用；转发 ChartView）。
    function centerMeasure() {
        return chartView ? chartView.centerMeasure() : 0
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
                    MetaPanel { meta: root.chartMeta; chartPath: root.chartPath }
                    SamplePanel { id: samplePanel; onSamplePicked: (id, file) => root.samplePicked(id, file) }
                    LintPanel {
                        onIssuePicked: (id) => {
                            // lint → 采样 双向往返：切到采样标签并定位该行
                            leftTabs.currentIndex = 1
                            samplePanel.requireId(id)
                        }
                    }
                    Label { text: qsTr("BGA 预览（后置）"); color: Theme.textFaint;
                            font.pixelSize: Theme.fsSmall }
                }
            }
        }

        // ---------- 中央视口（时间轴占位，M2 第 5 步用 QQuickPaintedItem 实现） ----------
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
                            text: qsTr("编辑工作区 · <b>SP7K</b> · %1/%2 snap")
                                    .arg(root.snapNum).arg(root.snapDen)
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
                    editorTool: root.editorTool
                    moveMode: root.moveMode
                    sampleId: root.sampleId
                    sampleText: root.sampleText
                    selection: root.selection
                    snapNum: root.snapNum
                    snapDen: root.snapDen
                    zoomToCursor: root.zoomToCursor
                    perfLog: root.perfLog
                    onHitPlaceRequested: (hit) => root.hitPlaceRequested(hit)
                    onSelectionFinished: (refs) => root.selectionFinished(refs)
                    onToolNotReady: (tool) => root.toolNotReady(tool)
                    onNoteClicked: (ref, ctrl) => root.noteClicked(ref, ctrl)
                    onCanvasClicked: () => root.canvasClicked()
                    onNoteRightDeleted: (ref) => root.noteRightDeleted(ref)
                    onMoveSelectionRequested: (deltaF, targetLane) => root.moveSelectionRequested(deltaF, targetLane)
                }
            }
        }

        // ---------- 右 Dock（属性面板占位） ----------
        Rectangle {
            SplitView.preferredWidth: 230
            SplitView.minimumWidth: 160
            color: Theme.surface
            border.color: Theme.border
            ColumnLayout {
                anchors.fill: parent
                anchors.margins: 8
                spacing: 6
                Label { text: qsTr("属性"); font.bold: true; color: Theme.text;
                        font.pixelSize: Theme.fsBase }
                Label {
                    text: root.chartMeta ? (root.chartMeta.TITLE !== undefined ? root.chartMeta.TITLE : "") : qsTr("未选中")
                    color: Theme.textMuted
                    elide: Text.ElideRight
                    Layout.fillWidth: true
                    font.pixelSize: Theme.fsSmall
                }
                Label { text: qsTr("lane / 时间 / 采样（M3）"); color: Theme.textFaint;
                        font.pixelSize: Theme.fsSmall }
            }
        }
    }
}
