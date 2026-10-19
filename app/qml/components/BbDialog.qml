// SPDX-License-Identifier: GPL-3.0-only
// 主题化模态对话框基座（2026-09 用户：Dialog 统一皮肤化——**全部颜色/字号走 Theme token**，
// 皮肤换 token 即整体换肤；默认 Qt Basic 样式白底不可换肤，与暗色控件割裂）。
// 参照 SettingsDialog 的 header/footer 主题化模式；geometry 约定（2026-09 实测踩坑）：
// - contentItem **不 anchors.fill**（其父级 = 整窗含 header/footer，fill 会压到 footer 下面）；
//   内容用内边距 12，与标题文字对齐；
// - Dialog 高度显式给；紧凑表单用 height: naturalHeight（计入正文/标题/页脚），
//   不再用固定高度挤压正文。旧的大型弹窗仍可自行指定高度。
// 用法：BbDialog { width: 320; height: 186; title: qsTr("…"); 内容直接写子项 }
// （子项自动进内容 ColumnLayout；取消按钮可经 showCancel 隐藏）。
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Dialog {
    id: root

    modal: true
    padding: 0

    BbDialogPositioner { id: dialogPositioner; dialog: root }

    /// 是否显示「取消」（关于类对话框只留确定）。
    property bool showCancel: true
    /// 内容子项（默认 property：直接写子项即可）。
    default property alias content: bodyColumn.data
    readonly property int contentPadding: 12
    // Opt-in for compact forms: include the actual rows, wrapping, header and footer.
    // Fixed legacy heights otherwise squeeze ColumnLayout below its implicit minimum.
    readonly property real naturalHeight: header.height + footer.height + 2 * spacing
                                          + topPadding + bottomPadding + contentItem.implicitHeight

    background: Rectangle {
        color: Theme.surface
        border.color: Theme.borderStrong
        border.width: 1
        radius: Theme.boxRadius
    }
    header: BbDialogHeader { dialog: root; positioner: dialogPositioner }
    footer: Rectangle {
        width: root.width
        height: 42
        color: Theme.surface2
        border.color: Theme.borderStrong
        border.width: 1
        RowLayout {
            anchors.fill: parent
            anchors.margins: 5
            anchors.rightMargin: 8
            spacing: 8
            Item { Layout.fillWidth: true }
            BbToolButton {
                text: qsTr("确定")
                onClicked: root.accept()
            }
            BbToolButton {
                text: qsTr("取消")
                visible: root.showCancel
                enabled: root.showCancel
                onClicked: root.reject()
            }
        }
    }
    contentItem: Item {
        implicitWidth: root.width
        implicitHeight: bodyColumn.implicitHeight + 20
        ColumnLayout {
            id: bodyColumn
            anchors.fill: parent
            anchors.leftMargin: root.contentPadding
            anchors.rightMargin: root.contentPadding
            anchors.topMargin: 10
            anchors.bottomMargin: 10
            spacing: 10
        }
    }
}
