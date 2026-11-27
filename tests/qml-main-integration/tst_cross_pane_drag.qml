// SPDX-License-Identifier: GPL-3.0-only
import QtQuick
import QtQuick.Controls
import QtTest
import "../../app/qml" as Production

// Use the production window, registry, command dispatcher and mouse grab. Geometry
// is discovered through the renderer's queries; no test-side move/snap planner.
TestCase {
    id: test
    name: "ProductionMainCrossPaneDrag"
    when: windowShown
    property var main: null
    property var split: null
    property var primary: null
    property var reference: null
    property var pressedViewport: null
    property point releasePoint: Qt.point(0, 0)
    readonly property string a: "main-integration-a"
    readonly property string b: "main-integration-b"
    Component { id: mainComponent; Production.Main {} }
    SignalSpy { id: committed; target: paneDragController; signalName: "selectionCommitted" }

    function command(id, name, args) {
        const response = JSON.parse(beatbench.dispatch(JSON.stringify({command: name,
            args: Object.assign({}, args || {}, {session_id: id})})))
        verify(response.ok, name + ": " + JSON.stringify(response.error))
        documents.refresh()
        return response.result
    }
    function start(sameDocument) {
        audioEngine.playing = false
        audioEngine.hasPcm = false
        audioEngine.positionSec = 0
        audioEngine.allowPreview = false
        mainFixture.reset(false)
        chartHistoryController.enabled = false
        // Leave both complete key banks visible after the production side docks
        // and compare pane consume their normal widths. Never bypass hit testing.
        main = mainComponent.createObject(null, {width: 1800, height: 900})
        verify(main !== null)
        mainFixture.bind(main)
        main.requestActivate()
        main.adoptChart(mainFixture.snapshot(a).path)
        wait(0)
        split = mainFixture.findProperty(main, "referenceDocumentId")
        verify(split !== null)
        split.synchronizeViewports = false
        split.referenceShare = 500 / split.usableWidth
        verify(split.selectReference(sameDocument ? a : b))
        tryVerify(() => split.referenceWidth > 450)
        primary = mainFixture.primaryView(main)
        reference = split.referenceView
        verify(primary !== null && reference !== null)
        main.editorTool = "select"
        main.setVerticalMoveMode(2)
        main.lnSelectMode = false
        tryCompare(primary, "editable", true)
        tryCompare(reference, "editable", true)
        compare(main.managedDragController, paneDragController)
        compare(primary.managedDragController, paneDragController)
        compare(reference.managedDragController, paneDragController)
        primary.topHigh = false
        reference.topHigh = false
        primary.measureHeight = 128
        reference.measureHeight = 192
        primary.viewportItem.scrollToStart()
        reference.viewportItem.scrollToStart()
        verify(waitForRendering(primary))
        committed.clear()
    }
    function cleanup() {
        // A failed assertion must not leave a Qt mouse grab for the next test.
        paneDragController.cancelActive("test-cleanup")
        if (pressedViewport) {
            mouseRelease(pressedViewport, releasePoint.x, releasePoint.y, Qt.LeftButton)
            pressedViewport = null
        }
        if (main) { main.visible = false; main.destroy(); main = null; wait(0) }
        compare(audioEngine.playbackCalls, 0)
    }
    function seed() {
        for (const note of [
            {pos: {num: 1, den: 4}, lane: 3, sample: 11, kind: "ln"},
            {pos: {num: 3, den: 4}, lane: 3, sample: 12, kind: "ln"},
            {pos: {num: 1, den: 4}, lane: 4, sample: 13, kind: "normal"},
            {pos: {num: 1, den: 4}, lane: 5, sample: 14, kind: "mine"}
        ]) command(a, "note.put", {measure: 0, pos: note.pos,
            lane: {player: 0, kind: "key", index: note.lane}, sample: note.sample, kind: note.kind})
        primary.viewportItem.scrollToStart()
        reference.viewportItem.scrollToStart()
        wait(0)
    }
    function refs(view) {
        // The fixture is intentionally tiny. Include offscreen caps too, retaining
        // complete NoteRefs (LN source channel, note kind and sub_line included).
        return view.viewportItem.notesInRect(-10000, -10000, 10000, 10000)
    }
    function xForLane(viewport, index) {
        let first = -1, last = -1
        for (let x = Math.ceil(viewport.rulerWidth + 1); x < viewport.width - 5; ++x) {
            const lane = viewport.laneAtX(x)
            if (lane.valid && lane.laneKind === "key" && lane.lanePlayer === 0 && lane.laneIndex === index) {
                if (first < 0) first = x
                last = x
            }
        }
        verify(first >= 0, "Missing visible key lane " + index)
        return Math.round((first + last) / 2)
    }
    function pressPoint(view) {
        const viewport = view.viewportItem, x = xForLane(viewport, 3)
        let best = -1, error = 1000
        for (let y = 24; y < viewport.height - 12; ++y) {
            const hit = viewport.objectAt(x, y)
            if (hit.valid && hit.kind === "note" && hit.sample === 11) {
                const distance = Math.abs(viewport.measureAtY(y) - 0.25)
                if (distance < error) { best = y; error = distance }
            }
        }
        verify(best >= 0, "Missing visible LN head cap")
        // Anchor the pointer near the note's musical line, not the top of its
        // 12px cap: that cap offset is 12/128 vs 12/192 measures at these zooms.
        // Starting at the first hit would put A->B on a 1/4-grid half-way tie.
        verify(error < 0.01, "Press must be within one pixel of the LN musical line")
        return Qt.point(x, best)
    }
    function targetPoint(view) {
        const viewport = view.viewportItem, x = xForLane(viewport, 4)
        let best = 0, error = 1000
        // Deliberately off-grid: 0.53 snaps to target's 1/4, not source's 1/16.
        for (let y = 24; y < viewport.height - 12; ++y) {
            const sample = viewport.dragSampleAt(x, y)
            if (sample.valid && Math.abs(sample.musicalF - 0.53) < error) {
                best = y; error = Math.abs(sample.musicalF - 0.53)
            }
        }
        verify(error < 0.015, "Target musical location is visible")
        return Qt.point(x, best)
    }
    function compareRef(actual, expected, label) {
        const detail = label + " actual=" + JSON.stringify(actual) + " expected=" + JSON.stringify(expected)
        // Per-field failures expose values that Qt otherwise prints only as
        // V4ReferenceObject; retain the complete comparison and key-set check.
        compare(Object.keys(actual).sort(), Object.keys(expected).sort(), detail + " keys")
        for (const field of Object.keys(expected)) compare(actual[field], expected[field], detail + " field=" + field)
        compare(actual, expected, detail)
    }
    function select(view, selected) {
        if (view === primary) main.selectionRefs = selected
        else referencePaneContext.selection = selected
        tryCompare(view, "selection", selected)
    }
    function beginDrag(source, target) {
        const viewport = source.viewportItem, press = pressPoint(source), drop = targetPoint(target)
        releasePoint = target.viewportItem.mapToItem(viewport, drop.x, drop.y)
        pressedViewport = viewport
        mousePress(viewport, press.x, press.y, Qt.LeftButton)
        mouseMove(viewport, press.x + 12, press.y, 20)
        tryCompare(paneDragController, "active", true)
        // Keep the original source mouse grab even though coordinates leave it.
        mouseMove(viewport, releasePoint.x, releasePoint.y, 20)
    }
    function finishDrag() {
        mouseRelease(pressedViewport, releasePoint.x, releasePoint.y, Qt.LeftButton)
        pressedViewport = null
        tryCompare(paneDragController, "active", false)
        compare(primary.viewportItem.movePreview, false)
        compare(reference.viewportItem.movePreview, false)
    }
    function directions() {
        return [{tag: "A-to-B", reverse: false}, {tag: "B-to-A", reverse: true}]
    }
    function test_sameDocument_explicitSelection_oneMoveUndo_data() {
        const rows = []
        for (const direction of directions())
            for (const both of [false, true]) rows.push({tag: direction.tag + (both ? "-both-LN-ends" : "-head-only"),
                                                       reverse: direction.reverse, both: both})
        return rows
    }
    function test_sameDocument_explicitSelection_oneMoveUndo(data) {
        start(true)
        seed()
        const source = data.reverse ? reference : primary, target = data.reverse ? primary : reference
        source.snapNum = 1; source.snapDen = 16
        target.snapNum = 1; target.snapDen = 4
        target.viewportItem.scrollY = 17
        verify(source.measureHeight !== target.measureHeight)
        verify(source.viewportItem.scrollY !== target.viewportItem.scrollY)
        const beforeRefs = refs(source), before = mainFixture.snapshot(a), other = mainFixture.snapshot(b)
        compare(beforeRefs.length, 5)
        const selected = beforeRefs.filter(n => n.sample === 11 || n.sample === 13 || n.sample === 14
                                          || (data.both && n.sample === 12))
        select(source, selected)
        beginDrag(source, target)
        compare(mainFixture.snapshot(a), before) // Preview is never an edit.
        finishDrag()
        compare(committed.count, 1)
        const after = mainFixture.snapshot(a), moved = refs(target)
        compare(after.undo, before.undo + 1)
        compare(after.redo, 0)
        compare(after.definitions, before.definitions)
        compare(after.meta, before.meta)
        compare(moved.length, beforeRefs.length)
        for (const sample of [11, 13, 14]) {
            const note = moved.find(n => n.sample === sample)
            const original = beforeRefs.find(n => n.sample === sample)
            verify(note !== undefined)
            compareRef(note, Object.assign({}, original, {pos: {num: 1, den: 2},
                lane: {player: 0, kind: "key", index: sample === 11 ? 4 : sample === 13 ? 5 : 6}}),
                data.tag + " moved sample=" + sample)
        }
        const tail = moved.find(n => n.sample === 12), oldTail = beforeRefs.find(n => n.sample === 12)
        compareRef(tail, data.both ? Object.assign({}, oldTail, {measure: 1, pos: {num: 0, den: 1},
            lane: {player: 0, kind: "key", index: 4}}) : oldTail, data.tag + " LN tail")
        compare(moved.find(n => n.sample === 1), beforeRefs.find(n => n.sample === 1))
        compare(source.selection.length, 0)
        compare(target.selection.length, selected.length)
        compare(refs(primary), refs(reference))
        compare(documents.activeId(), a)
        compare(mainFixture.snapshot(b), other)
        // One production undo restores the whole identity set, not just positions.
        verify(uiActions.invoke("edit.undo", {}))
        compare(mainFixture.snapshot(a).undo, before.undo)
        compare(mainFixture.snapshot(a).notes, before.notes)
        compare(refs(primary), beforeRefs)
        compare(refs(reference), beforeRefs)
        compare(mainFixture.snapshot(b), other)
    }
    function test_referenceLocalMove_preservesOtherDocumentPaneState() {
        start(false)
        seed()
        command(b, "note.put", {measure: 0, pos: {num: 1, den: 4},
            lane: {player: 0, kind: "key", index: 3}, sample: 11, kind: "normal"})
        const pending = refs(primary).find(n => n.sample === 11)
        verify(pending !== undefined)
        main.pendingLnHead = pending
        const selectedA = refs(primary).filter(n => n.sample === 13)
        main.selectionRefs = selectedA
        const selectedMeta = {kind: "bpm", measure: 2, pos: {num: 0, den: 1}, value: 180}
        command(a, "timing.put", selectedMeta)
        main.metaSelection = [selectedMeta]
        select(reference, refs(reference).filter(n => n.sample === 11))
        const beforeA = mainFixture.snapshot(a), beforeB = mainFixture.snapshot(b)
        beginDrag(reference, reference)
        finishDrag()
        compare(committed.count, 1)
        compare(mainFixture.snapshot(b).undo, beforeB.undo + 1)
        compare(mainFixture.snapshot(a), beforeA)
        compare(main.pendingLnHead, pending)
        compare(main.selectionRefs, selectedA)
        compare(main.metaSelection, [selectedMeta])
        compare(documents.activeId(), a)
    }
    function test_differentDocument_dropIsNoOp_data() { return directions() }
    function test_differentDocument_dropIsNoOp(data) {
        start(false)
        seed()
        const source = data.reverse ? reference : primary, target = data.reverse ? primary : reference
        if (data.reverse) {
            // Seed the same pointer fixture in B through the real command registry.
            command(b, "note.put", {measure: 0, pos: {num: 1, den: 4},
                lane: {player: 0, kind: "key", index: 3}, sample: 11, kind: "ln"})
        }
        select(source, refs(source).filter(n => n.sample === 11))
        const beforeA = mainFixture.snapshot(a), beforeB = mainFixture.snapshot(b)
        const refsA = refs(primary), refsB = refs(reference)
        beginDrag(source, target)
        finishDrag()
        compare(committed.count, 0)
        compare(mainFixture.snapshot(a), beforeA)
        compare(mainFixture.snapshot(b), beforeB)
        compare(refs(primary), refsA)
        compare(refs(reference), refsB)
        compare(documents.activeId(), a)
    }
    function test_escape_cancelsGrab_withoutReleaseFallback_data() { return directions() }
    function test_escape_cancelsGrab_withoutReleaseFallback(data) {
        start(true)
        seed()
        const source = data.reverse ? reference : primary, target = data.reverse ? primary : reference
        select(source, refs(source).filter(n => n.sample === 11 || n.sample === 12))
        const beforeA = mainFixture.snapshot(a), beforeB = mainFixture.snapshot(b), beforeRefs = refs(primary)
        beginDrag(source, target)
        keyClick(Qt.Key_Escape)
        tryCompare(paneDragController, "active", false)
        finishDrag()
        compare(committed.count, 0)
        compare(mainFixture.snapshot(a), beforeA)
        compare(mainFixture.snapshot(b), beforeB)
        compare(refs(primary), beforeRefs)
        compare(refs(reference), beforeRefs)
    }
}
