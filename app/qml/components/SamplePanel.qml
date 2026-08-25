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
    /// 双击行 → 手动重命名该采样的 #WAV id（2026-09 用户；为 BGA 编辑打基础）。
    /// from = 旧 id 文本，to = 新 id 文本；→ Main 走 sample.rename 并刷新面板。
    signal sampleRenameRequested(string from, string to)

    function requireId(id) {
        const idx = sampleModel.indexOfId(id)
        if (idx >= 0) {
            sampleModel.selectId(id)
            listView.positionViewAtIndex(idx, ListView.Contain)
        }
    }

    spacing: 6

    // ---- 搜索确认框 + 排序切换（文件名 / ID；ID 大小写敏感，base62 区分 1a/1A） ----
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
                /// 双击编辑 id 模式（2026-09）：显示内联 TextField 改 #WAV id，Enter/失焦提交。
                property bool editing: false
                onEditingChanged: if (editing) idEdit.forceActiveFocus()

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
                        visible: !row.editing
                        color: sampleModel.currentSample === row.id ? Theme.primary : Theme.textMuted
                        font.family: Theme.fontMono
                        font.pixelSize: Theme.fsSmall
                    }
                    TextField {
                        id: idEdit
                        visible: row.editing
                        Layout.preferredWidth: 84
                        text: row.id
                        selectByMouse: true
                        color: Theme.text
                        placeholderTextColor: Theme.textFaint
                        font.family: Theme.fontMono
                        font.pixelSize: Theme.fsSmall
                        background: Rectangle {
                            radius: Theme.radiusSm
                            border.width: 1
                            border.color: idEdit.activeFocus ? Theme.primary : Theme.borderStrong
                            color: Theme.surface2
                        }
                        onAccepted: {
                            const newId = text.trim()
                            row.editing = false
                            if (newId !== "" && newId !== row.id)
                                root.sampleRenameRequested(row.id, newId)
                        }
                        Keys.onEscapePressed: { row.editing = false }
                        Keys.onReleased: (event) => {
                            if (event.key === Qt.Key_Tab) row.editing = false
                        }
                        onActiveFocusChanged: if (!activeFocus) row.editing = false
                    }
                    Label {
                        text: row.file
                        color: Theme.textFaint
                        elide: Text.ElideMiddle
                        Layout.fillWidth: true
                        font.pixelSize: Theme.fsTiny
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