// SPDX-License-Identifier: GPL-3.0-only
import QtQuick
import QtQuick.Controls
import QtTest
import "../../app/qml/components" as Components

Item {
    id: scene
    width: 1100; height: 620
    Component {
        id: surface
        Components.ChartCompareSplit {
            id: compareSplit
            width: scene.width; height: scene.height
            documentService: compareDocuments
            primarySession: compareDocuments.currentSession
            primaryChartView: primaryChart
            property alias primaryView: primaryChart
            Components.ChartView {
                id: primaryChart
                anchors.fill: parent
                documentSession: compareSplit.primarySession
            }
        }
    }
    TestCase {
        id: tc
        name: "ChartWaveformInteraction"
        when: windowShown
        property var split: null
        property var source: null
        property var wave: null
        SignalSpy { id: seeks; target: tc.wave; signalName: "seekRequested" }
        SignalSpy { id: selections; target: tc.source; signalName: "selectionFinished" }
        function verticalBar(view) {
            for (const child of view.children) if ("maxY" in child) return child
            fail("Missing production vertical scrollbar")
            return null
        }
        function render(session) {
            if (!session.waveformInfo().valid) {
                verify(session.renderAsync(8000, false))
                tryVerify(() => session.waveformInfo().valid === true, 10000)
            }
        }
        function init() {
            bindingFixture.resetCompare()
            bindingFixture.resetAudioProbe(true)
            render(bridgeA); render(bridgeB)
            split = createTemporaryObject(surface, scene)
            verify(split !== null)
            split.synchronizeViewports = false
            split.hideSecondaryChannels = false
            verify(split.selectReference("chart-view-binding-b"))
            split.splitLayout.itemAt(1).SplitView.preferredWidth = 480
            tryVerify(() => split.referenceWidth > 400)
            source = split.primaryView
            wave = bindingFixture.childrenOf(source).waveform
            tryCompare(wave, "visible", true)
            tryVerify(() => wave.width > 0 && wave.height > 0)
            seeks.clear(); selections.clear()
        }
        function cleanup() {
            // Always end a synthetic gesture, including after a failed assertion.
            if (wave) mouseRelease(wave, wave.width / 2, wave.height / 2, Qt.LeftButton)
            bindingFixture.resetAudioProbe(false)
            source = null; wave = null
        }
        function test_drag_data() {
            const rows = []
            for (const side of ["A", "B"])
                for (const top of [true, false])
                    for (const tool of ["pan", "select"])
                        rows.push({tag: side + "_" + top + "_" + tool, side: side, top: top, tool: tool})
            return rows
        }
        function test_drag(data) {
            source = data.side === "A" ? split.primaryView : split.referenceView
            wave = bindingFixture.childrenOf(source).waveform
            source.topHigh = data.top
            split.editorTool = data.tool
            source.editorTool = data.tool
            const a = JSON.stringify(bindingFixture.documentState("chart-view-binding-a"))
            const b = JSON.stringify(bindingFixture.documentState("chart-view-binding-b"))
            seeks.clear(); selections.clear()
            const x = Math.round(wave.width / 2), y = Math.round(wave.height * 0.75)
            mousePress(wave, x, y, Qt.LeftButton)
            compare(seeks.count, 1)
            compare(audioEngine.seekCount, data.side === "A" ? 1 : 0)
            for (let n = 1; n <= 4; ++n) {
                mouseMove(wave, x, y - n * 45, 20)
                compare(wave.scrubbing, true, "Waveform must retain the mouse grab, not pan the background")
                verify(seeks.count >= n + 1)
                const expected = (data.top ? 1 - (y - n * 45) / wave.height
                                           : (y - n * 45) / wave.height)
                               * source.documentSession.waveformInfo().durationSec
                fuzzyCompare(seeks.signalArguments[seeks.count - 1][0], expected, 0.00001)
                compare(audioEngine.seekCount, data.side === "A" ? 1 : 0, "Moves are silent")
            }
            mouseRelease(wave, x, y - 180, Qt.LeftButton)
            compare(wave.scrubbing, false)
            compare(audioEngine.seekCount, data.side === "A" ? 2 : 0, "Release commits once, B never seeks audio")
            compare(selections.count, 0)
            compare(source._panning, false)
            compare(source._lastY, -1)
            compare(JSON.stringify(bindingFixture.documentState("chart-view-binding-a")), a)
            compare(JSON.stringify(bindingFixture.documentState("chart-view-binding-b")), b)
        }
        function test_click_jitter_does_not_duplicate_audio_seek() {
            mousePress(wave, 30, 300, Qt.LeftButton)
            mouseMove(wave, 30, 301, 20)
            compare(wave.scrubbing, false)
            mouseRelease(wave, 30, 301, Qt.LeftButton)
            compare(seeks.count, 1)
            compare(audioEngine.seekCount, 1)
            compare(selections.count, 0)
        }
        function test_primary_wave_drag_synchronizes_compact_reference() {
            split.hideSecondaryChannels = true
            split.synchronizeViewports = true
            mousePress(wave, 30, 450, Qt.LeftButton)
            mouseMove(wave, 30, 350, 30)
            compare(wave.scrubbing, true)
            mouseRelease(wave, 30, 350, Qt.LeftButton)
            const a = split.primaryView.viewportItem
            const b = split.referenceView.viewportItem
            tryVerify(() => Math.abs(a.viewportAnchor.measure + a.viewportAnchor.fraction
                                  - b.viewportAnchor.measure - b.viewportAnchor.fraction) < 0.00001)
            compare(bindingFixture.childrenOf(split.referenceView).waveform.visible, false)
            compare(audioEngine.seekCount, 2)
        }
        function test_drag_outside_and_hidden_cancels_grab() {
            source = split.referenceView
            wave = bindingFixture.childrenOf(source).waveform
            source.topHigh = true
            mousePress(wave, wave.width / 2, wave.height / 2, Qt.LeftButton)
            mouseMove(wave, -30, -20, 30)
            compare(wave.scrubbing, true)
            fuzzyCompare(seeks.signalArguments[seeks.count - 1][0], bridgeB.waveformInfo().durationSec, 0.00001)
            split.hideSecondaryChannels = true
            tryCompare(wave, "visible", false)
            tryCompare(wave, "scrubbing", false)
            mouseRelease(wave, -30, -20, Qt.LeftButton)
            split.hideSecondaryChannels = false
            tryCompare(wave, "visible", true)
            mouseClick(wave, wave.width / 2, wave.height * 0.25)
            compare(wave.scrubbing, false)
            compare(audioEngine.seekCount, 0)
        }
        function test_compact_hides_wave_and_reclaims_width_even_after_render() {
            const a = bindingFixture.childrenOf(split.primaryView)
            const b = bindingFixture.childrenOf(split.referenceView)
            tryCompare(a.waveform, "visible", true)
            tryCompare(b.waveform, "visible", true)
            const aw = a.chart.width, bw = b.chart.width
            const state = JSON.stringify(bindingFixture.documentState("chart-view-binding-b"))
            split.hideSecondaryChannels = true
            tryCompare(b.waveform, "visible", false)
            tryCompare(b.chart, "width", bw + b.waveform.width)
            compare(verticalBar(split.referenceView).x, b.chart.width, "Scrollbar must stay outside reclaimed lanes")
            compare(a.waveform.visible, true); compare(a.chart.width, aw)
            bridgeB.renderFinished(true, "", bridgeB.waveformInfo().durationSec)
            compare(b.waveform.visible, false)
            split.hideSecondaryChannels = false
            tryCompare(b.waveform, "visible", true)
            tryCompare(b.chart, "width", bw)
            // Failed/stale render reserves width when expanded, as before, but is not interactive.
            bridgeB.renderFinished(false, "", 0)
            compare(b.waveform.visible, false)
            compare(b.chart.width, bw)
            split.hideSecondaryChannels = true
            tryCompare(b.chart, "width", bw + b.waveform.width)
            bridgeB.renderFinished(true, "", bridgeB.waveformInfo().durationSec)
            compare(b.waveform.visible, false)
            split.hideSecondaryChannels = false
            tryCompare(b.waveform, "visible", true)
            compare(JSON.stringify(bindingFixture.documentState("chart-view-binding-b")), state)
        }
    }
}
