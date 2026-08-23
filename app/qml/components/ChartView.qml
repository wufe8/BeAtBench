// SPDX-License-Identifier: GPL-3.0-only
// 中央视口：竖向时间轴（M2 第 5 步，doc/07 §3 步 5）。
// ChartViewItem 自绘（QPainter，真数据 = chartSession 上下文属性）；滚动/缩放/水平滚动
// 由本组件驱动（⚠️ Flickable 无 onWheel → WheelHandler，doc/04 §5）。秒标尺/波形铺底后置 Phase B。
// 编辑接线（M3 命令，handoff-m2-m3）：
// - 工具分发：select 普通拖 = 滚动（保持原交互）、Shift+拖 = 框选（selectionFinished）；
//   pan 拖 = 滚动；note 点击 = hitTest → hitPlaceRequested（Main 走 note.put）；
//   地雷/LN 工具暂提示（M3 命令无 kind 语义）。
// - 选中高亮：Main 把 selection 回填 → view.selection（ChartViewItem 绘制）。
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
    property int snapNum: 1              // 吸附粒度分子（放置吸附 + 槽位弱线）
    property int snapDen: 16             // 吸附粒度分母
    property bool showChannelIds: false  // 列头显示实际 BMS 通道 id（工具条勾选）
    property bool bgmExpanded: false     // BGM 轨展开（列头点击切换；--bgm-expand 调试）
    property int noteSampleMode: 0       // note 采样标签：0 隐藏 / 1 id / 2 文件名
    property bool showExtras: false      // 更多轨道（BGA 图层通道列，游玩轨与背景轨之间）
    /// 当前缩放（相对默认 96px 小节高度；工具条显示用）
    readonly property int zoomPercent: Math.round(root.measureHeight / 96 * 100)
    // ---- 编辑接线（M3） ----
    property string editorTool: "select" // select/note/ln/mine/pan（Main 会话状态）
    property bool moveMode: true           // 平移开关（默认开）：拖拽选中 note = 时间轴移动
    property int sampleId: -1            // 放置用采样数值 id（chartSession.sampleValueOf）
    property string sampleText: ""       // 当前采样展示（无放置采样时提示）
    property var selection: []           // 选中 note 集合（NoteRef；回填 view.selection 高亮）
    property bool perfLog: false         // paint 帧耗时采样（--perf-log）
    property bool _ctrlHeld: false       // 本次按下是否 Ctrl（多选切换）

    signal hitPlaceRequested(var hit)       // note 工具点击 → Main 走 note.put
    signal selectionFinished(var refs)      // 框选完成 → Main 存 selection + 复制
    signal toolNotReady(string tool)        // ln/mine 工具点击（命令未接）
    signal noteClicked(var ref, bool ctrl)  // select 点击命中 note（选中；ctrl = 多选切换）
    signal canvasClicked()                  // select 点击空白（清空选中）
    signal noteRightDeleted(var ref)        // 右键命中 note（删除）
    // 平移：deltaF = 时间轴位移（拍位小数，0=不动）；targetLane = 横向移动目标列
    // （laneAtX 结果 {valid,lanePlayer,laneKind,laneIndex}；null=纯时间移动）
    signal moveSelectionRequested(real deltaF, var targetLane)

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
        snapNum: root.snapNum
        snapDen: root.snapDen
        noteSampleMode: root.noteSampleMode
        showExtras: root.showExtras
        selection: root.selection
        perfLog: root.perfLog
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

    // 手势（M3 接线）：列头命中 → 展开；否则按工具分发：
    //   拖动（select/pan/note 均可）滚动；select + Shift 拖动 = 框选；note 点击 = 放置。
    // ⚠️ press 抓取保证 release 仍回到本 Area（落在滚动条上也不丢）。
    // 状态抽为 root 级（handlePress/Release），MouseArea 与调试入口（--click）共用同一路径。
    property real _lastY: -1
    property real _lastX: 0
    property int _pressX: 0
    property int _pressY: 0
    property bool _boxSelect: false
    property bool _dragged: false
    property bool _panning: false   // 中键拖动 = 滚动（任何工具下）
    property bool _moving: false    // 拖拽选中 note = 移动（选中 note 上按下即进入，无门控）
    property real _moveStartF: 0    // 按下的拍位（measure + pos 小数）
    property real _moveDeltaF: 0    // 当前时间位移（拍位小数）
    property var _moveTargetLane: null  // 横向目标列（laneAtX 结果；null = 时间只动）

    /// 平移判定：按下点在选中集内的某个 note 上？
    function isSelectedNote(hit) {
        if (!hit || !hit.valid) return false
        for (var i = 0; i < root.selection.length; i++) {
            const s = root.selection[i]
            if (s.measure === hit.measure && s.sample === hit.sample &&
                    s.lane.kind === hit.lane.kind &&
                    s.lane.index === hit.lane.index &&
                    s.lane.player === hit.lane.player &&
                    s.pos.num === hit.pos.num && s.pos.den === hit.pos.den)
                return true
        }
        return false
    }

    function handlePress(x, y, shift, ctrl) {
        if (y <= 18 && view.bgmHeaderIndexAt(x) >= 0) {
            root.bgmExpanded = !root.bgmExpanded
            return
        }
        _lastY = y
        _lastX = x
        _pressX = x
        _pressY = y
        _dragged = false
        _ctrlHeld = ctrl
        // 拖选中 note（仅 select 工具）→ 移动（无门控：拖拽永远可移动，issue 5）。
        // 「平移」勾选与否只决定轴锁定（方向主轴），见 handleRelease。note/ln/mine 工具
        // 不进入移动（note=点击放置；点已有 note 不做移动，防误触）。
        if (root.editorTool === "select") {
            const hit = view.noteAt(x, y)
            if (hit.valid) {
                if (!isSelectedNote(hit)) root.noteClicked(hit, false)
                _moving = true
                _moveDeltaF = 0
                _moveTargetLane = null
                _moveStartF = view.measureAtY(y)
                return
            }
        }
        // select 工具 = 框选（拖动）；pan 工具 = 滚动（拖动）；note = 点击放置
        _boxSelect = root.editorTool === "select"
        if (_boxSelect) {
            selRect.x = x
            selRect.y = y
            selRect.width = 0
            selRect.height = 0
            selRect.visible = true
        }
    }
    function handleMove(x, y) {
        if (_lastY < 0 && !_moving) return
        if (_moving) {
            const moved = Math.abs(y - _pressY) + Math.abs(x - _pressX)
            if (moved > 4) _dragged = true
            // 实时 delta 拍位（供状态栏/预览；release 一次性应用）
            if (_dragged) {
                _moveDeltaF = view.measureAtY(y) - _moveStartF
                // 横向目标列（沿列头 y 带命中；无 2 轴联动时不该用到）
                const laneHit = view.laneAtX(x)
                _moveTargetLane = laneHit && laneHit.valid ? laneHit : null
            }
            _lastY = y
            _lastX = x
            return
        }
        const moved = Math.abs(y - _pressY) + Math.abs(x - _pressX)
        if (moved > 4) _dragged = true
        if (_boxSelect) {
            selRect.x = Math.min(_pressX, x)
            selRect.y = Math.min(_pressY, y)
            selRect.width = Math.abs(x - _pressX)
            selRect.height = Math.abs(y - _pressY)
            return
        }
        if (_dragged) {
            view.scrollY -= (y - _lastY)
            view.scrollX -= (x - _lastX)
        }
        _lastY = y
        _lastX = x
    }
    function handleRelease(x, y) {
        _lastY = -1
        if (_moving) {
            _moving = false
            const moved = Math.abs(y - _pressY) + Math.abs(x - _pressX)
            let deltaF = 0
            let targetLane = null
            if (_dragged && moved > 4) {
                const dy = y - _pressY
                const dx = x - _pressX
                // 「平移」= 轴锁定（方向主轴）；未勾 = 自由 2D（时间+通道都动）
                if (root.moveMode) {
                    // 轴锁定：主导轴决定改什么
                    if (Math.abs(dx) > Math.abs(dy)) {
                        // 横向：改通道，时间不变
                        const laneHit = view.laneAtX(x)
                        targetLane = laneHit && laneHit.valid ? laneHit : null
                        deltaF = 0
                    } else {
                        // 纵向：改时间，通道不变
                        deltaF = _moveDeltaF
                        targetLane = null
                    }
                } else {
                    // 自由 2D：时间 + 通道都动
                    deltaF = _moveDeltaF
                    const laneHit = view.laneAtX(x)
                    targetLane = laneHit && laneHit.valid ? laneHit : null
                }
            }
            if (Math.abs(deltaF) > 0.001 || targetLane) {
                // 应用前 snap 时间（拍位吸附到 snapNum/snapDen；在 Main 侧再做）
                root.moveSelectionRequested(deltaF, targetLane)
            }
            _dragged = false
            return
        }
        if (_boxSelect) {
            selRect.visible = false
            const draggedSel = _dragged && selRect.width > 3 && selRect.height > 3
            _boxSelect = false
            if (draggedSel) {
                root.selectionFinished(view.notesInRect(selRect.x, selRect.y,
                                                        selRect.x + selRect.width,
                                                        selRect.y + selRect.height))
                return
            }
            // 点击（未拖动）：点选 note（Ctrl = 多选切换）/ 空白清空
            if (y > 18) {
                const hit = view.noteAt(x, y)
                if (hit.valid) root.noteClicked(hit, _ctrlHeld)
                else root.canvasClicked()
            }
            return
        }
        const moved = Math.abs(y - _pressY) + Math.abs(x - _pressX)
        if (_dragged || moved > 4) return
        // 点击：放置工具（note / ln / mine）→ hitTest → hitPlaceRequested（Main 按工具定 kind）。
        // M3 note.put 已接 kind（normal/ln/mine）→ 三种工具都真实放置，不再提示 toolNotReady。
        if ((root.editorTool === "note" || root.editorTool === "ln" ||
                root.editorTool === "mine") && y > 18) {
            const hit = view.hitTest(x, y)
            if (hit.valid) root.hitPlaceRequested(hit)
        }
    }
    /// 调试入口（--click，与真实事件同一分发路径）：点击后立即释放。
    function clickAt(x, y) {
        handlePress(x, y, false, false)
        handleRelease(x, y)
    }

    /// 缩放重置（工具条「缩放」按钮）：恢复默认小节高度。
    function resetZoom() {
        root.measureHeight = 96
    }

    MouseArea {
        anchors.fill: parent
        acceptedButtons: Qt.LeftButton | Qt.RightButton | Qt.MiddleButton
        // select = 箭头（框选/点选）；pan = 手（拖动滚动）；note = 十字（点击放置）
        cursorShape: root.editorTool === "note" ? Qt.CrossCursor
                     : root.editorTool === "pan" ? Qt.OpenHandCursor
                     : Qt.ArrowCursor

        onPressed: (mouse) => {
            if (mouse.button === Qt.RightButton) {
                // 右键命中 note → 直接删除（BMS 编辑器惯例；select/pan/note 工具下可用）
                if (root.editorTool === "select" || root.editorTool === "pan" ||
                        root.editorTool === "note") {
                    const hit = view.noteAt(mouse.x, mouse.y)
                    if (hit.valid) root.noteRightDeleted(hit)
                }
                return  // 右键不进入拖拽/框选/放置
            }
            if (mouse.button === Qt.MiddleButton) {
                // 中键拖动 = 滚动（任何工具下通用导航）
                root._panning = true
                root._lastY = mouse.y
                root._lastX = mouse.x
                root._dragged = false
                return
            }
            root.handlePress(mouse.x, mouse.y,
                             (mouse.modifiers & Qt.ShiftModifier) !== 0,
                             (mouse.modifiers & Qt.ControlModifier) !== 0)
        }
        onPositionChanged: (mouse) => {
            if (root._panning) {
                const moved = Math.abs(mouse.y - root._pressY) + Math.abs(mouse.x - root._pressX)
                if (moved > 4) root._dragged = true
                if (root._dragged) {
                    view.scrollY -= (mouse.y - root._lastY)
                    view.scrollX -= (mouse.x - root._lastX)
                }
                root._lastY = mouse.y
                root._lastX = mouse.x
                return
            }
            root.handleMove(mouse.x, mouse.y)
        }
        onReleased: (mouse) => {
            if (root._panning) {
                root._panning = false
                root._lastY = -1
                return
            }
            if (mouse.button === Qt.RightButton) return
            root.handleRelease(mouse.x, mouse.y)
        }
    }

    // 框选矩形（select + Shift+拖；半透明主色 + 边框）
    Rectangle {
        id: selRect
        visible: false
        z: 3
        color: Theme.primarySoft
        border.color: Theme.accent
        border.width: 1
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

    // ---- 编辑接线辅助 ----
    /// 视口中心小节（粘贴 target_measure 用；topHigh 反向已处理）。
    function centerMeasure() {
        const h = view.height / 2
        return view.topHigh ? (view.contentHeight - (view.scrollY + h)) / view.measureHeight
                            : (view.scrollY + h) / view.measureHeight
    }

    // 状态栏用：鼠标位置 + note 信息（hoverText 由 ChartViewItem 计算）
    readonly property string hoverText: view.hoverText
}
