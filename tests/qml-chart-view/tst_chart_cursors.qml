// SPDX-License-Identifier: GPL-3.0-only
import QtQuick
import QtTest
import "../../app/qml/components" as Components

Item {
    id: scene
    width: 1100; height: 500
    Component {
        id: chartComponent
        Components.ChartView {
            width: 650; height: 450
            documentSession: bridgeA
            topHigh: false
        }
    }
    Component {
        id: compareComponent
        Components.ChartCompareSplit { width: 1050; height: 450; overlayMode: true }
    }
    TestCase {
        name: "SemanticChartCursors"
        when: windowShown
        property var chart
        property var area
        function init() {
            verify(bindingFixture.activate(false))
            chart = createTemporaryObject(chartComponent, scene)
            verify(chart !== null)
            area = findChild(chart, "chartInteractionArea")
            verify(area !== null)
            verify(waitForRendering(scene))
        }
        function test_opacity_slider_resize_cursor_preserves_value_input() {
            const split = createTemporaryObject(compareComponent, scene)
            verify(split !== null)
            const slider = findChild(split, "overlayOpacitySlider")
            verify(slider !== null)
            const handler = findChild(slider, "semanticCursor")
            verify(handler !== null)
            compare(handler.cursorShape, Qt.SizeHorCursor)
            mouseClick(slider, slider.width - slider.rightPadding - 1, slider.height / 2)
            verify(split.overlayOpacity > 0.45)
            slider.enabled = false
            compare(handler.cursorShape, Qt.ArrowCursor)
            const previous = split.overlayOpacity
            mouseClick(slider, slider.leftPadding + 1, slider.height / 2)
            compare(split.overlayOpacity, previous)
        }
        function test_tool_cursors_and_disabled_default() {
            chart.editorTool = "select"
            compare(area.cursorShape, Qt.ArrowCursor)
            chart.editorTool = "note"
            compare(area.cursorShape, Qt.CrossCursor)
            chart.editable = false
            compare(area.cursorShape, Qt.ArrowCursor)
            chart.editorTool = "pan"
            compare(area.cursorShape, Qt.OpenHandCursor) // read-only still navigates
            chart.enabled = false
            compare(area.cursorShape, Qt.ArrowCursor)
        }
        function test_left_pan_closed_hand_until_release() {
            chart.editorTool = "pan"
            compare(area.cursorShape, Qt.OpenHandCursor)
            mousePress(area, 200, 200, Qt.LeftButton)
            compare(area.cursorShape, Qt.ClosedHandCursor)
            mouseMove(area, 200, 240, 20)
            compare(area.cursorShape, Qt.ClosedHandCursor)
            mouseRelease(area, 200, 240, Qt.LeftButton)
            compare(area.cursorShape, Qt.OpenHandCursor)
        }
        function test_middle_pan_overrides_note_cross_and_restores() {
            chart.editorTool = "note"
            mousePress(area, 200, 200, Qt.MiddleButton)
            compare(chart._panning, true)
            compare(area.cursorShape, Qt.ClosedHandCursor)
            mouseMove(area, 200, 240, 20)
            mouseRelease(area, 200, 240, Qt.MiddleButton)
            compare(chart._panning, false)
            compare(area.cursorShape, Qt.CrossCursor)
        }
    }
}
