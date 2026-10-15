// SPDX-License-Identifier: GPL-3.0-only
import QtQuick
import QtTest
import "../../app/qml/pages" as Pages

Item {
    id: scene
    // Keep the complete production export toolbar on screen, including its button.
    width: 1700
    height: 800

    Component {
        id: pageComponent
        Pages.SlicePage { width: scene.width; height: scene.height; placeToChart: true }
    }

    TestCase {
        id: testCase
        name: "SliceExportConfirmation"
        when: windowShown
        property var page
        property var idBox
        property var exportButton
        property var idDialog
        property var fileDialog
        property var before

        function named(name) {
            const item = findChild(page, name)
            verify(item !== null, "Missing production objectName: " + name)
            return item
        }
        function buttonWithText(item, text) {
            if (item.text === text && typeof item.clicked === "function") return item
            const children = item.children || []
            for (let i = 0; i < children.length; ++i) {
                const button = buttonWithText(children[i], text)
                if (button) return button
            }
            return null
        }
        function clickChoice(dialog, text) {
            tryCompare(dialog, "opened", true)
            const button = buttonWithText(dialog.footer, text)
            verify(button !== null, "Missing production choice: " + text)
            verify(button.enabled && button.visible)
            mouseClick(button, button.width / 2, button.height / 2, Qt.LeftButton)
        }
        function unchanged() {
            compare(JSON.stringify(sliceFixture.snapshot()), JSON.stringify(before),
                    "Preparing/choosing only one policy must not edit either document or any file")
            compare(page.rawText, "")
            compare(page.exportResult, null)
        }
        function init() {
            sliceFixture.reset()
            page = createTemporaryObject(pageComponent, scene)
            verify(page !== null)
            verify(waitForRendering(scene))
            idBox = named("exportIdBox")
            exportButton = named("sliceExportButton")
            idDialog = named("wavIdConflictDialog")
            fileDialog = named("exportConflictDialog")
            verify(exportButton.enabled)
            compare(chartSession.sessionId, "slice-confirmation-b")
            compare(sliceWorkspace.exportTargets.length, 2)
            compare(sliceWorkspace.slices.length, 2)
            compare(idBox.value, 6)
            compare(idBox.contentItem.text, "06")
            before = sliceFixture.snapshot()
            compare(before.active, "slice-confirmation-b")
            compare(Object.keys(before.a.samples).length, 3)
            compare(Object.keys(before.b.samples).length, 5)
        }
        function cleanup() {
            // Also clean up after an assertion fails with a modal popup still open.
            if (idDialog && idDialog.visible) idDialog.reject()
            if (fileDialog && fileDialog.visible) fileDialog.reject()
            sliceWorkspace.discardExportPlan()
            if (page) page.visible = false
        }

        function type04AndExport() {
            const input = idBox.contentItem // the actual BbSpinBox custom TextInput
            mouseClick(input, input.width / 2, input.height / 2, Qt.LeftButton)
            tryCompare(input, "activeFocus", true)
            keyClick(Qt.Key_A, Qt.ControlModifier)
            keyClick(Qt.Key_0)
            keyClick(Qt.Key_4)
            compare(input.text, "04")
            // Deliberately no Enter, no value assignment, no debugExport/doExport
            // call. A stale .value=06 would bypass the ID warning and show the
            // disk warning first; this is the regression, not a file-dialog bug.
            mouseClick(exportButton, exportButton.width / 2, exportButton.height / 2,
                       Qt.LeftButton)
            tryVerify(function() { return idDialog.visible || fileDialog.visible })
            verify(idDialog.visible, "Typed 04 must be prepared: ID warning must precede disk warning")
            verify(!fileDialog.visible, "Disk choice cannot bypass the ID choice")
            tryCompare(idDialog, "opened", true)
            compare(idDialog.preview.idCollisions.join(","), "04,05")
            compare(idDialog.preview.targets.length, 2)
            unchanged()
        }
        function authorizeIds(policy) {
            clickChoice(idDialog, policy === "skip" ? "从空位分配" : "覆盖已有 ID")
            tryCompare(fileDialog, "opened", true)
            verify(!idDialog.visible)
            compare(fileDialog.idPolicy, policy)
            compare(fileDialog.collisions.length, 2)
            compare(fileDialog.targets.length, 2)
            unchanged() // ID overwrite never authorizes file overwrite by itself
        }

        function test_cancel_at_either_confirmation_is_side_effect_free() {
            type04AndExport()
            clickChoice(idDialog, "取消")
            tryCompare(idDialog, "visible", false)
            verify(!fileDialog.visible)
            unchanged()

            // Retry from the real toolbar, then cancel after the ID choice.
            type04AndExport()
            authorizeIds("overwrite")
            clickChoice(fileDialog, "取消")
            tryCompare(fileDialog, "visible", false)
            unchanged()
        }

        function test_id_and_file_policies_data() {
            return [
                { tag: "overwrite-ids-overwrite-files", idPolicy: "overwrite", continueFiles: false, choice: "覆盖重名文件" },
                { tag: "overwrite-ids-continue-files", idPolicy: "overwrite", continueFiles: true, choice: "接续后续编号" },
                { tag: "skip-ids-overwrite-files", idPolicy: "skip", continueFiles: false, choice: "覆盖重名文件" },
                { tag: "skip-ids-continue-files", idPolicy: "skip", continueFiles: true, choice: "接续后续编号" }
            ]
        }
        function test_id_and_file_policies(data) {
            type04AndExport()
            authorizeIds(data.idPolicy)
            clickChoice(fileDialog, data.choice)
            tryVerify(function() { return page.exportResult !== null })
            verify(page.exportResult.ok, JSON.stringify(page.exportResult))
            verify(page.exportResult.placed)
            verify(page.exportResult.placementOk)
            compare(page.exportResult.count, 2)
            compare(page.exportResult.placedTargets, 2)
            const firstId = data.idPolicy === "skip" ? 6 : 4
            compare(page.exportResult.nextStartId, firstId + 2)
            compare(idBox.value, firstId + 2)
            verify(page.rawText.indexOf("#WAV0" + firstId + " ") >= 0)
            verify(page.rawText.indexOf("#WAV0" + (firstId + 1) + " ") >= 0)
            if (data.idPolicy === "overwrite")
                verify(page.rawText.indexOf("#WAV06 ") < 0, "ID overwrite must not silently skip to free IDs")

            const after = sliceFixture.snapshot()
            compare(after.active, before.active)
            const firstFile = data.continueFiles ? "slice_002.wav" : "slice_000.wav"
            const secondFile = data.continueFiles ? "slice_003.wav" : "slice_001.wav"
            for (const name of ["a", "b"]) {
                const doc = after[name]
                compare(doc.samples["" + firstId], firstFile)
                compare(doc.samples["" + (firstId + 1)], secondFile)
                if (data.idPolicy === "overwrite") {
                    verify(doc.samples["6"] === undefined)
                    verify(doc.samples["7"] === undefined)
                } else {
                    compare(doc.samples["4"], before[name].samples["4"])
                    compare(doc.samples["5"], before[name].samples["5"])
                }
                compare(JSON.stringify(doc.notes), JSON.stringify([firstId, firstId + 1]))
                verify(doc.dirty)
                compare(doc.undoDepth, 1)
            }
            for (const name of ["a", "b"])
                for (const id of ["1", "2", "3"])
                    compare(after[name].samples[id], before[name].samples[id])
            // Chart files remain unsaved, and the source audio is never replaced.
            for (const name of ["a.bms", "b.bms", "source.wav"])
                compare(after.disk[name], before.disk[name])
            compare(after.disk[firstFile].substring(0, 8), "52494646") // real RIFF bytes
            compare(after.disk[secondFile].substring(0, 8), "52494646")
            if (data.continueFiles) {
                compare(after.disk["slice_000.wav"], before.disk["slice_000.wav"])
                compare(after.disk["slice_001.wav"], before.disk["slice_001.wav"])
                compare(Object.keys(after.disk).length, 7)
            } else {
                verify(after.disk["slice_000.wav"] !== before.disk["slice_000.wav"])
                verify(after.disk["slice_001.wav"] !== before.disk["slice_001.wav"])
                verify(after.disk["slice_002.wav"] === undefined)
                verify(after.disk["slice_003.wav"] === undefined)
                compare(Object.keys(after.disk).length, 5)
            }
        }
    }
}
