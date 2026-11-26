// SPDX-License-Identifier: GPL-3.0-only
import QtQuick
import QtQuick.Controls
import QtTest
import "../../app/qml/components" as Components

Item {
    id: scene
    width: 400
    height: 240
    Components.BbSpinBox {
        id: box
        x: 20; y: 20
        from: 1; to: 1295; value: 22; editable: true
        validatorOverride: RegularExpressionValidator { regularExpression: /^[0-9A-Z]{0,2}$/ }
        textFromValue: function(v) { const s = v.toString(36).toUpperCase(); return s.length < 2 ? "0" + s : s }
        valueFromText: function(text) { return parseInt(text, 36) }
    }
    Button {
        id: action
        x: 20; y: 70
        focusPolicy: Qt.NoFocus
        property int capturedValue: -1
        onClicked: {
            if (typeof box.commitPendingEdit !== "function" || box.commitPendingEdit())
                capturedValue = box.value
        }
    }
    TextField { id: other; x: 20; y: 120 }
    TestCase {
        name: "SpinBoxPendingInput"
        when: windowShown
        function init() {
            other.forceActiveFocus()
            box.value = 22
            action.capturedValue = -1
        }
        function typeId(text) {
            box.contentItem.forceActiveFocus()
            box.contentItem.selectAll()
            for (var i = 0; i < text.length; ++i) keyClick(text.charAt(i))
            compare(box.contentItem.text, text)
        }
        function test_focusLoss_commits_typed_id() {
            typeId("04")
            other.forceActiveFocus()
            compare(box.value, 4)
        }
        function test_noFocusButton_reads_pending_text_without_Enter() {
            typeId("04")
            mouseClick(action, action.width / 2, action.height / 2)
            compare(action.capturedValue, 4)
        }
        function test_Enter_and_programmatic_autoAdvance_remain_consistent() {
            typeId("0A")
            keyClick(Qt.Key_Return)
            compare(box.value, 10)
            box.value = 22
            compare(box.contentItem.text, "0M")
        }
    }
}
