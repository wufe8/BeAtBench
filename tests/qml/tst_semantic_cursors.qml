// SPDX-License-Identifier: GPL-3.0-only
import QtQuick
import QtQuick.Controls
import QtTest
import "../../app/qml/components" as Components

Item {
    id: scene
    width: 640; height: 480
    Component { id: tool; Components.BbToolButton { text: "Action" } }
    Component { id: check; Components.BbCheckBox { text: "Option" } }
    Component { id: tab; Components.BbTabButton { text: "Tab" } }
    Component { id: combo; Components.BbComboBox { model: ["First", "Second"] } }
    Component { id: field; Components.BbTextField { text: "selectable text" } }
    Component { id: spin; Components.BbSpinBox { from: 0; to: 100; value: 12; editable: true } }
    Component { id: pages; Components.PageSwitcher {} }
    Component { id: dialogComponent; Components.BbDialog { width: 260; height: 180; title: "Drag title" } }
    SignalSpy { id: clickedSpy; signalName: "clicked" }
    SignalSpy { id: activatedSpy; signalName: "activated" }
    SignalSpy { id: pageSpy; signalName: "pageRequested" }

    TestCase {
        name: "SemanticControlCursors"
        when: windowShown
        function cursor(control) {
            const handler = findChild(control, "semanticCursor")
            verify(handler !== null)
            return handler
        }
        function make(component) {
            const control = createTemporaryObject(component, scene, { x: 20, y: 20, width: 180, height: 32 })
            verify(control !== null)
            return control
        }
        function cleanup() {
            clickedSpy.target = null
            activatedSpy.target = null
            pageSpy.target = null
        }
        function test_clickable_controls_hand_disabled_arrow_data() {
            return [{tag: "tool", component: tool}, {tag: "checkbox", component: check},
                    {tag: "tab", component: tab}, {tag: "combo", component: combo}]
        }
        function test_clickable_controls_hand_disabled_arrow(data) {
            const control = make(data.component)
            const handler = cursor(control)
            compare(handler.cursorShape, Qt.PointingHandCursor)
            control.enabled = false
            compare(handler.cursorShape, Qt.ArrowCursor)
            control.enabled = true
            compare(handler.cursorShape, Qt.PointingHandCursor)
        }
        function test_passive_cursor_preserves_click_and_disabled_behavior_data() {
            return [{tag: "tool", component: tool}, {tag: "checkbox", component: check},
                    {tag: "tab", component: tab}]
        }
        function test_passive_cursor_preserves_click_and_disabled_behavior(data) {
            const control = make(data.component)
            clickedSpy.target = control
            clickedSpy.clear()
            mouseClick(control, control.width / 2, control.height / 2)
            compare(clickedSpy.count, 1)
            if (data.component === check) compare(control.checked, true)
            control.enabled = false
            mouseClick(control, control.width / 2, control.height / 2)
            compare(clickedSpy.count, 1)
        }
        function test_text_ibeam_preserves_selection_and_typing() {
            const control = make(field)
            compare(cursor(control).cursorShape, Qt.IBeamCursor)
            mouseClick(control, 20, control.height / 2)
            verify(control.activeFocus)
            control.selectAll()
            compare(control.selectedText, "selectable text")
            keyClick(Qt.Key_X)
            compare(control.text, "x")
            control.readOnly = true
            compare(cursor(control).cursorShape, Qt.IBeamCursor) // selectable, not an action
            control.enabled = false
            compare(cursor(control).cursorShape, Qt.ArrowCursor)
        }
        function test_spin_editor_ibeam_arrows_hand_and_step_delivery() {
            const control = make(spin)
            const editorCursor = cursor(control.contentItem)
            const up = findChild(control.up.indicator, "spinStepArea")
            const down = findChild(control.down.indicator, "spinStepArea")
            verify(up !== null && down !== null)
            compare(editorCursor.cursorShape, Qt.IBeamCursor)
            compare(up.cursorShape, Qt.PointingHandCursor)
            compare(down.cursorShape, Qt.PointingHandCursor)
            mouseClick(up, up.width / 2, up.height / 2)
            compare(control.value, 13)
            mouseClick(down, down.width / 2, down.height / 2)
            compare(control.value, 12)
            control.editable = false
            compare(editorCursor.cursorShape, Qt.ArrowCursor)
            control.enabled = false
            compare(up.cursorShape, Qt.ArrowCursor)
            compare(down.cursorShape, Qt.ArrowCursor)
        }
        function test_combo_popup_hand_preserves_activation() {
            const control = make(combo)
            activatedSpy.target = control
            activatedSpy.clear()
            mouseClick(control, control.width / 2, control.height / 2)
            tryCompare(control.popup, "opened", true)
            const list = control.popup.contentItem
            list.forceLayout()
            const second = list.itemAtIndex(1)
            verify(second !== null)
            compare(cursor(second).cursorShape, Qt.PointingHandCursor)
            mouseClick(second, second.width / 2, second.height / 2)
            compare(control.currentIndex, 1)
            compare(activatedSpy.count, 1)
            tryCompare(control.popup, "visible", false)
        }
        function test_dialog_title_open_closed_hand_and_disabled_default() {
            const dialog = createTemporaryObject(dialogComponent, scene)
            verify(dialog !== null)
            dialog.open()
            tryCompare(dialog, "opened", true)
            const drag = findChild(dialog.header, "dialogDragArea")
            verify(drag !== null)
            compare(drag.cursorShape, Qt.OpenHandCursor)
            mousePress(drag, 50, 17)
            compare(drag.cursorShape, Qt.ClosedHandCursor)
            mouseRelease(drag, 50, 17)
            compare(drag.cursorShape, Qt.OpenHandCursor)
            dialog.header.enabled = false
            compare(drag.cursorShape, Qt.ArrowCursor)
            dialog.close()
        }
        function test_page_tab_hand_preserves_page_request() {
            const control = make(pages)
            control.width = 600
            const button = findChild(control, "pageTab1")
            verify(button !== null)
            compare(cursor(button).cursorShape, Qt.PointingHandCursor)
            pageSpy.target = control
            pageSpy.clear()
            mouseClick(button, button.width / 2, button.height / 2)
            compare(pageSpy.count, 1)
            compare(pageSpy.signalArguments[0][0], 1)
            control.enabled = false
            compare(cursor(button).cursorShape, Qt.ArrowCursor)
        }
    }
}
