// SPDX-License-Identifier: GPL-3.0-only
import QtQuick
import QtQuick.Controls
import QtTest
import "../../app/qml/components" as Components

Item {
    id: window
    width: 1000
    height: 1100
    // Creation-context mock shadows the shared context engine, never opens OS audio.
    QtObject {
        id: audioEngine
        property bool available: true
        property bool asioCompiled: true
        property string errorText: ""
        property var devices: []
        property int deviceIndex: -1
        property int sampleRate: 0
        property int actualRate: 48000
        property int framesPerBuffer: 0
        property real masterVolume: 73
        property real latencyMs: 4.5
        property bool waitRenderSetting: false
        property string activeDeviceText: "Accepted device"
        property bool defaultAsio: false
        property string capabilityError: ""
        property bool rejectSettings: false
        property var lastApply: []
        property int panelCalls: 0
        function deviceCapabilities(index) {
            return { asio: index === 7 || (index === -1 && defaultAsio),
                     bufferMin: 96, bufferMax: 288, bufferPreferred: 192,
                     bufferGranularity: 96, error: capabilityError }
        }
        function bufferOptions(index) {
            // Deliberately non-generic ASIO sizes: UI must use values, not row math.
            if (index === 7 || (index === -1 && defaultAsio))
                return [{ text: "Driver default", value: 0 }, { text: "96 frames", value: 96 },
                        { text: "192 frames", value: 192 }, { text: "288 frames", value: 288 }]
            return [{ text: "Device default", value: 0 }, { text: "64 frames", value: 64 },
                    { text: "128 frames", value: 128 }, { text: "256 frames", value: 256 },
                    { text: "512 frames", value: 512 }, { text: "1024 frames", value: 1024 }]
        }
        function applySettings(device, rate, frames, volume) {
            lastApply = [device, rate, frames, volume]
            if (rejectSettings) {
                errorText = "Driver rejected request; restored previously accepted device."
                return false
            }
            deviceIndex = device
            sampleRate = rate
            framesPerBuffer = frames
            masterVolume = volume
            return true
        }
        function showDeviceControlPanel() {
            panelCalls++
            errorText = "Test panel failed; stream reopened."
            return false
        }
    }
    Component { id: dialogComponent; Components.SettingsDialog { height: 1000 } }
    TestCase {
        name: "AudioSettingsUI"
        when: windowShown
        property var dialog: null
        function control(name) {
            const result = findChild(dialog, name)
            verify(result !== null, name)
            return result
        }
        function selectRow(combo, row) {
            mouseClick(combo)
            tryCompare(combo.popup, "opened", true)
            const list = combo.popup.contentItem
            list.positionViewAtIndex(row, ListView.Contain)
            tryVerify(function() { return list.itemAtIndex(row) !== null })
            mouseClick(list.itemAtIndex(row))
            tryCompare(combo.popup, "visible", false)
        }
        function init() {
            audioEngine.available = true
            audioEngine.asioCompiled = true
            audioEngine.errorText = ""
            audioEngine.devices = [
                { index: 3, name: "Shared output", api: "WASAPI", apiType: 13, isAsio: false, sampleRate: 44100 },
                { index: 7, name: "ASIO output", api: "ASIO", apiType: 3, isAsio: true, sampleRate: 48000 }
            ]
            audioEngine.deviceIndex = -1
            audioEngine.sampleRate = 0
            audioEngine.framesPerBuffer = 0
            audioEngine.masterVolume = 73
            audioEngine.defaultAsio = false
            audioEngine.capabilityError = ""
            audioEngine.rejectSettings = false
            audioEngine.lastApply = []
            audioEngine.panelCalls = 0
            dialog = createTemporaryObject(dialogComponent, window)
            verify(dialog !== null)
            dialog.open()
            tryCompare(dialog, "opened", true)
        }
        function cleanup() {
            dialog.close()
            tryCompare(dialog, "visible", false)
        }
        function test_default_and_filtered_current_selection() {
            const devices = control("audioDeviceCombo")
            compare(devices.count, 3)
            compare(devices.model[0].index, -1)
            compare(devices.currentIndex, 0)
            compare(control("audioRateCombo").model[0].text, "设备默认")
            selectRow(control("audioApiFilter"), 2)
            compare(devices.count, 1)
            compare(devices.model[0].index, 7)
            compare(devices.currentIndex, -1)
            compare(devices.displayText, "")
            compare(audioEngine.lastApply.length, 0)
            compare(control("asioControlPanelButton").enabled, false)
            selectRow(devices, 0)
            compare(audioEngine.lastApply, [7, 0, 0, 73])
            compare(devices.currentIndex, 0)
            compare(control("asioControlPanelButton").enabled, true)
            selectRow(control("audioApiFilter"), 1)
            compare(devices.currentIndex, -1)
            // Filtering is not a device change; panel still targets accepted ASIO device.
            compare(control("asioControlPanelButton").enabled, true)
        }
        function test_device_activation_resets_buffer_and_default_is_selectable() {
            audioEngine.framesPerBuffer = 512
            selectRow(control("audioDeviceCombo"), 2)
            compare(audioEngine.lastApply, [7, 0, 0, 73])
            compare(control("audioRateCombo").model[0].text, "设备默认（48000 Hz）")
            selectRow(control("audioDeviceCombo"), 0)
            compare(audioEngine.lastApply, [-1, 0, 0, 73])
            compare(control("audioDeviceCombo").currentIndex, 0)
        }
        function test_legal_buffer_values_and_rollback_selection() {
            selectRow(control("audioDeviceCombo"), 2)
            const buffers = control("audioBufferCombo")
            compare(buffers.count, 4)
            audioEngine.framesPerBuffer = 192
            tryCompare(buffers, "currentIndex", 2)
            selectRow(buffers, 3)
            compare(audioEngine.lastApply, [7, 0, 288, 73])
            compare(buffers.currentIndex, 3)
            audioEngine.rejectSettings = true
            selectRow(buffers, 1)
            compare(buffers.currentIndex, 3)
            selectRow(control("audioDeviceCombo"), 1)
            compare(control("audioDeviceCombo").currentIndex, 2)
            compare(audioEngine.deviceIndex, 7)
            audioEngine.framesPerBuffer = 999
            tryCompare(buffers, "currentIndex", -1)
        }
        function test_rate_rollback_restores_accepted_selection() {
            const rates = control("audioRateCombo")
            selectRow(rates, 1)
            compare(audioEngine.sampleRate, 44100)
            compare(rates.currentIndex, 1)
            audioEngine.rejectSettings = true
            selectRow(rates, 2)
            compare(audioEngine.lastApply, [-1, 48000, 0, 73])
            compare(audioEngine.sampleRate, 44100)
            compare(rates.currentIndex, 1)
            audioEngine.sampleRate = 96000
            tryCompare(rates, "currentIndex", -1)
            compare(rates.displayText, "")
            audioEngine.sampleRate = 0
            tryCompare(rates, "currentIndex", 0)
        }
        function test_volume_rollback_restores_accepted_value() {
            const volume = control("audioVolumeSlider")
            volume.forceActiveFocus()
            keyClick(Qt.Key_Right)
            compare(audioEngine.masterVolume, 74)
            compare(volume.value, 74)
            audioEngine.rejectSettings = true
            keyClick(Qt.Key_Right)
            compare(audioEngine.lastApply, [-1, 0, 0, 75])
            compare(audioEngine.masterVolume, 74)
            compare(volume.value, 74)
            audioEngine.masterVolume = 61
            tryCompare(volume, "value", 61)
        }
        function test_default_asio_panel_enable_and_failure() {
            compare(control("asioControlPanelButton").enabled, false)
            audioEngine.defaultAsio = true
            // A query's index is unchanged: recreate to model cached init capabilities.
            dialog.close()
            dialog.destroy()
            dialog = createTemporaryObject(dialogComponent, window)
            dialog.open()
            tryCompare(dialog, "opened", true)
            const panel = control("asioControlPanelButton")
            compare(panel.enabled, true)
            mouseClick(panel)
            compare(audioEngine.panelCalls, 1)
            compare(panel.enabled, true)
            audioEngine.available = false
            compare(panel.enabled, true)
            audioEngine.asioCompiled = false
            compare(panel.enabled, false)
            compare(control("audioErrorText").text, audioEngine.errorText)
        }
        function test_capability_error_keeps_recovery_panel_available() {
            audioEngine.capabilityError = "Capability query failed"
            selectRow(control("audioDeviceCombo"), 2)
            compare(control("asioControlPanelButton").enabled, true)
            compare(control("asioCapabilitiesError").text, "Capability query failed")
            audioEngine.available = false
            compare(control("asioControlPanelButton").enabled, true)
            selectRow(control("audioDeviceCombo"), 1)
            compare(control("asioControlPanelButton").enabled, false)
        }
        function test_asio_availability_distinguishes_build_and_devices() {
            audioEngine.asioCompiled = false
            compare(control("asioAvailabilityText").text, "此版本未编译 ASIO 支持。")
            audioEngine.devices = []
            audioEngine.asioCompiled = true
            compare(control("asioAvailabilityText").text, "已编译 ASIO 支持，但未发现兼容的 ASIO 输出设备。")
        }
        function test_long_rollback_error_wraps_without_fixed_height() {
            audioEngine.errorText = ("Audio stream reopen failed; rollback also failed. ").repeat(12)
            const bar = control("audioErrorBar")
            const label = control("audioErrorText")
            tryVerify(function() { return label.implicitHeight > 36 })
            verify(bar.height >= label.implicitHeight + 16)
            compare(bar.opacity, 1)
            compare(label.opacity, 1)
            verify(Math.abs(bar.color.a - 0.15) < 0.01)
            verify(label.color.a > 0.99)
        }
    }
}
