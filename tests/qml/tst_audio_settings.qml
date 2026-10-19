// SPDX-License-Identifier: GPL-3.0-only
import QtQuick
import QtQuick.Controls
import QtTest
import "../../app/qml/components" as Components

Item {
    id: window
    width: 1000
    height: 760
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
        property int toneCalls: 0
        function playTestTone() { toneCalls++ }
        function setWaitRenderSetting(value) { waitRenderSetting = value }
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
    Component { id: dialogComponent; Components.SettingsDialog {} }
    TestCase {
        name: "AudioSettingsUI"
        when: windowShown
        property var dialog: null
        function control(name) {
            const result = findChild(dialog, name)
            verify(result !== null, name)
            return result
        }
        function reveal(item) {
            const scroll = control("audioScrollView")
            const view = scroll.contentItem
            const y = item.mapToItem(control("audioSettingsColumn"), 0, 0).y
            view.contentY = Math.max(0, Math.min(view.contentHeight - view.height,
                                                y - (view.height - item.height) / 2))
            waitForRendering(scroll)
        }
        function selectRow(combo, row) {
            reveal(combo)
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
            audioEngine.toneCalls = 0
            audioEngine.activeDeviceText = "Accepted device"
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
            reveal(volume)
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
            reveal(panel)
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
        function assertHorizontallyContained(item) {
            const scroll = control("audioScrollView")
            const point = item.mapToItem(scroll, 0, 0)
            verify(point.x >= -1, item.objectName + " starts outside viewport")
            verify(point.x + item.width <= scroll.availableWidth + 1,
                   item.objectName + " ends outside viewport")
            verify(item.width > 0, item.objectName + " has usable width")
        }
        function assertBottomVisible() {
            const scroll = control("audioScrollView")
            const footer = control("settingsFooter")
            for (const name of ["audioTestToneButton", "audioActualStatus"]) {
                const item = control(name)
                const point = item.mapToItem(scroll, 0, 0)
                verify(point.y >= -1, name + " above viewport")
                verify(point.y + item.height <= scroll.availableHeight + 1, name + " below viewport")
                const bottom = item.mapToItem(footer, 0, item.height)
                verify(bottom.y <= 0, name + " overlaps footer")
                assertHorizontallyContained(item)
            }
            verify(control("audioActualStatusText").text.indexOf("48000 Hz") !== -1)
            verify(control("audioActualStatusText").text.indexOf("4.5 ms") !== -1)
        }
        function wheelToBottom() {
            const scroll = control("audioScrollView")
            const view = scroll.contentItem
            // Send real wheel events over blank space, not a Slider/ComboBox that consumes them.
            // Ten-notch bursts keep travel bounded even with small platform wheel steps.
            const initialY = view.contentY
            for (let i = 0; i < 40 && view.contentY < view.contentHeight - view.height - 1; ++i) {
                mouseWheel(scroll, scroll.availableWidth - 2, scroll.height / 2, 0, -1200)
                wait(40)
            }
            tryVerify(function() { return view.contentY >= view.contentHeight - view.height - 1 })
            verify(view.contentY > initialY || initialY >= view.contentHeight - view.height - 1,
                   "Actual wheel input must move overflowing content")
            tryCompare(view, "moving", false)
            waitForRendering(scroll)
        }
        function test_default_and_narrow_layout_data() {
            return [{ tag: "default", width: 680 }, { tag: "narrow", width: 520 }]
        }
        function test_default_and_narrow_layout(data) {
            compare(dialog.width, 680)
            compare(dialog.height, 520)
            dialog.width = data.width
            const longName = "Long manufacturer output device / ".repeat(5)
            audioEngine.devices = [{ index: 3, name: longName, api: "WASAPI", apiType: 13,
                                     isAsio: false, sampleRate: 48000 }]
            audioEngine.deviceIndex = 3
            audioEngine.activeDeviceText = longName
            const scroll = control("audioScrollView")
            const column = control("audioSettingsColumn")
            tryCompare(column, "width", scroll.availableWidth)
            compare(scroll.contentWidth, scroll.availableWidth)
            tryVerify(function() { return scroll.contentHeight > scroll.availableHeight })
            compare(scroll.contentHeight, column.implicitHeight)
            const bar = control("audioVerticalScrollBar")
            compare(bar.policy, ScrollBar.AlwaysOn)
            tryVerify(function() { return bar.visible && bar.opacity > 0 })
            for (const name of ["audioApiFilter", "audioDeviceCombo", "audioRateCombo",
                                "audioBufferCombo", "audioVolumeSlider", "audioVolumePercent",
                                "asioAvailabilityText", "asioControlPanelWarning", "audioBufferExplanation"])
                assertHorizontallyContained(control(name))
            const percent = control("audioVolumePercent")
            compare(percent.text, "73%")
            verify(percent.width >= percent.implicitWidth)
            reveal(percent)
            const point = percent.mapToItem(scroll, 0, 0)
            verify(point.y >= 0 && point.y + percent.height <= scroll.availableHeight)
            scroll.forceActiveFocus()
            keyClick(Qt.Key_Home)
            tryCompare(scroll.contentItem, "contentY", 0)
            // Real wheel input must reach the actual status and clickable test tone.
            wheelToBottom()
            assertBottomVisible()
            mouseClick(control("audioTestToneButton"))
            compare(audioEngine.toneCalls, 1, "tone enabled=" + control("audioTestToneButton").enabled
                    + " geometry=" + control("audioTestToneButton").width + "x" + control("audioTestToneButton").height
                    + " moving=" + control("audioScrollView").contentItem.moving)
        }
        function test_scrollbar_and_keyboard_reach_bottom() {
            const scroll = control("audioScrollView")
            const bar = control("audioVerticalScrollBar")
            const view = scroll.contentItem
            waitForRendering(scroll)
            tryVerify(function() { return bar.size > 0 && bar.size < 1 })
            tryVerify(function() { return Math.abs(bar.height - scroll.availableHeight) <= 1 },
                      5000, "Scrollbar track must span the viewport, not remain thumb-sized")
            // Drag the actual thumb, rather than assigning position/contentY.
            const thumbY = bar.topPadding + (bar.visualPosition + bar.visualSize / 2) * bar.availableHeight
            mouseDrag(bar, bar.width / 2, thumbY, 0, bar.availableHeight - thumbY - 1, Qt.LeftButton)
            tryVerify(function() { return view.contentY >= view.contentHeight - view.height - 1 })
            assertBottomVisible()
            scroll.forceActiveFocus()
            keyClick(Qt.Key_Home)
            tryCompare(view, "contentY", 0)
            keyClick(Qt.Key_Down)
            tryVerify(function() { return view.contentY > 0 })
            keyClick(Qt.Key_Home)
            tryCompare(view, "contentY", 0)
            keyClick(Qt.Key_PageDown)
            tryVerify(function() { return view.contentY > 0 })
            keyClick(Qt.Key_End)
            tryVerify(function() { return view.contentY >= view.contentHeight - view.height - 1 })
            assertBottomVisible()
        }
        function test_long_rollback_error_wraps_without_fixed_height() {
            // Multiline records guarantee overflow even with the smallest skin font;
            // each record is also long enough to exercise width-dependent wrapping.
            audioEngine.errorText = (("Audio stream reopen failed; rollback also failed. ").repeat(4) + "\n").repeat(16)
            const bar = control("audioErrorBar")
            const label = control("audioErrorText")
            const scroll = control("audioScrollView")
            waitForRendering(scroll)
            tryVerify(function() { return label.implicitHeight > scroll.availableHeight })
            tryVerify(function() { return bar.height >= label.implicitHeight + 16 })
            verify(scroll.availableHeight > 200, "Error must scroll, not consume the viewport")
            compare(bar.opacity, 1)
            compare(label.opacity, 1)
            verify(Math.abs(bar.color.a - 0.15) < 0.01)
            verify(label.color.a > 0.99)
            assertHorizontallyContained(bar)
            wheelToBottom()
            assertBottomVisible()
            mouseClick(control("audioTestToneButton"))
            compare(audioEngine.toneCalls, 1)
        }
        function test_capture_default_settings() {
            // Opt in by selecting this exact QuickTest function on the command line.
            // No production screenshot hook or environment dependency is needed.
            if (Qt.application.arguments.indexOf("AudioSettingsUI::test_capture_default_settings") < 0)
                skip("Select AudioSettingsUI::test_capture_default_settings to save acceptance images")
            compare(dialog.width, 680)
            compare(dialog.height, 520)
            let path = decodeURIComponent(Qt.resolvedUrl("../../local/screenshot/").toString().replace(/^file:\/\//, ""))
            if (/^\/[A-Za-z]:/.test(path)) path = path.substring(1)
            waitForRendering(dialog.contentItem)
            const topImage = grabImage(dialog.contentItem.parent)
            compare(topImage.width, 680)
            compare(topImage.height, 520)
            topImage.save(path + "audio-settings-default.png")
            wheelToBottom()
            assertBottomVisible()
            const bottomImage = grabImage(dialog.contentItem.parent)
            compare(bottomImage.width, 680)
            compare(bottomImage.height, 520)
            bottomImage.save(path + "audio-settings-default-bottom.png")
        }
    }
}
