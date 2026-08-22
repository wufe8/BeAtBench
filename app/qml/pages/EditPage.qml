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
    /// 拍子线 [num]/[den]（默认 [1]/[4] = 每 4 分音符）
    property int beatNum: 1
    property int beatDen: 4
    /// note 采样标签：0 隐藏 / 1 id / 2 文件名
    property int noteSampleMode: 0
    /// 更多轨道（BGA 图层通道列，游玩轨与背景轨之间）
    property bool showExtras: false
    /// 状态栏：鼠标位置 + note 信息（ChartViewItem.hoverText）
    readonly property string hoverText: chartView ? chartView.hoverText : ""

    /// 采样被选中（面板点击/键盘确认）→ Main 记录为当前采样（M3 放置落点）
    signal samplePicked(string id, string file)

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
                            text: qsTr("编辑工作区 · <b>SP7K</b> · 1/16 snap")
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
                    beatNum: root.beatNum
                    beatDen: root.beatDen
                    noteSampleMode: root.noteSampleMode
                    showExtras: root.showExtras
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
