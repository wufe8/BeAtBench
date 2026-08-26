// SPDX-License-Identifier: GPL-3.0-only
// 会话控制器（SessionController，2026-09）：Main.qml 业务逻辑层抽离。
// 用途：Main.qml（ApplicationWindow 根）因持续追加编辑/选择/保存逻辑而膨胀到 ~1750 行，
//       已属 god-object。本控制器承载全部「会话编辑逻辑」（note/meta/timing/bga/bmp/
//       保存/撤销/剪贴板/量化变换），把 Main.qml 瘦身为纯 chrome 壳（窗口/菜单/工具条/
//       状态栏/对话框）+ 一个控制器实例 + 每个逻辑函数一行委托。
// 依赖注入：Main 实例化时设置 window（宿主 ApplicationWindow）与 editPage（EditPage 实例）。
//       逻辑函数内部照旧用 window.*（读会话状态）与 editPage.*（视图操作）；
//       chartSession/sampleModel/lintModel/beatbench 是全局 context property，可直接访问。
// 双语言纪律（doc/08 §2）：本组件是「业务逻辑 + 状态主机」，不含 UI 元素；UI 信号经 Main 委托进。
import QtQuick

QtObject {
    id: root

    /// 宿主窗口（Main.qml 的 ApplicationWindow；提供 window.* 会话状态）。
    property var window: null
    /// EditPage 实例（视图操作：reloadBga/collectMetaEdits/centerMeasure/...）。
    property var editPage: null

    /// 状态栏消息（转发到宿主窗口的 setStatus；逻辑函数内部大量裸调用 setStatus）。
    /// ⚠️ 不能在本组件定义真实 statusText/statusClearTimer——那是 Main 的 chrome 职责。
    function setStatus(msg) {
        if (root.window) root.window.setStatus(msg)
    }

    // ---------- 编辑命令封装（M3 协议：note.put/move/delete、session.*、clipboard.*） ----------
    // 统一 dispatch + 成功即 chartSession.refresh()（指纹判定文档/内容变化，视图自动刷新）。
    function sessionCmd(name, args) {
        var req = JSON.stringify({ command: name, args: args || {} })
        var resp = JSON.parse(beatbench.dispatch(req))
        if (!resp.ok) {
            setStatus(resp.error.code + ": " + resp.error.message)
            return null
        }
        chartSession.refresh()
        refreshLint()  // 编辑后刷新 lint 面板（内存 lint：LN 未配对等）
        return resp.result
    }
    /// 只 dispatch 不 refresh（批删除循环里用；调用方完成后统一 refresh 一次）。
    function dispatchCmd(name, args) {
        var req = JSON.stringify({ command: name, args: args || {} })
        var resp = JSON.parse(beatbench.dispatch(req))
        if (!resp.ok) {
            setStatus(resp.error.code + ": " + resp.error.message)
            return null
        }
        return resp.result
    }
    /// 内存 lint（session.lint）→ lintModel（编辑后「LN 通道 note 未组成完整 LN」等提示）。
    function refreshLint() {
        var req = JSON.stringify({ command: "session.lint", args: {} })
        var resp = beatbench.dispatch(req)
        lintModel.loadFromIssues(resp)
    }
    function deleteNoteAt(ref) {
        var args = {
            measure: ref.measure, pos: ref.pos, lane: ref.lane, sample: ref.sample
        }
        if (ref.bgm_line !== undefined) args.bgm_line = ref.bgm_line
        var r = sessionCmd("note.delete", args)
        if (r) setStatus(qsTr("已删除（可撤销）"))
    }
    function deleteSelection() {
        if (!window.selectionRefs || window.selectionRefs.length === 0) {
            setStatus(qsTr("没有选中（点击 note 选中 / Shift+框选）"))
            return
        }
        var refs = window.selectionRefs.slice()
        var done = 0
        for (var i = 0; i < refs.length; i++) {
            var args = {
                measure: refs[i].measure, pos: refs[i].pos,
                lane: refs[i].lane, sample: refs[i].sample
            }
            if (refs[i].bgm_line !== undefined) args.bgm_line = refs[i].bgm_line
            var r = dispatchCmd("note.delete", args)
            if (r) done++
        }
        if (done > 0) {
            chartSession.refresh()
            refreshLint()  // 2026-09 用户：删除后 lint 也要刷新（之前只刷新视图没刷新 lint）
            window.selectionRefs = []
            setStatus(qsTr("已删除 %1 个 note（Undo 可恢复）").arg(done))
        }
    }
    function placeNote(hit) {
        // 元事件轨（BPM/STOP，路线 A：工具栏值 + 点击列放置）→ timing.put；值取当前 BPM/STOP 值。
        if (hit.metaKind === "bpm" || hit.metaKind === "stop") {
            const tKind = hit.metaKind
            const value = tKind === "bpm" ? window.currentBpmValue : window.currentStopValue
            const tArgs = { kind: tKind, measure: hit.measure,
                            pos: { num: hit.num, den: hit.den }, value: value }
            const tr = sessionCmd("timing.put", tArgs)
            if (tr) {
                refreshTiming()
                setStatus(qsTr("放置 %1：小节 %2 · %3/%4 = %5")
                          .arg(tKind.toUpperCase()).arg(hit.measure).arg(hit.num).arg(hit.den).arg(value))
            }
            return
        }
        // BGA 图层列（更多轨道 base/poor/layer/layer2）：note 工具点击 = 放一个当前 #BMP 的
        // BGA 事件（bga.put；镜像 BPM/STOP 列放置）。需先选择 #BMP（BGA 面板行点击 / 默认首个）。
        if (hit.bgaLayer !== undefined && hit.bgaLayer >= 0) {
            placeBgaAt(hit)
            return
        }
        // LNTYPE 2（#LNOBJ）：LN 工具 = 头尾状态机（同轨点尾 / 异轨重放头 / Esc 取消）。
        if (window.editorTool === "ln" && chartSession.lnType() === 2) {
            placeLnType2(hit)
            return
        }
        // kind 语义（M3 note.put 已支持）：note→normal / ln→LN 自动配对 / mine→地雷。
        var kind = "normal"
        if (window.editorTool === "ln") kind = "ln"
        else if (window.editorTool === "mine") kind = "mine"
        // 2026-09 用户：LN 只能放在「游玩轨/LN 轨」——BMS 中 BGM（ch01）无 LN 通道表示
        //（映射层 bms_channel_for(Bgm,ln) 为空，写出会丢/降级）。前端先行阻止。
        if (kind === "ln" && hit.laneKind === "bgm") {
            setStatus(qsTr("BGM 轨不能放置 LN（格式无 LN 通道表示）；请用「3 放置」放普通背景音"))
            return
        }
        // BGM 展开列带 sampleHint（该列固定 #WAV id）→ 直接用；否则取当前采样
        if (hit.sampleHint !== undefined && hit.sampleHint >= 0) {
            var putArgs = {
                measure: hit.measure,
                pos: { num: hit.num, den: hit.den },
                lane: { player: hit.lanePlayer, kind: hit.laneKind, index: hit.laneIndex },
                sample: hit.sampleHint,
                kind: kind
            }
            if (hit.bgm_line !== undefined && hit.bgm_line >= 0)
                putArgs.bgm_line = hit.bgm_line
            var r0 = sessionCmd("note.put", putArgs)
            if (r0)
                setStatus(kind === "ln"
                          ? qsTr("放置 LN #WAV%1（BGM 列）· 小节 %2").arg(hit.sampleHint).arg(hit.measure)
                          : kind === "mine"
                                ? qsTr("放置地雷 #WAV%1（BGM 列）· 小节 %2").arg(hit.sampleHint).arg(hit.measure)
                                : qsTr("放置 #WAV%1（BGM 列）· 小节 %2").arg(hit.sampleHint).arg(hit.measure))
            return
        }
        if (window.currentSampleId === "") {
            setStatus(qsTr("先选择采样：左 Dock「采样」面板点击 #WAVxx"))
            return
        }
        var v = chartSession.sampleValueOf(window.currentSampleId)
        if (v < 0) {
            setStatus(qsTr("当前采样 #WAV%1 不在定义表中").arg(window.currentSampleId))
            return
        }
        // ⚠️ 问题1（2026-09）：BGM 展开列（bgmLine>=0，无固定 sampleHint）放置必须传
        // bgm_line——否则 note.put 默认 bgm_line=0 落到 bgm1（虚拟子通道行号丢失）。
        var putArgs2 = {
            measure: hit.measure,
            pos: { num: hit.num, den: hit.den },
            lane: { player: hit.lanePlayer, kind: hit.laneKind, index: hit.laneIndex },
            sample: v,
            kind: kind
        }
        if (hit.bgm_line !== undefined && hit.bgm_line >= 0)
            putArgs2.bgm_line = hit.bgm_line
        var r = sessionCmd("note.put", putArgs2)
        if (r) {
            var st = kind === "ln"
                     ? qsTr("放置 LN #WAV%1 · 小节 %2 · %3/%4")
                     : kind === "mine"
                           ? qsTr("放置地雷 #WAV%1 · 小节 %2 · %3/%4")
                           : qsTr("放置 #WAV%1 · 小节 %2 · %3/%4")
            setStatus(st.arg(window.currentSampleId).arg(hit.measure).arg(hit.num).arg(hit.den))
        }
    }
    /// LNTYPE 2（#LNOBJ）LN 放置：头尾状态机（2026-09 用户）。
    /// 无待定头 → 放头（当前采样普通 note）；已在当前轨 → 放尾（#LNOBJ 采样 note，rebuild 自动
    /// 配对）；异轨 → 重放头（note.move 把头 note 移到新轨/新位置，一个 undo 步）；Esc → 删除头。
    function placeLnType2(hit) {
        if (hit.laneKind === "bgm") {
            setStatus(qsTr("BGM 轨不能放置 LN（格式无 LN 通道表示）；请用「3 放置」放普通背景音"))
            return
        }
        const lane = { player: hit.lanePlayer, kind: hit.laneKind, index: hit.laneIndex }
        const pos = { num: hit.num, den: hit.den }
        // —— 无待定头 → 放置头（普通 note，当前采样）——
        if (!window.pendingLnHead) {
            if (window.currentSampleId === "") {
                setStatus(qsTr("先选择采样：左 Dock「采样」面板点击 #WAVxx"))
                return
            }
            const v = chartSession.sampleValueOf(window.currentSampleId)
            if (v < 0) {
                setStatus(qsTr("当前采样 #WAV%1 不在定义表中").arg(window.currentSampleId))
                return
            }
            const r = sessionCmd("note.put", { measure: hit.measure, pos: pos, lane: lane,
                                               sample: v, kind: "normal" })
            if (r) {
                window.pendingLnHead = { measure: hit.measure, pos: pos, lane: lane, sample: v }
                setStatus(qsTr("LN 头 #WAV%1 · 小节 %2 · %3/%4（同轨点尾 / 异轨重放头 / Esc 取消）")
                          .arg(window.currentSampleId).arg(hit.measure).arg(hit.num).arg(hit.den))
            }
            return
        }
        const h = window.pendingLnHead
        const sameLane = h.lane.player === lane.player && h.lane.kind === lane.kind &&
                         h.lane.index === lane.index
        // —— 待定头在当前轨 → 放尾（#LNOBJ 采样 note）——
        if (sameLane) {
            const lnojb = chartSession.lnobjSample()
            if (lnojb < 0) {
                setStatus(qsTr("未定义 #LNOBJ（元信息面板设 #LNTYPE=2 并填 #LNOBJ，缺省 ZZ）"))
                return
            }
            const headF = h.measure + h.pos.num / h.pos.den
            const tailF = hit.measure + hit.num / hit.den
            if (tailF <= headF) { setStatus(qsTr("LN 尾须在头之后（当前点早于头）")); return }
            const r = sessionCmd("note.put", { measure: hit.measure, pos: pos, lane: lane,
                                               sample: lnojb, kind: "normal" })
            if (r) {
                window.pendingLnHead = null
                setStatus(qsTr("LN 完成（#LNOBJ 尾 · 小节 %1 · %2/%3）")
                          .arg(hit.measure).arg(hit.num).arg(hit.den))
            }
            return
        }
        // —— 异轨 → 重放头：note.move 把头 note 移到新轨/新位置（保持采样；一个 undo 步）——
        const to = { measure: hit.measure, pos: pos, lane: lane }
        const r = sessionCmd("note.move", { moves: [{ from: h, to: to }] })
        if (r) {
            window.pendingLnHead = { measure: hit.measure, pos: pos, lane: lane, sample: h.sample }
            setStatus(qsTr("LN 头移到新轨 · 小节 %1 · %2/%3（同轨点尾）")
                      .arg(hit.measure).arg(hit.num).arg(hit.den))
        }
    }
    /// Esc / 切走工具：取消未完成的 LN 头（删除已放的头 note）。
    function cancelPendingLn() {
        if (!window.pendingLnHead) { setStatus(qsTr("没有待完成的 LN 头")); return }
        const h = window.pendingLnHead
        const r = sessionCmd("note.delete", { measure: h.measure, pos: h.pos, lane: h.lane,
                                              sample: h.sample })
        window.pendingLnHead = null
        if (r) setStatus(qsTr("已取消 LN 放置（删除头）"))
    }
    function onSelectionMade(refs) {
        window.selectionRefs = refs
        setStatus(qsTr("已选中 %1 个 note（Ctrl+C 复制）").arg(refs.length))
    }
    function refEquals(a, b) {
        return a && b && a.measure === b.measure && a.sample === b.sample &&
               a.lane.kind === b.lane.kind && a.lane.index === b.lane.index &&
               a.lane.player === b.lane.player &&
               a.pos.num === b.pos.num && a.pos.den === b.pos.den &&
               (a.bgm_line === undefined || b.bgm_line === undefined || a.bgm_line === b.bgm_line)
    }
    function onNoteClicked(ref, ctrl) {
        // LN 选取模式（默认关）：点 LN 任一段 → 自动纳入配对段。ref 由 noteAt 返回，
        // 命中 LN 时带 lnPartner（配对段的 NoteRef）。选中集可整体移动/删除。
        if (window.lnSelectMode && ref.lnPartner) {
            var picked = [ref.lnPartner, ref]
            if (!ctrl) { window.selectionRefs = picked }
            else {
                // Ctrl+点击：把两端整体加入（若已含则移除）——简单化：追加未含的段
                var arr = window.selectionRefs.slice()
                for (var i = 0; i < picked.length; i++) {
                    var exists = false
                    for (var j = 0; j < arr.length; j++)
                        if (refEquals(arr[j], picked[i])) { exists = true; break }
                    if (!exists) arr.push(picked[i])
                }
                window.selectionRefs = arr
            }
            setStatus(qsTr("已选中 LN（%1 段）").arg(window.selectionRefs.length))
            return
        }
        if (ctrl) {
            // 多选切换（Ctrl+点击，文件管理器逻辑）：已选中 → 移除；否则追加
            var arr = window.selectionRefs.slice()
            var idx = -1
            for (var i = 0; i < arr.length; i++)
                if (refEquals(arr[i], ref)) { idx = i; break }
            if (idx >= 0) arr.splice(idx, 1)
            else arr.push(ref)
            window.selectionRefs = arr
            setStatus(qsTr("已选中 %1 个 note").arg(arr.length))
            return
        }
        window.selectionRefs = [ref]
        setStatus(qsTr("选中 #WAV%1（Del 删除 / 右键删除 / 拖拽平移）").arg(ref.sample))
    }
    function onCanvasClicked() {
        if (window.selectionRefs.length > 0) window.selectionRefs = []
        if (window.metaSelection.length > 0) window.metaSelection = []
    }
    // ---------- BGA / BPM / STOP 对象（视口交互，2026-09） ----------

    /// meta 对象键（kind|measure|num|den|layer/sample）：选中判定用。
    function metaKey(o) {
        return (o.kind + "|" + o.measure + "|" + o.pos.num + "/" + o.pos.den +
                "|" + (o.layer !== undefined ? o.layer : "_") + "|" +
                (o.sample !== undefined ? o.sample : "_"))
    }
    /// BGA/BPM/STOP 点选（click/拖起）。ctrl = 多选切换；否则替换（并清空 note 选中）。
    function onMetaClicked(obj, ctrl) {
        if (ctrl) {
            let arr = window.metaSelection.slice()
            const key = metaKey(obj)
            const idx = arr.findIndex(function (x) { return metaKey(x) === key })
            if (idx >= 0) arr.splice(idx, 1)
            else arr.push(obj)
            window.metaSelection = arr
            setStatus(qsTr("已选中 %1 个对象").arg(arr.length))
            return
        }
        window.selectionRefs = []
        window.metaSelection = [obj]
        setStatus(qsTr("选中 %1 · 小节 %2·%3/%4（拖拽移动 / 双击编辑 / 右键·Del 删除）")
                  .arg(obj.kind.toUpperCase()).arg(obj.measure).arg(obj.pos.num).arg(obj.pos.den))
    }
    /// 删除单个 meta 对象（右键 / 删除选中）。
    function deleteMetaObject(obj) {
        let r = null
        if (obj.kind === "bga")
            r = sessionCmd("bga.delete", { layer: obj.layer, measure: obj.measure, pos: obj.pos })
        else if (obj.kind === "bpm" || obj.kind === "stop")
            r = sessionCmd("timing.delete", { kind: obj.kind, measure: obj.measure, pos: obj.pos })
        if (r) {
            refreshTiming()
            if (typeof editPage !== "undefined" && editPage) editPage.reloadBga()
            window.metaSelection = window.metaSelection.filter(function (x) {
                return metaKey(x) !== metaKey(obj)
            })
            setStatus(qsTr("已删除 %1 对象（可撤销）").arg(obj.kind.toUpperCase()))
        }
    }
    /// 删除选中的全部 meta 对象。
    function deleteMetaSelection() {
        if (window.metaSelection.length === 0) { setStatus(qsTr("没有选中的 BGA/BPM/STOP 对象")); return }
        const arr = window.metaSelection.slice()
        let done = 0
        for (let i = 0; i < arr.length; i++) {
            if (deleteMetaObject(arr[i])) done++
            else window.metaSelection = window.metaSelection.filter(function (x) { return metaKey(x) !== metaKey(arr[i]) })
        }
        if (done > 0) setStatus(qsTr("已删除 %1 个对象（Undo 可恢复）").arg(done))
    }
    /// 位移增量 → 目标 (measure, pos)（带小节进位；分数约分）。
    function metaTargetPos(measure, p, delta) {
        const rn = p.num, rd = p.den, dn = delta.pos.num, dd = delta.pos.den
        const newNum = rn * dd + dn * rd, newDen = rd * dd
        const carry = Math.floor(newNum / newDen)
        const g = gcd(newNum - carry * newDen, newDen)
        return { measure: measure + delta.measure + carry,
                 pos: { num: (newNum - carry * newDen) / g, den: newDen / g } }
    }
    /// 拖动移动 BGA/BPM/STOP 对象（时间 + 图层[仅 bga]；单 undo 步）。deltaF = 拍位位移。
    /// 拖回游玩轨（key/scratch/pedal）→ note.convertBack（反转换：元事件 → note）。
    function moveMetaObject(kind, obj, deltaF, targetLane) {
        const num = window.snapNum, den = window.snapDen
        const slots = Math.max(1, Math.floor(den / num))
        const snappedF = Math.round(deltaF * slots) / slots
        const m = Math.floor(snappedF)
        const delta = { measure: m, pos: { num: Math.round((snappedF - m) * den), den: den } }
        const target = metaTargetPos(obj.measure, obj.pos, delta)
        // 游玩轨目标（key/scratch/pedal；非元事件列 / 非 BGA 列）
        const playLane = targetLane && targetLane.valid &&
                         (targetLane.laneKind === "key" || targetLane.laneKind === "scratch" ||
                          targetLane.laneKind === "pedal") &&
                         targetLane.metaKind === undefined &&
                         (targetLane.bgaLayer === undefined || targetLane.bgaLayer < 0)
        if (playLane) {
            const r = sessionCmd("note.convertBack", {
                kind: kind,
                source: { layer: kind === "bga" ? obj.layer : undefined,
                          measure: obj.measure, pos: obj.pos },
                target: { lane: { player: targetLane.lanePlayer, kind: targetLane.laneKind,
                                  index: targetLane.laneIndex } },
                to: { measure: target.measure, pos: target.pos }
            })
            if (r) {
                refreshTiming()
                if (typeof editPage !== "undefined" && editPage) editPage.reloadBga()
                window.metaSelection = []
                setStatus(qsTr("%1 → note（小节 %2 · %3/%4）")
                          .arg(kind.toUpperCase()).arg(target.measure).arg(target.pos.num).arg(target.pos.den))
            }
            return
        }
        if (kind === "bga") {
            // 横向 = 目标图层（laneAtX 的 bgaLayer；无则保持）
            let layer = obj.layer
            if (targetLane && targetLane.valid && targetLane.bgaLayer !== undefined && targetLane.bgaLayer >= 0)
                layer = targetLane.bgaLayer
            const r = sessionCmd("bga.move", {
                from: { layer: obj.layer, measure: obj.measure, pos: obj.pos },
                to: { layer: layer, measure: target.measure, pos: target.pos }
            })
            if (r) {
                if (typeof editPage !== "undefined" && editPage) editPage.reloadBga()
                setStatus(qsTr("移动 BGA → 图层%1 · 小节 %2 · %3/%4")
                          .arg(layer).arg(target.measure).arg(target.pos.num).arg(target.pos.den))
            }
        } else if (kind === "bpm" || kind === "stop") {
            const r = sessionCmd("timing.move", {
                kind: kind,
                from: { measure: obj.measure, pos: obj.pos },
                to: { measure: target.measure, pos: target.pos }
            })
            if (r) {
                refreshTiming()
                setStatus(qsTr("移动 %1 → 小节 %2 · %3/%4")
                          .arg(kind.toUpperCase()).arg(target.measure).arg(target.pos.num).arg(target.pos.den))
            }
        }
    }
    /// 双击编辑 BGA/BPM/STOP 对象（弹出对话框；BGA 改 BMP/图层，bpm/stop 改值）。

    /// 平移选中 note（统一位移：拖拽/框选整段/多选）。
    /// deltaF = 连续拍位位移（可负可跨小节）；targetLane = 横向目标列（laneAtX；null=纯时间）。
    /// 2026-09 跨命名空间：targetLane 带 metaKind（bpm/stop）→ note.convert（id 不变）；
    /// BGA 图层列（bgaLayer >= 0）→ note.convert（bga_*）；其余 = note.moveRegion。
    /// 移动选中 note（统一位移：拖拽/框选整段/多选）。
    /// deltaF = 连续拍位位移（可负可跨小节）；targetLane = 横向目标列（laneAtX；null=纯时间）；
    /// sourceLane = 拖起 note 所在轨（{player,kind,index}）。跨命名空间（BPM/STOP/BGA）→ note.convert；
    /// 普通轨 → note.move（moves 数组，**跨通道只把「拖起轨 sourceLane 的 note」改到目标轨**，
    /// 其余 note 仅时间移动、保持原轨道——修复 2026-09 多选跨通道全部挤到松开通道的 bug）。
    /// ⚠️ note.move = CompositeCommand 一个 undo 步。移动后**保持选中**（selectionRefs 更新到新位置）。
    function moveSelection(deltaF, targetLane, sourceLane) {
        if (!window.selectionRefs || window.selectionRefs.length === 0) {
            setStatus(qsTr("先选中 note（选择工具点击/框选）再移动"))
            return
        }
        const refs = window.selectionRefs.slice()
        // delta snap：把连续拍位位移吸附到当前槽（snapNum/snapDen 小节），再拆成
        // {measure(int 小节分量), pos(节内分数分量)}。BMS 槽位为离散步长，吸附后对齐网格。
        const num = window.snapNum, den = window.snapDen
        const slots = Math.max(1, Math.floor(den / num))
        const snappedF = Math.round(deltaF * slots) / slots
        const m = Math.floor(snappedF)
        const frac = snappedF - m
        const delta = { measure: m, pos: { num: Math.round(frac * den), den: den } }
        if (targetLane && targetLane.valid) {
            // 跨命名空间：BPM/STOP 列（metaKind）→ note.convert；BGA 图层列 → note.convert（bga_*）
            if (targetLane.metaKind === "bpm" || targetLane.metaKind === "stop") {
                const r0 = sessionCmd("note.convert", { selection: refs, target: targetLane.metaKind, delta: delta })
                if (r0) {
                    window.selectionRefs = []
                    refreshTiming()  // 转出到 BPM/STOP 事件 → 右 Dock 时间轴列表须重取
                    setStatus(qsTr("已转换 %1 个 note → %2（id 不变）").arg(r0.notes).arg(targetLane.metaKind.toUpperCase()))
                }
                return
            }
            if (targetLane.bgaLayer !== undefined && targetLane.bgaLayer >= 0) {
                const bgaTarget = targetLane.bgaLayer === 1 ? "bga_poor"
                                  : targetLane.bgaLayer === 2 ? "bga_layer"
                                  : targetLane.bgaLayer === 3 ? "bga_layer2" : "bga_base"
                const r1 = sessionCmd("note.convert", { selection: refs, target: bgaTarget, delta: delta })
                if (r1) {
                    window.selectionRefs = []
                    if (typeof editPage !== "undefined" && editPage) editPage.reloadBga()  // 左 Dock BGA 面板须重取
                    setStatus(qsTr("已转换 %1 个 note → BGA（id 不变）").arg(r1.notes))
                }
                return
            }
        }
        // 普通轨道移动：逐 note 计算 to（绝对位置 = 源 + delta，带进位）。
        // **通道「同距离偏移」**（2026-09 用户：拖动时每个选中 note 都移动相同距离）：
        // 拖起轨 key i → 目标轨 key j，偏移 = j-i；所有选中 key note 左/右移相同量（保相对位置），
        // 不再「全部挤到目标轨」或「只动拖起轨」。非 key note（皿/踏板/BGM）不变；
        // 跨 kind（如 key→皿）走 sourceLane 兜底（拖起轨 note 改到目标 kind）。
        let channelOffset = 0
        const keyToKey = sourceLane && sourceLane.kind === "key" &&
                         targetLane && targetLane.valid && targetLane.laneKind === "key"
        if (keyToKey) channelOffset = targetLane.laneIndex - sourceLane.index
        const moves = []
        const newRefs = []
        for (let i = 0; i < refs.length; i++) {
            const ref = refs[i]
            const to = addPosDelta(ref, delta)
            let changedLane = false
            if (channelOffset !== 0 && ref.lane.kind === "key") {
                // 同距离偏移（key 轨；钳到合法 key 范围 1..7）
                const ni = Math.max(1, Math.min(7, ref.lane.index + channelOffset))
                if (ni !== ref.lane.index) {
                    to.lane = { player: ref.lane.player, kind: "key", index: ni }
                    changedLane = true
                }
            } else if (targetLane && targetLane.valid && sourceLane &&
                    laneEquals(ref.lane, sourceLane.kind, sourceLane.index, sourceLane.player) &&
                    !laneEquals(ref.lane, targetLane.laneKind, targetLane.laneIndex, targetLane.lanePlayer)) {
                // 跨 kind 兜底：拖起轨 note 改到目标 kind（其余 keep）
                to.lane = { player: targetLane.lanePlayer, kind: targetLane.laneKind,
                            index: targetLane.laneIndex }
                if (targetLane.bgm_line !== undefined && targetLane.bgm_line >= 0)
                    to.bgm_line = targetLane.bgm_line
                changedLane = true
            }
            moves.push({ from: ref, to: to })
            // 移动后保持选中：selectionRefs 更新到新位置（measure/pos/lane/bgm_line）
            const nr = { measure: to.measure, pos: to.pos,
                         lane: changedLane ? to.lane : ref.lane,
                         sample: ref.sample }
            if (to.bgm_line !== undefined) nr.bgm_line = to.bgm_line
            else if (ref.bgm_line !== undefined) nr.bgm_line = ref.bgm_line
            newRefs.push(nr)
        }
        const r = sessionCmd("note.move", { moves: moves })
        if (r) {
            window.selectionRefs = newRefs   // 保持选中（新位置）
            const chan = targetLane && targetLane.valid && Math.abs(deltaF) < 0.0001
            setStatus(chan ? qsTr("已移动 %1 个 note（改通道）").arg(r.moved)
                           : qsTr("已移动 %1 个 note（+%2 拍）").arg(r.moved).arg(deltaF.toFixed(3)))
        }
    }
    /// 源位置 + 位移增量 → 绝对目标位置（带小节进位；分数约分，保证与 core Rational 一致）。
    function addPosDelta(ref, delta) {
        const rn = ref.pos.num, rd = ref.pos.den
        const dn = delta.pos.num, dd = delta.pos.den
        const newNum = rn * dd + dn * rd
        const newDen = rd * dd
        const carry = Math.floor(newNum / newDen)
        const g = gcd(newNum - carry * newDen, newDen)
        return { measure: ref.measure + delta.measure + carry,
                 pos: { num: (newNum - carry * newDen) / g, den: newDen / g } }
    }
    function gcd(a, b) {
        a = Math.abs(a); b = Math.abs(b)
        while (b) { const t = b; b = a % b; a = t }
        return a || 1
    }
    function laneEquals(lane, kind, index, player) {
        return lane && lane.kind === kind && lane.index === index && lane.player === player
    }
    /// 单点 ↔ LN 转换（工具栏「单点/LN」按钮；selection 批量一个 undo 步）。
    /// LNTYPE 翻转选中 note 的 LN 通道（LNTYPE 1：ln_channel ←→ 普通；配对由 rebuild 自动）。
    function toggleLnSelection() {
        if (!window.selectionRefs || window.selectionRefs.length === 0) {
            setStatus(qsTr("先选中 note（点击/框选）再转换单点/LN"))
            return
        }
        // 2026-09：BGM 轨无 LN 通道表示，过滤掉这类 note（其余继续转换）
        var playable = window.selectionRefs.filter(function (r) {
            return r.lane && r.lane.kind !== "bgm"
        })
        if (playable.length === 0) {
            setStatus(qsTr("选中的都是 BGM 轨 note（无 LN 通道表示），无法转换"))
            return
        }
        var r = sessionCmd("note.toggleLn", { selection: playable.slice() })
        if (r) {
            window.selectionRefs = []
            setStatus(qsTr("已转换 %1 个 note（单点↔LN）").arg(r.notes))
        }
    }
    /// 量化：把选中 note 的 pos 吸附到当前 snap 网格（note.quantize；一个 undo 步）。
    /// 量化后**保持选中**（selectionRefs 的 pos 更新到吸附值）。
    function quantizeSelection() {
        if (!window.selectionRefs || window.selectionRefs.length === 0) {
            setStatus(qsTr("先选中 note（点击/框选）再量化"))
            return
        }
        const sn = window.snapNum, sd = window.snapDen
        var r = sessionCmd("note.quantize", {
            selection: window.selectionRefs.slice(),
            snap: { num: sn, den: sd }
        })
        if (r) {
            // 保持选中：pos 更新到吸附值（k*snapNum/snapDen，约分）
            window.selectionRefs = window.selectionRefs.map(function(ref) {
                const p = ref.pos.num / ref.pos.den
                const k = Math.round(p * sd / sn)
                const nnum = k * sn, nden = sd
                const g = gcd(nnum, nden)
                return { measure: ref.measure, pos: { num: nnum / g, den: nden / g },
                         lane: ref.lane, sample: ref.sample,
                         bgm_line: ref.bgm_line }
            })
            setStatus(qsTr("已量化 %1 个 note（吸附到 %2/%3）").arg(r.notes).arg(sn).arg(sd))
        }
    }
    /// 变换：镜像（mirror=true）/ 旋转（rotate=±1）；note.transform；一个 undo 步。
    /// 变换后**保持选中**（selectionRefs 的 key 轨道更新为镜像/旋转后的下标；相对位置不变）。
    /// ⚠️ 只处理 key 轨（镜像/旋转不影响皿/踏板/BGM）；按 7key 映射（key i ↔ key 8-i）。
    function transformSelection(mirror, rotate) {
        if (!window.selectionRefs || window.selectionRefs.length === 0) {
            setStatus(qsTr("先选中 note（点击/框选）再变换"))
            return
        }
        var args = { selection: window.selectionRefs.slice() }
        if (mirror) args.mirror = true
        if (rotate !== 0) args.rotate = rotate
        var r = sessionCmd("note.transform", args)
        if (r) {
            window.selectionRefs = window.selectionRefs.map(function(ref) {
                if (ref.lane.kind !== "key") return ref   // 非 key 轨不变
                const idx = ref.lane.index
                const nidx = mirror ? (8 - idx) : (((idx - 1 + rotate) % 7 + 7) % 7 + 1)
                return { measure: ref.measure, pos: ref.pos,
                         lane: { player: ref.lane.player, kind: "key", index: nidx },
                         sample: ref.sample, bgm_line: ref.bgm_line }
            })
            setStatus(mirror ? qsTr("已镜像 %1 个 note").arg(r.notes)
                             : qsTr("已旋转 %1 个 note").arg(r.notes))
        }
    }
    /// 网格开关：折叠/展开槽位弱线显示（不影响吸附）。
    function toggleGrid() {
        window.showGrid = !window.showGrid
        setStatus(window.showGrid ? qsTr("网格显示：开") : qsTr("网格显示：关"))
    }
    function copySelection() {
        if (!window.selectionRefs || window.selectionRefs.length === 0) {
            setStatus(qsTr("没有选中（选择工具下 Shift+拖拽框选）"))
            return
        }
        var r = sessionCmd("clipboard.copy", { selection: window.selectionRefs })
        if (r) {
            window.clipboardLines = r.lines
            setStatus(qsTr("已复制 %1 个 note（%2 行）").arg(r.count).arg(r.lines.length))
        }
    }
    function pasteClipboard() {
        if (!window.clipboardLines || window.clipboardLines.length === 0) {
            setStatus(qsTr("剪贴板为空（先框选 Ctrl+C）"))
            return
        }
        var target = 0
        if (typeof editPage !== "undefined" && editPage)
            target = Math.floor(editPage.centerMeasure() || 0)
        var r = sessionCmd("clipboard.paste", {
            lines: window.clipboardLines,
            target_measure: Math.max(0, target)
        })
        if (r)
            setStatus(qsTr("已粘贴 %1 个 note 到小节 %2").arg(r.notes).arg(r.target_measure))
    }
    function undoEdit() {
        var r = sessionCmd("session.undo")
        if (r) {
            refreshTiming()  // 撤销可能恢复/移除 BPM/STOP 事件 → 时间轴面板列表须重取
            refreshSamples()  // 撤销可能回退 sample.setFile/rename → 采样面板须刷新
            if (typeof editPage !== "undefined" && editPage) editPage.reloadBga()  // BGA/BMP 面板同刷新
            if (r.ok)
                setStatus(qsTr("已撤销（可重做 %1 步）").arg(r.redo_depth))
            else
                setStatus(qsTr("无可撤销"))
        }
    }
    function redoEdit() {
        var r = sessionCmd("session.redo")
        if (r) {
            refreshTiming()  // 同上：重做改变时间轴事件 → 重取列表
            refreshSamples()  // 同上：重做改变采样/文件名 → 采样面板须刷新
            if (typeof editPage !== "undefined" && editPage) editPage.reloadBga()  // BGA/BMP 面板同刷新
            if (r.ok)
                setStatus(qsTr("已重做（可撤销 %1 步）").arg(r.undo_depth))
            else
                setStatus(qsTr("无可重做"))
        }
    }
    /// 设置采样槽位绑定的文件名（双击采样行编辑；sample.setFile 一个 undo 步）。成功后
    /// 从内存会话刷新采样面板（id/file/引用数；session.samples 与 info 同构，枚举全部槽位）。
    function setSampleFile(id, file) {
        if (id === undefined || id === null || id === "") return
        const r = sessionCmd("sample.setFile", { id: id, file: file })
        if (!r) return
        refreshSamples()
        sampleModel.selectId(id)  // 保持当前采样为该行；不滚动（视口保持不变）
        setStatus(qsTr("#WAV%1 → %2（Undo 可恢复）").arg(id, file))
    }

    /// 重载采样列表（session.samples → sampleModel），并保持列表滚动位置（编辑后视口不跳）。
    function refreshSamples() {
        const sresp = beatbench.dispatch(JSON.stringify({ command: "session.samples", args: {} }))
        if (!sresp) return
        const r2 = JSON.parse(sresp)
        if (!r2.ok) return
        const panel = (typeof editPage !== "undefined" && editPage) ? editPage.samplePanelObj : null
        const prevY = (panel && panel.listScrollY) ? panel.listScrollY() : -1
        sampleModel.loadFromInfo(JSON.stringify({ ok: true, result: r2.result }))
        if (panel && panel.restoreScrollY && prevY >= 0) panel.restoreScrollY(prevY)
    }

    /// 编辑区双击 note → 改其引用采样 id（切音手工版）。弹出对话框，收新 #WAV id → note.setSample。

    /// 时间轴事件（BPM/STOP）列表重取（timing.list）→ 回填右 Dock 时间轴面板。
    function refreshTiming() {
        var bpm = [], stop = []
        var rb = beatbench.dispatch(JSON.stringify({ command: "timing.list", args: { kind: "bpm" } }))
        if (rb) {
            var rbp = JSON.parse(rb)
            if (rbp.ok) bpm = rbp.result.events || []
        }
        var rs = beatbench.dispatch(JSON.stringify({ command: "timing.list", args: { kind: "stop" } }))
        if (rs) {
            var rsp = JSON.parse(rs)
            if (rsp.ok) stop = rsp.result.events || []
        }
        window.timingBpm = bpm
        window.timingStop = stop
    }
    /// 添加/改值时间轴事件（timing.put；一个 undo 步；成功后重取列表）。
    function editTiming(kind, measure, num, den, value, ref) {
        var args = { kind: kind, measure: measure, pos: { num: num, den: den }, value: value }
        if (ref && ref !== "") args.ref = ref
        var r = sessionCmd("timing.put", args)
        if (r) {
            refreshTiming()
            setStatus(qsTr("已设置 %1 事件：小节 %2 · %3/%4 = %5")
                      .arg(kind.toUpperCase()).arg(measure).arg(num).arg(den).arg(value))
        }
    }
    /// 删除时间轴事件（timing.delete；一个 undo 步；成功后重取列表）。
    function deleteTiming(kind, measure, num, den) {
        var args = { kind: kind, measure: measure, pos: { num: num, den: den } }
        var r = sessionCmd("timing.delete", args)
        if (r) {
            refreshTiming()
            setStatus(qsTr("已删除 %1 事件：小节 %2 · %3/%4")
                      .arg(kind.toUpperCase()).arg(measure).arg(num).arg(den))
        }
    }

    /// 添加/改值 BGA 事件（bga.put；一个 undo 步；成功后重取面板）。bmpId 为文本 id → 解码数值。
    function editBga(layer, measure, num, den, bmpId) {
        if (typeof editPage === "undefined" || !editPage) return
        if (bmpId === "") { setStatus(qsTr("请填 #BMP id")); return }
        const sample = chartSession.decodeId(bmpId)
        if (sample < 0) { setStatus(qsTr("#BMP id 非法：%1").arg(bmpId)); return }
        const args = { layer: layer, measure: measure, pos: { num: num, den: den }, sample: sample }
        const r = sessionCmd("bga.put", args)
        if (r) {
            editPage.reloadBga()
            setStatus(qsTr("已设置 %1 BGA：#BMP%2 · 小节 %3 · %4/%5")
                      .arg(bgaLayerName(layer)).arg(bmpId).arg(measure).arg(num).arg(den))
        }
    }
    /// 删除 BGA 事件（bga.delete；一个 undo 步；成功后重取面板）。
    function deleteBga(layer, measure, num, den) {
        if (typeof editPage === "undefined" || !editPage) return
        const args = { layer: layer, measure: measure, pos: { num: num, den: den } }
        const r = sessionCmd("bga.delete", args)
        if (r) {
            editPage.reloadBga()
            setStatus(qsTr("已删除 BGA：%1 · 小节 %2 · %3/%4")
                      .arg(bgaLayerName(layer)).arg(measure).arg(num).arg(den))
        }
    }
    /// 添加/覆盖 #BMP 定义（sample.setFile kind=bmp；一个 undo 步）。
    function bmpAdd(id, file) {
        if (id === "") { setStatus(qsTr("#BMP id 不能为空")); return }
        const r = sessionCmd("sample.setFile", { id: id, file: file, kind: "bmp" })
        if (r) {
            if (typeof editPage !== "undefined" && editPage) editPage.reloadBga()
            setStatus(qsTr("#BMP%1 → %2").arg(id).arg(file || "(未设文件)"))
        }
    }
    /// 设置 #BMP 文件（sample.setFile kind=bmp）。
    function bmpSetFile(id, file) {
        if (id === "") { setStatus(qsTr("#BMP id 不能为空")); return }
        const r = sessionCmd("sample.setFile", { id: id, file: file, kind: "bmp" })
        if (r) {
            if (typeof editPage !== "undefined" && editPage) editPage.reloadBga()
            setStatus(qsTr("#BMP%1 文件 → %2").arg(id).arg(file || "(已清空)"))
        }
    }
    /// 重命名 #BMP id（sample.rename kind=bmp）。
    function bmpRename(fromId, toId) {
        if (fromId === "" || toId === "") { setStatus(qsTr("#BMP 重命名需 from/to")); return }
        if (fromId === toId) return
        const r = sessionCmd("sample.rename", { from: fromId, to: toId, kind: "bmp" })
        if (r) {
            if (typeof editPage !== "undefined" && editPage) editPage.reloadBga()
            setStatus(qsTr("#BMP%1 → #BMP%2").arg(fromId).arg(toId))
        }
    }
    /// 删除 #BMP 定义（sample.delete kind=bmp；引用保留原 id，Undo 可恢复）。
    function bmpDelete(id) {
        if (id === "") { setStatus(qsTr("#BMP id 不能为空")); return }
        const r = sessionCmd("sample.delete", { id: id, kind: "bmp" })
        if (r) {
            if (typeof editPage !== "undefined" && editPage) editPage.reloadBga()
            setStatus(qsTr("已删除 #BMP%1 定义（Undo 可恢复）").arg(id))
        }
    }
    /// BGA 图层名（状态栏/面板展示）。
    function bgaLayerName(l) {
        switch (l) {
            case 1: return "poor"
            case 2: return "layer"
            case 3: return "layer2"
            default: return "base"
        }
    }
    /// 视口 BGA 列点击放置：用当前 #BMP 在命中图层放一个 BGA 事件（bga.put）。
    function placeBgaAt(hit) {
        if (window.currentBmpId === "") {
            setStatus(qsTr("先在 BGA 面板添加/选择 #BMP（视口 BGA 列放置用）"))
            return
        }
        const sample = chartSession.decodeId(window.currentBmpId)
        if (sample < 0) { setStatus(qsTr("#BMP%1 不存在").arg(window.currentBmpId)); return }
        const args = { layer: hit.bgaLayer, measure: hit.measure,
                       pos: { num: hit.num, den: hit.den }, sample: sample }
        const r = sessionCmd("bga.put", args)
        if (r) {
            if (typeof editPage !== "undefined" && editPage) editPage.reloadBga()
            setStatus(qsTr("放置 %1 BGA：#BMP%2 · 小节 %3 · %4/%5")
                      .arg(bgaLayerName(hit.bgaLayer)).arg(window.currentBmpId)
                      .arg(hit.measure).arg(hit.num).arg(hit.den))
        }
    }
    /// 选择当前 #BMP（BGA 面板行点击）。
    function setCurrentBmp(id) {
        window.currentBmpId = id
        if (typeof editPage !== "undefined" && editPage) editPage.currentBmpId = id
        setStatus(qsTr("当前 #BMP：%1（视口 BGA 列放置用）").arg(id))
    }

    function saveChart() {
        // 2026-09：元信息修改交整个文件保存——先应用元信息编辑 + 扩展代码，再 session.save。
        if (typeof editPage !== "undefined" && editPage) {
            const edits = editPage.collectMetaEdits()
            if (edits && edits.length > 0) {
                const em = sessionCmd("meta.edit", { edits: edits })
                if (em) setStatus(qsTr("元信息已保存 %1 处").arg(edits.length))
            }
            editPage.applyRawEdits()
            refreshLint()
        }
        var r = sessionCmd("session.save", { overwrite: true })
        if (r) {
            window.chartPath = r.output
            setStatus(qsTr("已保存：%1（%2 字节）").arg(r.output).arg(r.bytes))
        }
    }
    /// 元信息面板「保存」按钮：只应用元信息编辑 + 扩展代码到内存会话（不写文件）。
    /// 之后 Ctrl+S / 另存为会随整个文件一并落盘。成功后提交基线（orig=value）清脏。
    function saveMetaEdits() {
        if (typeof editPage === "undefined" || !editPage) return
        const edits = editPage.collectMetaEdits()
        let n = 0
        if (edits && edits.length > 0) {
            const em = sessionCmd("meta.edit", { edits: edits })
            if (!em) return   // 失败：sessionCmd 已置状态栏
            n = edits.length
        }
        if (editPage.applyRawEdits()) n++
        editPage.commitMeta()
        refreshLint()
        setStatus(n > 0
                  ? qsTr("元信息已保存 %1 处（写文件时随整体保存落盘）").arg(n)
                  : qsTr("元信息无改动"))
    }
    function saveChartAs(path) {
        if (typeof editPage !== "undefined" && editPage) {
            const edits = editPage.collectMetaEdits()
            if (edits && edits.length > 0) sessionCmd("meta.edit", { edits: edits })
            editPage.applyRawEdits()
        }
        var r = sessionCmd("session.save", { path: path, overwrite: true })
        if (r) {
            window.chartPath = r.output
            setStatus(qsTr("已另存为：%1").arg(r.output))
        }
    }
}
