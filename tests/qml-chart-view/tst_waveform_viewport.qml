// SPDX-License-Identifier: GPL-3.0-only
import QtQuick
import QtTest
import "../../app/qml/components" as Components

Item {
    id: scene
    width: 720; height: 204
    Component {
        id: surface
        Components.ChartView {
            width: scene.width; height: scene.height
            documentSession: bridgeA
            referenceSession: bridgeB
            referenceOverlayEnabled: true
            followPlayhead: false
        }
    }
    TestCase {
        name: "ChartWaveformViewport"
        when: windowShown
        property var view
        property var chart
        property var wave
        property var primary: null
        function cleanup() {
            bindingFixture.releaseOverlayCaptures()
            if (view) view.documentSession = null
            if (primary) bindingFixture.destroyOverlaySession(primary)
            primary = null
            bindingFixture.resetAudioProbe(false)
            bindingFixture.resetCompare()
        }
        function near(a, b) { fuzzyCompare(a, b, 0.00001) }
        function windowIs(valid, t0, t1) {
            // Scalar-only comparisons: never recursively stringify Qt wrappers.
            const w = wave.viewportTimeWindow()
            compare(w.valid, valid)
            near(w.t0, t0); near(w.t1, t1)
        }
        function positionLow(px) {
            // Independent linear A geometry: 96px/4 beats, 120 BPM => 2s/96px.
            // Do not use measureAtY/viewportTimeWindow to construct expectations.
            const low = chart.leadMeasures * 96 + px
            const scroll = view.topHigh ? chart.contentHeight - low - chart.height : low
            chart.scrollY = scroll
            near(chart.scrollY, scroll) // Fixture never relies on a clamped scroll request.
        }
        function pixelAtSecond(image, sec) {
            const dur = primary.waveformInfo().durationSec
            const fraction = view.topHigh ? 1 - sec / dur : sec / dur
            return image.pixel(2, Math.floor(fraction * wave.height))
        }
        function distance(a, b) {
            return Math.abs(a.r - b.r) + Math.abs(a.g - b.g) + Math.abs(a.b - b.b)
        }
        function test_own_timing_window_data() {
            return [{tag: "uniform-up", variable: false, high: true},
                    {tag: "uniform-down", variable: false, high: false},
                    {tag: "variable-up", variable: true, high: true},
                    {tag: "variable-down", variable: true, high: false}]
        }
        function test_own_timing_window(data) {
            bindingFixture.resetWaveformViewport(data.variable)
            bindingFixture.resetAudioProbe(false)
            // <=13 seconds of silent PCM (including renderer tail) at 8kHz.
            // Fresh bridge avoids accepting an old waveform while auto-refresh runs.
            primary = bindingFixture.temporaryWaveformSession()
            verify(primary.renderAsync(8000, false))
            tryVerify(() => primary.waveformInfo().valid === true, 10000)
            view = createTemporaryObject(surface, scene, {topHigh: data.high, documentSession: primary})
            verify(view !== null)
            wait(0) // Settle document visuals; all subsequent range checks are immediate.
            const parts = bindingFixture.childrenOf(view)
            chart = parts.chart; wave = parts.waveform
            compare(wave.chartView, chart)
            verify(wave.visible)
            near(chart.height, 192)
            const end = data.variable ? 10 : 8
            const totalA = data.variable ? 480 : 384
            near(chart.secondsForPosition(4, 0), end)
            verify(view.referenceOverlayInfo.extentExtended)
            const a = JSON.stringify(bindingFixture.documentState(bridgeA.sessionId))
            const b = JSON.stringify(bindingFixture.documentState(bridgeB.sessionId))

            positionLow(48)
            windowIs(true, 1, 5) // Starts inside A's x2 measure; ends after x0.5.
            const inside = bindingFixture.paintOverlay(wave)
            verify(distance(pixelAtSecond(inside, 2), Theme.surface) > 0.02,
                   "Production waveform painter must highlight the own-A interval")
            verify(distance(pixelAtSecond(inside, 6), Theme.surface) < 0.01,
                   "Outside own-A interval must not be highlighted")

            positionLow(192)
            windowIs(true, 4, 8) // Spans x0.5 + x1.5 measures for variable A.

            positionLow(totalA - 96)
            windowIs(true, end - 2, end) // Cross A end into view-only B.
            verify(wave.viewportTimeWindow().t0 > 0, "A end must not wrap to time zero")

            positionLow(totalA + 96)
            windowIs(false, end, end) // Entirely B-only: hidden, clamped to A end.
            const tail = bindingFixture.paintOverlay(wave)
            verify(distance(pixelAtSecond(tail, 2), Theme.surface) < 0.01)
            verify(distance(pixelAtSecond(tail, 6), Theme.surface) < 0.01)
            const columns = chart.probe(0, 40).columns
            const key = columns.find(c => c.label === "1")
            verify(!chart.hitTest(key.x + key.w / 2, 80).valid, "B tail remains view-only")
            compare(chart.timeAtY(80), -1, "B tail is not an A seek position")

            // Mismatched or unwired overview cannot reuse another document's timing.
            wave.chartView = null
            compare(wave.viewportTimeWindow().valid, false)
            wave.chartView = chart
            wave.session = bridgeB
            compare(wave.viewportTimeWindow().valid, false)
            wave.session = primary
            windowIs(false, end, end)
            compare(audioEngine.seekCount, 0)
            compare(JSON.stringify(bindingFixture.documentState(bridgeA.sessionId)), a)
            compare(JSON.stringify(bindingFixture.documentState(bridgeB.sessionId)), b)
        }
    }
}
