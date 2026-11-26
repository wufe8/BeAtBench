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
    property string previewFile: ""
    function stopPreview() { previewStops++; previewFile = ""; referenceNotePreview = null }
    function playPreview(file) {
        if (!allowPreview) { unexpected(); return false }
        previewFiles = previewFiles.concat([file])
        referenceNotePreview = null
        previewFile = file
        return true
    }

    property var referenceNotePreviewCalls: []
    property var referenceNotePreview: null
    property int referenceNotePreviewStops: 0
    function playReferenceNotePreview(session, sampleId) {
        // 先记录实际 API 调用；未定义的 ID 也应抵达 C++，由其决定静音。
        const call = { path: session ? session.path : "", sampleId: sampleId,
                       file: session ? session.wavFileOfId(sampleId) : "" }
        referenceNotePreviewCalls = referenceNotePreviewCalls.concat([call])
        if (call.file === "") return false
        // 与生产一致：A/B 共用一个单音试听通道，不与整曲播放共用。
        previewFile = ""
        referenceNotePreview = call
        return true
    }
    function stopReferenceNotePreview() {
        if (referenceNotePreview === null) return
        referenceNotePreviewStops++
        referenceNotePreview = null
    }
    function refTogglePlay() { unexpected() }
    function refStop() { unexpected() }
    function refSeek(sec) { unexpected() }
    function applySettings() { unexpected() }
    function playTestTone() { unexpected() }
    function setWaitRenderSetting(value) { unexpected() }
}
