// SPDX-License-Identifier: GPL-3.0-only
import QtQuick

// Deliberately NOT AudioEngine: no PortAudio construction, devices, or settings.
QtObject {
    readonly property bool available: false
    readonly property string errorText: "integration test: no audio device"
    readonly property var devices: []
    readonly property bool asioCompiled: false
    function deviceCapabilities(index) { return { asio: false, error: "" } }
    function bufferOptions(index) { return [{ text: "Device default", value: 0 }, { text: "512 frames", value: 512 }] }
    function showDeviceControlPanel() { unexpected(); return false }
    readonly property int deviceIndex: -1
    readonly property int sampleRate: 44100
    readonly property int actualRate: 44100
    readonly property int framesPerBuffer: 512
    readonly property real masterVolume: 80
    readonly property real latencyMs: 0
    readonly property bool waitRenderSetting: false
    readonly property string activeDeviceText: "test-only"
    signal playbackChanged()
    property bool playing: false
    property bool hasPcm: false
    property real positionSec: 0
    readonly property real durationSec: 0
    readonly property real loopA: -1
    readonly property real loopB: -1
    readonly property bool refPlaying: false
    readonly property bool refHasPcm: false
    readonly property real refPositionSec: 0
    property string chartPath: ""
    property int pathChanges: 0
    property int playbackCalls: 0
    function setChartPath(path) { chartPath = path; pathChanges++ }
    function unexpected() { playbackCalls++; throw new Error("Unexpected audio operation in Main integration test") }
    function play() { unexpected() }
    function pause() { unexpected() }
    function stopPlay() { unexpected() }
    function seekSeconds(sec) { unexpected() }
    property bool allowPreview: false
    property var previewFiles: []
    property int previewStops: 0
    function stopPreview() { previewStops++ }
    function playPreview(file) {
        if (!allowPreview) { unexpected(); return false }
        previewFiles = previewFiles.concat([file])
        return true
    }
    function refTogglePlay() { unexpected() }
    function refStop() { unexpected() }
    function refSeek(sec) { unexpected() }
    function applySettings() { unexpected() }
    function playTestTone() { unexpected() }
    function setWaitRenderSetting(value) { unexpected() }
}
