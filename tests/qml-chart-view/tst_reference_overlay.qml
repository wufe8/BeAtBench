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
        Components.ChartView {
            width: scene.width
            height: scene.height
            documentSession: bridgeA
            referenceSession: bridgeB
            referenceOverlayEnabled: true
            referenceOverlayColor: "#e040b0"
            referenceOverlayOpacity: 0.45
            topHigh: false
            showGrid: false
            editorTool: "select"
        }
    }

    TestCase {
        id: tc
        name: "ReferenceOverlay"
        when: windowShown
        property var view
        property var chart
        SignalSpy { id: infoSpy; target: tc.chart; signalName: "referenceOverlayInfoChanged" }

        function trace(stage) { bindingFixture.traceOverlayStage(stage) }
        function init() {
            trace("init.begin")
            bindingFixture.resetOverlay("variable")
            trace("init.createView")
            view = createTemporaryObject(surface, scene)
            trace("init.viewCreated")
            verify(view !== null)
            wait(0) // Settle deferred document visuals before native pixel captures.
            const parts = bindingFixture.childrenOf(view)
            compare(parts.chartCount, 1) // Real production ChartView, not a substitute painter.
            chart = parts.chart
            chart.scrollToStart()
            image() // Synchronize initial layout without waiting for an unscheduled frame.
            infoSpy.clear()
            trace("init.end")
        }
        function cleanup() {
            trace("cleanup.begin")
            view.referenceSession = null
            bindingFixture.releaseOverlayCaptures()
            bindingFixture.resetCompare()
            wait(0)
            trace("cleanup.end")
        }
        function expected(sample) {
            const e = bindingFixture.overlayExpected(chart, sample)
            verify(e.sourceIndex !== undefined, "Fixture event " + sample)
            return e
        }
        function probe(sample) {
            return chart.referenceOverlayProbe(expected(sample).sourceIndex)
        }
        function near(actual, wanted, message) {
            verify(Math.abs(actual - wanted) < 0.02, message + ": " + actual + " vs " + wanted)
        }
        function rectNear(actual, wanted, message) {
            for (const field of ["x", "y", "width", "height"])
                near(actual[field], wanted[field], message + " " + field)
        }
        // Never give native QRectF wrappers to QtTest's generic recursive comparator.
        function probeNear(actual, wanted, message) {
            for (const field of ["valid", "status", "sourceIndex", "column", "mine", "paired", "enabled", "line"])
                compare(actual[field], wanted[field], message + " " + field)
            for (const field of ["rect", "body", "clip", "clippedRect", "clippedBody"])
                rectNear(actual[field], wanted[field], message + " " + field)
        }
        function center(rect) {
            return {x: rect.x + rect.width / 2, y: rect.y + rect.height / 2}
        }
        function positionViewport(sample) {
            const e = expected(sample)
            chart.scrollToTime(chart.secondsForPosition(e.measure, e.fraction))
            return probe(sample)
        }
        function image() {
            // Sample the real production painter; GUI scene composition is covered by Main screenshots.
            return bindingFixture.paintOverlay(chart)
        }
        function pixel(img, point) {
            // Qt Quick Test can return physical pixels on a high-DPI test runner.
            return img.pixel(Math.floor(point.x * img.width / chart.width),
                             Math.floor(point.y * img.height / chart.height))
        }
        function colorNear(actual, wanted, message) {
            for (const channel of ["r", "g", "b"])
                verify(Math.abs(actual[channel] - wanted[channel]) <= 3 / 255,
                       message + " (" + channel + "): " + actual + " vs " + wanted)
        }
        function blended(background, tint, opacity) {
            return Qt.rgba(background.r * (1 - opacity) + tint.r * opacity,
                           background.g * (1 - opacity) + tint.g * opacity,
                           background.b * (1 - opacity) + tint.b * opacity, 1)
        }
        function changed(a, b, message) {
            verify(Math.abs(a.r - b.r) + Math.abs(a.g - b.g) + Math.abs(a.b - b.b) > 0.06,
                   message)
        }
        function intersect(rect, clip) {
            const x = Math.max(rect.x, clip.x), y = Math.max(rect.y, clip.y)
            const right = Math.min(rect.x + rect.width, clip.x + clip.width)
            const bottom = Math.min(rect.y + rect.height, clip.y + clip.height)
            if (right <= x || bottom <= y) return Qt.rect(0, 0, 0, 0)
            return Qt.rect(x, y, right - x, bottom - y)
        }
        function snapshot(bridge) {
            return {state: bindingFixture.documentState(bridge.sessionId),
                    content: bridge.debugContentHash(), timing: bridge.debugTimingHash(),
                    samples: bridge.debugSamplesHash()}
        }
        function checkCounts() {
            const info = view.referenceOverlayInfo
            compare(info.total, 9)
            compare(info.invalid, 0)
            compare(info.unmapped, 2) // Full lane identity: 2P key1 and 1P key9 never alias 1P key1.
            compare(info.hidden, 1)
            const extended = info.extentExtended
            compare(typeof extended, "boolean")
            compare(info.outsideExtent, extended ? 0 : 1)
            compare(info.visibleMappings, extended ? 6 : 5)
            compare(probe(109).status, extended ? "visibleMappings" : "outsideExtent")
            compare(probe(109).valid, extended)
            compare(info.total, info.visibleMappings + info.unmapped + info.hidden
                    + info.outsideExtent + info.invalid)
        }

        function test_projection_uses_A_geometry_data() {
            return [{tag: "uniform-bpm-diff", profile: "uniform", high: false},
                    {tag: "variable-length-bpm-stop-diff", profile: "variable", high: false},
                    {tag: "reverse-axis", profile: "variable", high: true},
                    {tag: "different-reference-mode", profile: "mode", high: false}]
        }
        function test_projection_uses_A_geometry(data) {
            trace("projection.begin " + data.tag)
            bindingFixture.resetOverlay(data.profile)
            wait(0)
            trace("projection.axisAndViewport")
            view.topHigh = data.high
            positionViewport(101)
            trace("projection.capture")
            image() // Populate the primary painter's public column diagnostics.
            trace("projection.bindings")
            verify(chart.session === bridgeA, "A bridge identity")
            verify(chart.referenceSession === bridgeB, "B bridge identity")
            compare(chart.referenceOverlayEnabled, view.referenceOverlayEnabled)
            for (const channel of ["r", "g", "b", "a"])
                near(chart.referenceOverlayColor[channel], view.referenceOverlayColor[channel], "Bound tint " + channel)
            near(chart.referenceOverlayOpacity, view.referenceOverlayOpacity, "Root opacity binding")
            trace("projection.counts")
            checkCounts()
            trace("projection.columns")
            const columns = chart.probe(0, 40).columns
            for (const sample of [101, 102, 103, 104, 105]) {
                trace("projection.sample.begin " + sample)
                const e = expected(sample)
                trace("projection.sample.probe " + sample)
                const p = chart.referenceOverlayProbe(e.sourceIndex)
                trace("projection.sample.assert " + sample)
                compare(p.valid, true)
                compare(p.status, "visibleMappings")
                near(p.rect.y + p.rect.height, e.y, "Musical position uses A, not B seconds")
                const col = columns[p.column]
                verify(col !== undefined)
                const lane = chart.laneAtX(col.x + col.w / 2)
                compare(lane.lanePlayer, e.player)
                compare(lane.laneIndex, e.index)
                compare(lane.laneKind, "key")
                near(p.rect.x + p.rect.width / 2, col.x + col.w / 2, "A column center")
                rectNear(p.clippedRect, intersect(p.rect, p.clip), "Clipped cap")
                rectNear(p.clippedBody, intersect(p.body, p.clip), "Clipped LN")
                trace("projection.sample.end " + sample)
            }
            trace("projection.invalidProbes")
            compare(probe(107).status, "unmapped")
            compare(probe(108).status, "unmapped")
            compare(chart.referenceOverlayProbe(-1).valid, false)
            compare(chart.referenceOverlayProbe(-1).status, "invalid")
            compare(chart.referenceOverlayProbe(99999).status, "invalid")
            trace("projection.end " + data.tag)
        }

        function test_pixels_translucency_color_overlap_rim_and_normal_A_center() {
            const common = positionViewport(101)
            const normalCenter = {x: center(common.rect).x,
                                  y: common.rect.y + common.rect.height - 3}
            const rim = {x: normalCenter.x, y: common.rect.y + 2}
            const bOnly = center(probe(102).rect)
            view.referenceOverlayEnabled = false
            const off = image()
            const baseFill = pixel(off, bOnly), baseRim = pixel(off, rim)
            const baseCenter = pixel(off, normalCenter)
            view.referenceOverlayEnabled = true
            for (const opacity of [0, 0.25, 0.7, 1]) {
                view.referenceOverlayOpacity = opacity
                const on = image()
                colorNear(pixel(on, bOnly), blended(baseFill, view.referenceOverlayColor, opacity),
                          "B-only raw fill alpha " + opacity)
                colorNear(pixel(on, rim), blended(baseRim, view.referenceOverlayColor, opacity),
                          "Exact-overlap rim remains visible " + opacity)
                colorNear(pixel(on, normalCenter), baseCenter, "A center stays its normal color")
            }
            view.referenceOverlayColor = "#20c0f0"
            view.referenceOverlayOpacity = 0.5
            const recolored = image()
            colorNear(pixel(recolored, bOnly), blended(baseFill, view.referenceOverlayColor, 0.5),
                      "Root color repaints B")
            changed(pixel(recolored, rim), baseRim, "Common notes are not removed as a semantic match")
        }

        function test_samples_do_not_classify_or_move_reference_notes() {
            positionViewport(101)
            const before = image()
            const geometries = []
            for (let i = 0; i < 9; ++i) geometries.push(chart.referenceOverlayProbe(i))
            const a = snapshot(bridgeA)
            bindingFixture.changeOverlayReferenceSamples()
            const after = image()
            for (let i = 0; i < geometries.length; ++i)
                probeNear(chart.referenceOverlayProbe(i), geometries[i], "Only sample ids changed")
            verify(before.equals(after), "No sample-based same/added/removed styling or geometry")
            compare(snapshot(bridgeA), a)
        }

        function test_LN_spans_mines_and_clipping() {
            const head = positionViewport(103), tail = probe(104), mine = probe(105)
            verify(head.paired && tail.paired)
            compare(head.mine, false)
            near(head.body.y, Math.min(expected(103).y, expected(104).y), "LN begins at A endpoint")
            near(head.body.height, Math.abs(expected(104).y - expected(103).y), "A LN span")
            compare(tail.body.width, 0)
            compare(tail.body.height, 0)
            compare(mine.mine, true)
            compare(mine.paired, false)
            near(mine.rect.width, mine.rect.height, "Mine diamond bounds")
            const bodyRim = {x: head.body.x + 1, y: head.body.y + head.body.height / 2}
            const mineRim = {x: center(mine.rect).x, y: mine.rect.y + 2}
            view.referenceOverlayEnabled = false
            const off = image()
            view.referenceOverlayEnabled = true
            const on = image()
            changed(pixel(on, bodyRim), pixel(off, bodyRim), "LN body underlay is painted beside A body")
            changed(pixel(on, mineRim), pixel(off, mineRim), "Mine underlay diamond rim is painted")

            // Both caps outside the viewport: the reference body must not be endpoint-culled.
            view.measureHeight = 4096
            positionViewport(103)
            let p = probe(103)
            chart.scrollY += p.body.y + p.body.height / 2 - chart.height / 2
            p = probe(103)
            verify(p.body.y < p.clip.y)
            verify(p.body.y + p.body.height > p.clip.y + p.clip.height)
            rectNear(p.clippedBody, intersect(p.body, p.clip), "Clipped LN span")
            near(p.clippedBody.height, p.clip.height, "LN crosses whole viewport")
            const point = {x: p.body.x + 1, y: chart.height / 2}
            view.referenceOverlayEnabled = false
            const spanOff = image()
            view.referenceOverlayEnabled = true
            const spanOn = image()
            changed(pixel(spanOn, point), pixel(spanOff, point), "Offscreen caps still paint the body")
            // Fixed header and ruler must never receive the reference paint.
            for (const pointOutside of [{x: point.x, y: 10}, {x: chart.rulerWidth / 2, y: 100}])
                colorNear(pixel(spanOn, pointOutside), pixel(spanOff, pointOutside), "Overlay clip")
        }

        function test_BGM_hidden_collapsed_and_expanded_full_lane_mapping() {
            compare(view.bgmExpanded, true) // New default; explicitly exercise collapsed mapping below.
            view.bgmExpanded = false
            compare(probe(106).status, "hidden")
            view.referenceOverlayHideSecondaryChannels = false
            image()
            let p = positionViewport(106)
            compare(p.status, "visibleMappings")
            compare(p.line, -1) // Collapsed BGM intentionally combines sub-lines.
            let lane = chart.laneAtX(center(p.rect).x)
            compare(lane.laneKind, "bgm")
            compare(lane.bgaLayer, -1)
            view.bgmExpanded = true
            image()
            p = probe(106)
            verify(p.valid)
            compare(p.line, 2) // Source sample 106 is NOT a column id.
            lane = chart.laneAtX(center(p.rect).x)
            compare(lane.sub_line, 2)
            compare(lane.bgaLayer, -1)
            compare(probe(107).status, "unmapped")
            compare(probe(108).status, "unmapped")
            view.hideSecondaryChannels = true
            image()
            compare(probe(106).status, "hidden")
            view.hideSecondaryChannels = false
            view.referenceOverlayHideSecondaryChannels = true
            compare(probe(106).status, "hidden")
        }

        function test_B_only_is_not_an_editable_A_note() {
            const p = positionViewport(102), point = center(p.rect)
            image()
            const emptyHit = chart.hitTest(point.x, point.y)
            verify(emptyHit.valid, "B note lies over a valid empty A placement slot")
            compare(emptyHit.lanePlayer, 0)
            compare(emptyHit.laneKind, "key")
            compare(emptyHit.laneIndex, 2)
            verify(!chart.noteAt(point.x, point.y).valid)
            compare(chart.notesInRect(p.rect.x, p.rect.y,
                                     p.rect.x + p.rect.width, p.rect.y + p.rect.height).length, 0)
            view.referenceOverlayEnabled = false
            image()
            compare(chart.hitTest(point.x, point.y), emptyHit, "hitTest has no B-derived state")
            verify(!chart.noteAt(point.x, point.y).valid)
            const common = positionViewport(101), aPoint = {x: center(common.rect).x,
                                                y: common.rect.y + common.rect.height - 3}
            verify(chart.noteAt(aPoint.x, aPoint.y).valid, "Normal A notes remain editable")
        }

        function test_reference_tail_is_view_only_and_invalid_seek_is_ignored() {
            const a = snapshot(bridgeA), b = snapshot(bridgeB)
            const info = view.referenceOverlayInfo
            verify(info.extentExtended)
            verify(info.viewMeasureCount > info.effectiveMeasureCount)
            compare(info.effectiveMeasureCount, bridgeA.measureCount)
            const oldPcm = audioEngine.hasPcm
            audioEngine.hasPcm = true
            try {
                for (const high of [false, true]) {
                    view.topHigh = high
                    chart.applyViewportAnchor({valid: true, measure: 32, fraction: 0})
                    image()
                    const p = probe(109), point = center(p.rect)
                    verify(p.valid && p.clippedRect.height > 0)
                    verify(!chart.hitTest(point.x, point.y).valid, "B tail is not an A placement slot")
                    verify(!chart.noteAt(point.x, point.y).valid)
                    compare(chart.timeAtY(point.y), -1)
                    compare(chart.secondsForPosition(32, 0), -1)
                    const before = chart.scrollY, seeks = audioEngine.seekCount
                    chart.playheadSec = 1.5
                    for (const sec of [-1, NaN, Infinity]) {
                        view.seekTo(sec)
                        view.seekToSeconds(sec)
                    }
                    compare(chart.scrollY, before)
                    compare(chart.playheadSec, 1.5)
                    compare(audioEngine.seekCount, seeks)
                }
            } finally { audioEngine.hasPcm = oldPcm }
            compare(snapshot(bridgeA), a)
            compare(snapshot(bridgeB), b)
        }

        function test_display_switches_preserve_both_documents_and_history() {
            verify(bindingFixture.seedCompareHistory())
            const a = snapshot(bridgeA), b = snapshot(bridgeB)
            verify(a.state.undo > 0 && a.state.redo > 0)
            verify(b.state.undo > 0 && b.state.redo > 0)
            const columns = chart.probe(0, 40).columns
            for (const reference of [bridgeA, null, bridgeB, bridgeA, bridgeB]) {
                view.referenceSession = reference
                view.referenceOverlayEnabled = !view.referenceOverlayEnabled
                view.referenceOverlayOpacity = 0.65
                view.referenceOverlayColor = "#d06030"
                image()
                compare(chart.probe(0, 40).columns, columns, "Reference never changes A columns")
                compare(snapshot(bridgeA), a)
                compare(snapshot(bridgeB), b)
            }
        }

        function test_B_content_change_repaints_and_same_document_is_safe() {
            positionViewport(101)
            const e = {measure: 1, fraction: 0.5}
            const b = probe(102)
            const point = {x: center(b.rect).x,
                           y: (expected(101).y + expected(102).y) / 2 - 3}
            const before = image(), a = snapshot(bridgeA)
            infoSpy.clear()
            verify(bindingFixture.addOverlayReferenceNote())
            tryVerify(function() { return infoSpy.count > 0 })
            compare(view.referenceOverlayInfo.total, 10)
            const added = probe(110)
            verify(added.valid)
            near(added.rect.y + added.rect.height,
                 expected(110).y, "New B note uses A geometry")
            const after = image()
            changed(pixel(after, point), pixel(before, point), "B contentChanged actually repaints")
            compare(snapshot(bridgeA), a)
            const bState = snapshot(bridgeB)
            const sameDocBridge = bindingFixture.temporaryOverlaySession(false)
            view.referenceSession = sameDocBridge
            image()
            compare(view.referenceOverlayInfo.total, 10)
            view.referenceSession = bridgeA // Same object/document as the primary is also legal.
            image()
            compare(view.referenceOverlayInfo.total, 6)
            compare(snapshot(bridgeA), a)
            compare(snapshot(bridgeB), bState)
            bindingFixture.destroyOverlaySession(sameDocBridge)
        }

        function test_missing_absent_and_destroyed_reference_are_safe() {
            positionViewport(101)
            view.referenceSession = null
            const absent = image()
            compare(view.referenceOverlayInfo.available, false)
            compare(view.referenceOverlayInfo.total, 0)
            compare(chart.referenceOverlayProbe(0).status, "invalid")
            const missing = bindingFixture.temporaryOverlaySession(true)
            view.referenceSession = missing
            verify(image().equals(absent))
            compare(view.referenceOverlayInfo.available, false)
            bindingFixture.destroyOverlaySession(missing)
            verify(chart.referenceSession === null, "Absent reference")
            const temporary = bindingFixture.temporaryOverlaySession(false)
            view.referenceSession = temporary
            compare(view.referenceOverlayInfo.available, true)
            image()
            bindingFixture.destroyOverlaySession(temporary)
            tryVerify(() => chart.referenceSession === null)
            compare(view.referenceOverlayInfo.total, 0)
            compare(view.referenceOverlayInfo.available, false)
            compare(chart.referenceOverlayProbe(0).valid, false)
            verify(image().equals(absent), "Destroyed reference removes underlay without touching A")
        }
    }
}
