// SPDX-License-Identifier: GPL-3.0-only
// BeatBench 主窗口（M2 页面式工作区外壳，doc/05 v0.2 + beatbench-ui-layouts.html 推荐图）。
// 结构：固定 chrome（菜单栏 + 底部页面条 + 状态栏）包裹 页面工具条 + 三栏页面内容。
// 第一条真链路：文件 → 打开谱面 → dispatch(info) → 元信息面板（左 Dock）。
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

    // ---------- 会话状态（不入 undo，doc/05 §1.2） ----------
    property int currentPage: 0          // 0 编辑 / 1 切音 / 2 测试
    property var chartMeta: null         // dispatch(info) 的 result.meta
    property string chartPath: ""        // 当前谱面路径（info 返回的规范化路径）
    property string statusText: qsTr("就绪")

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

        // 页面工具条（随页面变化）
        ToolBar {
            Layout.fillWidth: true
            RowLayout {
                anchors.fill: parent
                spacing: 6
                Label { text: qsTr("页面工具条"); color: "#6b7484"; padding: 4 }
                ToolButton { text: "snap 1/16"; enabled: chartMeta !== null }
                ToolButton { text: qsTr("量化"); enabled: chartMeta !== null }
                ToolButton { text: qsTr("网格"); enabled: chartMeta !== null }
                ToolButton { text: qsTr("缩放"); enabled: chartMeta !== null }
                Item { Layout.fillWidth: true }
                ToolButton { text: qsTr("▶ 试听（Phase B）"); enabled: false }
                Label { text: "SP7K"; color: "#8b95a7"; padding: 4 }
            }
        }

        // 编辑工具条（编辑页专属）
        ToolBar {
            visible: currentPage === 0
            Layout.fillWidth: true
            RowLayout {
                anchors.fill: parent
                spacing: 6
                Label { text: qsTr("编辑工具"); color: "#6b7484"; padding: 4 }
                ToolButton { text: "V 选择"; checkable: true; checked: true }
                ToolButton { text: "N 放置"; checkable: true }
                ToolButton { text: "L LN"; checkable: true }
                ToolButton { text: "M 地雷"; checkable: true }
                ToolButton { text: "H 平移"; checkable: true }
                Item { Layout.fillWidth: true }
                Label { text: chartPath ? chartPath : qsTr("未打开谱面"); color: "#8b95a7"; elide: Text.ElideMiddle }
            }
        }

        // 页面内容区
        Item {
            Layout.fillWidth: true
            Layout.fillHeight: true

            // 三栏：左 Dock + 中央视口（占位）+ 右 Dock
            RowLayout {
                anchors.fill: parent
                spacing: 0

                // 左 Dock（面板容器）
                Rectangle {
                    Layout.preferredWidth: 230
                    Layout.fillHeight: true
                    color: "#12161c"
                    border.color: "#20252e"

                    ColumnLayout {
                        anchors.fill: parent
                        anchors.margins: 8
                        spacing: 8
                        TabBar {
                            id: leftTabs
                            Layout.fillWidth: true
                            TabButton { text: qsTr("元信息") }
                            TabButton { text: qsTr("采样") }
                            TabButton { text: qsTr("lint") }
                            TabButton { text: qsTr("BGA") }
                        }
                        StackLayout {
                            Layout.fillWidth: true
                            Layout.fillHeight: true
                            currentIndex: leftTabs.currentIndex
                            MetaPanel { meta: window.chartMeta; chartPath: window.chartPath }
                            Label { text: qsTr("采样管理（M2 第 4 步）"); color: "#6b7484" }
                            Label { text: qsTr("lint 报告（M2 第 4 步）"); color: "#6b7484" }
                            Label { text: qsTr("BGA 预览（后置）"); color: "#6b7484" }
                        }
                    }
                }

                // 中央视口（时间轴占位，M2 第 5 步用 QQuickPaintedItem 实现）
                Rectangle {
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    color: "#0b0e13"
                    border.color: "#20252e"
                    Label {
                        anchors.centerIn: parent
                        text: chartMeta
                              ? qsTr("竖向时间轴（上=高小节）\nBPM %1 · 已加载").arg(chartMeta.BPM !== undefined ? chartMeta.BPM : "—")
                              : qsTr("打开谱面开始编辑（Ctrl+O）")
                        horizontalAlignment: Text.AlignHCenter
                        color: "#5b6472"
                    }
                }

                // 右 Dock（属性面板占位）
                Rectangle {
                    Layout.preferredWidth: 220
                    Layout.fillHeight: true
                    color: "#12161c"
                    border.color: "#20252e"
                    ColumnLayout {
                        anchors.fill: parent
                        anchors.margins: 8
                        spacing: 6
                        Label { text: qsTr("属性"); font.bold: true; color: "#c8cdd6" }
                        Label { text: chartMeta ? (chartMeta.TITLE !== undefined ? chartMeta.TITLE : "") : qsTr("未选中"); color: "#8b95a7" }
                        Label { text: qsTr("lane / 时间 / 采样（M3）"); color: "#6b7484" }
                    }
                }
            }
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
            Layout.preferredHeight: 26
            color: "#12161c"
            border.color: "#20252e"
            RowLayout {
                anchors.fill: parent
                anchors.leftMargin: 10
                anchors.rightMargin: 10
                Label { text: window.statusText; color: "#8b95a7"; elide: Text.ElideMiddle; Layout.fillWidth: true }
                Label { text: chartMeta ? "SP7K · " + (chartMeta.PLAYER !== undefined ? chartMeta.PLAYER : "") : ""; color: "#6b7484" }
            }
        }
    }

    // ---------- 第一条真链路：打开谱面 → dispatch(info) → 元信息 ----------
    FileDialog {
        id: fileDialog
        title: qsTr("打开 BMS 谱面")
        nameFilters: [qsTr("BMS 谱面 (*.bms)"), qsTr("所有文件 (*)")]
        onAccepted: {
            var path = urlToPath(selectedFile)
            var req = JSON.stringify({ command: "info", args: { path: path } })
            var resp = beatbench.dispatch(req)
            var r = JSON.parse(resp)
            if (r.ok) {
                window.chartMeta = r.result.meta
                window.chartPath = r.result.path
                window.statusText = qsTr("已打开：%1（%2 个采样）").arg(r.result.path, String(Object.keys(r.result.samples ?? {}).length))
            } else {
                window.chartMeta = null
                window.statusText = qsTr("打开失败：%1 %2").arg(r.error.code, r.error.message)
            }
        }
        onRejected: { /* 用户取消 */ }
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
            Label { text: qsTr("协议：命令即接口（doc/06 §3）"); color: "#8b95a7" }
        }
    }
}
