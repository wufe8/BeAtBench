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
                TabBar {
                    id: leftTabs
                    Layout.fillWidth: true
                    // 标签样式 = 默认皮肤组件库 BbTabButton（.dock-tab 样式）
                    BbTabButton { text: qsTr("元信息") }
                    BbTabButton { text: qsTr("采样") }
                    BbTabButton { text: qsTr("lint") }
                    BbTabButton { text: qsTr("BGA") }
                }
                StackLayout {
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    currentIndex: leftTabs.currentIndex
                    MetaPanel { meta: root.chartMeta; chartPath: root.chartPath }
                    Label { text: qsTr("采样管理（M2 第 4 步）"); color: Theme.textFaint;
                            font.pixelSize: Theme.fsSmall }
                    Label { text: qsTr("lint 报告（M2 第 4 步）"); color: Theme.textFaint;
                            font.pixelSize: Theme.fsSmall }
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

                // 视口主体（占位）
                Item {
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    Label {
                        anchors.centerIn: parent
                        text: root.chartMeta
                              ? qsTr("竖向时间轴（上=高小节）")
                              : qsTr("打开谱面开始编辑（Ctrl+O）")
                        horizontalAlignment: Text.AlignHCenter
                        color: Theme.textFaint
                        font.pixelSize: Theme.fsBase
                    }
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
