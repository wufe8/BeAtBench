// SPDX-License-Identifier: GPL-3.0-only
import QtQuick

// Deliberately NOT AudioEngine: no PortAudio construction, devices, or settings.
QtObject {
    readonly property bool available: false
    readonly property string errorText: "integration test: no audio device"
    readonly property var devices: []
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
    function playPreview(file) { unexpected() }
    function refTogglePlay() { unexpected() }
    function refStop() { unexpected() }
    function refSeek(sec) { unexpected() }
    function applySettings() { unexpected() }
    function playTestTone() { unexpected() }
    function setWaitRenderSetting(value) { unexpected() }
}
