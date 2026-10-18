// SPDX-License-Identifier: GPL-3.0-only
// Central A/B surface. The document collection owns content; this owns only B's
// identity and viewport. Activating/swapping A is confirmed by Main, never here.
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Item {
    id: root
    default property alias primaryContent: primary.data
    property var documentService: null
    property var primarySession: null
    property var primaryChartView: null
    property bool overlayMode: false
    property bool overlayVisible: true
    property real overlayOpacity: 0.45
    property real _splitReferenceWidth: 0
    property var _modeAnchor: null
    property real _modePlayhead: -1
    function setOverlayMode(value) {
        if (overlayMode === value) return
        _modeAnchor = primaryChartView ? primaryChartView.viewportItem.viewportAnchor : null
        _modePlayhead = primaryChartView ? primaryChartView.playheadSec : -1
        if (value) { _splitReferenceWidth = referenceWidth; ensureReference() }
        overlayMode = value
        referenceFocused = false
        if (!value) secondary.SplitView.preferredWidth = _splitReferenceWidth >= 120
                ? _splitReferenceWidth : Math.max(160, root.width / 2)
        presentationTimer.restart()
    }
    property Timer presentationTimer: Timer {
        interval: 0
        onTriggered: {
            if (root._modeAnchor && root.primaryChartView) {
                root.primaryChartView.viewportItem.applyViewportAnchor(root._modeAnchor)
                root.primaryChartView.viewportItem.playheadSec = root._modePlayhead
            }
            root._modeAnchor = null
            root.queueSync(root.primaryChartView)
        }
    }
    property Item overlayHeader: Item {
        id: overlayHeaderHost
        parent: root
        anchors.top: parent.top
        anchors.left: parent.left
        anchors.right: parent.right
        height: 30
        visible: root.overlayMode
    }
    property Item referenceControls: Rectangle {
        parent: root.overlayMode ? overlayHeaderHost : referenceHeaderHost
        anchors.fill: parent
        color: Theme.surface
        RowLayout {
            anchors.fill: parent
            anchors.leftMargin: 4
            anchors.rightMargin: 4
            spacing: 4
            BbToolButton {
                visible: root.overlayMode
                objectName: "returnToSplitButton"
                text: qsTr("A/B 分栏")
                onClicked: root.setOverlayMode(false)
            }
            Label {
                text: root.overlayMode ? qsTr("A 编辑 / B 参考") : qsTr("B · 只读")
                color: root.overlayMode ? Theme.accent2 : Theme.textMuted
                font.pixelSize: Theme.fsSmall
            }
            BbComboBox {
                id: selector
                onActiveFocusChanged: if (activeFocus) root.referenceFocused = true
                Layout.fillWidth: true
                Layout.minimumWidth: 0
                model: root.choices
                textRole: "text"
                // BbComboBox writes currentIndex on activation; synchronize
                // explicitly by stable ID after collection/role changes.
                currentIndex: 0
                onActivated: (index) => root.selectReference(root.choices[index].id)
                ToolTip.visible: hovered
                ToolTip.text: root.referenceSession
                              ? (root.referenceSession.path || qsTr("未命名谱面"))
                                + "\n" + root.referenceDocumentId
                              : qsTr("请先从项目页或文件菜单打开谱面；新建谱面也可选择")
            }
            BbCheckBox {
                visible: !root.overlayMode
                objectName: "synchronizeViewportsCheck"
                text: qsTr("同步")
                checked: root.synchronizeViewports
                onToggled: root.synchronizeViewports = checked
                ToolTip.visible: hovered
                ToolTip.text: qsTr("按拍位同步位置和缩放；开启时以 A 为准，水平滚动保持独立")
            }
            BbCheckBox {
                visible: root.overlayMode
                objectName: "overlayVisibleCheck"
                text: qsTr("显示 B")
                checked: root.overlayVisible
                onToggled: root.overlayVisible = checked
            }
            Label {
                visible: root.overlayMode
                text: qsTr("不透明度")
                color: Theme.textMuted
                font.pixelSize: Theme.fsSmall
            }
            Slider {
                visible: root.overlayMode
                objectName: "overlayOpacitySlider"
                Layout.preferredWidth: 90
                from: 0.1; to: 0.8; stepSize: 0.05
                value: root.overlayOpacity
                onMoved: root.overlayOpacity = value
            }
            Label {
                visible: root.overlayMode
                text: Math.round(root.overlayOpacity * 100) + "%"
                color: Theme.textMuted
                font.pixelSize: Theme.fsSmall
            }
            BbToolButton {
                text: qsTr("交换")
                enabled: root.referenceSession !== null && root.referenceSession !== root.primarySession
                onActiveFocusChanged: if (activeFocus) root.referenceFocused = true
                onClicked: root.requestSwap()
                ToolTip.visible: hovered
                ToolTip.text: qsTr("交换 A/B：左侧编辑并播放，右侧只读")
            }
            BbToolButton {
                text: "×"
                onActiveFocusChanged: if (activeFocus) root.referenceFocused = true
                onClicked: root.collapse()
                ToolTip.visible: hovered
                ToolTip.text: qsTr("收起对照（不关闭文档）")
            }
        }
    }
    property bool synchronizeViewports: true
    property bool _applyingViewport: false
    property var _pendingSource: null
    property var _swapAnchor: null
    property var structureStatus: ({compatible: true, message: ""})
    readonly property bool syncActive: synchronizeViewports && !overlayMode && expanded && visible
                                      && primaryChartView !== null && referenceSession !== null
    function queueSync(source) {
        if (_applyingViewport || !syncActive) return
        _pendingSource = source
        syncTimer.restart()
    }
    function refreshStructure() {
        structureStatus = primaryChartView
                ? primaryChartView.viewportItem.compareStructure(referenceSession)
                : ({compatible: true, message: ""})
    }
    function synchronizeNow() {
        if (!syncActive) return
        const source = _pendingSource || primaryChartView
        _pendingSource = null
        const target = source === referenceChart ? primaryChartView : referenceChart
        const anchor = _swapAnchor || source.viewportItem.viewportAnchor
        _swapAnchor = null
        if (!anchor.valid) return
        _applyingViewport = true
        try {
            target.topHigh = source.topHigh
            target.measureHeight = source.measureHeight
            if (source === primaryChartView) {
                if (_restoringSwap) primaryChartView.viewportItem.applyViewportAnchor(anchor)
                target.viewportItem.applyViewportAnchor(primaryChartView.viewportItem.viewportAnchor)
            } else {
                // B requests a navigation; A remains authoritative at its boundaries.
                primaryChartView.viewportItem.applyViewportAnchor(anchor)
                referenceChart.viewportItem.applyViewportAnchor(primaryChartView.viewportItem.viewportAnchor)
            }
            projectPlayhead()
        } finally { _applyingViewport = false; _restoringSwap = false }
    }
    property bool _restoringSwap: false
    function discardPreparedSwap() { _swapAnchor = null; _restoringSwap = false }
    function prepareSwap() {
        if (!primaryChartView || (!syncActive && !overlayMode)) return
        _swapAnchor = primaryChartView.viewportItem.viewportAnchor
        _restoringSwap = true
    }
    function finishSwapViewport() {
        if (overlayMode) swapRestoreTimer.restart()
        else queueSync(primaryChartView)
    }
    property Timer swapRestoreTimer: Timer {
        interval: 0
        onTriggered: {
            if (root._swapAnchor && root.primaryChartView)
                root.primaryChartView.viewportItem.applyViewportAnchor(root._swapAnchor)
            root.discardPreparedSwap()
        }
    }
    function projectPlayhead() {
        if (!syncActive || !primaryChartView) return
        const a = primaryChartView.viewportItem
        const pos = a.positionForSeconds(primaryChartView.playheadSec)
        referenceChart.viewportItem.playheadSec = pos.valid
                ? referenceChart.viewportItem.secondsForPosition(pos.measure, pos.fraction) : -1
    }
    onSyncActiveChanged: {
        if (syncActive) queueSync(primaryChartView)
        else {
            syncTimer.stop()
            _pendingSource = null
            _swapAnchor = null
            _restoringSwap = false
            referenceChart.viewportItem.playheadSec = -1
        }
    }
    onPrimarySessionChanged: {
        _modeAnchor = null
        Qt.callLater(root.refreshStructure)
        queueSync(primaryChartView)
    }
    property Timer syncTimer: Timer { interval: 0; onTriggered: root.synchronizeNow() }
    property Connections primaryViewportConnection: Connections {
        target: root.primaryChartView
        function onViewportChanged() { root.queueSync(root.primaryChartView) }
        function onPlayheadSecChanged() { root.projectPlayhead() }
        function onHeightChanged() { if (root._modeAnchor) root.presentationTimer.restart() }
    }
    property Connections primaryContentConnection: Connections {
        target: root.primarySession
        function onContentChanged() { Qt.callLater(root.refreshStructure); root.queueSync(root.primaryChartView) }
        function onDocumentChanged() { Qt.callLater(root.refreshStructure); root.queueSync(root.primaryChartView) }
    }
    property Connections referenceContentConnection: Connections {
        target: root.referenceSession
        function onContentChanged() { Qt.callLater(root.refreshStructure); root.queueSync(root.primaryChartView) }
        function onDocumentChanged() { Qt.callLater(root.refreshStructure); root.queueSync(root.primaryChartView) }
    }
    property string referenceDocumentId: ""
    property bool referenceFocused: false
    property string editorTool: "pan"
    property bool hideSecondaryChannels: true
    property bool showExtras: false
    readonly property var instances: documentService ? documentService.instances : []
    readonly property var referenceSession: {
        for (let i = 0; i < instances.length; ++i)
            if (instances[i].id === referenceDocumentId) return instances[i].session
        return null
    }
    readonly property var referenceView: referenceChart
    readonly property bool expanded: !overlayMode && secondary.width > 0
    readonly property real referenceWidth: overlayMode ? 0 : secondary.width
    readonly property var choices: {
        let rows = [{ id: "", text: qsTr("选择已打开谱面"), path: "" }]
        for (let i = 0; i < instances.length; ++i) {
            const d = instances[i]
            rows.push({ id: d.id, text: d.label + " · " + d.id.slice(-6)
                        + (d.dirty ? " *" : ""), path: d.path })
        }
        return rows
    }
    signal swapRequested(string documentId)
    signal areaPressed()

    function syncSelector() {
        let index = 0
        for (let i = 0; i < choices.length; ++i)
            if (choices[i].id === referenceDocumentId) { index = i; break }
        selector.currentIndex = index
    }
    onChoicesChanged: Qt.callLater(root.syncSelector)
    onReferenceDocumentIdChanged: Qt.callLater(root.syncSelector)
    Component.onCompleted: syncSelector()

    function selectReference(id) {
        if (id === "") { referenceDocumentId = ""; return true }
        for (let i = 0; i < instances.length; ++i) {
            if (instances[i].id === id) { referenceDocumentId = id; return true }
        }
        return false
    }
    function openReference(id) {
        if (!id || !selectReference(id)) return false
        if (!expanded && !overlayMode) secondary.SplitView.preferredWidth = Math.max(160, root.width / 2)
        return true
    }
    function ensureReference() {
        if (referenceSession) return
        for (let i = 0; i < instances.length; ++i) {
            if (instances[i].session !== primarySession) {
                referenceDocumentId = instances[i].id
                return
            }
        }
        if (instances.length) referenceDocumentId = instances[0].id
    }
    function collapse() {
        if (overlayMode) setOverlayMode(false)
        referenceFocused = false
        secondary.SplitView.preferredWidth = 0
    }
    function settleSplit() {
        if (overlayMode) return
        if (secondary.width < 120) collapse()
        else ensureReference()
    }
    function requestSwap() {
        if (referenceSession && referenceSession !== primarySession)
            swapRequested(referenceDocumentId)
    }
    onReferenceSessionChanged: {
        referenceChart.selection = []
        referenceChart.metaSelection = []
        Qt.callLater(root.refreshStructure)
        queueSync(primaryChartView)
    }

    // Observe presses without consuming the production controls/viewport gestures.
    property Item focusTracker: MouseArea {
        parent: root
        anchors.fill: parent
        z: 100
        propagateComposedEvents: true
        acceptedButtons: Qt.AllButtons
        onPressed: (mouse) => {
            const p = secondary.mapFromItem(root, mouse.x, mouse.y)
            root.referenceFocused = root.expanded && p.x >= 0 && p.x < secondary.width
            mouse.accepted = false
        }
    }

    property Item splitLayout: SplitView {
        id: split
        parent: root
        anchors.fill: parent
        anchors.topMargin: root.overlayMode ? 30 : 0
        orientation: Qt.Horizontal
        onResizingChanged: if (!resizing) root.settleSplit()
        handle: Rectangle {
            implicitWidth: 4
            color: Theme.border
            HoverHandler { id: hover; cursorShape: Qt.SplitHCursor }
            Rectangle {
                anchors.fill: parent
                color: Theme.accent
                opacity: hover.hovered ? 0.35 : 0
            }
            ToolTip.visible: hover.hovered
            ToolTip.text: qsTr("向左拖出只读对照；向右拖回收起")
        }
        Item {
            id: primary
            SplitView.fillWidth: true
            SplitView.minimumWidth: Math.min(240, Math.max(0, split.width - 4))
            clip: true
        }
        Item {
            id: secondary
            visible: !root.overlayMode
            enabled: root.expanded
            SplitView.preferredWidth: 0
            SplitView.minimumWidth: 0
            SplitView.maximumWidth: Math.max(0, split.width - 244)
            clip: true
            ColumnLayout {
                anchors.fill: parent
                spacing: 0
                Item {
                    id: referenceHeaderHost
                    Layout.fillWidth: true
                    Layout.preferredHeight: 30
                }
                ChartView {
                    id: referenceChart
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    documentSession: root.referenceSession
                    editable: false
                    editorTool: root.editorTool
                    hideSecondaryChannels: root.hideSecondaryChannels
                    showExtras: root.showExtras
                    onNavigationCompleted: root.queueSync(referenceChart)
                    onHeightChanged: root.queueSync(root.primaryChartView)
                    onNoteClicked: (ref, ctrl) => {
                        if (!ctrl) { selection = [ref]; metaSelection = []; return }
                        let next = selection.slice()
                        const key = JSON.stringify(ref)
                        const index = next.findIndex(r => JSON.stringify(r) === key)
                        if (index < 0) next.push(ref)
                        else next.splice(index, 1)
                        selection = next
                    }
                    onSelectionFinished: (refs) => { selection = refs; metaSelection = [] }
                    onCanvasClicked: { selection = []; metaSelection = [] }
                    onMetaObjectClicked: (obj, ctrl) => { metaSelection = [obj]; selection = [] }
                    onEditAreaPressed: root.areaPressed()
                    // Deliberately no edit/audition consumers: B is visual only.
                }
                Label {
                    Layout.fillWidth: true
                    Layout.minimumWidth: 0
                    Layout.preferredHeight: 20
                    text: !root.referenceSession ? qsTr("从上方选择已打开文档")
                          : !root.structureStatus.compatible
                            ? qsTr("%1；可关闭同步").arg(root.structureStatus.message)
                            : referenceChart.hoverText
                    HoverHandler { id: syncWarningHover }
                    ToolTip.visible: syncWarningHover.hovered && !root.structureStatus.compatible
                    ToolTip.text: text
                    elide: Text.ElideRight
                    color: Theme.textMuted
                    font.pixelSize: Theme.fsSmall
                }
            }
        }
    }
}
