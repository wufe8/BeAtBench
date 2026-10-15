// SPDX-License-Identifier: GPL-3.0-only
import QtQuick
import QtTest
import "../../app/qml/components" as Components

Item {
    id: scene
    width: 1100
    height: 620

    Component {
        id: defaultView
        Components.ChartView {
            width: 530; height: 600
            topHigh: false
            // Selection is consumer-owned, just as in the real editor.
            onNoteClicked: (ref, ctrl) => { selection = [ref] }
        }
    }
    Component {
        id: explicitView
        Components.ChartView {
            x: 550; width: 530; height: 600
            topHigh: false
            documentSession: bridgeB
            onNoteClicked: (ref, ctrl) => { selection = [ref] }
        }
    }

    TestCase {
        id: testCase
        name: "ChartViewDocumentBinding"
        when: windowShown
        property var viewA
        property var viewB
        property var partsA
        property var partsB

        SignalSpy { id: placeSpy; target: testCase.viewB; signalName: "hitPlaceRequested" }
        SignalSpy { id: deleteSpy; target: testCase.viewB; signalName: "noteRightDeleted" }
        SignalSpy { id: editSpy; target: testCase.viewB; signalName: "noteEditRequested" }

        function inspect(view) {
            const parts = bindingFixture.childrenOf(view)
            compare(parts.chartCount, 1)
            compare(parts.overlayCount, 1)
            compare(parts.waveformCount, 1)
            return parts
        }
        function compareSessions(parts, expected) {
            compare(parts.chart.session, expected)
            compare(parts.overlay.session, expected)
            compare(parts.waveform.session, expected)
        }
        // Probe the real lane layout, then hit-test real notes/empty placement
        // slots. Never assume a theme-dependent ruler/lane width or note height.
        function pointFor(view, parts, wantNote) {
            const columns = view.probe(0, 40).columns
            for (let c = 0; c < columns.length; ++c) {
                const x = columns[c].x + columns[c].w / 2
                if (x <= parts.chart.rulerWidth || x >= parts.chart.width - 4) continue
                for (let y = 30; y < parts.chart.height - 8; y += 2) {
                    const hit = parts.chart.hitTest(x, y)
                    if (!hit.valid || hit.laneKind !== "key") continue
                    const note = parts.chart.noteAt(x, y)
                    if (note.valid === wantNote)
                        return { x: x, y: y, note: note }
                }
            }
            fail("No " + (wantNote ? "note" : "empty key-lane slot") + " in production view")
            return null
        }
        function init() {
            verify(bindingFixture.activate(false))
            viewA = createTemporaryObject(defaultView, scene)
            viewB = createTemporaryObject(explicitView, scene)
            verify(viewA !== null)
            verify(viewB !== null)
            // Both items share one scene/frame. A second sequential frame wait
            // can time out after the first wait has already painted both views.
            verify(waitForRendering(scene))
            partsA = inspect(viewA)
            partsB = inspect(viewB)
            partsA.chart.scrollToStart()
            partsB.chart.scrollToStart()
            placeSpy.clear()
            deleteSpy.clear()
            editSpy.clear()
        }

        function test_explicit_session_survives_active_switch() {
            compare(viewA.documentSession, bridgeA) // legacy context fallback
            compare(viewB.documentSession, bridgeB)
            verify(bridgeA.hasChart && bridgeB.hasChart)
            compare(bridgeA.idBase(), 36)
            compare(bridgeB.idBase(), 62)
            compareSessions(partsA, bridgeA)
            compareSessions(partsB, bridgeB)
            const hashA = bridgeA.debugTimingHash()
            const hashB = bridgeB.debugTimingHash()
            verify(hashA !== hashB, "Fixture must have distinct timing")
            const notePoint = pointFor(viewB, partsB, true)
            compare(notePoint.note.sample, 37)
            for (const second of [true, false, true, false]) {
                verify(bindingFixture.activate(second))
                compareSessions(partsA, bridgeA)
                compareSessions(partsB, bridgeB)
                compare(bridgeA.idBase(), 36)
                compare(bridgeB.idBase(), 62)
                compare(bridgeA.debugTimingHash(), hashA)
                compare(bridgeB.debugTimingHash(), hashB)
                compare(partsB.chart.noteAt(notePoint.x, notePoint.y).sample, 37)
            }
        }

        function test_viewport_and_selection_are_independent() {
            viewB.editorTool = "select"
            viewB.editable = false // read-only still allows selection/navigation
            const note = pointFor(viewB, partsB, true)
            mouseClick(viewB, note.x, note.y, Qt.LeftButton)
            compare(viewB.selection.length, 1)
            compare(partsB.chart.selection.length, 1)
            compare(viewA.selection.length, 0)
            compare(partsA.chart.selection.length, 0)
            const aY = partsA.chart.scrollY
            const aZoom = viewA.measureHeight
            const bY = partsB.chart.scrollY
            mouseWheel(viewB, note.x, 350, 0, -120)
            verify(partsB.chart.scrollY !== bY, "B must really scroll")
            compare(partsA.chart.scrollY, aY)
            viewB.zoomAt(300, 1.5)
            compare(viewB.measureHeight, aZoom * 1.5)
            compare(partsB.chart.measureHeight, viewB.measureHeight)
            compare(viewA.measureHeight, aZoom)
            compare(partsA.chart.scrollY, aY)
            const bAfterZoom = partsB.chart.scrollY
            viewA.zoomAt(250, 1.2)
            compare(viewB.measureHeight, aZoom * 1.5)
            compare(partsB.chart.scrollY, bAfterZoom)
            const aNote = pointFor(viewA, partsA, true)
            mouseClick(viewA, aNote.x, aNote.y, Qt.LeftButton)
            compare(partsA.chart.selection.length, 1)
            compare(partsA.chart.selection[0].sample, 1)
            compare(partsB.chart.selection.length, 1)
            compare(partsB.chart.selection[0].sample, 37)
            viewB.selection = []
            compare(partsA.chart.selection.length, 1)
            compare(partsB.chart.selection.length, 0)
        }

        function test_readonly_denies_mutation_but_editable_emits() {
            const blank = pointFor(viewB, partsB, false)
            viewB.editorTool = "note"
            viewB.editable = false
            compare(partsB.chart.previewNoteKind, "")
            mouseClick(viewB, blank.x, blank.y, Qt.LeftButton)
            compare(placeSpy.count, 0)
            viewB.editable = true
            compare(partsB.chart.previewNoteKind, "normal")
            mouseClick(viewB, blank.x, blank.y, Qt.LeftButton)
            compare(placeSpy.count, 1) // positive control: actual valid MouseArea path
            verify(placeSpy.signalArguments[0][0].valid)

            const note = pointFor(viewB, partsB, true)
            viewB.editorTool = "select"
            viewB.editable = false
            mouseClick(viewB, note.x, note.y, Qt.RightButton)
            mouseDoubleClickSequence(viewB, note.x, note.y, Qt.LeftButton)
            compare(deleteSpy.count, 0)
            compare(editSpy.count, 0)
            viewB.editable = true
            mouseClick(viewB, note.x, note.y, Qt.RightButton)
            compare(deleteSpy.count, 1)
            mouseDoubleClickSequence(viewB, note.x, note.y, Qt.LeftButton)
            compare(editSpy.count, 1)
            // Signals have no command consumer in this fixture: nothing was edited.
            compare(partsB.chart.noteAt(note.x, note.y).sample, 37)
            compare(bridgeB.dirty, false)
        }
    }
}
