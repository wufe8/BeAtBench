// SPDX-License-Identifier: GPL-3.0-only
// 项目页（M7 阶段①「只读索引版」，doc/07 §9.1）：谱面表 + 资源表 + 筛选，
// 选中 → 在编辑页打开 / 设为切音目标。数据来自 `project`（ProjectController，
// 两段式：先枚举出表，再后台补元信息/统计/引用）。
//
// 本页不直接写盘/持有文档；项目索引只读，历史操作委托独立控制器。
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

import "../components"

Item {
    id: root

    signal primaryAssignRequested(string absolutePath)
    signal referenceOpenRequested(string absolutePath)

    objectName: "projectPage"
    property int detailTab: 0
    function refreshHistoryTarget() {
        if (detailTab !== 1 || !visible || chartHistoryController.busy) return
        if (selectedChart !== "") chartHistoryController.browseHistory(project.absolutePath(selectedChart))
        else chartHistoryController.showHistory(documents.activeId(), false)
    }
    onDetailTabChanged: refreshHistoryTarget()
    onVisibleChanged: refreshHistoryTarget()
    property string selectedChart: ""
    property string saveText: ""
    property var selectedResource: null
    property var selectedChartInfo: ({})
    property bool onlyOpen: false
    property string chartSearch: ""

    // 资源筛选状态
    property string kindFilter: ""
    property bool onlyMissing: false
    property bool onlyUnreferenced: false
    property string searchText: ""

    // C++ models expose visible rows only. Do not copy/filter the complete snapshot
    // into JS arrays on every scan signal (including every nested resource use).
    function refreshResources() {
        project.setResourceFilter(root.kindFilter, root.onlyMissing,
                                  root.onlyUnreferenced, root.searchText)
        if (root.selectedResource !== null) {
            var detail = project.selectResource(root.selectedResource.path)
            root.selectedResource = detail.path === undefined ? null : detail
        }
    }

    function refreshCharts() {
        project.setChartFilter(root.onlyOpen, root.chartSearch)
        root.selectedChartInfo = project.chartDetails(root.selectedChart)
    }
    onSelectedChartChanged: {
        root.selectedChartInfo = project.chartDetails(root.selectedChart)
        root.refreshHistoryTarget()
    }

    /// “未打开”是会话状态；清洁文档按是否实际保存区分“未修改 / 已保存”。
    function docBadge(m) {
        var st = project.openStates[m.absPath]
        if (!st) return qsTr("未打开")
        if (st.active && window.metaDraftDirty) return qsTr("草稿未存")
        if (st.dirty) return qsTr("未保存")
        return st.saved ? qsTr("已保存") : qsTr("未修改")
    }
    function docBadgeColor(m) {
        var st = project.openStates[m.absPath]
        if (!st) return Theme.textFaint
        return st.dirty || (st.active && window.metaDraftDirty) ? Theme.primary : Theme.textMuted
    }
    function openCount() {
        var n = 0
        for (var k in project.openStates) if (project.openStates.hasOwnProperty(k)) ++n
        return n
    }
    function dirtyCount() {
        var n = 0
        for (var k in project.openStates)
            if (project.openStates.hasOwnProperty(k) && project.openStates[k].dirty) ++n
        return n
    }

    function stateText() {
        switch (project.state) {
        case "indexing": return qsTr("枚举中…")
        case "parsing": return qsTr("解析中…（谱面表已可用）")
        case "ready": return qsTr("就绪")
        case "cancelled": return qsTr("已取消（结果可能不完整）")
        case "error": return qsTr("错误：") + project.errorText
        default: return qsTr("未打开文件夹")
        }
    }

    function chartStatus(m) {
        var parts = []
        if (m.missing > 0) parts.push(qsTr("缺失 ") + m.missing)
        if (m.random) parts.push(qsTr("RANDOM"))
        if (m.errors > 0) parts.push(qsTr("错误 ") + m.errors)
        if (m.readError !== "") parts.push(qsTr("读取失败"))
        return parts.join(" · ")
    }

    Component.onCompleted: { refreshResources(); refreshCharts() }
    Connections {
        target: project
        function onResourcesChanged() { root.refreshResources() }
        function onChartsChanged() { root.refreshCharts() }
        function onStateChanged() {
            if (project.state === "indexing" || project.state === "idle") {
                root.selectedChart = ""
                root.selectedResource = null
                root.saveText = ""
            }
        }
    }

    ResourceBindingDialog {
        id: bindingDialog
        parent: Overlay.overlay
        controller: resourceBindingController
        projectRoot: project.root
        targetPaths: {
            const selected = project.sliceTargets // Track the existing multi-selection.
            return selected.length > 0 ? project.effectiveSliceTargets("") : []
        }
        onApplied: (message) => {
            root.saveText = message
            window.setStatus(message)
        }
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        // ---------- 顶部状态条 ----------
        Rectangle {
            Layout.fillWidth: true
            implicitHeight: 34
            color: Theme.surface2
            border.color: Theme.border

            RowLayout {
                anchors.fill: parent
                anchors.leftMargin: 10
                anchors.rightMargin: 10
                spacing: 10

                Label {
                    text: root.stateText()
                    color: project.state === "error" ? Theme.danger : Theme.text
                    font.pixelSize: Theme.fsSmall
                }
                Label {
                    text: qsTr("谱面 %1 · 资源 %2 · 缺失 %3 · 无引用 %4")
                          .arg(project.chartCount).arg(project.resourceCount)
                          .arg(project.missingCount).arg(project.unreferencedCount)
                    color: Theme.textMuted
                    font.pixelSize: Theme.fsSmall
                }
                Label {
                    text: qsTr("已打开 %1（未保存 %2）").arg(root.openCount()).arg(root.dirtyCount())
                    color: root.dirtyCount() > 0 ? Theme.primary : Theme.textMuted
                    font.pixelSize: Theme.fsSmall
                }
                Label {
                    text: qsTr("切音目标 %1").arg(project.sliceTargetCount)
                    color: project.sliceTargetCount > 0 ? Theme.primary : Theme.textMuted
                    font.pixelSize: Theme.fsSmall
                }
                Label {
                    Layout.fillWidth: true
                    text: project.root
                    elide: Text.ElideMiddle
                    color: Theme.textFaint
                    font.pixelSize: Theme.fsSmall
                }
                BbToolButton {
                    text: qsTr("选文件夹")
                    onClicked: window.uiActionOpenFolder()
                }
                BbToolButton {
                    text: qsTr("取消")
                    enabled: project.state === "indexing" || project.state === "parsing"
                    onClicked: project.cancel()
                }
            }
            Rectangle { anchors.bottom: parent.bottom; width: parent.width; height: 1; color: Theme.border }
        }

        SplitView {
            Layout.fillWidth: true
            Layout.fillHeight: true
            orientation: Qt.Horizontal

            // ================= 谱面表 =================
            Rectangle {
                SplitView.preferredWidth: 620
                SplitView.minimumWidth: 360
                color: Theme.bg

                ColumnLayout {
                    anchors.fill: parent
                    spacing: 0

                    RowLayout {
                        Layout.fillWidth: true
                        Layout.leftMargin: 10
                        Layout.rightMargin: 10
                        Layout.topMargin: 6
                        Layout.bottomMargin: 4
                        spacing: 8
                        BbTextField {
                            Layout.fillWidth: true
                            placeholderText: qsTr("按谱面路径筛选…")
                            onTextChanged: { root.chartSearch = text; root.refreshCharts() }
                        }
                        BbCheckBox {
                            text: qsTr("仅已打开")
                            onToggled: { root.onlyOpen = checked; root.refreshCharts() }
                        }
                        Label {
                            text: qsTr("%1 项").arg(chartList.count)
                            color: Theme.textMuted
                            font.pixelSize: Theme.fsSmall
                        }
                    }
                    RowLayout {
                        Layout.fillWidth: true
                        Layout.leftMargin: 10
                        Layout.rightMargin: 10
                        Layout.topMargin: 6
                        Layout.bottomMargin: 4
                        spacing: 8
                        BbCheckBox {
                            id: allTargetsBox
                            text: ""
                            // 全选态 = 有谱面且全部勾选；用户点击后 setAll 驱动模型，再恢复绑定
                            checked: project.chartCount > 0 && project.sliceTargetCount === project.chartCount
                            onToggled: {
                                project.setAllSliceTargets(checked)
                                allTargetsBox.checked = Qt.binding(function () {
                                    return project.chartCount > 0
                                           && project.sliceTargetCount === project.chartCount
                                })
                            }
                        }
                        Label { text: qsTr("谱面"); color: Theme.textMuted; font.pixelSize: Theme.fsSmall; Layout.fillWidth: true }
                        Label { text: qsTr("难度"); color: Theme.textMuted; font.pixelSize: Theme.fsSmall; Layout.preferredWidth: 54 }
                        Label { text: qsTr("等级"); color: Theme.textMuted; font.pixelSize: Theme.fsSmall; Layout.preferredWidth: 40 }
                        Label { text: qsTr("notes"); color: Theme.textMuted; font.pixelSize: Theme.fsSmall; Layout.preferredWidth: 52 }
                        Label { text: qsTr("保存状态"); color: Theme.textMuted; font.pixelSize: Theme.fsSmall; Layout.preferredWidth: 76 }
                        Label { text: qsTr("诊断"); color: Theme.textMuted; font.pixelSize: Theme.fsSmall; Layout.preferredWidth: 96 }
                    }
                    Rectangle { Layout.fillWidth: true; height: 1; color: Theme.border }

                    ListView {
                        id: chartList
                        enabled: root.detailTab !== 1 || !chartHistoryController.busy
                        objectName: "projectChartList"
                        Layout.fillWidth: true
                        Layout.fillHeight: true
                        clip: true
                        model: project.chartModel
                        boundsBehavior: Flickable.StopAtBounds
                        ScrollBar.vertical: ScrollBar {}

                        delegate: Rectangle {
                            id: chartRow
                            objectName: "projectChartRow:" + modelData.path
                            required property var modelData
                            width: chartList.width
                            height: 24
                            color: root.selectedChart === modelData.path
                                   ? Theme.primarySoft
                                   : (chartHover.hovered ? Theme.surface2 : "transparent")

                            HoverHandler { id: chartHover }
                            MouseArea {
                                anchors.fill: parent
                                onClicked: root.selectedChart = chartRow.modelData.path
                                onDoubleClicked: project.openChart(chartRow.modelData.path)
                            }

                            RowLayout {
                                anchors.fill: parent
                                anchors.leftMargin: 10
                                anchors.rightMargin: 10
                                spacing: 8
                                BbCheckBox {
                                    text: ""
                                    checked: project.sliceTargets.indexOf(chartRow.modelData.path) >= 0
                                    onToggled: project.setSliceTargetChecked(chartRow.modelData.path, checked)
                                }
                                Label {
                                    Layout.fillWidth: true
                                    text: chartRow.modelData.path
                                    elide: Text.ElideMiddle
                                    HoverHandler { id: chartPathHover }
                                    ToolTip.visible: chartPathHover.hovered
                                    ToolTip.text: chartRow.modelData.path
                                    ToolTip.delay: 500
                                    color: Theme.text
                                    font.pixelSize: Theme.fsSmall
                                }
                                Label { Layout.preferredWidth: 54; text: chartRow.modelData.difficulty; color: Theme.textMuted; font.pixelSize: Theme.fsSmall; elide: Text.ElideRight }
                                Label { Layout.preferredWidth: 40; text: chartRow.modelData.playlevel; color: Theme.textMuted; font.pixelSize: Theme.fsSmall }
                                Label { Layout.preferredWidth: 52; text: chartRow.modelData.parsed ? chartRow.modelData.notesText : "…"; color: Theme.textMuted; font.pixelSize: Theme.fsSmall }
                                Label {
                                    Layout.preferredWidth: 76
                                    text: root.docBadge(chartRow.modelData)
                                    color: root.docBadgeColor(chartRow.modelData)
                                    font.pixelSize: Theme.fsSmall
                                    elide: Text.ElideRight
                                }
                                Label {
                                    Layout.preferredWidth: 96
                                    text: root.chartStatus(chartRow.modelData)
                                    color: chartRow.modelData.missing > 0 || chartRow.modelData.errors > 0
                                           ? Theme.warning : Theme.textFaint
                                    font.pixelSize: Theme.fsSmall
                                    elide: Text.ElideRight
                                }
                            }
                        }
                    }

                    Rectangle { Layout.fillWidth: true; height: 1; color: Theme.border }

                    // 选中谱面：详情 + 动作
                    ColumnLayout {
                        Layout.fillWidth: true
                        Layout.leftMargin: 10
                        Layout.rightMargin: 10
                        Layout.topMargin: 6
                        Layout.bottomMargin: 6
                        spacing: 4

                        Label {
                            Layout.fillWidth: true
                            visible: root.selectedChart !== ""
                            text: {
                                var c = root.selectedChartInfo
                                if (c.path === undefined) return ""
                                return (c.title || qsTr("(无标题)")) + " — " + (c.artist || "?")
                                       + "  ·  " + (c.genre || "?") + "  ·  " + (c.mode || "?")
                            }
                            color: Theme.text
                            font.pixelSize: Theme.fsSmall
                            elide: Text.ElideRight
                        }
                        GridLayout {
                            Layout.fillWidth: true
                            columns: width < 520 ? 2 : 3
                            columnSpacing: 6
                            rowSpacing: 4
                            BbToolButton {
                                text: qsTr("在 A 中编辑")
                                enabled: root.selectedChart !== ""
                                onClicked: root.primaryAssignRequested(project.absolutePath(root.selectedChart))
                            }
                            BbToolButton {
                                text: qsTr("在 B 中对照")
                                enabled: root.selectedChart !== ""
                                onClicked: root.referenceOpenRequested(project.absolutePath(root.selectedChart))
                            }
                            BbToolButton {
                                objectName: "projectUpdateBindingsButton"
                                text: qsTr("更新资源绑定…")
                                enabled: project.sliceTargetCount > 0 && project.state === "ready"
                                onClicked: {
                                    if (window.metaDraftDirty) {
                                        root.saveText = qsTr("请先提交或取消面板草稿，再更新资源绑定")
                                        return
                                    }
                                    bindingDialog.open()
                                }
                            }
                            BbToolButton {
                                text: qsTr("保存全部已打开")
                                onClicked: window.saveAllDocuments()
                            }
                            BbToolButton {
                                text: qsTr("保存勾选谱面")
                                enabled: project.sliceTargetCount > 0 || window.chartPath !== ""
                                onClicked: {
                                    if (window.metaDraftDirty) {
                                        root.saveText = qsTr("请先提交面板草稿，或使用保存全部已打开")
                                        return
                                    }
                                    window.saveSelectedDocuments(project.effectiveSliceTargets(window.chartPath))
                                }
                            }
                            Label {
                                Layout.columnSpan: parent.columns
                                Layout.fillWidth: true
                                wrapMode: Text.Wrap
                                visible: root.saveText !== ""
                                text: root.saveText
                                color: Theme.textMuted
                                font.pixelSize: Theme.fsSmall
                            }
                            Label {
                                Layout.fillWidth: true
                                Layout.columnSpan: parent.columns
                                text: project.sliceTargetCount > 0
                                      ? qsTr("已勾选 %1 谱：资源绑定更新 / 切音导出与铺放目标").arg(project.sliceTargetCount)
                                      : qsTr("未勾选任何谱面 → 切音目标 = 当前编辑页打开的谱面")
                                color: Theme.textFaint
                                font.pixelSize: Theme.fsSmall
                                elide: Text.ElideRight
                            }
                        }
                    }
                }
            }

            // ================= 资源 / 谱面历史（共享右侧空间） =================
            Rectangle {
                SplitView.fillWidth: true
                SplitView.minimumWidth: 320
                color: Theme.bg

                BbTabStrip {
                    id: detailTabs
                    objectName: "projectDetailTabs"
                    anchors.left: parent.left
                    anchors.right: parent.right
                    anchors.top: parent.top
                    model: [qsTr("资源"), qsTr("谱面历史")]
                    currentIndex: root.detailTab
                    onIndexRequested: (index) => root.detailTab = index
                }
                ChartHistoryPanel {
                    objectName: "projectHistoryPanel"
                    anchors.fill: parent
                    anchors.topMargin: detailTabs.height + 8
                    anchors.leftMargin: 10
                    anchors.rightMargin: 10
                    anchors.bottomMargin: 8
                    visible: root.detailTab === 1
                    controller: chartHistoryController
                    saveAllowed: controller.documentId !== "" &&
                                 controller.documentId === chartSession.sessionId && controller.sourcePath !== ""
                }
                ColumnLayout {
                    anchors.fill: parent
                    anchors.topMargin: detailTabs.height
                    visible: root.detailTab === 0
                    spacing: 0

                    RowLayout {
                        Layout.fillWidth: true
                        Layout.leftMargin: 10
                        Layout.rightMargin: 10
                        Layout.topMargin: 6
                        Layout.bottomMargin: 4
                        spacing: 8
                        BbComboBox {
                            id: kindBox
                            Layout.preferredWidth: 110
                            model: ["", "wav", "ogg", "mp3", "bmp", "png", "jpg", "mpg"]
                            displayText: currentIndex === 0 ? qsTr("全部类型") : currentText
                            onActivated: { root.kindFilter = currentText; root.refreshResources() }
                        }
                        BbCheckBox {
                            text: qsTr("仅缺失")
                            onToggled: { root.onlyMissing = checked; root.refreshResources() }
                        }
                        BbCheckBox {
                            text: qsTr("仅无引用")
                            onToggled: { root.onlyUnreferenced = checked; root.refreshResources() }
                        }
                        BbTextField {
                            Layout.fillWidth: true
                            placeholderText: qsTr("按路径筛选…")
                            onTextChanged: { root.searchText = text; root.refreshResources() }
                        }
                        Label {
                            text: qsTr("%1 项").arg(resList.count)
                            color: Theme.textMuted
                            font.pixelSize: Theme.fsSmall
                        }
                    }
                    Rectangle { Layout.fillWidth: true; height: 1; color: Theme.border }

                    Label {
                        Layout.fillWidth: true
                        Layout.leftMargin: 10
                        Layout.rightMargin: 10
                        text: qsTr("资源表为磁盘扫描结果；未保存的绑定请在编辑页检查，保存后刷新文件夹更新此表。")
                        wrapMode: Text.Wrap
                        color: Theme.textFaint
                        font.pixelSize: Theme.fsSmall
                    }
                    // 资源表列头（与行同间距/同列宽）
                    RowLayout {
                        Layout.fillWidth: true
                        Layout.leftMargin: 10
                        Layout.rightMargin: 10
                        Layout.topMargin: 4
                        Layout.bottomMargin: 2
                        spacing: 8
                        Label { Layout.fillWidth: true; text: qsTr("文件"); color: Theme.textMuted; font.pixelSize: Theme.fsSmall }
                        Label { Layout.preferredWidth: 44; text: qsTr("类型"); color: Theme.textMuted; font.pixelSize: Theme.fsSmall }
                        Label { Layout.preferredWidth: 88; text: qsTr("状态"); color: Theme.textMuted; font.pixelSize: Theme.fsSmall }
                        Label { Layout.preferredWidth: 44; text: qsTr("引用"); color: Theme.textMuted; font.pixelSize: Theme.fsSmall }
                    }
                    Rectangle { Layout.fillWidth: true; height: 1; color: Theme.border }

                    ListView {
                        id: resList
                        objectName: "projectResourceList"
                        Layout.fillWidth: true
                        Layout.fillHeight: true
                        clip: true
                        model: project.resourceModel
                        boundsBehavior: Flickable.StopAtBounds
                        ScrollBar.vertical: ScrollBar {}

                        delegate: Rectangle {
                            id: resRow
                            required property var modelData
                            width: resList.width
                            height: 24
                            color: (root.selectedResource !== null && root.selectedResource.path === modelData.path)
                                   ? Theme.primarySoft
                                   : (resHover.hovered ? Theme.surface2 : "transparent")

                            HoverHandler { id: resHover }
                            MouseArea {
                                anchors.fill: parent
                                onClicked: root.selectedResource = project.selectResource(resRow.modelData.path)
                            }

                            RowLayout {
                                anchors.fill: parent
                                anchors.leftMargin: 10
                                anchors.rightMargin: 10
                                spacing: 8
                                Label {
                                    Layout.fillWidth: true
                                    text: resRow.modelData.path
                                    elide: Text.ElideMiddle
                                    color: resRow.modelData.onDisk ? Theme.text : Theme.warning
                                    font.pixelSize: Theme.fsSmall
                                }
                                Label { Layout.preferredWidth: 44; text: resRow.modelData.kind; color: Theme.textFaint; font.pixelSize: Theme.fsSmall }
                                Label {
                                    Layout.preferredWidth: 88
                                    text: resRow.modelData.state
                                    color: resRow.modelData.state === "缺失" ? Theme.warning
                                           : (resRow.modelData.state === "无引用" ? Theme.textFaint : Theme.textMuted)
                                    font.pixelSize: Theme.fsSmall
                                }
                                Label { Layout.preferredWidth: 44; text: resRow.modelData.useText; color: Theme.textMuted; font.pixelSize: Theme.fsSmall }
                            }
                        }
                    }

                    Rectangle { Layout.fillWidth: true; height: 1; color: Theme.border }

                    // 选中资源：引用出处（文件 ← 谱面 / 定义 ID / 头部字段）
                    ColumnLayout {
                        id: resDetail
                        Layout.fillWidth: true
                        Layout.maximumHeight: 150
                        Layout.leftMargin: 10
                        Layout.rightMargin: 10
                        Layout.topMargin: 6
                        Layout.bottomMargin: 6
                        spacing: 2

                        Label {
                            Layout.fillWidth: true
                            visible: root.selectedResource === null
                            text: qsTr("选中资源查看引用出处（文件 ← 谱面 / 定义 ID / 头部字段）")
                            color: Theme.textFaint
                            font.pixelSize: Theme.fsSmall
                        }
                        Label {
                            Layout.fillWidth: true
                            visible: root.selectedResource !== null
                            text: root.selectedResource === null ? "" :
                                  (root.selectedResource.declared !== ""
                                   ? qsTr("声明：%1 → 实际：%2").arg(root.selectedResource.declared)
                                       .arg(root.selectedResource.path)
                                   : root.selectedResource.path)
                            color: Theme.text
                            font.pixelSize: Theme.fsSmall
                            elide: Text.ElideMiddle
                        }
                        ListView {
                            id: resourceUsesList
                            objectName: "projectResourceUsesList"
                            Layout.fillWidth: true
                            Layout.preferredHeight: Math.min(contentHeight, 100)
                            clip: true
                            visible: root.selectedResource !== null
                            model: project.resourceUsesModel
                            boundsBehavior: Flickable.StopAtBounds
                            ScrollBar.vertical: ScrollBar {}
                            delegate: Label {
                                required property var modelData
                                width: resourceUsesList.width
                                text: "· " + modelData.chart + "  [" + modelData.source + "] "
                                      + (modelData.id !== "" ? ("#" + modelData.kind + modelData.id)
                                                             : ("#" + modelData.field))
                                      + "  refs=" + modelData.refs
                                color: Theme.textMuted
                                font.pixelSize: Theme.fsSmall
                                elide: Text.ElideRight
                            }
                        }
                    }
                }
            }
        }
    }
}
