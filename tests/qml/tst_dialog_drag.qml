// SPDX-License-Identifier: GPL-3.0-only
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtTest
import "../../app/qml/components" as Components

Item {
        id: window
        width: 960
        height: 720
        readonly property Item contentItem: window.Window.window ? window.Window.window.contentItem : null
        property int apiFilterIndex: 0
        property int backgroundClicks: 0
        property int bodyClicks: 0
        Button {
            id: behindButton
            x: 10; y: 10
            text: "behind modal"
            onClicked: window.backgroundClicks++
        }
        Components.BbDialog {
            id: centeredDialog
            width: 340; height: 250
            title: "centered test"
            closePolicy: Popup.CloseOnEscape
            TextField { id: editor; Layout.fillWidth: true; text: "editable text" }
            Button { id: bodyButton; text: "body button"; onClicked: window.bodyClicks++ }
        }
        Item {
            id: offsetParent
            x: 137; y: 83; width: 480; height: 360
            Components.BbDialog {
                id: nestedDialog
                width: 300; height: 200
                title: "non-overlay parent"
            }
        }
        Components.SettingsDialog { id: settingsDialog }
        QtObject {
            id: chartSession
            function idTextOf(value) { return value === 1 ? "01" : "00" }
        }
        Components.BgaPanel {
            id: bgaPanel
            x: 310; y: 80; width: 250; height: 500
            events: [{measure: 2, pos: {num: 1, den: 2}, sample: 1}]
            bmpDefs: [{id: "01", file: "image.png", refs: 1}]
        }
        Components.TimelinePanel { id: timelinePanel; x: 650; y: 90; width: 260; height: 480 }
        Dialog {
            id: legacyDialog
            width: 260; height: 180
            modal: true
            title: "right aligned standalone"
            standardButtons: Dialog.Ok | Dialog.Cancel
            Components.BbDialogPositioner {
                id: legacyPositioner
                dialog: legacyDialog
                horizontalAlignment: Qt.AlignRight
            }
            header: Components.BbDialogHeader { dialog: legacyDialog; positioner: legacyPositioner }
            Label { text: "standard footer remains usable" }
        }
    TestCase {
        name: "InWindowDialogDrag"
        when: windowShown

        function pointOf(dialog) {
            return dialog.parent.mapToItem(window.contentItem, dialog.x, dialog.y)
        }
        function comparePoint(actual, x, y) {
            verify(Math.abs(actual.x - x) <= 1, "x: " + actual.x + " expected " + x)
            verify(Math.abs(actual.y - y) <= 1, "y: " + actual.y + " expected " + y)
        }
        function openDialog(dialog) {
            dialog.open()
            tryCompare(dialog, "opened", true)
            waitForRendering(dialog.header)
        }
        function drag(dialog, dx, dy, button) {
            const p = dialog.header.mapToItem(window.contentItem, 48, 17)
            const b = button === undefined ? Qt.LeftButton : button
            mousePress(window.contentItem, p.x, p.y, b)
            mouseMove(window.contentItem, p.x + dx, p.y + dy, 20)
            mouseRelease(window.contentItem, p.x + dx, p.y + dy, b)
        }
        function init() {
            window.width = 960
            window.height = 720
            centeredDialog.width = 340
            centeredDialog.height = 250
            window.backgroundClicks = 0
            window.bodyClicks = 0
        }
        function cleanup() {
            centeredDialog.close()
            nestedDialog.close()
            settingsDialog.close()
            legacyDialog.close()
            const bga = findChild(bgaPanel, "bgaEventDialog")
            if (bga) bga.close()
            const timing = findChild(timelinePanel, "timelineEventDialog")
            if (timing) timing.close()
            tryCompare(centeredDialog, "visible", false)
            tryCompare(settingsDialog, "visible", false)
        }
        function test_center_drag_and_reopen_resets() {
            openDialog(centeredDialog)
            comparePoint(pointOf(centeredDialog), 310, 235)
            drag(centeredDialog, 90, 60)
            comparePoint(pointOf(centeredDialog), 400, 295)
            centeredDialog.close()
            tryCompare(centeredDialog, "visible", false)
            openDialog(centeredDialog)
            comparePoint(pointOf(centeredDialog), 310, 235)
        }
        function test_right_button_does_not_drag() {
            openDialog(centeredDialog)
            const before = pointOf(centeredDialog)
            drag(centeredDialog, 60, 40, Qt.RightButton)
            comparePoint(pointOf(centeredDialog), before.x, before.y)
        }
        function test_body_input_buttons_and_focus_are_preserved() {
            openDialog(centeredDialog)
            editor.forceActiveFocus()
            verify(editor.activeFocus)
            const before = pointOf(centeredDialog)
            mouseDrag(editor, 12, editor.height / 2, 70, 0)
            comparePoint(pointOf(centeredDialog), before.x, before.y)
            drag(centeredDialog, 40, 30)
            verify(editor.activeFocus, "dragging header must not steal text focus")
            keyClick(Qt.Key_End)
            keyClick(Qt.Key_X)
            verify(editor.text.endsWith("x") || editor.text.endsWith("X"))
            mouseClick(bodyButton)
            compare(window.bodyClicks, 1)
        }
        function test_modal_and_escape_remain_intact() {
            openDialog(centeredDialog)
            mouseClick(behindButton)
            compare(window.backgroundClicks, 0)
            verify(centeredDialog.visible)
            keyClick(Qt.Key_Escape)
            tryCompare(centeredDialog, "visible", false)
        }
        function test_window_resize_clamps_without_recentering() {
            openDialog(centeredDialog)
            drag(centeredDialog, 250, 160)
            window.width = 600
            window.height = 400
            tryVerify(function() {
                const p = pointOf(centeredDialog)
                return p.x >= 0 && p.y >= 0 && p.x + centeredDialog.width <= 600
                       && p.y + centeredDialog.height <= 400
            })
            const p = pointOf(centeredDialog)
            window.width = 960
            window.height = 720
            comparePoint(pointOf(centeredDialog), p.x, p.y)
        }
        function test_drag_stops_at_window_edges() {
            openDialog(centeredDialog)
            drag(centeredDialog, -500, -400)
            comparePoint(pointOf(centeredDialog), 8, 8)
            drag(centeredDialog, 1500, 1500)
            comparePoint(pointOf(centeredDialog), 612, 462)
        }
        function test_oversized_dialog_keeps_title_reachable() {
            centeredDialog.width = 1200
            centeredDialog.height = 900
            openDialog(centeredDialog)
            const p = pointOf(centeredDialog)
            verify(p.x < window.width && p.x + centeredDialog.width > 0)
            verify(p.y >= 0 && p.y + centeredDialog.header.height <= window.height)
            drag(centeredDialog, 0, 100)
            const after = pointOf(centeredDialog)
            verify(after.y >= 0 && after.y + centeredDialog.header.height <= window.height)
            window.width = 180
            window.height = 80
            tryVerify(function() {
                const v = pointOf(centeredDialog)
                return v.x < 180 && v.x + centeredDialog.width > 0 && v.y >= 0 && v.y <= 46
            })
        }
        function test_non_overlay_parent_maps_window_coordinates() {
            openDialog(nestedDialog)
            comparePoint(pointOf(nestedDialog), 330, 260)
            verify(nestedDialog.parent === offsetParent)
            drag(nestedDialog, 60, 40)
            comparePoint(pointOf(nestedDialog), 390, 300)
            window.width = 500
            window.height = 340
            tryVerify(function() {
                const p = pointOf(nestedDialog)
                return p.x >= 0 && p.y >= 0 && p.x + 300 <= 500 && p.y + 200 <= 340
            })
        }
        function test_settings_default_and_reopen_policy() {
            openDialog(settingsDialog)
            comparePoint(pointOf(settingsDialog), 268, 40)
            drag(settingsDialog, -90, 50)
            comparePoint(pointOf(settingsDialog), 178, 90)
            settingsDialog.close()
            tryCompare(settingsDialog, "visible", false)
            openDialog(settingsDialog)
            comparePoint(pointOf(settingsDialog), 268, 40)
        }
        function doubleClickFirstEvent(panel, listName) {
            const list = findChild(panel, listName)
            verify(list !== null)
            tryVerify(() => list.itemAtIndex(0) !== null)
            const row = list.itemAtIndex(0)
            mouseDoubleClickSequence(row, 25, row.height / 2)
        }
        function test_dock_numeric_click_accept_data() {
            return [{ tag: "bga", bga: true }, { tag: "timeline", bga: false }]
        }
        function openDockEditor(data) {
            const panel = data.bga ? bgaPanel : timelinePanel
            if (!data.bga) {
                timelinePanel.kind = "bpm"
                timelinePanel.bpmEvents = [{measure: 2, pos: {num: 1, den: 2}, value: 180, ref: ""}]
            }
            const dialog = findChild(panel, data.bga ? "bgaEventDialog" : "timelineEventDialog")
            verify(dialog !== null)
            doubleClickFirstEvent(panel, data.bga ? "bgaEventList" : "timelineEventList")
            tryCompare(dialog, "opened", true)
            waitForRendering(dialog.contentItem)
            return dialog
        }
        function numericEditors(body) {
            const editors = []
            function collect(item) {
                if (typeof item.commitPendingEdit === "function") {
                    editors.push(item.contentItem)
                    return
                }
                const children = item.children
                for (let i = 0; i < children.length; ++i) collect(children[i])
            }
            collect(body)
            compare(editors.length, 3)
            return editors
        }
        function replaceNumericText(editor, key) {
            mouseClick(editor)
            verify(editor.activeFocus)
            keyClick(Qt.Key_A, Qt.ControlModifier)
            keyClick(key)
        }
        function watchDockSubmit(data) {
            const spy = createTemporaryQmlObject('import QtTest; SignalSpy {}', window)
            spy.target = data.bga ? bgaPanel : timelinePanel
            spy.signalName = data.bga ? "bgaEditRequested" : "timingEditRequested"
            verify(spy.valid)
            return spy
        }
        function test_dock_numeric_click_accept(data) {
            const spy = watchDockSubmit(data)
            const dialog = openDockEditor(data)
            const measure = numericEditors(dialog.contentItem)[0]
            replaceNumericText(measure, Qt.Key_7)
            compare(measure.text, "7")
            mouseClick(findChild(dialog.footer, "bbDialogAcceptButton"))
            tryCompare(dialog, "visible", false)
            compare(spy.count, 1)
            compare(Array.from(spy.signalArguments[0]).slice(1, 4), [7, 1, 2])
        }
        function test_dock_invalid_numeric_click_accept_data() {
            return test_dock_numeric_click_accept_data()
        }
        function test_dock_invalid_numeric_click_accept(data) {
            const spy = watchDockSubmit(data)
            const dialog = openDockEditor(data)
            const den = numericEditors(dialog.contentItem)[2]
            replaceNumericText(den, Qt.Key_Backspace)
            compare(den.text, "")
            verify(!den.acceptableInput)
            mouseClick(findChild(dialog.footer, "bbDialogAcceptButton"))
            compare(spy.count, 0)
            verify(dialog.opened)
            compare(den.text, "")
            replaceNumericText(den, Qt.Key_4)
            mouseClick(findChild(dialog.footer, "bbDialogAcceptButton"))
            tryCompare(dialog, "visible", false)
            compare(spy.count, 1)
            compare(Array.from(spy.signalArguments[0]).slice(1, 4), [2, 1, 4])
        }
        function test_actual_bga_dialog_drag() {
            const dialog = findChild(bgaPanel, "bgaEventDialog")
            verify(dialog !== null)
            doubleClickFirstEvent(bgaPanel, "bgaEventList")
            tryCompare(dialog, "opened", true)
            compare(dialog.background.color, Theme.surface)
            verify(dialog.contentItem.height + 1 >= dialog.contentItem.implicitHeight)
            verify(dialog.contentItem.mapToItem(dialog.footer, 0, dialog.contentItem.height).y <= 1)
            const before = pointOf(dialog)
            comparePoint(before, (960 - dialog.width) / 2, (720 - dialog.height) / 2)
            drag(dialog, -50, 30)
            comparePoint(pointOf(dialog), before.x - 50, before.y + 30)
            mouseClick(findChild(dialog.footer, "bbDialogCancelButton"))
            tryCompare(dialog, "visible", false)
        }
        function test_actual_timeline_dialog_drag() {
            const dialog = findChild(timelinePanel, "timelineEventDialog")
            verify(dialog !== null)
            dialog.openForAdd("bpm")
            tryCompare(dialog, "opened", true)
            const before = pointOf(dialog)
            comparePoint(before, 960 - dialog.width - 12, (720 - dialog.height) / 2)
            drag(dialog, -50, 30)
            comparePoint(pointOf(dialog), before.x - 50, before.y + 30)
            mouseClick(findChild(dialog.footer, "bbDialogCancelButton"))
            tryCompare(dialog, "visible", false)
        }
        function test_timeline_initial_theme_and_form_bounds_data() {
            return [{ tag: "bpm", kind: "bpm", value: 180, ref: "03" },
                    { tag: "stop", kind: "stop", value: 96, ref: "01" },
                    { tag: "measure", kind: "measure", value: 4, ref: "" }]
        }
        function test_timeline_initial_theme_and_form_bounds(data) {
            const dialog = findChild(timelinePanel, "timelineEventDialog")
            verify(dialog !== null)
            const events = [{measure: 21, pos: {num: 1, den: 2}, value: data.value, ref: data.ref}]
            timelinePanel.bpmEvents = events
            timelinePanel.stopEvents = events
            timelinePanel.measureEvents = events
            timelinePanel.kind = data.kind
            doubleClickFirstEvent(timelinePanel, "timelineEventList")
            tryCompare(dialog, "opened", true)
            const body = dialog.contentItem
            waitForRendering(body)
            compare(dialog.background.color, Theme.surface)
            compare(dialog.footer.color, Theme.surface2)
            const ok = findChild(dialog.footer, "bbDialogAcceptButton")
            const cancel = findChild(dialog.footer, "bbDialogCancelButton")
            verify(ok !== null && cancel !== null)
            for (const button of [ok, cancel]) {
                compare(button.background.color, Theme.surface2)
                compare(button.contentItem.color, Theme.textMuted)
            }
            verify(body.height + 1 >= body.implicitHeight)
            let fields = 0
            function inspect(item) {
                if (!item.visible) return
                if (item.text !== undefined && item.font !== undefined && item.text !== "") {
                    const p = item.mapToItem(body, 0, 0)
                    verify(p.x >= -1 && p.x + item.width <= body.width + 1, item.text)
                    verify(p.y >= -1 && p.y + item.height <= body.height + 1, item.text)
                    if (item.contentHeight !== undefined)
                        verify(item.contentHeight <= item.height + 1, item.text)
                    ++fields
                }
                const children = item.children
                for (let i = 0; i < children.length; ++i) inspect(children[i])
            }
            inspect(body)
            verify(fields >= 4)
            verify(body.mapToItem(dialog.footer, 0, body.height).y <= 1)
            mouseClick(cancel)
            tryCompare(dialog, "visible", false)
        }
        function test_standalone_header_does_not_break_standard_buttons() {
            openDialog(legacyDialog)
            comparePoint(pointOf(legacyDialog), 688, 270)
            drag(legacyDialog, -50, 40)
            comparePoint(pointOf(legacyDialog), 638, 310)
            const ok = legacyDialog.standardButton(Dialog.Ok)
            verify(ok !== null)
            mouseClick(ok)
            tryCompare(legacyDialog, "visible", false)
        }
    }
}
