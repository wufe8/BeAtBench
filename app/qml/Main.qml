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
                BbToolButton { text: "V 选择"; checkable: true; checked: true; flatStyle: true }
                BbToolButton { text: "N 放置"; checkable: true; flatStyle: true }
                BbToolButton { text: "L LN"; checkable: true; flatStyle: true }
                BbToolButton { text: "M 地雷"; checkable: true; flatStyle: true }
                BbToolButton { text: "H 平移"; checkable: true; flatStyle: true }
                Item { Layout.fillWidth: true }
                Label { text: chartPath ? chartPath : qsTr("未打开谱面"); color: Theme.textFaint
                        elide: Text.ElideMiddle; font.pixelSize: Theme.fsSmall }
            }
        }

        // 页面内容区（切页 = 切视图，doc/05 §2）
        StackLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            currentIndex: window.currentPage
            EditPage { chartMeta: window.chartMeta; chartPath: window.chartPath }
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
                Label { text: window.statusText; color: Theme.textMuted
                        elide: Text.ElideMiddle; Layout.fillWidth: true
                        font.family: Theme.fontMono; font.pixelSize: Theme.fsSmall }
                Label {
                    text: chartMeta ? "SP7K · " + (chartMeta.PLAYER !== undefined ? chartMeta.PLAYER : "") : ""
                    color: Theme.textFaint; font.family: Theme.fontMono; font.pixelSize: Theme.fsSmall
                }
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
            Label { text: qsTr("协议：命令即接口（doc/06 §3）"); color: Theme.textMuted }
        }
    }
}
