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
    property bool zoomToCursor: true     // 缩放锚点：true = 鼠标位置放大（推荐，默认开）；false = 视口中心
    property int snapNum: 1              // 吸附粒度分子（放置吸附 + 槽位弱线）
    property int snapDen: 16             // 吸附粒度分母
    property bool showChannelIds: false  // 列头显示实际 BMS 通道 id（工具条勾选）
    property bool bgmExpanded: false     // BGM 轨展开（列头点击切换；--bgm-expand 调试）
    property int noteSampleMode: 0       // note 采样标签：0 隐藏 / 1 id / 2 文件名
    property bool lnSelectMode: false    // LN 选取模式（默认关）：点 LN 任一段自动选配对两端
    property bool showExtras: false      // 更多轨道（BGA 图层通道列，游玩轨与背景轨之间）
    property bool showGrid: true         // 槽位弱线显示开关（「网格」按钮；吸附不依赖此开关）
    /// 当前缩放（相对默认 96px 小节高度；工具条显示用）
    readonly property int zoomPercent: Math.round(root.measureHeight / 96 * 100)
    // ---- 编辑接线（M3） ----
    property string editorTool: "select" // select/note/ln/mine/pan（Main 会话状态）
    property bool moveMode: false          // 平移开关（默认关，2026-09 用户确认：
                                           // 「默认关闭 note 平移限制」→ 自由 2D 拖动，
                                           // 时间+通道可同时动；勾选后按轴锁定）
    property int sampleId: -1            // 放置用采样数值 id（chartSession.sampleValueOf）
    property string sampleText: ""       // 当前采样展示（无放置采样时提示）
    property var selection: []           // 选中 note 集合（NoteRef；回填 view.selection 高亮）
    property var metaSelection: []       // 选中 BGA/BPM/STOP 对象集合（回填 view.metaSelection 高亮）
    property bool perfLog: false         // paint 帧耗时采样（--perf-log）
    property bool _ctrlHeld: false       // 本次按下是否 Ctrl（多选切换）
    /// M5.2 播放头跟随（默认开——用户拍板；用户滚动自动置 false；工具条 checkbox 经
    /// EditPage.followPlayhead 绑定此属性）
    property bool followPlayhead: true
    /// M5.2 视口光标（红线，固定底部10%）读数：秒 + 拍位文本（m·n/d）。转发 view，
    /// 状态栏用它（红线=视口光标，滚动内容滚过红线→值随视口变）（2026-09 用户）。
    readonly property real cursorSec: view.cursorSec
    readonly property string cursorPosText: view.cursorPosText
    /// M4.3c：波形条首次渲染后置 true（此后恒真）——内容变化隐藏波形条（stale）时
    /// **保留**右侧预留宽度，视口宽度不变 → 列不重排（用户 2026-09 移动 note 会跳位）。
    property bool waveformShown: false

    signal hitPlaceRequested(var hit)       // note 工具点击 → Main 走 note.put
    signal selectionFinished(var refs)      // 框选完成 → Main 存 selection + 复制
    signal noteClicked(var ref, bool ctrl)  // select 点击命中 note（选中；ctrl = 多选切换）
    signal playNoteSample(var ref)          // 点击 note（按下→释放无拖动）→ 播放该采样（M4.3 前端）
    signal canvasClicked()                  // select 点击空白（清空选中）
    signal noteRightDeleted(var ref)        // 右键命中 note（删除）
    signal noteEditRequested(var ref)       // 双击命中 note（切音手工版：改引用采样 id）
    signal editAreaPressed()                // 编辑区任意按下 → Main 释放文本框焦点（2026-09）
    // 平移：deltaF = 时间轴位移（拍位小数，0=不动）；targetLane = 横向移动目标列
    // （laneAtX 结果 {valid,lanePlayer,laneKind,laneIndex}；null=纯时间移动）；
    // sourceLane = 拖起的 note 所在轨（{player,kind,index}；跨通道多选只移此轨 note，2026-09）
    signal moveSelectionRequested(real deltaF, var targetLane, var sourceLane)
    // BGA/BPM/STOP 对象（2026-09）：点选（选中 + 可移动）
    signal metaObjectClicked(var obj, bool ctrl)
    // BGA/BPM/STOP 移动：kind + 对象 + 时间位移 + 横向目标列（bga 跨图层）
    signal metaMoveRequested(string kind, var obj, real deltaF, var targetLane)
    // BGA/BPM/STOP 右键删除
    signal metaRightDeleted(var obj)
    // BGA/BPM/STOP 双击编辑
    signal metaEditRequested(var obj)

    onBgmExpandedChanged: view.bgmExpanded = bgmExpanded

    ChartViewItem {
        id: view
        anchors.fill: parent
        anchors.bottomMargin: 12   // 底部水平滚动条区域
        anchors.rightMargin: (waveformShown ? (waveform.width + 12) : 12)  // 右侧波形条 + 垂直滚动条区域
        // ⚠️ 用 waveformShown（首次渲染后恒真）而非 waveform.visible：内容变化隐藏波形条
        // （stale）时保持视口宽度不变 → 列不再重排（用户 2026-09：移动 note 后所有 note 跳位）。
        session: typeof chartSession !== "undefined" ? chartSession : null
        theme: Theme
        measureHeight: root.measureHeight
        topHigh: root.topHigh
        snapNum: root.snapNum
        snapDen: root.snapDen
        noteSampleMode: root.noteSampleMode
        lnSelectMode: root.lnSelectMode
        showExtras: root.showExtras
        showGrid: root.showGrid
        selection: root.selection
        metaSelection: root.metaSelection
        perfLog: root.perfLog
        // M5.2 播放头跟随（单向：root.followPlayhead → view；用户滚动改 root
        // （EditPage.followPlayhead 绑定 root → 工具条 checkbox 自动更新）
        // ⚠️ 不写 onFollowPlayheadChanged 回写——双向回写 QML 会断绑定（2026-09 实测）
        followPlayhead: root.followPlayhead
        // Ctrl 按住临时切换（C++ KeyMonitor 应用级事件过滤；QML Keys 收不到独立修饰键）
        showChannelIds: root.showChannelIds !== keyMonitor.ctrlHeld
        // M6 编辑预览：放置工具 → ghost note 类型（note/ln/mine）；其它工具 = 关
        previewNoteKind: root.editorTool === "note" ? "normal"
                       : root.editorTool === "ln" ? "ln"
                       : root.editorTool === "mine" ? "mine" : ""
        onChartChanged: {
            // 谱面切换 → 定位到起点（小节 0 落在播放线/视口 90%——开头留白兜底，
            // 避免「0-1 小节播放头锁不住 90% → 开始播放跳变」；2026-09 用户）
            view.scrollToStart()
        }
    }

    // ---- M5.2 播放头叠层（性能拆层 2026-09）：红线 + A/B 标记只重绘本层（<1ms），
    //     播放时钟 20Hz 更新不触发 ChartViewItem 全量重绘（30fps → 60fps+） ----
    PlayheadOverlayItem {
        id: playheadOverlay
        anchors.fill: view
        session: typeof chartSession !== "undefined" ? chartSession : null
        measureHeight: view.measureHeight
        scrollY: view.scrollY
        contentHeight: view.contentHeight
        topHigh: view.topHigh
        rulerWidth: view.rulerWidth
        leadMeasures: view.leadMeasures
        loopASec: -1
        loopBSec: -1
        z: 10
    }

    // ---- M4.3c 波形总览条（2026-09 用户：右侧垂直条；Space 渲染后显示；点击/拖动跳转） ----
    WaveformOverviewItem {
        id: waveform
        anchors.top: parent.top
        anchors.bottom: parent.bottom
        anchors.right: parent.right
        anchors.topMargin: 0
        anchors.bottomMargin: 12   // 底部水平滚动条区域留白
        orientation: 1             // 垂直（右侧条；时间轴方向与视口一致 topHigh 感知）
        visible: false              // 有波形时显示（renderFinished 后置 true）
        width: 60
        session: typeof chartSession !== "undefined" ? chartSession : null
        theme: Theme
        // 视口状态绑定（换算可见窗口指示）
        measureHeight: root.measureHeight
        scrollY: view.scrollY
        contentHeight: view.contentHeight
        topHigh: root.topHigh
        viewportHeight: view.height
        onSeekRequested: (sec) => root.seekTo(sec)
    }

    // 渲染完成 → 显示波形总览；文档/内容变化 → 隐藏（等下一次渲染）
    // ⚠️ onRenderFinished 置 waveformShown=true（此后恒真）：内容变化隐藏波形条时视口宽度
    // 不变（仅波形内容变 blank）→ 列不重排。onDocumentChanged 重置 false（新谱面未渲染）。
    Connections {
        target: typeof chartSession !== "undefined" ? chartSession : null
        function onRenderFinished(ok, outPath, durationSec) {
            waveformShown = true
            waveform.visible = ok
        }
    }
    Connections {
        target: typeof chartSession !== "undefined" ? chartSession : null
        function onDocumentChanged() {
            waveform.visible = false
            waveformShown = false
            view.playheadSec = -1  // 谱面切换：隐藏播放头（等新渲染/播放）
        }
        function onContentChanged() {
            waveform.visible = false
            // 编辑即停（AudioEngine 已停）；播放头保留当前时间点（暂停态位置）
        }
    }

    // ---- M5.2 播放头：当前时间点红线 + 视口跟随 ----
    // 时间源 = AudioEngine 播放时钟（20Hz 刷新；暂停/停止也显示当前位置）。
    // 跟随：播放头出安全区 → 视口滚动（自动）；用户滚动 → 关闭（见 wheel/滚动条）。
    Connections {
        target: typeof audioEngine !== "undefined" ? audioEngine : null
        function onPlaybackChanged() {
            // 红线 = 视口光标（固定，见 PlayheadOverlayItem）；这里只：
            // ① view.playheadSec = 播放时钟（follow tick 让红线下方 = 时钟）
            // ② 滚动内容让红线下方内容 = 播放时钟（播放中）
            // ③ A/B 标记（内容锚定，随滚动）
            // ④ 红线时间读数 → 状态栏
            if (typeof audioEngine !== "undefined" && audioEngine.hasPcm) {
                view.playheadSec = audioEngine.positionSec
                playheadOverlay.loopASec = audioEngine.loopA
                playheadOverlay.loopBSec = audioEngine.loopB
                if (view.followPlayhead && audioEngine.playing)
                    view.followPlayheadTick()
            }
        }
    }

    /// 诊断探针（--probe）：转发到 ChartViewItem.probe(x,y)，返回命中/列布局诊断。
    function probe(x, y) {
        return view.probe(x, y)
    }

    /// 运行时换肤（doc/08 §3.3）：Theme.tokensChanged 后强制重绘视口（转发 ChartViewItem.refreshTheme）。
    function refreshTheme() {
        view.refreshTheme()
    }

    /// 以屏幕 y 为锚点缩放（--zoom-at / 滚轮缩放锚点；转发 ChartViewItem.zoomAt）。
    /// ⚠️ zoomAt 直接改 C++ measureHeight（锚点滚动计算），完成后回写 root.measureHeight
    /// 保持工具条「缩放 %」显示同步（root→view 单向绑定，回写同值不会二次缩放）。
    function zoomAt(y, factor) {
        view.zoomAt(y, factor)
        root.measureHeight = view.measureHeight
    }

    // ---- 缩放级别（等比，无漂移）----
    // 滚轮缩放改成**离散级别跳步**（索引移动），不再连续乘除——否则从上限 480px 连续 ÷1.2
    // 无法整除回 96px（会落到 107%/97% 这类非 100% 值；2026-09 用户实测反馈）。
    // 级别 = 以 96px(100%) 为基准向两端等比(ratio=1.2)延伸，上下限钳到 24px(25%)/480px(500%)；
    // 96px 恒为精确级别（25%/500% 滚回即达 100%）。
    property var _zoomLevels: buildZoomLevels(1.2)
    function buildZoomLevels(ratio) {
        const up = []
        let v = 96
        while (true) { v = v * ratio; if (v > 480) break; up.push(v) }
        const down = []
        v = 96
        while (true) { v = v / ratio; if (v < 24) break; down.push(v) }
        const levels = down.slice().reverse()   // 下半段升序（>24）
        levels.push(96)
        for (let i = 0; i < up.length; i++) levels.push(up[i])  // 上半段升序
        if (levels[0] > 24) levels.unshift(24)                  // 钳下端点（25%）
        if (levels[levels.length - 1] < 480) levels.push(480)   // 钳上端点（500%）
        return levels
    }
    function nearestIndex(arr, val) {
        let bi = 0, bd = Infinity
        for (let i = 0; i < arr.length; i++) {
            const d = Math.abs(arr[i] - val)
            if (d < bd) { bd = d; bi = i }
        }
        return bi
    }
    /// 滚轮缩放一步：从当前 measureHeight 所在级别向上/下跳一级。光标锚点分支用
    /// zoomAt(cursor, factor=相邻级别比) 保持鼠标处拍位不动；中心锚点分支直接设新级别。
    function zoomStep(y, up) {
        const lv = root._zoomLevels
        const idx = nearestIndex(lv, root.measureHeight)
        const nIdx = up ? Math.min(lv.length - 1, idx + 1) : Math.max(0, idx - 1)
        if (nIdx === idx) return
        if (root.zoomToCursor) root.zoomAt(y, lv[nIdx] / lv[idx])
        else root.measureHeight = lv[nIdx]
    }

    WheelHandler {
        acceptedDevices: PointerDevice.Mouse | PointerDevice.TouchPad
        onWheel: (event) => {
            if (event.modifiers & Qt.ControlModifier) {
                // 缩放：指小节高度（内容比例不变，仅视口密度）——**离散级别跳步**（等比，无漂移，
                // 100% 恒可达；2026-09 用户：旧实现连乘除碰顶非整除 → 滚回只能到 107%/97%）。
                // 锚点 = 鼠标 y（zoomToCursor 开，默认）/ 视口中心（关）。
                const dy = event.angleDelta.y
                if (dy !== 0) root.zoomStep(event.y, dy > 0)
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
            root.userScroll()  // 用户滚动 → 关跟随
            event.accepted = true
        }
    }

    /// 用户主动滚动：**不阻断**（跟随只在播放中锁定——用户 2026-09「只要在播放期间锁定即可」；
    /// 暂停/停止可自由滚动浏览）。此函数保留为空壳（原自动关跟随逻辑废弃——硬锁定优先）。
    function userScroll() {
        // no-op：跟随开启时仅播放中锁定（见 onPlaybackChanged 的 playing 判定）
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
    property string _moveKind: ""   // 移动的对象类型：""(note) / bga / bpm / stop
    property var _moveObj: null     // 移动的 bga/bpm/stop 对象（objectAt 返回；note 时为 null）
    property real _moveStartF: 0    // 按下的拍位（measure + pos 小数）
    property real _moveDeltaF: 0    // 当前时间位移（拍位小数）
    property var _moveTargetLane: null  // 横向目标列（laneAtX 结果；null = 时间只动）
    property var _moveSourceLane: null  // 拖起的 note 所在轨（{player,kind,index}；跨通道多换轨判定用）
    property var _pressedNoteRef: null  // press 命中的 note（有值 = 点击待确认；release 无拖译才播放）

    /// 平移判定：按下点在选中集内的某个 note 上？
    function isSelectedNote(hit) {
        if (!hit || !hit.valid) return false
        for (var i = 0; i < root.selection.length; i++) {
            const s = root.selection[i]
            if (s.measure === hit.measure && s.sample === hit.sample &&
                    s.lane.kind === hit.lane.kind &&
                    s.lane.index === hit.lane.index &&
                    s.lane.player === hit.lane.player &&
                    s.pos.num === hit.pos.num && s.pos.den === hit.pos.den &&
                    (s.bgm_line === undefined || s.bgm_line === hit.bgm_line))
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
        // M5 seek 交互：点秒标尺列（x<rulerWidth、y>头部）→ 设播放位置（seek + 滚到红线）。
        // 与 note/框选/放置解耦；只响应单点（拖动不触发，保持标尺纯跳转语义）。
        if (x < view.rulerWidth && y > 18) {
            root.seekTo(view.timeAtY(y))
            return
        }
        // 编辑工具（V 选择 / N 放置 / L LN / M 地雷）命中 note → 选中并进入移动准备。
        // 拖动 note = 移动（任何编辑工具，设计确认 2026-09）；点击（无位移）→ release 时
        // 走工具语义（放置或选中）。pan（H 拖拽=纯滚动）永不进入移动。
        const isEditTool = root.editorTool === "select" || root.editorTool === "note" ||
                           root.editorTool === "ln" || root.editorTool === "mine"
        if (isEditTool) {
            const obj = view.objectAt(x, y)
            if (obj.valid) {
                if (obj.kind === "bga" || obj.kind === "bpm" || obj.kind === "stop") {
                    // BGA/BPM/STOP 对象：选中 + 进入移动准备（与 note 平行；值/id 由命令层保持）
                    root.metaObjectClicked(obj, ctrl)
                    _moving = true
                    _moveKind = obj.kind
                    _moveObj = obj
                    _moveDeltaF = 0
                    _moveTargetLane = null
                    _moveSourceLane = null
                    _moveStartF = view.measureAtY(y)
                    return
                }
                // —— note 路径（原逻辑） ——
                if (ctrl) {
                    // Ctrl+点击：多选切换（toggle），不进入移动——保持交互清晰（选中态预览）。
                    // ⚠️ 旧代码硬编码 noteClicked(hit, false) 丢失 Ctrl → 多选失效（问题1根因）。
                    root.noteClicked(obj, true)
                    return
                }
                // 点击（无论是否已选中）→ noteClicked（选中；播放延后到 release 无拖动
                // 时——拖动 = 移动操作不播，2026-09 用户「点击播放/移动不播」语义。
                // 重复点击（已选中）也播放（用户「重复点击播放」确认）。
                root.noteClicked(obj, false)
                _pressedNoteRef = obj
                _moving = true
                _moveKind = ""
                _moveObj = null
                _moveDeltaF = 0
                _moveTargetLane = null
                _moveSourceLane = obj.lane   // 拖起 note 的轨（跨通道多选移动只动此轨，2026-09）
                view.moveSourceLane = obj.lane  // M6 预览：拖起 lane → ghost 跨列判断用
                _moveStartF = view.measureAtY(y)
                return
            }
        }
        // select 工具空白 = 框选（拖动）；pan 工具 = 滚动（拖动）；note/ln/mine 空白 = 点击放置
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
                // M6 编辑预览：note 移动 → ghost（原始+delta；meta 对象沿用旧行为）
                if (_moveKind === "") {
                    view.movePreview = true
                    view.moveDeltaF = _moveDeltaF
                    // 目标列规范化为 {player,kind,index}（laneAtX 返回 lanePlayer/laneKind/laneIndex）；
                    // 带 bgm_line：BGM 虚拟子通道目标（ghost 落对应 bgmN 列）
                    view.moveTargetLane = laneHit && laneHit.valid
                        ? { player: laneHit.lanePlayer, kind: laneHit.laneKind,
                            index: laneHit.laneIndex,
                            bgm_line: laneHit.bgm_line }
                        : ({})
                }
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
            root.userScroll()  // 拖动滚动 → 关跟随
        }
        _lastY = y
        _lastX = x
    }
    function handleRelease(x, y) {
        _lastY = -1
        if (_moving) {
            _moving = false
            view.movePreview = false  // M6 编辑预览：结束拖拽 → 关 ghost
            view.moveTargetLane = ({})
            view.moveSourceLane = ({})
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
                if (root._moveKind !== "") {
                    root.metaMoveRequested(root._moveKind, root._moveObj, deltaF, targetLane)
                } else {
                    root.moveSelectionRequested(deltaF, targetLane, root._moveSourceLane)
                }
            }
            // 点击（按下→释放，无拖动）= 播放一次；拖动（移动 note）= 不播。
            if (!_dragged && _pressedNoteRef) {
                root.playNoteSample(_pressedNoteRef)
            }
            _pressedNoteRef = null
            root._moveKind = ""
            root._moveObj = null
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
            // 点击（未拖动）：点选 note / BGA/BPM/STOP 对象（Ctrl = 多选切换）/ 空白清空
            if (y > 18) {
                const obj = view.objectAt(x, y)
                if (obj.valid) {
                    if (obj.kind === "bga" || obj.kind === "bpm" || obj.kind === "stop")
                        root.metaObjectClicked(obj, _ctrlHeld)
                    else root.noteClicked(obj, _ctrlHeld)
                } else root.canvasClicked()
            }
            return
        }
        const moved = Math.abs(y - _pressY) + Math.abs(x - _pressX)
        if (_dragged || moved > 4) return
        // 点击：放置工具（note / ln / mine）→ hitTest → hitPlaceRequested（Main 按工具定 kind）。
        // M3 note.put 已接 kind（normal/ln/mine）→ 三种工具都真实放置。
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

    /// 调试入口（--drag x1 y1 x2 y2）：模拟按下→移动→释放（真实拖拽同一分发路径，
    /// 用于复现 BGM 子轨移动等交互问题；与 clickAt 同为一次事件序列）。
    function dragAt(x1, y1, x2, y2) {
        handlePress(x1, y1, false, false)
        handleMove(x2, y2)
        handleRelease(x2, y2)
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
            // 2026-09：编辑区任意按下 → Main 释放文本框焦点（否则快捷键被文本框吞掉）
            root.editAreaPressed()
            if (mouse.button === Qt.RightButton) {
                // 右键命中 note / BGA / BPM / STOP → 直接删除（BMS 编辑器惯例；select/pan/note 工具下可用）
                if (root.editorTool === "select" || root.editorTool === "pan" ||
                        root.editorTool === "note") {
                    const obj = view.objectAt(mouse.x, mouse.y)
                    if (obj.valid) {
                        if (obj.kind === "bga" || obj.kind === "bpm" || obj.kind === "stop")
                            root.metaRightDeleted(obj)
                        else root.noteRightDeleted(obj)
                    }
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
        onDoubleClicked: (mouse) => {
            // 双击命中 note / BGA / BPM / STOP（select 工具）→ 编辑（note 改采样；bga 改 BMP/图层；
            // bpm/stop 改值；Main 弹对话框）
            if (mouse.button === Qt.LeftButton && root.editorTool === "select" && mouse.y > 18) {
                const obj = view.objectAt(mouse.x, mouse.y)
                if (obj.valid) {
                    if (obj.kind === "bga" || obj.kind === "bpm" || obj.kind === "stop")
                        root.metaEditRequested(obj)
                    else root.noteEditRequested(obj)
                }
            }
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

    // 右侧垂直滚动条（内容高于视口时出现；点击/拖拽滑块滑动）。
    // 2026-09：波形条在右缘 → vbar 移到波形条左侧（波形可见时）。
    Rectangle {
        id: vbar
        anchors.top: parent.top
        anchors.bottom: parent.bottom
        anchors.right: waveformShown ? waveform.left : parent.right
        width: 12
        z: 2
        color: Theme.surface2
        visible: view.contentHeight > view.height + 1
        readonly property real maxY: Math.max(1, view.maxScrollY)
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
                root.userScroll()
            }
            onPositionChanged: (mouse) => {
                if (pressed) {
                    view.scrollY = (mouse.y - grabOffset) /
                                   Math.max(1, vbar.height - vthumb.height) * vbar.maxY
                    root.userScroll()
                }
            }
        }
    }

    // 底部水平滚动条（轨道列超宽时出现；点击/拖拽滑块滑动）。
    // 2026-09：波形条在右侧（垂直，bottomMargin 12 给 hbar 让位）→ hbar 全宽不受影响。
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
    /// 视口中心小节（粘贴 target_measure 用；topHigh 反向已处理；含开头留白偏差——经
    /// measureAtY 自动扣除，留白区夹到 0）。
    function centerMeasure() {
        return Math.max(0, view.measureAtY(view.height / 2))
    }

    /// 秒 → 视口滚动（波形总览 seekRequested / --seek 调试走同一路径）。
    function seekToSeconds(sec) {
        view.scrollToTime(sec)
    }

    /// M5 seek 交互：设播放位置 + 滚到红线（视口光标）。
    /// ① 音频时钟 seek（有 PCM 才生效，否则安全 no-op）→ 播放头红线跟进；
    /// ② 视口滚到「该时间落红线(90%)」→ 红线读数 = 目标时间（非播放时红线显示就位）。
    /// 点秒标尺 / 波形条点击 / 波形条拖动（DAW 式 scrub）共用此路径。
    function seekTo(sec) {
        if (typeof audioEngine !== "undefined" && audioEngine && audioEngine.hasPcm)
            audioEngine.seekSeconds(sec)
        if (view) view.scrollCursorToSec(sec)
    }

    // 状态栏用：鼠标位置 + note 信息（hoverText 由 ChartViewItem 计算）
    readonly property string hoverText: view.hoverText
}
