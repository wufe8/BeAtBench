// SPDX-License-Identifier: GPL-3.0-only
import QtQuick
import QtTest
import "../../app/qml/components" as Components

Item {
    id: scene
    width: 1100
    height: 620
    property int otherReleases: 0
    Rectangle {
        x: 570; width: 530; height: 600
        color: "transparent"
        MouseArea { anchors.fill: parent; onReleased: scene.otherReleases++ }
    }
    Component {
        id: surface
        Components.ChartView {
            x: 20; y: 10; width: 530; height: 600
            topHigh: false
            documentSession: bridgeA
            editorTool: "select"
            onNoteClicked: (ref, ctrl) => { selection = [ref] }
        }
    }
    TestCase {
        id: test
        name: "ManagedNoteDragSurface"
        when: windowShown
        property var source: null
        SignalSpy { id: moveSpy; target: test.source; signalName: "moveSelectionRequested" }
        SignalSpy { id: metaSpy; target: test.source; signalName: "metaMoveRequested" }
        SignalSpy { id: placeSpy; target: test.source; signalName: "hitPlaceRequested" }
        SignalSpy { id: auditionSpy; target: test.source; signalName: "playNoteSample" }
        SignalSpy { id: deleteSpy; target: test.source; signalName: "noteRightDeleted" }

        function init() {
            verify(bindingFixture.activate(false))
            source = createTemporaryObject(surface, scene)
            verify(source !== null)
            verify(waitForRendering(scene))
            source.viewportItem.scrollToStart()
            scene.otherReleases = 0
            moveSpy.clear(); metaSpy.clear(); placeSpy.clear(); auditionSpy.clear(); deleteSpy.clear()
        }
        function cleanup() {
            if (source) source.cancelManagedDrag("test-cleanup")
        }
        function notePoint() {
            const chart = source.viewportItem
            const columns = source.probe(0, 40).columns
            for (let c = 0; c < columns.length; ++c) {
                const x = Math.floor(columns[c].x + columns[c].w / 2)
                if (x <= chart.rulerWidth || x >= chart.width - 4) continue
                for (let y = 30; y < chart.height - 8; y += 2) {
                    const note = chart.objectAt(x, y)
                    if (note.valid && note.kind !== "bga" && note.kind !== "bpm" && note.kind !== "stop")
                        return {x: x, y: y, note: note}
                }
            }
            fail("No real note in production viewport")
            return null
        }
        function interactionArea() {
            for (let i = 0; i < source.children.length; ++i)
                if (source.children[i].objectName === "chartInteractionArea") return source.children[i]
            fail("Missing production MouseArea")
            return null
        }
        function controller(accepted, failure) {
            const record = {begins: [], updates: [], finishes: [], cancels: []}
            record.begin = function(view, press, current, ref, selection) {
                record.begins.push({view: view, press: Qt.point(press.x, press.y),
                                    current: Qt.point(current.x, current.y), ref: ref,
                                    selection: selection, selectedAtBegin: view.selection.length})
                view.viewportItem.movePreview = true // external controller, not the legacy ghost path
                if (failure === "begin") throw new Error("begin failed")
                return accepted
            }
            record.update = function(view, point) {
                record.updates.push({view: view, point: Qt.point(point.x, point.y)})
                if (failure === "update") throw new Error("update failed")
            }
            record.finish = function(view, point) {
                record.finishes.push({view: view, point: Qt.point(point.x, point.y)})
                if (failure === "finish") throw new Error("finish failed")
            }
            record.cancel = function(view, reason) { record.cancels.push({view: view, reason: reason}) }
            return record
        }
        function noLegacyFallback() {
            compare(moveSpy.count, 0)
            compare(metaSpy.count, 0)
            compare(placeSpy.count, 0)
            compare(auditionSpy.count, 0)
            compare(deleteSpy.count, 0)
            compare(source._moving, false)
            compare(source.viewportItem.movePreview, false)
            compare(source.viewportItem.moveSourceSubLine, -1)
        }
        function beginDrag(record) {
            source.managedDragController = record
            const point = notePoint()
            mousePress(source, point.x, point.y, Qt.LeftButton)
            compare(record.begins.length, 0)
            compare(source.selection.length, 1)
            mouseMove(source, point.x + 6, point.y, 20)
            compare(record.begins.length, 1)
            compare(source.managedDragConsumed, true)
            return point
        }
        function test_null_controller_preserves_legacy_move() {
            compare(source.managedDragController, null)
            const point = notePoint()
            mouseDrag(source, point.x, point.y, 0, 48, Qt.LeftButton)
            compare(moveSpy.count, 1)
            compare(placeSpy.count, 0)
            compare(auditionSpy.count, 0)
            compare(source.managedDragConsumed, false)
        }
        function test_click_and_four_pixel_motion_preserve_audition_data() {
            return [{tag: "click", dx: 0, dy: 0}, {tag: "four-pixels", dx: 2, dy: 2}]
        }
        function test_click_and_four_pixel_motion_preserve_audition(data) {
            const record = controller(true, "")
            source.managedDragController = record
            const point = notePoint()
            mousePress(source, point.x, point.y, Qt.LeftButton)
            mouseMove(source, point.x + data.dx, point.y + data.dy, 20)
            compare(record.begins.length, 0)
            mouseRelease(source, point.x + data.dx, point.y + data.dy, Qt.LeftButton)
            compare(record.begins.length, 0)
            compare(record.finishes.length, 0)
            compare(auditionSpy.count, 1)
            compare(moveSpy.count, 0)
            compare(placeSpy.count, 0)
        }
        function test_pan_tool_does_not_enter_managed_note_drag() {
            const record = controller(true, "")
            source.managedDragController = record
            source.editorTool = "pan"
            const point = notePoint()
            mouseDrag(source, point.x, point.y, 0, 40, Qt.LeftButton)
            compare(record.begins.length, 0)
            compare(record.finishes.length, 0)
            compare(source.managedDragConsumed, false)
            noLegacyFallback()
        }
        function test_managed_drag_keeps_source_grab_and_viewport_points() {
            const record = controller(true, "")
            const point = beginDrag(record)
            compare(record.begins[0].view, source)
            compare(record.begins[0].selectedAtBegin, 1)
            compare(record.begins[0].selection.length, 1)
            compare(record.begins[0].selection[0].sample, point.note.sample)
            compare(record.begins[0].ref.sample, point.note.sample)
            source.selection = []
            compare(record.begins[0].selection.length, 1, "Controller receives a selection snapshot, not the live array")
            compare(record.begins[0].press, Qt.point(point.x, point.y))
            compare(record.begins[0].current, Qt.point(point.x + 6, point.y))
            const sceneY = source.y + point.y + 30
            mouseMove(scene, 850, sceneY, 20)
            verify(record.updates.length > 0)
            const last = record.updates[record.updates.length - 1]
            compare(last.view, source)
            verify(last.point.x > source.width, "Source must continue receiving out-of-bounds pointer motion")
            const expected = source.viewportItem.mapFromItem(scene, 850, sceneY)
            compare(last.point, Qt.point(expected.x, expected.y))
            mouseRelease(scene, 850, sceneY, Qt.LeftButton)
            compare(record.finishes.length, 1)
            compare(record.finishes[0].point, Qt.point(expected.x, expected.y))
            compare(record.cancels.length, 0)
            compare(scene.otherReleases, 0)
            compare(source.managedDragConsumed, false)
            noLegacyFallback()
        }
        function test_production_lane_refs_serialize_as_json_numbers() {
            const point = notePoint()
            // The fixture serializes C++ output directly, before QML can coerce QVariant UChar.
            const result = JSON.parse(bindingFixture.chartLaneJson(source.viewportItem, point.x, point.y))
            function checkLane(lane, playerKey, indexKey) {
                compare(typeof lane[playerKey], "number", playerKey + " must be a JSON number")
                compare(typeof lane[indexKey], "number", indexKey + " must be a JSON number")
                verify(Number.isInteger(lane[playerKey]))
                verify(Number.isInteger(lane[indexKey]))
            }
            verify(result.note.valid)
            verify(result.object.valid)
            verify(result.selected.length > 0)
            for (const ref of [result.note, result.object].concat(result.selected)) {
                checkLane(ref.lane, "player", "index")
                compare(ref.lane.player, 0)
                compare(ref.lane.index, 1)
            }
            verify(result.hit.valid)
            verify(result.lane.valid)
            verify(result.drag.valid)
            verify(result.columns.length > 0)
            verify(result.performanceColumns.length > 0)
            for (const lane of [result.hit, result.lane, result.drag.column]
                               .concat(result.columns, result.performanceColumns))
                checkLane(lane, "lanePlayer", "laneIndex")
        }
        function test_selected_group_is_preserved_for_managed_drag_data() {
            return [{tag: "drag", drag: true}, {tag: "click", drag: false}]
        }
        function test_selected_group_is_preserved_for_managed_drag(data) {
            const record = controller(true, "")
            source.managedDragController = record
            const point = notePoint()
            // Both real fixture notes share lane/sample/position, but have distinct measures.
            const distant = Object.assign({}, point.note, {measure: 24})
            const group = [point.note, distant]
            source.selection = group
            const snapshot = JSON.stringify(group)
            mousePress(source, point.x, point.y, Qt.LeftButton)
            compare(JSON.stringify(source.selection), snapshot, "Press must not collapse a selected drag group")
            if (data.drag) {
                mouseMove(source, point.x + 6, point.y, 20)
                compare(record.begins.length, 1)
                compare(record.begins[0].selectedAtBegin, 2)
                compare(JSON.stringify(record.begins[0].selection), snapshot, "Full note identity reaches begin")
            }
            mouseRelease(source, point.x + (data.drag ? 6 : 0), point.y, Qt.LeftButton)
            if (data.drag) {
                compare(record.finishes.length, 1)
                compare(JSON.stringify(source.selection), snapshot)
                noLegacyFallback()
            } else {
                compare(record.begins.length, 0)
                compare(record.finishes.length, 0)
                compare(source.selection.length, 1, "A plain click retains existing collapse semantics")
                compare(auditionSpy.count, 1)
            }
        }
        function test_secondary_button_during_drag_cannot_delete_or_pan_data() {
            return [
                {tag: "right-first", button: Qt.RightButton, secondaryFirst: true},
                {tag: "middle-first", button: Qt.MiddleButton, secondaryFirst: true},
                {tag: "left-before-right", button: Qt.RightButton, secondaryFirst: false},
                {tag: "left-before-middle", button: Qt.MiddleButton, secondaryFirst: false}
            ]
        }
        function test_secondary_button_during_drag_cannot_delete_or_pan(data) {
            const record = controller(true, "")
            source.managedDragController = record
            const point = notePoint()
            const area = interactionArea()
            const scrollX = source.viewportItem.scrollX
            const scrollY = source.viewportItem.scrollY
            // Qt 6.11 QuickTest mousePress sends buttons=button and mouseRelease sends
            // buttons=NoButton (src/qmltest/quicktestevent.cpp). Use real QMouseEvents
            // with the full chord mask, through QQuickWindow delivery/grab handling.
            function send(action, button, buttons, dx, dy) {
                verify(bindingFixture.sendPointerEvent(source, point.x + dx, point.y + dy,
                                                       button, buttons, action))
            }
            send("press", Qt.LeftButton, Qt.LeftButton, 0, 0)
            send("move", Qt.NoButton, Qt.LeftButton, 6, 0)
            compare(record.begins.length, 1)
            compare(source.managedDragConsumed, true)
            send("press", data.button, Qt.LeftButton | data.button, 6, 0)
            compare(deleteSpy.count, 0)
            compare(source._panning, false)
            compare(record.cancels.length, 0, "A secondary press must not re-arm the legacy path")
            compare(area.pressedButtons, Qt.LeftButton | data.button)
            send("move", Qt.NoButton, Qt.LeftButton | data.button, 12, 20)
            verify(record.updates.length > 0)
            compare(source.viewportItem.scrollX, scrollX)
            compare(source.viewportItem.scrollY, scrollY)
            if (data.secondaryFirst) {
                send("release", data.button, Qt.LeftButton, 12, 20)
                compare(source.managedDragConsumed, true)
                compare(record.finishes.length, 0)
                compare(area.pressedButtons, Qt.LeftButton)
            }
            send("release", Qt.LeftButton, data.secondaryFirst ? Qt.NoButton : data.button, 12, 20)
            compare(record.finishes.length, 1)
            compare(record.finishes[0].point, Qt.point(point.x + 12, point.y + 20))
            if (!data.secondaryFirst)
                send("release", data.button, Qt.NoButton, 12, 20)
            compare(record.finishes.length, 1, "A trailing secondary release must not finish twice")
            compare(record.cancels.length, 0)
            compare(source.managedDragConsumed, false)
            compare(area.pressedButtons, Qt.NoButton)
            compare(source._panning, false)
            compare(source.viewportItem.scrollX, scrollX)
            compare(source.viewportItem.scrollY, scrollY)
            noLegacyFallback()
        }
        function test_cancel_or_invalid_never_falls_back_after_restore_data() {
            return ["rejected", "external", "escape", "read-only", "hidden", "disabled",
                    "document", "overlay", "controller", "pointer-cancel"].map(value => ({tag: value, reason: value}))
        }
        function test_cancel_or_invalid_never_falls_back_after_restore(data) {
            source.editorTool = "note" // a stale release must not fall into placement
            const record = controller(data.reason !== "rejected", "")
            const point = beginDrag(record)
            if (data.reason === "external") source.cancelManagedDrag("external")
            else if (data.reason === "escape") keyClick(Qt.Key_Escape)
            else if (data.reason === "read-only") { source.editable = false; source.editable = true }
            else if (data.reason === "hidden") { source.visible = false; source.visible = true }
            else if (data.reason === "disabled") { source.enabled = false; source.enabled = true }
            else if (data.reason === "document") { source.documentSession = bridgeB; source.documentSession = bridgeA }
            else if (data.reason === "overlay") { source.referenceOverlayEnabled = true; source.referenceOverlayEnabled = false }
            else if (data.reason === "controller") source.managedDragController = controller(true, "")
            else if (data.reason === "pointer-cancel") interactionArea().canceled()
            compare(record.cancels.length, 1)
            compare(source.managedDragConsumed, true)
            compare(source._managedDragActive, false)
            // Even if a lost grab later delivers a stale release, it must remain consumed.
            mouseRelease(source, point.x, point.y, Qt.LeftButton)
            if (source.managedDragConsumed) source.handleRelease(point.x, point.y)
            compare(record.finishes.length, 0)
            compare(source.managedDragConsumed, false)
            noLegacyFallback()
        }
        function test_controller_exception_is_consumed_data() {
            return ["begin", "update", "finish"].map(value => ({tag: value, failure: value}))
        }
        function test_controller_exception_is_consumed(data) {
            const record = controller(true, data.failure)
            const point = beginDrag(record)
            if (data.failure === "update") mouseMove(source, point.x + 12, point.y, 20)
            mouseRelease(source, point.x, point.y, Qt.LeftButton)
            compare(record.cancels.length, 1)
            compare(record.finishes.length, data.failure === "finish" ? 1 : 0)
            compare(source.managedDragConsumed, false)
            noLegacyFallback()
        }
    }
}
