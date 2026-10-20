// SPDX-License-Identifier: GPL-3.0-only
import QtQuick
import QtTest
import "../../app/qml/components" as Components

Item {
    id: scene
    width: 1100
    height: 620

    Component {
        id: surface
        Components.ChartCompareSplit {
            id: compareSplit
            width: scene.width
            height: scene.height
            documentService: compareDocuments
            primarySession: compareDocuments.currentSession
            primaryChartView: primaryChart
            property alias primaryView: primaryChart
            // Exercise the production default property, not a parallel test layout.
            Components.ChartView {
                id: primaryChart
                anchors.fill: parent
                documentSession: compareSplit.primarySession
                topHigh: false
                editorTool: "select"
                onNoteClicked: (ref, ctrl) => { selection = [ref] }
            }
        }
    }

    Component {
        id: invalidViewport
        Components.ChartView { width: 320; height: 0; documentSession: null }
    }

    TestCase {
        id: tc
        name: "ChartCompareSplit"
        when: windowShown
        readonly property string aId: "chart-view-binding-a"
        readonly property string bId: "chart-view-binding-b"
        property var split
        property var selector
        SignalSpy { id: swapSpy; target: tc.split; signalName: "swapRequested" }
        SignalSpy { id: placeSpy; target: tc.split ? tc.split.referenceView : null; signalName: "hitPlaceRequested" }
        SignalSpy { id: deleteSpy; target: tc.split ? tc.split.referenceView : null; signalName: "noteRightDeleted" }
        SignalSpy { id: editSpy; target: tc.split ? tc.split.referenceView : null; signalName: "noteEditRequested" }
        SignalSpy { id: moveSpy; target: tc.split ? tc.split.referenceView : null; signalName: "moveSelectionRequested" }

        function init() {
            bindingFixture.resetCompare()
            bindingFixture.resetAudioProbe(false)
            split = createTemporaryObject(surface, scene)
            verify(split !== null)
            verify(waitForRendering(scene))
            selector = bindingFixture.compareSelector(split)
            verify(selector !== null, "Find the actual production BbComboBox")
            compare(split.expanded, false)
            compare(split.referenceWidth, 0)
            compare(split.primarySession, compareDocuments.session(aId))
            swapSpy.clear()
            placeSpy.clear()
            deleteSpy.clear()
            editSpy.clear()
            moveSpy.clear()
        }
        function cleanup() {
            if (selector && selector.popup.visible) selector.popup.close()
            bindingFixture.resetAudioProbe(false)
            bindingFixture.resetCompare()
        }
        function parts(view) {
            const p = bindingFixture.childrenOf(view)
            compare(p.chartCount, 1)
            compare(p.overlayCount, 1)
            compare(p.waveformCount, 1)
            return p
        }
        function boundTo(view, session) {
            compare(view.documentSession, session)
            const p = parts(view)
            compare(p.chart.session, session)
            compare(p.overlay.session, session)
            compare(p.waveform.session, session)
        }
        function dragToWidth(width) {
            // The sole 4px SplitView handle stays at the right edge when collapsed.
            // Never assign preferredWidth or invoke settleSplit to simulate a drag.
            const from = split.width - split.referenceWidth - 2
            const to = split.width - width - 2
            const y = split.height / 2
            mousePress(split, from, y, Qt.LeftButton)
            for (let n = 1; n <= 12; ++n)
                mouseMove(split, from + (to - from) * n / 12, y, 10)
            mouseRelease(split, to, y, Qt.LeftButton)
        }
        function expand() {
            dragToWidth(480)
            tryVerify(() => split.expanded && split.referenceWidth >= 450)
            tryCompare(split, "referenceDocumentId", bId)
            boundTo(split.referenceView, compareDocuments.session(bId))
            // Selection on handle release changes the session after the last frame.
            verify(waitForRendering(scene))
        }
        function indexOf(id) {
            for (let i = 0; i < split.choices.length; ++i)
                if (split.choices[i].id === id) return i
            fail("Missing opened document " + id)
            return -1
        }
        function mouseChoose(id) {
            mouseClick(selector, selector.width / 2, selector.height / 2)
            tryCompare(selector.popup, "opened", true)
            const list = selector.popup.contentItem
            const index = indexOf(id)
            list.positionViewAtIndex(index, ListView.Contain)
            tryVerify(() => list.itemAtIndex(index) !== null)
            const delegate = list.itemAtIndex(index)
            mouseClick(delegate, delegate.width / 2, delegate.height / 2)
            tryCompare(selector.popup, "visible", false)
            tryCompare(split, "referenceDocumentId", id)
            compare(selector.currentIndex, index)
            compare(selector.displayText, split.choices[index].text)
        }
        function pointFor(view, p, wantNote) {
            const columns = view.probe(0, 40).columns
            for (let c = 0; c < columns.length; ++c) {
                const x = columns[c].x + columns[c].w / 2
                if (x <= p.chart.rulerWidth || x >= p.chart.width - 4) continue
                for (let y = 30; y < p.chart.height - 8; y += 2) {
                    const hit = p.chart.hitTest(x, y)
                    if (hit.valid && hit.laneKind === "key"
                            && p.chart.noteAt(x, y).valid === wantNote)
                        return {x: x, y: y}
                }
            }
            fail("No real " + (wantNote ? "note" : "empty slot") + " in chart: "
                 + JSON.stringify({w: p.chart.width, h: p.chart.height, y: p.chart.scrollY,
                                   content: p.chart.contentHeight, top: view.topHigh, itemTop: p.chart.topHigh, zoom: view.measureHeight, columns: columns}))
            return null
        }
        function state(id) { return JSON.stringify(bindingFixture.documentState(id)) }

        // Compare musical coordinates, not pixels or seconds: B owns its timing/layout.
        function anchorValue(view) {
            const a = view.viewportItem.viewportAnchor
            return a.valid ? a.measure + a.fraction : NaN
        }
        function closeTo(a, b) { return isFinite(a) && isFinite(b) && Math.abs(a - b) < 0.00001 }
        function aligned() {
            tryVerify(() => closeTo(anchorValue(split.primaryView), anchorValue(split.referenceView)),
                      2000, "Both 90%-height cursors must represent the same musical position")
            compare(split.referenceView.measureHeight, split.primaryView.measureHeight)
            compare(split.referenceView.topHigh, split.primaryView.topHigh)
        }
        function flushSync() {
            // Unified C++ notification and the coordinator each queue one event-loop turn.
            wait(40)
        }
        function primeSync(profile) {
            bindingFixture.resetCompareTiming(profile || "same")
            expand()
            compare(split.synchronizeViewports, true)
            tryCompare(split, "syncActive", true)
            split.primaryView.measureHeight = 180
            split.primaryView.viewportItem.applyViewportAnchor({valid: true, measure: 10, fraction: 0.375})
            aligned()
            flushSync()
        }

        function test_bgm_default_expansion_and_reference_collapse_do_not_edit_documents() {
            compare(split.primaryView.bgmExpanded, true)
            compare(split.primaryView.viewportItem.bgmExpanded, true)
            // B is initialized before a reference document is selected.
            compare(split.referenceView.bgmExpanded, true)
            compare(split.referenceView.viewportItem.bgmExpanded, true)
            split.hideSecondaryChannels = false
            expand()
            verify(bindingFixture.seedCompareHistory())
            const a = state(aId), b = state(bId)
            const reference = split.referenceView, item = reference.viewportItem
            const column = item.probe(0, 40).columns.find(c => c.label === "bgm1")
            verify(column !== undefined)
            item.scrollX = Math.max(0, column.x - item.rulerWidth - 20)
            const visibleColumn = item.probe(0, 40).columns.find(c => c.label === "bgm1")
            verify(visibleColumn !== undefined)
            const x = visibleColumn.x + visibleColumn.w / 2
            verify(x > item.rulerWidth && x < item.width)
            mouseClick(reference, x, 9)
            compare(reference.bgmExpanded, false)
            compare(item.bgmExpanded, false)
            compare(split.primaryView.bgmExpanded, true)
            compare(split.primaryView.viewportItem.bgmExpanded, true)
            compare(state(aId), a)
            compare(state(bId), b)
            compare(split.referenceDocumentId, bId)
            compare(compareDocuments.activeId(), aId)
        }

        function test_sync_default_enable_A_wins_and_disabled_is_independent() {
            compare(split.synchronizeViewports, true)
            compare(split.syncActive, false) // collapsed, despite default enabled
            split.synchronizeViewports = false
            expand()
            const av = split.primaryView, bv = split.referenceView
            av.measureHeight = 180
            av.viewportItem.applyViewportAnchor({valid: true, measure: 9, fraction: 0.25})
            bv.measureHeight = 120
            bv.viewportItem.applyViewportAnchor({valid: true, measure: 4, fraction: 0.5})
            flushSync()
            const a = anchorValue(av)
            verify(!closeTo(a, anchorValue(bv)))
            const toggle = findChild(split, "synchronizeViewportsCheck")
            verify(toggle !== null)
            compare(toggle.checked, false)
            mouseClick(toggle, toggle.width / 2, toggle.height / 2)
            tryCompare(split, "synchronizeViewports", true)
            aligned()
            verify(closeTo(anchorValue(av), a), "Enabling sync must not let stale B win")
            split.synchronizeViewports = false
            const oldB = anchorValue(bv)
            av.seekToSeconds(30)
            flushSync()
            verify(!closeTo(anchorValue(av), a))
            verify(closeTo(anchorValue(bv), oldB))
        }

        function test_sync_navigation_data() {
            const rows = []
            for (const side of ["A", "B"])
                for (const action of ["wheel", "pan", "middle", "scrollbar", "seek", "ruler", "zoom"])
                    rows.push({tag: side + "_" + action, side: side, action: action})
            return rows
        }
        function test_sync_navigation(data) {
            primeSync("bpm")
            verify(bindingFixture.seedCompareHistory())
            flushSync()
            const a = state(aId), b = state(bId)
            const av = split.primaryView, bv = split.referenceView
            const view = data.side === "A" ? av : bv
            const before = anchorValue(view), zoom = view.measureHeight
            bindingFixture.resetAudioProbe(true)
            if (data.action === "wheel") mouseWheel(view, 200, 300, 0, -120)
            else if (data.action === "pan" || data.action === "middle") {
                view.editorTool = "pan"
                mouseDrag(view, 200, 350, 0, -72,
                          data.action === "middle" ? Qt.MiddleButton : Qt.LeftButton)
            } else if (data.action === "scrollbar") {
                // Actual 12px production scrollbar, not a scrollY assignment.
                mouseClick(view, view.width - 6, view.height * 0.72)
            } else if (data.action === "seek") view.seekToSeconds(8)
            else if (data.action === "ruler") mouseClick(view, 8, 240)
            else mouseWheel(view, 200, 300, 0, 120, Qt.NoButton, Qt.ControlModifier)
            if (data.action === "zoom") verify(view.measureHeight !== zoom)
            else verify(!closeTo(anchorValue(view), before), "Input must really navigate the source")
            aligned()
            flushSync()
            aligned()
            compare(state(aId), a)
            compare(state(bId), b)
            // B may request A's viewport, never A's audio seek path.
            compare(audioEngine.seekCount, data.side === "A" && data.action === "ruler" ? 1 : 0)
            compare(compareDocuments.activeId(), aId)
        }

        function test_sync_programmatic_cpp_changes_and_horizontal_independence() {
            primeSync("same")
            const av = split.primaryView, bv = split.referenceView
            const ap = av.viewportItem, bp = bv.viewportItem
            ap.scrollY += 137 // programmatic/follow path has no QML navigationCompleted
            aligned()
            ap.scrollToTime(12)
            aligned()
            av.zoomAt(250, 1.2)
            aligned()
            av.topHigh = true
            aligned()
            ap.laneWidth = 120
            bp.laneWidth = 120
            ap.scrollX = 80
            bp.scrollX = 140
            flushSync()
            compare(ap.scrollX, 80)
            compare(bp.scrollX, 140)
            mouseWheel(bv, 200, 300, 0, -120)
            aligned()
            compare(ap.scrollX, 80)
            compare(bp.scrollX, 140)
        }

        function test_sync_variable_lengths_different_BPM_and_geometry() {
            primeSync("variable")
            const av = split.primaryView, bv = split.referenceView
            const ap = av.viewportItem, bp = bv.viewportItem
            verify(closeTo(anchorValue(av), 10.375))
            verify(Math.abs(ap.scrollY - bp.scrollY) > 20, "Recipient must not copy pixel scroll")
            verify(Math.abs(av.cursorSec - bv.cursorSec) > 1, "Musical sync must not copy seconds")
            split.height = 480
            flushSync()
            aligned()
            dragToWidth(600)
            flushSync()
            aligned()
            for (const measure of [6, 2, 1, 14]) {
                av.seekTo(ap.secondsForPosition(measure, 0.375))
                aligned()
                verify(closeTo(anchorValue(av), measure + 0.375))
            }
            bv.seekTo(bp.secondsForPosition(7, 0.625))
            aligned()
            verify(closeTo(anchorValue(av), 7.625))
            // A's playhead is projected using B's timing, not copied as absolute seconds.
            ap.playheadSec = ap.secondsForPosition(8, 0.25)
            tryVerify(() => Math.abs(bp.playheadSec - bp.secondsForPosition(8, 0.25)) < 0.00001)
            verify(Math.abs(ap.playheadSec - bp.playheadSec) > 1)
        }

        function test_sync_short_B_never_pulls_A_back() {
            bindingFixture.resetCompareTiming("shortB")
            expand()
            const av = split.primaryView, bv = split.referenceView
            av.measureHeight = 180
            av.seekTo(av.viewportItem.secondsForPosition(20, 0.5))
            flushSync()
            verify(closeTo(anchorValue(av), 20.5))
            verify(anchorValue(bv) < anchorValue(av), "Short recipient clamps locally")
            const a = anchorValue(av), b = anchorValue(bv)
            flushSync()
            verify(closeTo(anchorValue(av), a), "B clamp notifications must not become navigation")
            verify(closeTo(anchorValue(bv), b))
            // A short, clamped B must not pull A back during horizontal/no-op input.
            bv.viewportItem.laneWidth = 120
            bv.viewportItem.scrollX = 100
            mouseDrag(bv, 250, 300, -50, 0, Qt.MiddleButton)
            flushSync()
            verify(closeTo(anchorValue(av), a), "Horizontal middle-pan must not navigate A vertically")
            split.editorTool = "pan"
            mouseDrag(bv, 250, 300, -50, 0, Qt.LeftButton)
            flushSync()
            verify(closeTo(anchorValue(av), a), "Horizontal left-pan must not navigate A vertically")
            mouseWheel(bv, 200, 300, 0, -120, Qt.NoButton, Qt.ShiftModifier)
            flushSync()
            verify(closeTo(anchorValue(av), a))
        }
        function test_sync_short_A_authoritative_when_B_navigates_past_end() {
            bindingFixture.resetCompareTiming("shortA")
            expand()
            const av = split.primaryView, bv = split.referenceView
            av.measureHeight = 180
            flushSync()
            bv.seekToSeconds(bv.viewportItem.secondsForPosition(20, 0.5))
            aligned()
            verify(anchorValue(av) < 20, "A's end boundary must win over B's longer chart")
            const end = anchorValue(av)
            flushSync()
            aligned()
            verify(closeTo(anchorValue(av), end))
        }

        function test_sync_invalid_anchor_and_missing_primary_are_safe() {
            primeSync("same")
            const ap = split.primaryView.viewportItem
            const before = ap.scrollY
            for (const anchor of [{valid: false, measure: 8, fraction: 0.5}, {},
                                  {valid: true, measure: NaN, fraction: 0.5},
                                  {valid: true, measure: 8, fraction: Infinity},
                                  {valid: true, measure: 8.5, fraction: 0},
                                  {valid: true, measure: 8, fraction: -0.1}]) {
                ap.applyViewportAnchor(anchor)
                compare(ap.scrollY, before)
            }
            compare(ap.positionForSeconds(-1).valid, false)
            compare(ap.positionForSeconds(NaN).valid, false)
            compare(ap.secondsForPosition(-1, 0), -1)
            const invalid = createTemporaryObject(invalidViewport, scene)
            verify(invalid !== null)
            const item = invalid.viewportItem
            compare(item.viewportAnchor.valid, false)
            item.applyViewportAnchor({valid: true, measure: 8, fraction: 0.5})
            compare(item.scrollY, 0)
            compare(item.positionForSeconds(1).valid, false)
            compare(item.secondsForPosition(1, 0), -1)
            compare(ap.compareStructure(null).compatible, false)
            invalid.documentSession = split.primarySession
            compare(item.viewportAnchor.valid, false) // zero-height item remains invalid
            split.primaryChartView = null
            compare(split.syncActive, false)
            split.referenceView.seekToSeconds(4)
            flushSync()
            compare(ap.scrollY, before)
            split.primaryChartView = split.primaryView
            aligned()
        }

        function test_sync_collapse_visibility_reopen_and_reference_switch_A_wins() {
            primeSync("variable")
            const av = split.primaryView, bv = split.referenceView
            split.collapse()
            tryCompare(split, "syncActive", false)
            av.seekTo(av.viewportItem.secondsForPosition(14, 0.25))
            const oldB = anchorValue(bv)
            flushSync()
            verify(closeTo(anchorValue(bv), oldB))
            expand()
            aligned()
            verify(closeTo(anchorValue(av), 14.25))
            split.visible = false
            compare(split.syncActive, false)
            av.seekTo(av.viewportItem.secondsForPosition(9, 0.5))
            flushSync()
            split.visible = true
            aligned()
            verify(split.selectReference(aId)) // same-document comparison remains navigable
            aligned()
            bv.seekTo(bv.viewportItem.secondsForPosition(7, 0.25))
            aligned()
            verify(closeTo(anchorValue(av), 7.25))
            verify(split.selectReference(bId))
            aligned()
            verify(closeTo(anchorValue(av), 7.25), "Changing B must not move A")
            compare(bv.ownsPlayback, false)
        }

        function test_sync_swap_preserves_A_musical_anchor_and_history() {
            primeSync("variable")
            verify(bindingFixture.seedCompareHistory())
            flushSync()
            const a = state(aId), b = state(bId)
            const hashA = split.primarySession.debugContentHash()
            const hashB = split.referenceSession.debugContentHash()
            const anchor = anchorValue(split.primaryView)
            split.requestSwap()
            compare(swapSpy.count, 1)
            // Main captures before it activates the new document (and its layout resets).
            split.prepareSwap()
            verify(compareDocuments.activate(bId).ok)
            verify(split.selectReference(aId))
            tryCompare(split, "primarySession", compareDocuments.session(bId))
            aligned()
            tryVerify(() => closeTo(anchorValue(split.primaryView), anchor))
            compare(split.primarySession.debugContentHash(), hashB)
            compare(split.referenceSession.debugContentHash(), hashA)
            compare(state(aId), a)
            compare(state(bId), b)
        }

        function test_sync_structure_warning_data() {
            return [{tag: "same", compatible: true}, {tag: "bpm", compatible: false},
                    {tag: "stop", compatible: false}, {tag: "bpmEvent", compatible: false},
                    {tag: "length", compatible: false}, {tag: "mode", compatible: false},
                    {tag: "shortB", compatible: false}]
        }
        function test_sync_structure_warning(data) {
            bindingFixture.resetCompareTiming(data.tag)
            expand()
            verify(bindingFixture.seedCompareHistory())
            flushSync()
            const a = state(aId), b = state(bId)
            const ap = split.primaryView.viewportItem
            // Session, not view, is the public compareStructure argument.
            const status = ap.compareStructure(split.referenceSession)
            compare(status.compatible, data.compatible)
            verify(status.message.length > 0)
            tryVerify(() => split.structureStatus.compatible === data.compatible)
            if (!data.compatible) {
                verify(split.structureStatus.message.length > 0)
                tryVerify(() => bindingFixture.hasVisibleCompareWarning(split, split.structureStatus.message),
                          2000, "Structural mismatch must be visible, not just diagnostic state")
            }
            verify(split.selectReference(aId))
            tryVerify(() => split.structureStatus.compatible === true)
            verify(split.selectReference(bId))
            tryVerify(() => split.structureStatus.compatible === data.compatible)
            compare(state(aId), a)
            compare(state(bId), b)
        }

        function test_edge_drag_resize_collapse_reopen_retains_B() {
            expand()
            const session = split.referenceSession
            const view = split.referenceView
            const firstWidth = split.referenceWidth
            dragToWidth(620)
            tryVerify(() => split.referenceWidth > firstWidth + 100)
            dragToWidth(70)
            tryCompare(split, "expanded", false)
            tryCompare(split, "referenceWidth", 0)
            compare(split.referenceDocumentId, bId)
            compare(split.referenceSession, session)
            expand()
            compare(split.referenceView, view)
            compare(split.referenceSession, session)
            split.collapse()
            tryCompare(split, "expanded", false)
            expand()
            compare(compareDocuments.activeId(), aId)
        }

        function test_actual_selector_mouse_unnamed_and_binding_sync() {
            expand()
            compare(split.choices.length, 3) // placeholder + two real unnamed charts
            const a = split.choices[indexOf(aId)]
            const b = split.choices[indexOf(bId)]
            compare(a.path, "")
            compare(b.path, "")
            verify(a.text !== b.text, "Two unnamed documents must be distinguishable")
            mouseChoose(aId)
            boundTo(split.referenceView, compareDocuments.session(aId))
            compare(compareDocuments.activeId(), aId)
            mouseChoose(bId)
            compare(compareDocuments.activeId(), aId)
            // Mouse activation must not sever synchronization with public selection.
            verify(split.selectReference(aId))
            tryCompare(selector, "currentIndex", indexOf(aId))
            compare(selector.displayText, a.text)
            verify(!split.selectReference("not-an-open-document"))
            compare(split.referenceDocumentId, aId)
        }

        function test_actual_selector_keyboard() {
            expand()
            verify(split.selectReference(aId))
            tryCompare(selector, "currentIndex", indexOf(aId))
            selector.forceActiveFocus()
            compare(split.referenceFocused, true)
            keyClick(Qt.Key_Space)
            tryCompare(selector.popup, "opened", true)
            keyClick(Qt.Key_Home)
            for (let i = 0; i < indexOf(bId); ++i) keyClick(Qt.Key_Down)
            keyClick(Qt.Key_Return)
            tryCompare(selector.popup, "visible", false)
            tryCompare(split, "referenceDocumentId", bId)
            compare(selector.displayText, split.choices[indexOf(bId)].text)
            boundTo(split.referenceView, compareDocuments.session(bId))
            compare(compareDocuments.activeId(), aId)
        }

        function test_reference_selection_scroll_zoom_and_readonly() {
            split.synchronizeViewports = false
            expand()
            const av = split.primaryView
            const bv = split.referenceView
            bv.topHigh = false
            const ap = parts(av)
            const bp = parts(bv)
            ap.chart.scrollToStart()
            bp.chart.scrollToStart()
            const beforeA = state(aId)
            const beforeB = state(bId)
            const note = pointFor(bv, bp, true)
            mouseClick(bv, note.x, note.y)
            compare(split.referenceFocused, true)
            compare(bv.selection.length, 1)
            compare(bp.chart.selection.length, 1)
            compare(bp.chart.selection[0].sample, 37)
            compare(av.selection.length, 0)
            compare(ap.chart.selection.length, 0)
            const ay = ap.chart.scrollY
            const az = av.measureHeight
            const by = bp.chart.scrollY
            mouseWheel(bv, note.x, 300, 0, -120)
            verify(bp.chart.scrollY !== by, "Wheel must really scroll B")
            compare(ap.chart.scrollY, ay)
            const bz = bv.measureHeight
            mouseWheel(bv, note.x, 300, 0, 120, Qt.NoButton, Qt.ControlModifier)
            verify(bv.measureHeight !== bz, "Ctrl+wheel must really zoom B")
            compare(av.measureHeight, az)
            compare(ap.chart.scrollY, ay)
            const bAfter = bp.chart.scrollY
            const bZoomAfter = bv.measureHeight
            av.zoomAt(250, 1.2)
            compare(bp.chart.scrollY, bAfter)
            compare(bv.measureHeight, bZoomAfter)
            bp.chart.scrollToStart()
            const blank = pointFor(bv, bp, false)
            bv.editorTool = "note"
            compare(bv.editable, false)
            compare(bp.chart.previewNoteKind, "")
            mouseClick(bv, blank.x, blank.y)
            bv.editorTool = "select"
            const hit = pointFor(bv, bp, true)
            mouseClick(bv, hit.x, hit.y, Qt.RightButton)
            mouseDoubleClickSequence(bv, hit.x, hit.y, Qt.LeftButton)
            bv.moveMode = true
            mouseDrag(bv, hit.x, hit.y, 20, 48, Qt.LeftButton)
            compare(placeSpy.count, 0)
            compare(deleteSpy.count, 0)
            compare(editSpy.count, 0)
            compare(moveSpy.count, 0)
            compare(state(aId), beforeA)
            compare(state(bId), beforeB)
            mouseClick(av, 200, 200)
            compare(split.referenceFocused, false)
            mouseClick(bv, 200, 200, Qt.RightButton)
            compare(split.referenceFocused, true)
            split.collapse()
            compare(split.referenceFocused, false)
        }

        function test_reference_tool_navigation_data() {
            return [{tag: "pan", tool: "pan", boxes: false},
                    {tag: "select", tool: "select", boxes: true},
                    {tag: "note", tool: "note", boxes: false},
                    {tag: "ln", tool: "ln", boxes: false},
                    {tag: "mine", tool: "mine", boxes: false}]
        }
        function test_reference_tool_navigation(data) {
            split.synchronizeViewports = false
            expand()
            split.editorTool = data.tool
            const bv = split.referenceView
            compare(bv.editorTool, data.tool)
            bv.topHigh = false
            const bp = parts(bv)
            const ap = parts(split.primaryView)
            bp.chart.scrollToStart()
            verify(waitForRendering(scene))
            const blank = pointFor(bv, bp, false)
            const ay = ap.chart.scrollY
            const by = bp.chart.scrollY
            const a = state(aId), b = state(bId)
            mouseDrag(bv, blank.x, blank.y, 0, -48, Qt.LeftButton)
            if (data.boxes) compare(bp.chart.scrollY, by)
            else verify(bp.chart.scrollY !== by, "Blank-space drag must pan B for " + data.tool)
            compare(ap.chart.scrollY, ay)
            compare(state(aId), a)
            compare(state(bId), b)
            compare(placeSpy.count + deleteSpy.count + editSpy.count + moveSpy.count, 0)
        }
        function test_reference_secondary_channels_toggle() {
            expand()
            const av = split.primaryView, bv = split.referenceView
            const ap = parts(av), bp = parts(bv)
            compare(split.hideSecondaryChannels, true)
            compare(bp.chart.hideSecondaryChannels, true)
            compare(ap.chart.hideSecondaryChannels, false)
            bv.showExtras = true
            const a = state(aId), b = state(bId)
            const ac = ap.chart.columnCount
            const compactCount = bp.chart.columnCount
            split.hideSecondaryChannels = false
            verify(bp.chart.columnCount > compactCount)
            verify(waitForRendering(scene))
            let columns = bv.probe(0, 0).columns
            verify(columns.some(c => c.label === "BPM"))
            verify(columns.some(c => c.label === "STOP"))
            split.hideSecondaryChannels = true
            compare(bp.chart.columnCount, compactCount)
            verify(waitForRendering(scene))
            columns = bv.probe(0, 0).columns
            compare(columns.length, 8) // SP7K: scratch + seven performance keys only
            verify(columns.every(c => c.label === "S" || /^[1-7]$/.test(c.label)))
            compare(ap.chart.columnCount, ac)
            compare(state(aId), a)
            compare(state(bId), b)
        }

        function test_reference_never_owns_playback_even_when_same_as_A() {
            expand()
            compare(split.primaryView.ownsPlayback, true) // positive control: live A
            compare(split.referenceView.ownsPlayback, false)
            verify(split.selectReference(aId))
            compare(split.referenceSession, split.primarySession)
            boundTo(split.referenceView, split.primarySession)
            compare(split.referenceView.editable, false)
            compare(split.referenceView.ownsPlayback, false)
            split.requestSwap()
            compare(swapSpy.count, 0) // same-document swap is a no-op
        }

        function test_swap_signal_then_explicit_activation_preserves_documents() {
            expand()
            verify(bindingFixture.seedCompareHistory())
            flushSync()
            const oldA = split.primarySession
            const oldB = split.referenceSession
            const beforeA = state(aId)
            const beforeB = state(bId)
            const history = bindingFixture.documentState(aId)
            verify(history.dirty)
            verify(history.undo > 0 && history.redo > 0)
            const hashA = oldA.debugContentHash()
            const hashB = oldB.debugContentHash()
            split.referenceView.selection = [{sample: 37}]
            split.requestSwap()
            compare(swapSpy.count, 1)
            compare(swapSpy.signalArguments[0][0], bId)
            // No implicit activation/command: models a cancelled Main confirmation.
            compare(compareDocuments.activeId(), aId)
            compare(split.primarySession, oldA)
            compare(split.referenceSession, oldB)
            compare(state(aId), beforeA)
            compare(state(bId), beforeB)
            // Main's confirmed continuation is explicit; do not fake a swap handler.
            verify(compareDocuments.activate(bId).ok)
            verify(split.selectReference(aId))
            tryCompare(split, "primarySession", oldB)
            compare(split.referenceSession, oldA)
            boundTo(split.primaryView, oldB)
            boundTo(split.referenceView, oldA)
            compare(split.referenceView.selection.length, 0)
            compare(split.referenceView.ownsPlayback, false)
            compare(oldA.debugContentHash(), hashA)
            compare(oldB.debugContentHash(), hashB)
            compare(state(aId), beforeA)
            compare(state(bId), beforeB)
        }
    }
}
