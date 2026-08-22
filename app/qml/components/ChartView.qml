// SPDX-License-Identifier: GPL-3.0-only
// 中央视口：竖向时间轴（M2 第 5 步，doc/07 §3 步 5）。
// ChartViewItem 自绘（QPainter，真数据 = chartSession 上下文属性）；滚动/缩放/水平滚动
// 由本组件驱动（⚠️ Flickable 无 onWheel → WheelHandler，doc/04 §5）。秒标尺/波形铺底后置 Phase B。
// BGM 列头点击 → 展开为按 #WAV id 分列（iBMSC 式后台轨分开显示，BMS 笔记 ch01 注）；
// 列头显示实际 BMS 通道 id = 工具条勾选 / Ctrl 临时（C++ KeyMonitor 事件过滤器，Adobe 式）。
// 皮肤边界（doc/08 §3.6 / doc/05 §8）：本组件 = 默认皮肤 surface「viewport」的组件库成员，
// L3 可按组件覆写。
import QtQuick
import BeatBench

Item {
    id: root

    property real measureHeight: 96      // 小节高度（缩放 = 改此值；Ctrl+滚轮快速缩放）
    property bool topHigh: true          // 默认「顶部=高小节」（preview.html，note 自上而下落）
    property int gridDiv: 16             // 槽位网格（1/16 snap）
    property bool showChannelIds: false  // 列头显示实际 BMS 通道 id（工具条勾选）
    property bool bgmExpanded: false     // BGM 轨展开（列头点击切换；--bgm-expand 调试）
    property int beatNum: 1              // 拍子线：每 num/den 音符一条（默认 [1]/[4] = 每 4 分音符）
    property int beatDen: 4
    property int noteSampleMode: 0       // note 采样标签：0 隐藏 / 1 id / 2 文件名
    property bool showExtras: false      // 更多轨道（BGA 图层通道列，游玩轨与背景轨之间）

    onBgmExpandedChanged: view.bgmExpanded = bgmExpanded

    ChartViewItem {
        id: view
        anchors.fill: parent
        anchors.bottomMargin: 12   // 底部水平滚动条区域
        anchors.rightMargin: 12    // 右侧垂直滚动条区域
        session: typeof chartSession !== "undefined" ? chartSession : null
        theme: Theme
        measureHeight: root.measureHeight
        topHigh: root.topHigh
        gridDiv: root.gridDiv
        beatNum: root.beatNum
        beatDen: root.beatDen
        noteSampleMode: root.noteSampleMode
        showExtras: root.showExtras
        // Ctrl 按住临时切换（C++ KeyMonitor 应用级事件过滤；QML Keys 收不到独立修饰键）
        showChannelIds: root.showChannelIds !== keyMonitor.ctrlHeld
        onChartChanged: {
            // 谱面切换 → 定位到开头（hi-top 下小节 0 在视口底部）
            view.scrollY = view.topHigh ? Math.max(0, view.contentHeight - view.height) : 0
        }
    }

    WheelHandler {
        acceptedDevices: PointerDevice.Mouse | PointerDevice.TouchPad
        onWheel: (event) => {
            if (event.modifiers & Qt.ControlModifier) {
                // 缩放：指小节高度（内容比例不变，仅视口密度）
                const f = event.angleDelta.y > 0 ? 1.2 : (event.angleDelta.y < 0 ? 1.0 / 1.2 : 1.0)
                if (f !== 1.0)
                    root.measureHeight = Math.min(240, Math.max(24, root.measureHeight * f))
                event.accepted = true
                return
            }
            // 水平滚动：触控板横移 / 滚轮横向倾斜 / Shift+滚轮（见更多通道）
            let dx = event.pixelDelta.x
            if (dx === 0) dx = event.angleDelta.x
            if (dx === 0 && (event.modifiers & Qt.ShiftModifier)) dx = event.angleDelta.y
            if (dx !== 0) {
                view.scrollX -= dx
                event.accepted = true
                return
            }
            // 垂直滚动（pixelDelta 触控板优先；angleDelta 为滚轮档位）
            const dy = event.pixelDelta.y !== 0 ? event.pixelDelta.y
                                                : event.angleDelta.y / 120 * 48
            view.scrollY -= dy
            event.accepted = true
        }
    }

    // 点击（顶部 18px 列头内命中 BGM → 展开/折叠；否则起拖） + 拖拽滚动（含水平）
    // （编辑/框选为 M3，届时与此手势互斥）
    MouseArea {
        anchors.fill: parent
        cursorShape: Qt.OpenHandCursor
        property real lastY: -1
        property real lastX: 0
        onPressed: (mouse) => {
            if (mouse.y <= 18 && view.bgmHeaderIndexAt(mouse.x) >= 0) {
                root.bgmExpanded = !root.bgmExpanded
                return
            }
            lastY = mouse.y
            lastX = mouse.x
        }
        onPositionChanged: (mouse) => {
            if (lastY >= 0) {
                view.scrollY -= (mouse.y - lastY)
                view.scrollX -= (mouse.x - lastX)
                lastY = mouse.y
                lastX = mouse.x
            }
        }
        onReleased: lastY = -1
    }

    // 右侧垂直滚动条（内容高于视口时出现；点击/拖拽滑块滑动）
    Rectangle {
        id: vbar
        anchors.top: parent.top
        anchors.bottom: parent.bottom
        anchors.right: parent.right
        width: 12
        z: 2
        color: Theme.surface2
        visible: view.contentHeight > view.height + 1
        readonly property real maxY: Math.max(1, view.contentHeight - view.height)
        readonly property real thumbH: Math.max(28, height * view.height / Math.max(1, view.contentHeight))
        Rectangle {
            id: vthumb
            width: parent.width
            height: vbar.thumbH
            color: Theme.surface3
            border.color: Theme.border
            y: (vbar.height - vbar.thumbH) * (view.scrollY / vbar.maxY)
        }
        MouseArea {
            anchors.fill: parent
            property real grabOffset: 0
            onPressed: (mouse) => {
                grabOffset = mouse.y - vthumb.y
                view.scrollY = (mouse.y - vthumb.height / 2) /
                               Math.max(1, vbar.height - vthumb.height) * vbar.maxY
            }
            onPositionChanged: (mouse) => {
                if (pressed)
                    view.scrollY = (mouse.y - grabOffset) /
                                   Math.max(1, vbar.height - vthumb.height) * vbar.maxY
            }
        }
    }

    // 底部水平滚动条（轨道列超宽时出现；点击/拖拽滑块滑动）
    Rectangle {
        id: hbar
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        height: 12
        z: 2
        color: Theme.surface2
        visible: view.contentWidth > view.width + 1
        readonly property real maxX: Math.max(1, view.contentWidth - view.width)
        readonly property real thumbW: Math.max(28, width * view.width / Math.max(1, view.contentWidth))
        Rectangle {
            id: thumb
            width: hbar.thumbW
            height: parent.height
            color: Theme.surface3
            border.color: Theme.border
            x: (hbar.width - hbar.thumbW) * (view.scrollX / hbar.maxX)
        }
        MouseArea {
            anchors.fill: parent
            property real grabOffset: 0
            onPressed: (mouse) => {
                grabOffset = mouse.x - thumb.x
                view.scrollX = (mouse.x - thumb.width / 2) /
                               Math.max(1, hbar.width - thumb.width) * hbar.maxX
            }
            onPositionChanged: (mouse) => {
                if (pressed)
                    view.scrollX = (mouse.x - grabOffset) /
                                   Math.max(1, hbar.width - thumb.width) * hbar.maxX
            }
        }
    }

    // 状态栏用：鼠标位置 + note 信息（hoverText 由 ChartViewItem 计算）
    readonly property string hoverText: view.hoverText
}
