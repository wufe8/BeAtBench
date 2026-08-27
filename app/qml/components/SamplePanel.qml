// SPDX-License-Identifier: GPL-3.0-only
// 采样面板（左 Dock「采样」标签，M2 第 4 步）：检索 + 动态分组 + 排序 + 选中即「当前采样」。
// 检索/分组/排序逻辑在 C++（SampleListModel，双语言纪律 doc/08 §2）；
// 本组件只做表现：搜索确认框（下拉已舍弃）+ 可横滚分组条 + 列表（索引条快跳）+ 排序切换。
// M3 起：当前采样 = 放置 note/部件的默认采样（note.put 的 sample 落点）。
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import "../components"

ColumnLayout {
    id: root

    signal samplePicked(string id, string file)
    /// 双击行 → 手动编辑该采样槽位绑定的**文件名**（切音工作区手工版；→ Main 走 sample.setFile）。
    /// id = 槽位 #WAV id 文本，file = 新文件名。
    signal sampleFileRequested(string id, string file)
    /// 添加一个新的 #WAV 定义（id + 文件名）→ Main 走 sample.addWav。
    signal sampleAddRequested(string id, string file)

    function requireId(id) {
        const idx = sampleModel.indexOfId(id)
        if (idx >= 0) {
            sampleModel.selectId(id)
            listView.positionViewAtIndex(idx, ListView.Contain)
        }
    }

    /// 读/恢复列表滚动位置（编辑文件后 reload 模型会重置 contentY → 保持视口不变）。
    function listScrollY() { return listView.contentY }
    function restoreScrollY(y) { listView.contentY = y }

    /// 内联编辑状态（2026-09）：跟踪正在编辑的行 id + 新值；点击其它行/空白时不丢焦点
    /// （非焦点项点击不触发 onActiveFocusChanged → 不保存，用户反馈）→ 由此统一提交。
    property string editingId: ""
    property string pendingFile: ""
    property string pendingOrig: ""
    /// 正在编辑的行（delegate 引用）：commitPending 负责把该行退出编辑态。
    /// （仅清空 editingId 不够——行 delegate 的 editing 仍是 true，TextField 仍显示。）
    property var _editingRow: null
    function commitPending() {
        if (root.editingId === "") return
        const id = root.editingId
        const file = root.pendingFile
        const orig = root.pendingOrig
        root.editingId = ""; root.pendingFile = ""; root.pendingOrig = ""
        // 退出编辑态（点击其它行/空白/Enter 三路皆经此；必须真正置回行 editing=false）
        if (root._editingRow) root._editingRow.editing = false
        if (file !== orig) root.sampleFileRequested(id, file)
    }

    spacing: 6

    // ---- 添加 #WAV 定义（id + 文件名 + 添加按钮，类似 BPM/STOP 定义区） ----
    RowLayout {
        Layout.fillWidth: true
        spacing: 4
        BbTextField {
            id: addWavId
            Layout.fillWidth: true
            Layout.minimumWidth: 40
            placeholderText: qsTr("id（如 01/ZZ/aa）")
            font.family: Theme.fontMono
            font.pixelSize: Theme.fsSmall
        }
        BbTextField {
            id: addWavFile
            Layout.fillWidth: true
            Layout.minimumWidth: 40
            placeholderText: qsTr("文件名")
            font.family: Theme.fontMono
            font.pixelSize: Theme.fsSmall
        }
        BbToolButton {
            text: qsTr("添加")
            Layout.preferredWidth: 40
            Layout.preferredHeight: 24
            onClicked: {
                const id = addWavId.text.trim()
                const file = addWavFile.text.trim()
                if (id === "") { addWavId.forceActiveFocus(); return }
                root.sampleAddRequested(id, file)
                addWavId.text = ""; addWavFile.text = ""
            }
            ToolTip.visible: hovered
            ToolTip.text: qsTr("添加/覆盖一个 #WAV 定义（支持 base62：00-99, AA-ZZ, aa-zz）")
        }
    }

    // ---- 搜索确认框 + 排序切换 ----
    RowLayout {
        Layout.fillWidth: true
        spacing: 6
        TextField {
            id: searchBox
            Layout.fillWidth: true
            placeholderText: qsTr("搜索 文件名 / ID…")
            color: Theme.text
            placeholderTextColor: Theme.textFaint
            font.family: Theme.fontMono
            font.pixelSize: Theme.fsSmall
            selectByMouse: true
            background: Rectangle {
                color: Theme.surface2
                border.color: Theme.borderStrong
                radius: Theme.radiusSm
            }
            onTextChanged: sampleModel.filterText = text
            Keys.onPressed: (event) => {
                if (event.key === Qt.Key_Down && listView.count > 0)
                    listView.incrementCurrentIndex()
                else if (event.key === Qt.Key_Up && listView.count > 0)
                    listView.decrementCurrentIndex()
                else if (event.key === Qt.Key_Return && listView.currentIndex >= 0) {
                    // 确认 = 设为当前采样（搜索确认框语义）
                    const id = sampleModel.idAt(listView.currentIndex)
                    sampleModel.selectId(id)
                    root.samplePicked(id, "")
                }
            }
        }
        // 排序模式（智能 = 引用数→首现小节→id 稳定序，MRU 只归「最近」）
        RowLayout {
            spacing: 0
            Repeater {
                model: [
                    { label: qsTr("智能"),  mode: 0, tip: qsTr("引用数 → 首现小节 → ID（稳定序，选中不重排）") },
                    { label: "ID",         mode: 1, tip: qsTr("按采样 ID 升序") },
                    { label: qsTr("文件"),  mode: 2, tip: qsTr("按文件名升序") },
                    { label: qsTr("引用"),  mode: 3, tip: qsTr("按谱面引用次数降序") },
                    { label: qsTr("最近"),  mode: 4, tip: qsTr("最近选用的采样置顶（随选中即时刷新）") }
                ]
                BbToolButton {
                    text: modelData.label
                    checkable: false
                    active: index === sampleModel.sortMode
                    flatStyle: true
                    onClicked: sampleModel.sortMode = modelData.mode
                    ToolTip.visible: hovered
                    ToolTip.text: modelData.tip
                    ToolTip.delay: 600
                }
            }
        }
    }

    // ---- 动态分组（全部 ⊃ 键音/皿/踏板/2P·… ⊃ 未引用/缺失；模型按 player 统计，不写死） ----
    BbTabStrip {
        Layout.fillWidth: true
        Layout.preferredHeight: 30
        model: sampleModel.groups
        currentIndex: {
            for (var i = 0; i < sampleModel.groups.length; ++i)
                if (sampleModel.groups[i].id === sampleModel.group) return i
            return 0
        }
        onIndexRequested: (index) => {
            const g = sampleModel.groups[index]
            if (g) sampleModel.group = g.id
        }
        // BbTabStrip 的 delegate 用文本；分组项 = "label count"
    }

    // ---- 列表（排序见上；缺失/扩展名徽标；右侧索引条快跳） ----
    Item {
        Layout.fillWidth: true
        Layout.fillHeight: true
        // 空白区点击 → 提交内联编辑（文件管理器改名字义：点面板空白也保存）。
        // 置于 ListView 之下（z:-1），行/索引条点击不被拦截；仅空白区命中。
        MouseArea {
            anchors.fill: parent
            z: -1
            onPressed: root.commitPending()
        }

        ListView {
            id: listView
            anchors.fill: parent
            clip: true
            model: sampleModel
            spacing: 2
            ScrollBar.vertical: ScrollBar { policy: ScrollBar.AsNeeded }
            delegate: Rectangle {
                id: row
                required property string id
                required property string file
                required property int refs
                required property bool missing
                required property bool extMismatch
                /// 双击编辑文件名模式（切音工作区手工版）：显示内联 TextField 改文件名，Enter/失焦提交。
                property bool editing: false
                property bool committing: false
                onEditingChanged: if (editing) {
                    fileEdit.forceActiveFocus()
                    root.editingId = row.id
                    root.pendingFile = row.file
                    root.pendingOrig = row.file
                    root._editingRow = row
                } else if (root._editingRow === row) {
                    root._editingRow = null
                }
                /// 保存（Enter/失焦/Tab）：走根级 commitPending（统一处理「点击其它行/空白」提交）。
                function commitEdit() {
                    if (!row.editing || row.committing) return
                    row.committing = true
                    row.editing = false
                    root.commitPending()
                    row.committing = false
                }

                width: ListView.view.width
                height: 26
                radius: Theme.radiusSm
                color: mouse.containsMouse && !row.editing ? Theme.surface3
                                                           : (sampleModel.currentSample === id ? Theme.primarySoft
                                                                                              : "transparent")
                RowLayout {
                    anchors.fill: parent
                    anchors.leftMargin: 6
                    anchors.rightMargin: 22  // 索引条 16 + 4，避免「扩展/计数」与其重叠
                    spacing: 6
                    Label {
                        text: "#WAV" + row.id
                        color: sampleModel.currentSample === row.id ? Theme.primary : Theme.textMuted
                        font.family: Theme.fontMono
                        font.pixelSize: Theme.fsSmall
                    }
                    Label {
                        text: row.file
                        visible: !row.editing
                        color: Theme.textFaint
                        elide: Text.ElideMiddle
                        Layout.fillWidth: true
                        font.pixelSize: Theme.fsTiny
                    }
                    TextField {
                        id: fileEdit
                        visible: row.editing
                        Layout.fillWidth: true
                        text: row.file
                        selectByMouse: true
                        color: Theme.text
                        placeholderTextColor: Theme.textFaint
                        font.family: Theme.fontMono
                        font.pixelSize: Theme.fsSmall
                        background: Rectangle {
                            radius: Theme.radiusSm
                            border.width: 1
                            border.color: fileEdit.activeFocus ? Theme.primary : Theme.borderStrong
                            color: Theme.surface2
                        }
                        // 文件管理器改名语义：Enter/失焦 = 保存；Esc = 取消（还原旧值）。
                        onTextChanged: { if (root.editingId === row.id) root.pendingFile = text }
                        onAccepted: row.commitEdit()
                        onActiveFocusChanged: if (!activeFocus && row.editing) row.commitEdit()
                        Keys.onEscapePressed: { row.editing = false; root.editingId = "" }
                        Keys.onReleased: (event) => {
                            if (event.key === Qt.Key_Tab) row.commitEdit()
                        }
                    }
                    // 状态点（缺失=黄；扩展名不符=青）——精简徽标；仅「缺失」提供悬停提示
                    // （扩展名不符为信息级，lint 面板聚合说明，此处不提示）
                    Rectangle {
                        width: 6
                        height: 6
                        radius: 3
                        visible: row.missing || row.extMismatch
                        color: row.missing ? Theme.warning : Theme.accent
                    }
                    Label {
                        text: row.refs > 0 ? "×" + row.refs : ""
                        color: Theme.textFaint
                        font.family: Theme.fontMono
                        font.pixelSize: Theme.fsTiny
                    }
                }
                MouseArea {
                    id: mouse
                    anchors.fill: parent
                    hoverEnabled: true
                    enabled: !row.editing
                    onClicked: {
                        root.commitPending()   // 先提交其它行的内联编辑（点击非焦点项不丢焦点）
                        sampleModel.selectId(row.id)
                        root.samplePicked(row.id, row.file)
                    }
                    onDoubleClicked: row.editing = true
                    ToolTip.visible: mouse.containsMouse && row.missing
                    ToolTip.text: qsTr("文件缺失")
                    ToolTip.delay: 400
                }
            }
            currentIndex: -1
            // 索引条快跳：按文件名首字符（0-9 A-Z；大小写折叠）。默认半透明防干扰，hover 变亮。
            Item {
                id: indexBar
                anchors.right: parent.right
                anchors.top: parent.top
                anchors.bottom: parent.bottom
                width: 16
                property bool hovering: false
                opacity: hovering ? 1.0 : 0.35
                Column {
                    anchors.fill: parent
                    Repeater {
                        model: "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ".split("")
                        Item {
                            width: indexBar.width
                            height: indexBar.height / 36
                            Text {
                                anchors.centerIn: parent
                                text: modelData
                                color: indexBarHover.containsMouse ? Theme.text : Theme.textFaint
                                font.family: Theme.fontMono
                                font.pixelSize: 8
                            }
                            MouseArea {
                                id: indexBarHover
                                anchors.fill: parent
                                hoverEnabled: true
                                onEntered: indexBar.hovering = true
                                onExited: indexBar.hovering = false
                                onClicked: {
                                    const r = sampleModel.firstRowWithFilePrefix(modelData)
                                    if (r >= 0)
                                        listView.positionViewAtIndex(r, ListView.Beginning)
                                }
                            }
                        }
                    }
                }
            }
        }
    }

    // ---- 底部：当前采样（M3 放置落点） ----
    Label {
        Layout.fillWidth: true
        elide: Text.ElideRight
        text: sampleModel.currentSampleText !== ""
              ? qsTr("当前采样：%1").arg(sampleModel.currentSampleText)
              : qsTr("未选择采样")
        color: sampleModel.currentSampleText !== "" ? Theme.accent : Theme.textFaint
        font.family: Theme.fontMono
        font.pixelSize: Theme.fsTiny
    }
    Label {
        Layout.fillWidth: true
        elide: Text.ElideRight
        text: qsTr("放置的 note / 部件将使用当前采样（M3）")
        color: Theme.textFaint
        font.pixelSize: Theme.fsTiny
    }
}