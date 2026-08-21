// SPDX-License-Identifier: GPL-3.0-only
// 底部页面条（Resolve 式页面切换）：[编辑 | 切音 | 测试]。
// 纯布局组件，不碰逻辑（doc/05 §4.1：位置可配置，默认底部）。
// 按钮样式对照 .seg.active（主色底 + 白字，doc/beatbench-ui-preview.html）。
import QtQuick
import QtQuick.Controls

Rectangle {
    id: root
    property int currentPage: 0
    signal pageRequested(int index)

    height: 34
    color: Theme.surface2
    border.color: Theme.border

    Row {
        anchors.fill: parent
        anchors.leftMargin: 10
        anchors.rightMargin: 10
        spacing: 6
        topPadding: 5
        bottomPadding: 5

        Repeater {
            model: [
                { label: qsTr("编辑"), hint: qsTr("调整谱面") },
                { label: qsTr("切音"), hint: qsTr("对音 / MIDI / key 音（占位）") },
                { label: qsTr("测试"), hint: qsTr("试玩 / 预览（占位）") }
            ]
            Button {
                id: pageBtn
                width: 112
                height: 26
                highlighted: index === root.currentPage
                text: modelData.label + " · " + modelData.hint
                onClicked: root.pageRequested(index)

                background: Rectangle {
                    radius: Theme.radiusSm
                    color: pageBtn.highlighted
                           ? Theme.primary
                           : (pageBtn.hovered ? Theme.surface3 : Theme.surface2)
                    border.width: 1
                    border.color: pageBtn.highlighted ? Theme.primary : Theme.borderStrong
                }
                contentItem: Label {
                    text: pageBtn.text
                    color: pageBtn.highlighted ? Theme.onAccent : Theme.textMuted
                    font.pixelSize: Theme.fsSmall
                    font.family: Theme.fontSans
                    elide: Text.ElideRight
                    horizontalAlignment: Text.AlignHCenter
                    verticalAlignment: Text.AlignVCenter
                }
            }
        }
    }
}
