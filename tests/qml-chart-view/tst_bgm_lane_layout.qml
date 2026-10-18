// SPDX-License-Identifier: GPL-3.0-only
import QtQuick
import QtTest
import BeatBench 1.0

Item {
    id: scene
    width: 1800
    height: 620

    Component {
        id: laneView
        ChartViewItem {
            width: scene.width
            height: scene.height
            session: bridgeA
            theme: Theme
            topHigh: false
        }
    }

    TestCase {
        name: "BgmLaneLayout"
        when: windowShown
        property var view

        function init() {
            bindingFixture.resetBgmLayout(false)
            view = createTemporaryObject(laneView, scene)
            verify(view !== null)
        }
        function cleanup() {
            bindingFixture.releaseOverlayCaptures()
            bindingFixture.resetCompare()
        }
        function bgmColumns() {
            // Probes and input resolve the current layout without waiting for paint.
            const columns = view.probe(0, 40).columns
            return columns.filter(function(c) {
                const lane = view.laneAtX(c.x + c.w / 2)
                return lane.valid && lane.laneKind === "bgm" && lane.bgaLayer === -1
            })
        }
        function slot(column) {
            const x = column.x + column.w / 2
            for (let y = 30; y < view.height - 8; y += 2) {
                const hit = view.hitTest(x, y)
                if (hit.valid && hit.laneKind === "bgm" && hit.bgaLayer === -1)
                    return {x: x, y: y, hit: hit}
            }
            fail("BGM column has no editable slot")
        }
        // Independent layout oracle. Metadata widths are fixed production 48px;
        // lane widths/ruler/scroll/centering are live inputs, not paint/probe output.
        function expectedColumns() {
            let result = []
            if (!view.hideSecondaryChannels)
                result = [{label: "BPM", w: 48}, {label: "STOP", w: 48}]
            let lanes = ["S", "1", "2", "3", "4", "5", "6", "7"]
            if (!view.hideSecondaryChannels) {
                if (view.showExtras) lanes = lanes.concat(["BGA", "LAYER", "POOR", "LAYER2", "P"])
                if (view.bgmExpanded) {
                    for (let i = 1; i <= 9; ++i) lanes.push("bgm" + i)
                } else lanes.push("BGM")
            }
            const meta = result.length * 48
            const available = view.width - view.rulerWidth - meta
            let laneX = view.rulerWidth + meta + (lanes.length * view.laneWidth <= available
                        ? (available - lanes.length * view.laneWidth) / 2 : -view.scrollX)
            for (let i = 0; i < result.length; ++i) result[i].x = view.rulerWidth + i * 48
            for (const label of lanes) {
                result.push({label: label, x: laneX, w: view.laneWidth})
                laneX += view.laneWidth
            }
            return result
        }
        function checkLiveColumns(oldXs) {
            const expected = expectedColumns()
            const actual = view.probe(0, 40).columns
            compare(actual.length, expected.length)
            let xs = oldXs || []
            for (let i = 0; i < expected.length; ++i) {
                compare(actual[i].label, expected[i].label)
                fuzzyCompare(actual[i].x, expected[i].x, 0.00001)
                fuzzyCompare(actual[i].w, expected[i].w, 0.00001)
                xs.push(expected[i].x + expected[i].w / 2)
            }
            // Both obsolete and new positions are checked: a safe index bound alone
            // still returns the wrong lane after compact re-centering or scrolling.
            const y = view.leadMeasures * view.measureHeight + 24
            for (const x of xs) {
                const column = expected.find(c => x >= c.x && x < c.x + c.w)
                const lane = view.laneAtX(x), hit = view.hitTest(x, y)
                compare(lane.valid, column !== undefined)
                compare(hit.valid, column !== undefined)
                if (column) {
                    compare(lane.label, column.label)
                    compare(hit.label, column.label)
                    if (column.label.indexOf("bgm") === 0) {
                        compare(lane.sub_line, Number(column.label.slice(3)) - 1)
                        compare(hit.sub_line, lane.sub_line)
                        verify(view.bgmHeaderIndexAt(x) >= 0)
                    }
                }
            }
            const key = expected.find(c => c.label === "1")
            const x = key.x + key.w / 2
            const noteY = view.leadMeasures * view.measureHeight + view.measureHeight - 2
            compare(view.noteAt(x, noteY).valid, true)
            compare(view.objectAt(x, noteY).kind, "note")
            compare(view.notesInRect(x - 2, noteY - 2, x + 2, noteY + 2).length, 1)
            return expected.map(c => c.x + c.w / 2)
        }
        function test_live_geometry_before_first_paint() {
            view = createTemporaryObject(laneView, scene, {visible: false})
            verify(view !== null) // Invisible from construction: guaranteed zero paints.
            checkLiveColumns()
            view.bgmExpanded = true
            checkLiveColumns()
        }
        function test_compact_expand_scroll_resize_without_render() {
            view.showExtras = true
            view.bgmExpanded = true
            // Explicit old-frame seed, then no waits/event processing/paint below.
            bindingFixture.paintOverlay(view)
            view.visible = false
            let xs = checkLiveColumns()
            view.hideSecondaryChannels = true
            xs = checkLiveColumns(xs)
            view.hideSecondaryChannels = false
            xs = checkLiveColumns(xs)
            view.width = 500
            view.scrollX = 110
            xs = checkLiveColumns(xs)
            view.bgmExpanded = false
            xs = checkLiveColumns(xs)
            view.showExtras = false
            view.laneWidth = 44
            view.rulerWidth = 64
            checkLiveColumns(xs)
            bindingFixture.releaseOverlayCaptures()
        }
        function test_empty_bgm_is_editable_data() {
            return [{tag: "key-only", empty: false}, {tag: "empty-chart", empty: true}]
        }
        function test_empty_bgm_is_editable(data) {
            bindingFixture.resetBgmLayout(data.empty)
            view.scrollToStart()
            compare(view.bgmExpanded, false)
            let columns = bgmColumns()
            compare(columns.length, 1, "No BGM events must still expose the collapsed BGM lane")
            compare(columns[0].label, "BGM")
            let point = slot(columns[0])
            compare(point.hit.sub_line, -1)
            compare(point.hit.lanePlayer, 0)
            compare(point.hit.laneIndex, 0)
            verify(!point.hit.sampleHint)
            verify(!view.noteAt(point.x, point.y).valid)
            compare(bindingFixture.documentState(bridgeA.sessionId).dirty, false)
            compare(bindingFixture.documentState(bridgeA.sessionId).undo, 0)

            verify(bindingFixture.putBgmAt(point.hit))
            verify(view.noteAt(point.x, point.y).valid)
            compare(bgmColumns().length, 1)
            verify(bindingFixture.undoBgm())
            verify(!view.noteAt(point.x, point.y).valid)
            compare(bgmColumns().length, 1, "Undoing the last BGM must retain its lane")

            view.showExtras = true
            view.showChannelIds = true
            columns = bgmColumns()
            compare(columns.length, 1)
            compare(columns[0].label, "01")
            point = slot(columns[0])
            compare(point.hit.bgaLayer, -1)
            view.bgmExpanded = true
            columns = bgmColumns()
            compare(columns.length, 9) // Existing base line plus eight spare sub-lines.
            compare(columns[0].label, "bgm1")
            compare(columns[8].label, "bgm9")
            point = slot(columns[8])
            compare(point.hit.sub_line, 8)
            verify(bindingFixture.putBgmAt(point.hit))
            columns = bgmColumns()
            compare(columns.length, 17) // Used line 8 + eight spare sub-lines.
            // Growing 9 -> 17 BGM lanes re-centers this fitting layout immediately.
            // The musical target is unchanged, but its old screen x is now stale.
            const placedX = columns[8].x + columns[8].w / 2
            fuzzyCompare(placedX, point.x - 4 * view.laneWidth, 0.00001)
            verify(!view.noteAt(point.x, point.y).valid, "Old centered x must not retain a stale hit")
            const placed = view.noteAt(placedX, point.y)
            verify(placed.valid)
            compare(placed.sub_line, point.hit.sub_line)
            compare(placed.measure, point.hit.measure)
            compare(placed.sample, 1)
            verify(bindingFixture.undoBgm())
            compare(bgmColumns().length, 17) // Preserve the existing high-water policy.
            view.bgmExpanded = false
            compare(bgmColumns().length, 1)
            compare(bindingFixture.documentState(bridgeA.sessionId).dirty, false)
        }
    }
}
