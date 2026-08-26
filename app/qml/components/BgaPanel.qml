// SPDX-License-Identifier: GPL-3.0-only
// BGA 面板（左 Dock「BGA」标签页，2026-09）：#BMP 定义 + base/poor/layer/layer2 事件管理。
// 数据源 = session.samples（bmp 数组）+ bga.list（当前层事件，升序）。
// 编辑经信号 → Main 走 bga.put/delete、sample.setFile/rename(kind=bmp)，成功后 Main 调 reload()。
// 双语言纪律（doc/08 §2）：本组件纯展示 + 事件分发，不直接 dispatch 编辑命令（只读 dispatch 可）。
// 预览（图像解码）后置：此处只编辑定义与事件，不渲染位图。
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import "../components"

ColumnLayout {
    id: root

    /// 当前图层（0=base 1=poor 2=layer 3=layer2）。⚠️ 不能命名为 layer（Item.layer 是 FINAL）。
    property int bgaLayer: 0
    /// BGA 事件（bga.list 当前层结果；[{measure, pos:{num,den}, layer, sample(数值)}]升序）。
    property var events: []
    /// #BMP 定义（session.samples 的 bmp 数组；[{id(文本), file, refs}]）。
    property var bmpDefs: []
    property bool expandBmp: false

    /// 当前 #BMP id 文本数组（事件对话框选 BMP 用）。
    function bmpIdList() {
        const a = []
        for (let i = 0; i < root.bmpDefs.length; i++) a.push(root.bmpDefs[i].id)
        return a
    }

    /// 添加/改值 BGA 事件 → Main 走 bga.put。bmpId 为文本 id。
    signal bgaEditRequested(int layer, int measure, int num, int den, string bmpId)
    /// 删除 BGA 事件 → Main 走 bga.delete。
    signal bgaDeleteRequested(int layer, int measure, int num, int den)
    /// 添加 #BMP 定义 → Main 走 sample.setFile(kind=bmp)。
    signal bmpAddRequested(string id, string file)
    /// 重命名 #BMP id → Main 走 sample.rename(kind=bmp)。
    signal bmpRenameRequested(string fromId, string toId)
    /// 删除 #BMP 定义 → Main 走 sample.delete(kind=bmp)。
    signal bmpDeleteRequested(string id)
    /// 选择当前 #BMP（视口 BGA 列放置用）→ Main 设 currentBmpId。
    signal bmpSelected(string id)
    /// 当前 #BMP（视口放置用；Main 回填高亮）。
    property string currentBmpId: ""

    /// #BMP 内联编辑状态（2026-09，同 SamplePanel）：点击其它行不丢焦点 → 统一提交。
    property string editingBmpId: ""
    property string pendingBmpFile: ""
    property string pendingBmpOrig: ""
    /// 正在编辑的行（delegate 引用）：commitBmpPending 把该行退出编辑态。
    property var _editingBmpRow: null
    function commitBmpPending() {
        if (root.editingBmpId === "") return
        const id = root.editingBmpId
        const file = root.pendingBmpFile
        const orig = root.pendingBmpOrig
        root.editingBmpId = ""; root.pendingBmpFile = ""; root.pendingBmpOrig = ""
        if (root._editingBmpRow) root._editingBmpRow.editing = false
        if (file !== orig) root.bmpSetFileRequested(id, file)
    }
    /// 设置 #BMP 文件 → Main 走 sample.setFile(kind=bmp)。
    signal bmpSetFileRequested(string id, string file)

    /// 读取 BMP 定义 + 当前层事件（打开谱面 / 每次编辑后由 Main 调用）。
    function reload() {
        const s = JSON.parse(beatbench.dispatch(JSON.stringify({ command: "session.samples", args: {} })))
        root.bmpDefs = (s.ok && s.result.samples && s.result.samples.bmp)
                       ? s.result.samples.bmp : []
        const e = JSON.parse(beatbench.dispatch(JSON.stringify({ command: "bga.list", args: { layer: root.bgaLayer } })))
        root.events = (e.ok && e.result.events) ? e.result.events : []
    }

    function reloadEvents() {
        const e = JSON.parse(beatbench.dispatch(JSON.stringify({ command: "bga.list", args: { layer: root.bgaLayer } })))
        root.events = (e.ok && e.result.events) ? e.result.events : []
    }

    spacing: 4

    // ---- 顶栏：图层切换 + 添加事件按钮 ----
    RowLayout {
        Layout.fillWidth: true
        spacing: 4
        BbTabStrip {
            id: layerTabs
            Layout.fillWidth: true
            Layout.preferredHeight: 28
            model: [qsTr("base"), qsTr("poor"), qsTr("layer"), qsTr("layer2")]
            currentIndex: root.bgaLayer
            onIndexRequested: (i) => { root.bgaLayer = i; root.reloadEvents() }
        }
        BbToolButton {
            text: qsTr("+")
            Layout.preferredWidth: 30
            Layout.preferredHeight: 28
            onClicked: eventDialog.openForAdd(root.bgaLayer)
            ToolTip.visible: hovered
            ToolTip.text: qsTr("在该图层的这个位置放一个 #BMP 事件")
        }
    }

    // ---- 事件列表（当前层） ----
    Item {
        Layout.fillWidth: true
        Layout.fillHeight: true

        Label {
            visible: root.events.length === 0
            anchors.top: parent.top
            anchors.left: parent.left
            anchors.right: parent.right
            text: qsTr("无 %1 事件（点「+」添加；或视口 BGA 列点击放置）").arg(root.layerName(root.bgaLayer))
            color: Theme.textFaint
            font.pixelSize: Theme.fsTiny
            wrapMode: Text.WordWrap
        }

        ListView {
            anchors.fill: parent
            clip: true
            model: root.events
            spacing: 2
            ScrollBar.vertical: ScrollBar { policy: ScrollBar.AsNeeded }
            delegate: Rectangle {
                id: row
                required property var modelData
                width: ListView.view.width
                height: 24
                radius: Theme.radiusSm
                color: rowMouse.containsMouse ? Theme.surface3 : "transparent"
                RowLayout {
                    anchors.fill: parent
                    anchors.leftMargin: 6
                    anchors.rightMargin: 6
                    spacing: 6
                    Label {
                        text: qsTr("小节 %1 · %2/%3").arg(row.modelData.measure)
                              .arg(row.modelData.pos.num).arg(row.modelData.pos.den)
                        color: Theme.textMuted
                        font.family: Theme.fontMono
                        font.pixelSize: Theme.fsTiny
                        elide: Text.ElideRight
                    }
                    Item { Layout.fillWidth: true }
                    Label {
                        text: "#BMP" + chartSession.idTextOf(row.modelData.sample)
                        color: Theme.accent
                        font.family: Theme.fontMono
                        font.pixelSize: Theme.fsSmall
                        elide: Text.ElideRight
                    }
                    BbToolButton {
                        text: "×"
                        Layout.preferredWidth: 20
                        Layout.preferredHeight: 18
                        flatStyle: true
                        onClicked: root.bgaDeleteRequested(
                                       root.bgaLayer, row.modelData.measure,
                                       row.modelData.pos.num, row.modelData.pos.den)
                        ToolTip.visible: hovered
                        ToolTip.text: qsTr("删除该 BGA 事件")
                    }
                }
                MouseArea {
                    id: rowMouse
                    // ⚠️ 置 z:-1：本 MouseArea 声明在行内容之后；默认 z 会盖住「×」删除按钮。
                    z: -1
                    anchors.fill: parent
                    hoverEnabled: true
                    // 双击编辑该事件
                    onDoubleClicked: eventDialog.openForEdit(
                                         root.bgaLayer, row.modelData.measure,
                                         row.modelData.pos.num, row.modelData.pos.den,
                                         "#BMP" + chartSession.idTextOf(row.modelData.sample))
                    acceptedButtons: Qt.LeftButton | Qt.RightButton
                    onClicked: (mouse) => {
                        if (mouse.button === Qt.RightButton)
                            root.bgaDeleteRequested(root.bgaLayer, row.modelData.measure,
                                                    row.modelData.pos.num, row.modelData.pos.den)
                    }
                }
            }
        }
    }

    // ---- #BMP 定义（折叠） ----
    Rectangle { Layout.fillWidth: true; height: 1; color: Theme.border }
    BbToolButton {
        text: (root.expandBmp ? "▾ " : "▸ ") + qsTr("#BMP 定义（%1）").arg(root.bmpDefs.length)
        flatStyle: true
        active: root.expandBmp
        onClicked: root.expandBmp = !root.expandBmp
        font.pixelSize: Theme.fsSmall
        Layout.fillWidth: true
        ToolTip.visible: hovered
        ToolTip.text: qsTr("BGA 图像定义表（#BMPxx → 文件）；预览解码后置")
    }
    ColumnLayout {
        visible: root.expandBmp
        Layout.fillWidth: true
        spacing: 4

        // 添加行：id + 文件 + 添加按钮
        RowLayout {
            Layout.fillWidth: true
            spacing: 4
            BbTextField {
                id: addBmpId
                Layout.fillWidth: true
                placeholderText: qsTr("id(01/ZZ)")
                font.family: Theme.fontMono
                font.pixelSize: Theme.fsSmall
            }
            BbTextField {
                id: addBmpFile
                Layout.fillWidth: true
                placeholderText: qsTr("文件.png/mpg")
                font.pixelSize: Theme.fsSmall
            }
            BbToolButton {
                text: qsTr("添加")
                Layout.preferredWidth: 40
                Layout.preferredHeight: 24
                onClicked: {
                    const id = addBmpId.text.trim()
                    const file = addBmpFile.text.trim()
                    if (id === "") { root.bmpAddRequested("", ""); return }
                    root.bmpAddRequested(id, file)
                    addBmpId.text = ""; addBmpFile.text = ""
                }
                ToolTip.visible: hovered
                ToolTip.text: qsTr("添加/覆盖一个 #BMP 定义")
            }
        }
        Label {
            visible: root.bmpDefs.length === 0
            Layout.fillWidth: true
            text: qsTr("（暂无 #BMP；上方 id+文件 添加）")
            color: Theme.textFaint
            font.pixelSize: Theme.fsTiny
        }
        Item {
            Layout.fillWidth: true
            Layout.preferredHeight: Math.min(140, root.bmpDefs.length * 22 + 4)
            // 空白区点击 → 提交内联编辑（文件管理器改名字义：点面板空白也保存）。z:-1 不拦截行点击。
            MouseArea {
                anchors.fill: parent
                z: -1
                onPressed: root.commitBmpPending()
            }
            ListView {
            anchors.fill: parent
            clip: true
            model: root.bmpDefs
            spacing: 2
            ScrollBar.vertical: ScrollBar { policy: ScrollBar.AsNeeded }
            delegate: Rectangle {
                id: bmpRow
                required property var modelData
                property bool editing: false
                property bool committing: false
                onEditingChanged: if (bmpRow.editing) {
                    bmpFileEdit.forceActiveFocus()
                    root.editingBmpId = bmpRow.modelData.id
                    root.pendingBmpFile = bmpRow.modelData.file
                    root.pendingBmpOrig = bmpRow.modelData.file
                    root._editingBmpRow = bmpRow
                } else if (root._editingBmpRow === bmpRow) {
                    root._editingBmpRow = null
                }
                /// 保存（Enter/失焦/Tab）：走根级 commitBmpPending（点其它行/空白统一提交）。
                function commitEdit() {
                    if (!bmpRow.editing || bmpRow.committing) return
                    bmpRow.committing = true
                    bmpRow.editing = false
                    root.commitBmpPending()
                    bmpRow.committing = false
                }
                width: ListView.view.width
                height: 24
                radius: Theme.radiusSm
                color: bmpRow.modelData.id === root.currentBmpId
                       ? Theme.primarySoft
                       : (bmpMouse.containsMouse ? Theme.surface3 : "transparent")
                RowLayout {
                    anchors.fill: parent
                    anchors.leftMargin: 6
                    anchors.rightMargin: 6
                    spacing: 4
                    Label {
                        text: "#BMP" + bmpRow.modelData.id
                        color: bmpRow.modelData.id === root.currentBmpId ? Theme.primary : Theme.textMuted
                        font.family: Theme.fontMono
                        font.pixelSize: Theme.fsTiny
                    }
                    Label {
                        text: bmpRow.modelData.file
                        visible: !bmpRow.editing
                        color: Theme.textMuted
                        font.pixelSize: Theme.fsTiny
                        elide: Text.ElideMiddle
                        Layout.fillWidth: true
                    }
                    TextField {
                        id: bmpFileEdit
                        visible: bmpRow.editing
                        Layout.fillWidth: true
                        text: bmpRow.modelData.file
                        selectByMouse: true
                        color: Theme.text
                        placeholderTextColor: Theme.textFaint
                        font.family: Theme.fontMono
                        font.pixelSize: Theme.fsTiny
                        background: Rectangle {
                            radius: Theme.radiusSm
                            border.width: 1
                            border.color: bmpFileEdit.activeFocus ? Theme.primary : Theme.borderStrong
                            color: Theme.surface2
                        }
                        // 文件管理器改名语义：Enter/失焦 = 保存；Esc = 取消（还原旧值）
                        onTextChanged: { if (root.editingBmpId === bmpRow.modelData.id) root.pendingBmpFile = text }
                        onAccepted: bmpRow.commitEdit()
                        onActiveFocusChanged: if (!activeFocus && bmpRow.editing) bmpRow.commitEdit()
                        Keys.onEscapePressed: { bmpRow.editing = false; root.editingBmpId = "" }
                    }
                    Label {
                        text: qsTr("(%1)").arg(bmpRow.modelData.refs)
                        color: Theme.textFaint
                        font.family: Theme.fontMono
                        font.pixelSize: Theme.fsTiny
                    }
                    BbToolButton {
                        text: "×"
                        Layout.preferredWidth: 20
                        Layout.preferredHeight: 18
                        flatStyle: true
                        onClicked: root.bmpDeleteRequested(bmpRow.modelData.id)
                        ToolTip.visible: hovered
                        ToolTip.text: qsTr("删除 #BMP%1 定义（引用保留原 id；Undo 可恢复）")
                                       .arg(bmpRow.modelData.id)
                    }
                }
                MouseArea {
                    id: bmpMouse
                    z: -1
                    anchors.fill: parent
                    enabled: !bmpRow.editing
                    hoverEnabled: true
                    // 左键 = 选为当前 #BMP（视口 BGA 列放置用）+ 先提交其它行的内联编辑；双击 = 内联改文件
                    onClicked: (mouse) => {
                        if (mouse.button === Qt.LeftButton) {
                            root.commitBmpPending()
                            root.bmpSelected(bmpRow.modelData.id)
                        }
                    }
                    onDoubleClicked: bmpRow.editing = true
                }
            }
            }
        }
    }

    // ---- 添加/改值 BGA 事件对话框 ----
    Dialog {
        id: eventDialog
        modal: true
        width: Math.min(root.width - 4, 240)
        anchors.centerIn: parent
        title: qsTr("BGA 事件")
        standardButtons: Dialog.Ok | Dialog.Cancel

        property int dlgLayer: 0
        property bool editing: false
        property int baseMeasure: 0
        property int baseNum: 0
        property int baseDen: 1
        property string baseBmpId: ""

        function openForAdd(layer) {
            eventDialog.dlgLayer = layer
            eventDialog.editing = false
            baseMeasure = 0; baseNum = 0; baseDen = 1
            baseBmpId = root.bmpIdList().length ? root.bmpIdList()[0] : ""
            applyFields()
            open()
        }
        function openForEdit(layer, measure, num, den, bmpIdText) {
            eventDialog.dlgLayer = layer
            eventDialog.editing = true
            baseMeasure = measure; baseNum = num; baseDen = den
            // "#BMP01" → "01"
            baseBmpId = bmpIdText.indexOf("#BMP") === 0 ? bmpIdText.substring(4) : bmpIdText
            applyFields()
            open()
        }
        function applyFields() {
            measureSpin.value = baseMeasure
            numSpin.value = baseNum
            denSpin.value = Math.max(1, baseDen)
            bmpIdField.text = baseBmpId
        }
        onOpened: measureSpin.forceActiveFocus()

        ColumnLayout {
            anchors.fill: parent
            spacing: 6
            RowLayout {
                spacing: 6
                Label { text: qsTr("图层"); color: Theme.textMuted; font.pixelSize: Theme.fsTiny }
                Label {
                    text: root.layerName(eventDialog.dlgLayer)
                    color: Theme.accent; font.family: Theme.fontMono; font.pixelSize: Theme.fsTiny
                }
            }
            RowLayout {
                spacing: 6
                Label { text: qsTr("小节"); color: Theme.textMuted; font.pixelSize: Theme.fsTiny }
                BbSpinBox { id: measureSpin; from: 0; to: 999; editable: true
                            escapeHandler: function() { eventDialog.reject() }; Layout.fillWidth: true }
            }
            RowLayout {
                spacing: 6
                Label { text: qsTr("位置"); color: Theme.textMuted; font.pixelSize: Theme.fsTiny }
                BbSpinBox { id: numSpin; from: 0; to: 999; editable: true
                            escapeHandler: function() { eventDialog.reject() }; Layout.fillWidth: true }
                Label { text: "/"; color: Theme.textMuted; font.pixelSize: Theme.fsTiny }
                BbSpinBox { id: denSpin; from: 1; to: 999; editable: true
                            escapeHandler: function() { eventDialog.reject() }; Layout.fillWidth: true }
            }
            RowLayout {
                spacing: 6
                Label { text: qsTr("#BMP"); color: Theme.textMuted; font.pixelSize: Theme.fsTiny }
                BbTextField {
                    id: bmpIdField
                    Layout.fillWidth: true
                    placeholderText: qsTr("01/ZZ")
                    font.family: Theme.fontMono
                    onAccepted: eventDialog.accept()
                    escapeHandler: function() { eventDialog.reject() }
                }
            }
            Label {
                visible: root.bmpDefs.length === 0
                text: qsTr("（暂无 #BMP；先在上方添加）")
                color: Theme.warning
                font.pixelSize: Theme.fsTiny
            }
        }
        onAccepted: root.bgaEditRequested(eventDialog.dlgLayer, measureSpin.value,
                                          numSpin.value, denSpin.value, bmpIdField.text.trim())
    }

    // ---- 展示辅助 ----
    function layerName(l) {
        switch (l) {
            case 1: return qsTr("poor")
            case 2: return qsTr("layer")
            case 3: return qsTr("layer2")
            default: return qsTr("base")
        }
    }
}
