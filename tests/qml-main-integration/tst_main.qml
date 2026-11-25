// SPDX-License-Identifier: GPL-3.0-only
import QtQuick
import QtQuick.Controls
import QtTest
import "../../app/qml" as Production

TestCase {
    id: test
    name: "ProductionMainIntegration"
    when: windowShown
    property var main: null
    property var split: null
    readonly property string a: "main-integration-a"
    readonly property string b: "main-integration-b"
    Component { id: mainComponent; Production.Main {} }
    function start(named, modeFields, referenceAudition) {
        audioEngine.allowPreview = false
        audioEngine.previewFiles = []
        audioEngine.previewStops = 0
        audioEngine.previewFile = ""
        audioEngine.referenceNotePreviewCalls = []
        audioEngine.referenceNotePreview = null
        audioEngine.referenceNotePreviewStops = 0
        audioEngine.playing = false
        audioEngine.hasPcm = false
        audioEngine.positionSec = 0
        if (referenceAudition) mainFixture.resetForReferenceAudition()
        else mainFixture.reset(named)
        if (modeFields) {
            const result = JSON.parse(beatbench.dispatch(JSON.stringify({command: "meta.edit", args: {
                session_id: a, edits: [{key: "LNTYPE", value: "2"}, {key: "LNOBJ", value: "ZZ"},
                                      {key: "PLAYER", value: "4"}, {key: "RANK", value: "0"}]
            }})))
            verify(result.ok)
            documents.refresh()
        }
        const beforeA = mainFixture.snapshot(a), beforeB = mainFixture.snapshot(b)
        main = mainComponent.createObject(null)
        verify(main !== null)
        compare(main.currentPage, 0) // Visual Project-first order must not change default Edit.
        mainFixture.bind(main)
        main.requestActivate()
        main.adoptChart(mainFixture.snapshot(a).path)
        wait(0) // Flush obsolete Repeater delegates from initial component creation/reload.
        split = mainFixture.findProperty(main, "referenceDocumentId")
        verify(split !== null)
        // Start other integration tests with a predictable initial B viewport.
        tryVerify(() => split.usableWidth > 0)
        split.referenceShare = 360 / split.usableWidth
        verify(split.selectReference(b))
        tryVerify(function() { return split.referenceWidth > 250 })
        tryCompare(main, "chartEditingAllowed", true)
        unchanged(beforeA, beforeB)
        compare(main.metaDraftDirty, false)
    }
    function cleanup() {
        publicationHeartbeat.stop()
        mainFixture.releaseProjectScan()
        tryCompare(chartHistoryController, "busy", false)
        chartHistoryController.createOnClose = false
        chartHistoryController.enabled = false
        referenceContents.target = null
        if (main) { main.visible = false; main.destroy(); main = null; wait(0) }
        compare(audioEngine.playbackCalls, 0)
        const ids = documents.instances.map(d => d.id)
        for (let i = 0; i < ids.length; ++i) {
            if (ids[i] === a || ids[i] === b) continue
            const closed = JSON.parse(beatbench.dispatch(JSON.stringify({command: "session.close", args: {id: ids[i]}})))
            verify(closed.ok)
        }
        documents.refresh()
    }
    function clickButton(parent, text) {
        const button = mainFixture.button(parent, text)
        verify(button !== null, "Missing button: " + text)
        verify(button.visible)
        verify(button.enabled)
        mouseClick(button, button.width / 2, button.height / 2)
    }
    function openBindingDialog(page) {
        clickButton(page, "自定义…")
        const dialog = mainFixture.dialog(main, "批量更新自定义谱面内容")
        verify(dialog !== null)
        // onOpened resets the preview. Do not send input into that transition.
        tryCompare(dialog, "opened", true)
        return dialog
    }
    function clickBindingPreview(dialog) {
        const scroll = mainFixture.objectNamed(dialog, "resourceBindingScroll")
        const button = mainFixture.objectNamed(dialog, "resourceBindingPreviewButton")
        verify(scroll !== null && button !== null && button.enabled)
        const view = scroll.contentItem
        // At compact heights the preview action is below the initial viewport.
        tryVerify(() => {
            const center = button.mapToItem(view.contentItem, button.width / 2, button.height / 2)
            view.contentY = Math.max(0, Math.min(view.contentHeight - view.height, center.y - view.height / 2))
            const point = button.mapToItem(view, button.width / 2, button.height / 2)
            return button.height > 0 && point.y >= 0 && point.y < view.height
        })
        mouseClick(button, button.width / 2, button.height / 2)
    }
    function swap() { clickButton(split, "交换") }
    function draft() {
        const field = mainFixture.textEditor(main, "Fixture A")
        verify(field !== null)
        mouseClick(field, field.width / 2, field.height / 2)
        field.selectAll()
        keySequence("Ctrl+A")
        keyClick(Qt.Key_D)
        tryCompare(main, "metaDraftDirty", true)
        compare(mainFixture.snapshot(a).meta.TITLE, "Fixture A")
        return field.text
    }
    function confirmButton(text) {
        // Popup content is reparented to the overlay; search that visual owner.
        const dialog = mainFixture.dialog(main, "未保存的更改")
        verify(dialog !== null)
        clickButton(dialog, text)
    }
    function unchanged(sa, sb) {
        compare(mainFixture.snapshot(a), sa)
        compare(mainFixture.snapshot(b), sb)
    }
    function test_chartHistory_oneClickSavesDraft_deduplicates_andRestoresCopy_data() {
        return [{tag: "default", width: 1280, height: 800},
                {tag: "narrow", width: 640, height: 480}]
    }
    function test_chartHistory_oneClickSavesDraft_deduplicates_andRestoresCopy(data) {
        start(true)
        main.width = data.width
        main.height = data.height
        compare(chartHistoryController.enabled, false)
        compare(chartHistoryController.createOnClose, false)
        const title = draft()
        chartHistoryController.showHistory(a)
        let dialog = mainFixture.dialog(main, "本地谱面历史")
        verify(dialog !== null)
        compare(chartHistoryController.busy, false)
        compare(chartHistoryController.entries.length, 0)
        chartHistoryController.enabled = true
        tryCompare(chartHistoryController, "busy", false)
        // 触发真实按钮与Main保存接线，确认草稿随同一次点击落盘。
        clickButton(dialog, "保存并创建快照")
        tryCompare(chartHistoryController, "busy", false)
        compare(chartHistoryController.entries.length, 1)
        compare(main.metaDraftDirty, false)
        compare(mainFixture.snapshot(a).dirty, false)
        compare(mainFixture.snapshot(a).meta.TITLE, title)
        const diskA = mainFixture.disk(a)
        const savedA = mainFixture.snapshot(a), savedB = mainFixture.snapshot(b)
        const id = chartHistoryController.entries[0].id
        dialog = mainFixture.dialog(main, "本地谱面历史")
        verify(dialog !== null)
        clickButton(dialog, "保存并创建快照")
        tryCompare(chartHistoryController, "busy", false)
        compare(chartHistoryController.entries.length, 1)
        compare(chartHistoryController.entries[0].id, id)
        dialog = mainFixture.dialog(main, "本地谱面历史")
        verify(dialog !== null)
        verify(dialog.width <= main.width && dialog.height <= main.height)
        wait(50)
        verify(mainFixture.screenshot())
        dialog.selectedSnapshotId = id
        const restore = mainFixture.button(dialog, "恢复为副本")
        const scroll = mainFixture.objectNamed(dialog, "chartHistoryScroll")
        verify(restore !== null && scroll !== null)
        // 恢复按钮可能位于滚动区下方；按实际布局滚到可见位置再注入鼠标。
        const flickable = scroll.contentItem
        verify(flickable !== null && flickable.contentItem !== null)
        const point = restore.mapToItem(flickable.contentItem, 0, 0)
        flickable.contentY = Math.max(0, point.y - flickable.height / 2)
        wait(0)
        clickButton(dialog, "恢复为副本")
        tryCompare(chartHistoryController, "busy", false)
        compare(mainFixture.restoredHistoryCopies().length, 1)
        compare(mainFixture.restoredHistoryText(), diskA)
        unchanged(savedA, savedB)
        compare(documents.activeId(), a)
        compare(documents.instances.length, 2)
    }

    function test_externalFileActions_followNamedPathWithoutSaving() {
        start(true)
        const path = main.chartPath
        for (const id of ["file.revealInFileManager", "file.openInTextEditor"]) {
            verify(uiActions.exists(id))
            verify(uiActions.enabled(id))
        }
        const before = mainFixture.snapshot(a), disk = mainFixture.disk(a)
        main.chartPath = "" // Path-only change must disable the menu even if meta is unchanged.
        verify(!uiActions.enabled("file.revealInFileManager"))
        verify(!uiActions.enabled("file.openInTextEditor"))
        main.chartPath = path
        verify(uiActions.enabled("file.revealInFileManager"))
        verify(uiActions.enabled("file.openInTextEditor"))
        compare(mainFixture.snapshot(a), before)
        compare(mainFixture.disk(a), disk)
    }

    function relocationSetup(data) {
        start(true)
        if (data) { main.width = data.width; main.height = data.height }
        const fixture = mainFixture.prepareRelocationProject()
        tryCompare(project, "state", "ready")
        main.adoptChart(fixture.aPath)
        main.currentPage = 3
        project.clearSliceTargets()
        const page = mainFixture.objectNamed(main, "projectPage")
        verify(page !== null)
        compare(project.missingCount, 2)
        return {fixture: fixture, page: page}
    }
    function relocationOpen(page, single) {
        const name = single ? "projectRepairSelectedButton" : "projectRepairMissingButton"
        const button = mainFixture.objectNamed(page, name)
        verify(button !== null && button.enabled)
        mouseClick(button, button.width / 2, button.height / 2)
        const dialog = mainFixture.dialog(main, "修复缺失资源")
        verify(dialog !== null)
        tryCompare(dialog, "opened", true)
        tryCompare(resourceRelocationController, "busy", false)
        return dialog
    }
    function relocationClick(dialog, name) {
        const button = mainFixture.objectNamed(dialog, name)
        verify(button !== null && button.enabled, name)
        const scroll = mainFixture.objectNamed(dialog, "resourceRelocationScroll")
        const flickable = scroll.contentItem
        const point = button.mapToItem(flickable.contentItem, 0, 0)
        flickable.contentY = Math.max(0, Math.min(flickable.contentHeight - flickable.height,
                                                point.y - flickable.height / 2))
        wait(0)
        mouseClick(button, button.width / 2, button.height / 2)
    }
    function relocationRow(suffix) {
        const rows = resourceRelocationController.rows
        for (let i = 0; i < rows.length; ++i)
            if (rows[i].oldPath.endsWith(suffix)) return rows[i]
        fail("Missing relocation row: " + suffix)
        return null
    }
    function relocationSearch(dialog, fixture) {
        dialog.searchFolder = fixture.searchFolder
        dialog.recursive = true
        relocationClick(dialog, "resourceRelocationSearchButton")
        tryCompare(resourceRelocationController, "busy", false)
        tryVerify(function() {
            return resourceRelocationController.rows.length > 0
                   && resourceRelocationController.rows.every(row => row.replacement !== "")
        })
    }
    function test_resourceRelocation_allReferences_preview_memoryOnly_andUndo_data() {
        return [{tag: "default", width: 1280, height: 800},
                {tag: "narrow-short", width: 800, height: 480}]
    }
    function test_resourceRelocation_allReferences_preview_memoryOnly_andUndo(data) {
        const setup = relocationSetup(data), page = setup.page, fixture = setup.fixture
        const sa = mainFixture.snapshot(a), sb = mainFixture.snapshot(b)
        const diskA = mainFixture.disk(a), diskB = mainFixture.disk(b)
        const referenceId = split.referenceDocumentId
        const dialog = relocationOpen(page, false)
        compare(dialog.checkedOnly, false) // Empty checked set explicitly means ALL only in this mode.
        compare(resourceRelocationController.rows.length, 2)
        compare(relocationRow("/old/kick.wav").uses.length, 2)
        compare(relocationRow("/old/bg.png").uses.length, 2)
        verify(dialog.width <= main.width && dialog.height <= main.height)
        relocationSearch(dialog, fixture)
        relocationClick(dialog, "resourceRelocationPreviewButton")
        compare(dialog.preview.ok, true)
        compare(dialog.preview.targetCount, 2)
        compare(dialog.preview.changedCount, 4)
        verify(dialog.preview.rows.every(row => row.old.startsWith("old/")
                                               && (row.new === "samples/kick.wav" || row.new === "images/bg.png")))
        unchanged(sa, sb)
        compare(mainFixture.disk(a), diskA); compare(mainFixture.disk(b), diskB)
        const preview = mainFixture.objectNamed(dialog, "resourceRelocationPreviewList")
        verify(preview !== null && preview.clip && preview.count === 4)
        preview.positionViewAtBeginning()
        tryVerify(function() { return preview.itemAtIndex(0) !== null })
        const previewText = preview.itemAtIndex(0).text
        verify(previewText.indexOf(fixture.root) < 0)
        verify(previewText.indexOf("|") < 0) // Internal identity keys are not user-facing labels.
        verify(previewText.indexOf("#") >= 0 && previewText.indexOf(" → ") >= 0)
        const apply = mainFixture.objectNamed(dialog, "resourceRelocationApplyButton")
        verify(apply.enabled)
        const top = apply.mapToItem(main.contentItem, 0, 0)
        const bottom = apply.mapToItem(main.contentItem, apply.width, apply.height)
        verify(top.x >= 0 && top.y >= 0 && bottom.x <= main.width && bottom.y <= main.height)
        verify(mainFixture.screenshot())
        clickButton(dialog, "应用引用更新")
        tryCompare(dialog, "visible", false)
        const aa = mainFixture.snapshot(a), bb = mainFixture.snapshot(b)
        compare(aa.undo, sa.undo + 1); compare(bb.undo, sb.undo + 1)
        compare(aa.notes, sa.notes); compare(bb.notes, sb.notes)
        compare(aa.meta.STAGEFILE, "images/bg.png")
        compare(documents.currentSession.wavFileOfId(1), "samples/kick.wav")
        compare(documents.session(b).wavFileOfId(2), "samples/kick.wav")
        compare(documents.activeId(), a); compare(split.referenceDocumentId, referenceId)
        verify(project.openStates[fixture.aPath].dirty && project.openStates[fixture.bPath].dirty)
        verify(page.saveText.indexOf("待保存") >= 0)
        compare(main.statusText, page.saveText)
        compare(project.missingCount, 2) // Disk index is not a claim that unsaved edits reached disk.
        compare(mainFixture.disk(a), diskA); compare(mainFixture.disk(b), diskB)
        for (const id of [a, b]) {
            const result = JSON.parse(beatbench.dispatch(JSON.stringify({command: "session.undo", args: {session_id: id}})))
            verify(result.ok)
        }
        documents.refresh()
        compare(mainFixture.snapshot(a).definitions, sa.definitions)
        compare(mainFixture.snapshot(a).meta, sa.meta)
        compare(mainFixture.snapshot(b).definitions, sb.definitions)
    }
    function test_resourceRelocation_checkedOnly_emptyRefuses_andDoesNotWiden() {
        const setup = relocationSetup(), page = setup.page, fixture = setup.fixture
        const sa = mainFixture.snapshot(a), sb = mainFixture.snapshot(b)
        const dialog = relocationOpen(page, false)
        dialog.checkedOnly = true
        compare(dialog.scopeAllowed, false)
        compare(resourceRelocationController.rows.length, 0)
        verify(!mainFixture.objectNamed(dialog, "resourceRelocationPreviewButton").enabled)
        verify(!mainFixture.objectNamed(dialog, "resourceRelocationApplyButton").enabled)
        verify(dialog.localMessage.indexOf("不会自动") >= 0)
        dialog.prepare(); dialog.applyPreview()
        unchanged(sa, sb)
        project.setSliceTargetChecked("main-integration-b.bms", true)
        tryCompare(dialog, "scopeAllowed", true)
        relocationSearch(dialog, fixture)
        relocationClick(dialog, "resourceRelocationSelectResolvedButton")
        verify(resourceRelocationController.rows.every(row => row.uses.every(use => !use.selected || use.chartPath === fixture.bPath)))
        relocationClick(dialog, "resourceRelocationPreviewButton")
        compare(dialog.preview.ok, true)
        compare(dialog.preview.targetCount, 1)
        compare(dialog.preview.changedCount, 2)
        verify(dialog.preview.rows.every(row => row.chart === fixture.bPath))
        clickButton(dialog, "应用引用更新")
        tryCompare(dialog, "visible", false)
        compare(mainFixture.snapshot(a), sa)
        compare(mainFixture.snapshot(b).undo, sb.undo + 1)
    }
    function test_resourceRelocation_singleMissing_mapping_cancel_andUseCheckbox() {
        const setup = relocationSetup(), page = setup.page, fixture = setup.fixture
        const sa = mainFixture.snapshot(a), sb = mainFixture.snapshot(b)
        const diskA = mainFixture.disk(a), diskB = mainFixture.disk(b)
        page.selectedResource = project.selectResource("old/kick.wav")
        const dialog = relocationOpen(page, true)
        compare(dialog.onlyResource, "old/kick.wav")
        compare(resourceRelocationController.rows.length, 1)
        dialog.oldDirectory = "old"
        dialog.newDirectory = "samples"
        relocationClick(dialog, "resourceRelocationMapButton")
        compare(relocationRow("/old/kick.wav").replacement, fixture.kickPath)
        const row = relocationRow("/old/kick.wav")
        const groups = mainFixture.objectNamed(dialog, "resourceRelocationGroups")
        const scroll = mainFixture.objectNamed(dialog, "resourceRelocationScroll").contentItem
        scroll.contentY = groups.mapToItem(scroll.contentItem, 0, 0).y
        groups.positionViewAtBeginning()
        wait(0)
        const use = mainFixture.objectNamed(groups, "resourceRelocationUse:" + row.uses[0].key)
        verify(use !== null && use.checked)
        const point = use.mapToItem(groups.contentItem, 0, 0)
        groups.contentY = Math.max(0, point.y - groups.height / 2)
        wait(0)
        mouseClick(use, use.width / 2, use.height / 2)
        compare(relocationRow("/old/kick.wav").selectedCount, 1)
        relocationClick(dialog, "resourceRelocationPreviewButton")
        compare(dialog.preview.changedCount, 1)
        unchanged(sa, sb)
        clickButton(dialog.footer, "取消")
        tryCompare(dialog, "visible", false)
        tryVerify(() => resourceRelocationController.rows.length === 0)
        unchanged(sa, sb)
        compare(mainFixture.disk(a), diskA); compare(mainFixture.disk(b), diskB)
    }
    function test_resourceRelocation_manualCandidate_partialSelection_andInvalidation() {
        const setup = relocationSetup(), page = setup.page, fixture = setup.fixture
        const sa = mainFixture.snapshot(a), sb = mainFixture.snapshot(b)
        const dialog = relocationOpen(page, false)
        const kick = relocationRow("/old/kick.wav")
        dialog.chooseCandidate(kick.key, fixture.kickPath)
        relocationClick(dialog, "resourceRelocationPreviewButton")
        verify(!dialog.preview.ok) // A still-selected unresolved image must not be silently skipped.
        relocationClick(dialog, "resourceRelocationSelectResolvedButton")
        compare(relocationRow("/old/bg.png").selectedCount, 0)
        relocationClick(dialog, "resourceRelocationPreviewButton")
        compare(dialog.preview.changedCount, 2)
        verify(dialog.canApply)
        dialog.searchFolder = fixture.root + "/samples"
        verify(!dialog.canApply) // Input edits invalidate visible authorization even before a new search.
        relocationClick(dialog, "resourceRelocationClearSelectionButton")
        relocationClick(dialog, "resourceRelocationPreviewButton")
        compare(dialog.preview.changedCount, 0)
        verify(!dialog.canApply)
        dialog.applyPreview()
        unchanged(sa, sb)
        clickButton(dialog.footer, "取消")
        tryCompare(dialog, "visible", false)
    }
    function test_resourceRelocation_panelDraftRefusesOpening() {
        const setup = relocationSetup(), page = setup.page
        main.currentPage = 0
        draft()
        const sa = mainFixture.snapshot(a), sb = mainFixture.snapshot(b)
        main.currentPage = 3
        const button = mainFixture.objectNamed(page, "projectRepairMissingButton")
        verify(button !== null && button.enabled)
        mouseClick(button, button.width / 2, button.height / 2)
        compare(mainFixture.dialog(main, "修复缺失资源"), null)
        verify(page.saveText.indexOf("草稿") >= 0)
        unchanged(sa, sb)
    }

    function bindingSetup(data) {
        start(true)
        if (data) { main.width = data.width; main.height = data.height }
        const path = mainFixture.snapshot(a).path
        project.scan(path.slice(0, path.lastIndexOf("/")))
        tryCompare(project, "state", "ready")
        main.currentPage = 3
        project.setAllSliceTargets(true)
        compare(project.sliceTargetCount, 2)
        const page = mainFixture.objectNamed(main, "projectPage")
        verify(page !== null)
        return page
    }
    function test_projectInlineWav_noPopup_andExplicitOverwrite() {
        const page = bindingSetup()
        const inline = mainFixture.objectNamed(page, "projectInlineWavBinding")
        verify(inline !== null)
        const fileInput = mainFixture.objectNamed(inline, "projectWavFileInput")
        const update = mainFixture.objectNamed(inline, "projectWavUpdateButton")
        verify(fileInput !== null && update !== null)
        compare(fileInput.font.family, update.contentItem.font.family)
        compare(fileInput.font.pixelSize, update.contentItem.font.pixelSize)
        const sa = mainFixture.snapshot(a), sb = mainFixture.snapshot(b)
        const diskA = mainFixture.disk(a), diskB = mainFixture.disk(b)
        inline.wavId = "01"; inline.fileName = "first.wav"
        clickButton(inline, "更新 WAV")
        compare(mainFixture.dialog(main, "批量更新自定义谱面内容"), null)
        compare(mainFixture.snapshot(a).undo, sa.undo + 1)
        compare(mainFixture.snapshot(b).undo, sb.undo + 1)
        inline.fileName = "second.wav"
        const aa = mainFixture.snapshot(a), bb = mainFixture.snapshot(b)
        clickButton(inline, "更新 WAV")
        verify(page.saveText.indexOf("覆盖") >= 0)
        verify(mainFixture.button(inline, "确认覆盖") !== null)
        unchanged(aa, bb)
        inline.fileName = "third.wav" // Editing invalidates previous authorization.
        verify(mainFixture.button(inline, "更新 WAV") !== null)
        clickButton(inline, "更新 WAV")
        clickButton(inline, "确认覆盖")
        compare(mainFixture.snapshot(a).undo, aa.undo + 1)
        compare(mainFixture.snapshot(b).undo, bb.undo + 1)
        compare(documents.currentSession.wavFileOfId(1), "third.wav")
        compare(mainFixture.snapshot(a).notes, sa.notes)
        compare(mainFixture.disk(a), diskA); compare(mainFixture.disk(b), diskB)
        verify(mainFixture.screenshot())
        project.clearSliceTargets()
        verify(!mainFixture.button(inline, "更新 WAV").enabled)
    }
    function test_projectResourceSplitter_andTwoRowControls_data() {
        return [{tag: "default", width: 1280, height: 800}, {tag: "narrow", width: 800, height: 600}]
    }
    function test_projectResourceSplitter_andTwoRowControls(data) {
        const page = bindingSetup(data)
        wait(0)
        const actions = mainFixture.objectNamed(page, "projectChartActions")
        const inline = mainFixture.objectNamed(page, "projectInlineWavBinding")
        verify(actions !== null && inline !== null)
        const row1 = mainFixture.button(actions, "编辑 A")
        const last = mainFixture.button(actions, "保存勾选")
        fuzzyCompare(row1.y, last.y, 1)
        verify(last.x + last.width <= actions.width + 1)
        const custom = mainFixture.button(page, "自定义…")
        const inlinePos = inline.mapToItem(page, 0, 0), customPos = custom.mapToItem(page, 0, 0)
        fuzzyCompare(inlinePos.y + inline.height / 2, customPos.y + custom.height / 2, 2)
        const controlEnd = inline.mapToItem(main.contentItem, inline.width, inline.height)
        verify(controlEnd.x <= main.width && controlEnd.y <= main.height)
        const vertical = page.resourceSplitView
        const handle = mainFixture.objectNamed(vertical, "projectResourceSplitHandle")
        const resources = mainFixture.objectNamed(page, "projectResourceList")
        const refs = mainFixture.objectNamed(page, "projectResourceUsesList")
        verify(handle !== null && resources !== null && refs !== null)
        const oldRefs = vertical.itemAt(1).height, oldResources = resources.height
        mousePress(handle, handle.width / 2, handle.height / 2)
        mouseMove(handle, handle.width / 2, handle.height / 2 - 70, 100)
        mouseRelease(handle, handle.width / 2, handle.height / 2 - 70)
        tryVerify(function() { return vertical.itemAt(1).height > oldRefs + 25 })
        verify(resources.height < oldResources - 20)
        verify(vertical.itemAt(0).height >= vertical.paneMinimumHeight - 1)
        verify(vertical.itemAt(1).height >= vertical.paneMinimumHeight - 1)
        const kept = vertical.itemAt(1).height
        clickButton(page, "谱面历史"); clickButton(page, "资源")
        fuzzyCompare(vertical.itemAt(1).height, kept, 2)
        compare(refs.count, 0) // Empty selection remains an empty-state pane, not stale uses.
    }
    function test_customContent_metadata_updatesPanel_withoutDraft_orDiskSave() {
        const page = bindingSetup()
        const sa = mainFixture.snapshot(a), sb = mainFixture.snapshot(b), diskA = mainFixture.disk(a)
        clickButton(page, "自定义…")
        const dialog = mainFixture.dialog(main, "批量更新自定义谱面内容")
        verify(dialog !== null)
        tryCompare(dialog, "opened", true)
        const warning = mainFixture.objectNamed(dialog, "customContentRiskWarning")
        verify(warning !== null && warning.visible && warning.text.indexOf("元数据") >= 0)
        dialog.rawText = "#TITLE Updated song\n#ARTIST New artist\n#WAV01 update.wav"
        wait(0) // Let native popup layout settle before injecting a real mouse click.
        clickButton(dialog, "预览内容更新")
        verify(dialog.preview.ok, dialog.message)
        verify(dialog.preview.collisionCount > 0)
        const consent = mainFixture.objectNamed(dialog, "resourceBindingOverwriteConsent")
        mouseClick(consent, consent.width / 2, consent.height / 2)
        wait(50)
        verify(mainFixture.screenshot())
        clickButton(dialog, "应用内容更新")
        compare(mainFixture.snapshot(a).meta.TITLE, "Updated song")
        compare(mainFixture.snapshot(b).meta.ARTIST, "New artist")
        compare(main.metaDraftDirty, false)
        compare(mainFixture.snapshot(a).undo, sa.undo + 1)
        compare(mainFixture.snapshot(b).undo, sb.undo + 1)
        compare(mainFixture.snapshot(a).notes, sa.notes)
        compare(mainFixture.disk(a), diskA)
        compare(documents.activeId(), a)
    }
    function test_resourceBindings_multiselect_preview_apply_andUndo_data() {
        return [{tag: "default", width: 1280, height: 800}, {tag: "narrow", width: 640, height: 480}]
    }
    function test_resourceBindings_multiselect_preview_apply_andUndo(data) {
        const page = bindingSetup(data)
        const sa = mainFixture.snapshot(a), sb = mainFixture.snapshot(b)
        const diskA = mainFixture.disk(a), diskB = mainFixture.disk(b)
        const referenceId = split.referenceDocumentId
        const dialog = openBindingDialog(page)
        dialog.rawText = "#WAV01 audio/new.wav\n#BMP02 bga/new.png"
        clickBindingPreview(dialog)
        compare(dialog.preview.ok, true)
        compare(dialog.preview.targetCount, 2)
        compare(dialog.preview.changedCount, 4)
        compare(dialog.preview.collisionCount, 0)
        const applyButton = mainFixture.objectNamed(dialog, "resourceBindingApplyButton")
        const bottom = applyButton.mapToItem(main.contentItem, 0, applyButton.height)
        verify(bottom.y <= main.height && bottom.x >= 0)
        unchanged(sa, sb)
        clickButton(dialog, "应用内容更新")
        tryCompare(dialog, "visible", false)
        verify(page.saveText.indexOf("自定义内容已更新") >= 0)
        compare(main.statusText, page.saveText)
        compare(documents.activeId(), a)
        compare(split.referenceDocumentId, referenceId)
        const aa = mainFixture.snapshot(a), bb = mainFixture.snapshot(b)
        compare(aa.undo, sa.undo + 1)
        compare(bb.undo, sb.undo + 1)
        compare(aa.notes, sa.notes)
        compare(bb.notes, sb.notes)
        compare(mainFixture.disk(a), diskA)
        compare(mainFixture.disk(b), diskB)
        compare(mainFixture.bridgeState(documents.session(b)).samples, 2)
        verify(project.openStates[sb.path].dirty)
        for (const id of [a, b]) {
            const undo = JSON.parse(beatbench.dispatch(JSON.stringify({command: "session.undo", args: {session_id: id}})))
            verify(undo.ok)
        }
        documents.refresh()
        compare(mainFixture.snapshot(a).definitions, sa.definitions)
        compare(mainFixture.snapshot(b).definitions, sb.definitions)
    }
    function test_resourceBindings_definedId_requiresConsent_cancel_andSelectionInvalidates() {
        const page = bindingSetup()
        verify(JSON.parse(beatbench.dispatch(JSON.stringify({command: "sample.setFile", args: {
            session_id: b, id: "01", file: "old.wav"}}))).ok)
        documents.refresh()
        const sa = mainFixture.snapshot(a), sb = mainFixture.snapshot(b)
        let dialog = openBindingDialog(page)
        dialog.rawText = "#WAV01 new.wav"
        clickButton(dialog, "预览内容更新")
        compare(dialog.preview.collisionCount, 1)
        const initialApply = mainFixture.objectNamed(dialog, "resourceBindingApplyButton")
        const initialConsent = mainFixture.objectNamed(dialog, "resourceBindingOverwriteConsent")
        verify(initialApply !== null && !initialApply.enabled)
        verify(initialConsent !== null && initialConsent.visible && !initialConsent.checked)
        // Qt 6.4 can retain the replaced base footer as a nonvisual QObject child.
        clickButton(dialog.footer, "取消")
        tryCompare(dialog, "visible", false)
        unchanged(sa, sb)
        dialog = openBindingDialog(page)
        dialog.rawText = "#WAV01 new.wav"
        clickButton(dialog, "预览内容更新")
        const apply = mainFixture.objectNamed(dialog, "resourceBindingApplyButton")
        const consent = mainFixture.objectNamed(dialog, "resourceBindingOverwriteConsent")
        verify(apply !== null && consent !== null)
        mouseClick(consent, consent.width / 2, consent.height / 2)
        verify(apply.enabled)
        project.setSliceTargetChecked("main-integration-a.bms", false)
        tryVerify(function() { return !dialog.preview.ok })
        verify(!apply.enabled)
        clickButton(dialog, "预览内容更新")
        compare(dialog.preview.targetCount, 1)
        compare(consent.checked, false)
        mouseClick(consent, consent.width / 2, consent.height / 2)
        clickButton(dialog, "应用内容更新")
        compare(mainFixture.snapshot(a), sa)
        compare(mainFixture.snapshot(b).undo, sb.undo + 1)
        compare(documents.activeId(), a)
    }
    function test_selectedSave_onlyFrozenSelection_notOtherDirtyDocuments() {
        const page = bindingSetup()
        project.setSliceTargetChecked("main-integration-a.bms", false)
        const sa = mainFixture.snapshot(a), diskA = mainFixture.disk(a)
        const diskB = mainFixture.disk(b)
        verify(sa.dirty && mainFixture.snapshot(b).dirty)
        const list = mainFixture.objectNamed(page, "projectChartList")
        verify(list !== null)
        // Qt 6.4 can evaluate visibility before polishing the newly shown SplitView.
        tryVerify(() => list.width > 0 && list.height > 0)
        clickButton(page, "保存勾选")
        tryVerify(() => !mainFixture.snapshot(b).dirty)
        tryCompare(main, "pendingDocAction", "")
        compare(mainFixture.snapshot(a), sa)
        compare(mainFixture.disk(a), diskA)
        verify(!diskB.includes("#GENRE history"))
        verify(mainFixture.disk(b).includes("#GENRE history"))
        compare(documents.activeId(), a)
    }
    function test_selectedSave_nonactivePms_cancelAcceptStale_data() {
        return [{tag: "cancel", action: "cancel"}, {tag: "accept", action: "accept"},
                {tag: "stale", action: "stale"}]
    }
    function test_selectedSave_nonactivePms_cancelAcceptStale(data) {
        start(true)
        const id = openPmsFixture(pmsProfileData()[0])
        savePmsToDangerousPath(id, main.chartPath + ".selected.bms")
        verify(JSON.parse(beatbench.dispatch(JSON.stringify({command: "meta.edit", args: {
            session_id: id, edits: [{key: "TITLE", value: "Selected PMS"}]}}))).ok)
        documents.refresh()
        const targetPath = mainFixture.snapshot(id).path, disk = mainFixture.disk(id)
        verify(documents.activate(a).ok)
        main.adoptChart(mainFixture.snapshot(a).path)
        const sa = mainFixture.snapshot(a), sb = mainFixture.snapshot(b)
        const diskA = mainFixture.disk(a), diskB = mainFixture.disk(b)
        main.saveSelectedDocuments([targetPath])
        const dialog = pmsDialog()
        // Changing project selection cannot expand this pending batch to A or B.
        project.setSliceTargetChecked(mainFixture.snapshot(a).path, true)
        if (data.action === "stale") {
            verify(JSON.parse(beatbench.dispatch(JSON.stringify({command: "meta.edit", args: {
                session_id: id, edits: [{key: "ARTIST", value: "Changed after preview"}]}}))).ok)
        }
        clickButton(dialog, data.action === "cancel" ? "取消保存" : "仍按此后缀保存")
        tryCompare(main, "pendingDocAction", "")
        compare(documents.activeId(), a)
        unchanged(sa, sb)
        compare(mainFixture.disk(a), diskA)
        compare(mainFixture.disk(b), diskB)
        if (data.action === "accept") {
            compare(mainFixture.snapshot(id).dirty, false)
            verify(mainFixture.disk(id).indexOf("#TITLE Selected PMS") >= 0)
        } else {
            compare(mainFixture.disk(id), disk)
            verify(mainFixture.snapshot(id).dirty)
        }
    }
    function test_resourceBindings_emptySelection_andPanelDraft_doNotFallBackToA() {
        const page = bindingSetup()
        project.clearSliceTargets()
        verify(!mainFixture.button(page, "自定义…").enabled)
        project.setAllSliceTargets(true)
        main.currentPage = 0
        draft()
        const sa = mainFixture.snapshot(a), sb = mainFixture.snapshot(b)
        main.currentPage = 3
        clickButton(page, "自定义…")
        compare(mainFixture.dialog(main, "批量更新自定义谱面内容"), null)
        verify(page.saveText.indexOf("草稿") >= 0)
        verify(main.metaDraftDirty)
        unchanged(sa, sb)
    }

    function test_projectHistoryTab_savesWithoutDialog_andKeepsSelectionIdentity() {
        start(true)
        main.currentPage = 3
        const page = mainFixture.objectNamed(main, "projectPage")
        verify(page !== null)
        const tabs = mainFixture.objectNamed(page, "projectDetailTabs")
        verify(tabs !== null)
        tryVerify(() => tabs.width > 0)
        clickButton(tabs, "谱面历史")
        const panel = mainFixture.objectNamed(page, "projectHistoryPanel")
        verify(panel !== null)
        tryCompare(panel, "visible", true)
        tryCompare(chartHistoryController, "documentId", a)
        chartHistoryController.enabled = true
        tryCompare(chartHistoryController, "busy", false)
        clickButton(panel, "保存并创建快照")
        tryVerify(() => chartHistoryController.entries.length === 1)
        compare(mainFixture.dialog(main, "本地谱面历史"), null)
        const sa = mainFixture.snapshot(a), sb = mainFixture.snapshot(b)
        page.selectedChart = mainFixture.snapshot(b).path
        tryCompare(chartHistoryController, "documentId", b)
        // Loading temporarily disables Save even for a valid target. Check
        // the persistent permission only after B's history request settles.
        tryCompare(chartHistoryController, "busy", false)
        const save = mainFixture.button(panel, "保存并创建快照")
        verify(save !== null && !save.enabled)
        compare(documents.activeId(), a)
        unchanged(sa, sb)
        page.selectedChart = ""
        tryCompare(chartHistoryController, "documentId", a)
        tryVerify(() => !chartHistoryController.busy && chartHistoryController.entries.length === 1)
        clickButton(page, "资源")
        tryCompare(panel, "visible", false)
    }

    function test_chartHistory_pmsCancellationCreatesNoSnapshot() {
        start(true)
        const id = openPmsFixture({bmesp: false, mode: "pms9k"})
        savePmsToDangerousPath(id, main.chartPath + ".history.bms")
        const title = draftTitleFor(id)
        const before = pmsSaveState(id)
        chartHistoryController.showHistory(id)
        chartHistoryController.enabled = true
        tryCompare(chartHistoryController, "busy", false)
        const dialog = mainFixture.dialog(main, "本地谱面历史")
        verify(dialog !== null)
        clickButton(dialog, "保存并创建快照")
        verify(chartHistoryController.busy)
        clickButton(pmsDialog(), "取消保存")
        tryCompare(chartHistoryController, "busy", false)
        compare(chartHistoryController.entries.length, 0)
        compare(pmsSaveState(id), before)
        verify(main.metaDraftDirty)
    }
    function test_chartHistory_exitFailureKeepsWindowAndCleanState() {
        start(true)
        verify(main.saveChart())
        chartHistoryController.showHistory(a)
        chartHistoryController.enabled = true
        tryCompare(chartHistoryController, "busy", false)
        chartHistoryController.createOnClose = true
        const before = mainFixture.snapshot(a)
        mainFixture.failSave() // Replace source with directory: deterministic snapshot failure.
        mainFixture.dialog(main, "本地谱面历史").close()
        main.allowClose = true // Final close gate after discarding dirty B, never snapshot B.
        main.close()
        tryCompare(main, "visible", true)
        tryCompare(chartHistoryController, "busy", false)
        compare(mainFixture.snapshot(a), before)
        verify(mainFixture.dialog(main, "本地谱面历史") !== null)
        verify(chartHistoryController.message.indexOf("退出快照失败") >= 0)
    }

    SignalSpy { id: activeChanges; target: documents; signalName: "currentSessionChanged" }
    SignalSpy { id: referenceContents; signalName: "contentChanged" }
    SignalSpy { id: slicePlacements; target: sliceWorkspace; signalName: "samplesPlaced" }
    function test_slicePlacement_refreshes_existing_inactive_reference_data() {
        return [
            {tag: "B-only-split", includeA: false, overlay: false},
            {tag: "A-and-B-split", includeA: true, overlay: false},
            {tag: "B-only-overlay", includeA: false, overlay: true},
            {tag: "A-and-B-overlay", includeA: true, overlay: true}
        ]
    }
    function test_slicePlacement_refreshes_existing_inactive_reference(data) {
        start(true)
        // Create and display B BEFORE the export, reproducing the stale reference path.
        const reference = split.referenceSession
        compare(reference, documents.session(b))
        const primary = mainFixture.primaryView(main)
        if (data.overlay) enterOverlay()
        const view = data.overlay ? primary.viewportItem : split.referenceView.viewportItem
        const oldHeight = view.contentHeight
        const beforeA = mainFixture.snapshot(a), beforeB = mainFixture.snapshot(b)
        const diskA = mainFixture.disk(a), diskB = mainFixture.disk(b)
        verify(mainFixture.bridgeState(reference).chart)
        verify(mainFixture.bridgeState(reference).timing)
        compare(mainFixture.bridgeState(reference).measureCount, 2)
        referenceContents.target = reference
        referenceContents.clear(); slicePlacements.clear(); activeChanges.clear()
        const plan = mainFixture.prepareSliceExport(data.includeA)
        verify(plan.ok, plan.error)
        compare(mainFixture.fileExists(plan.outputFile), false)
        // Actual production commit emits samplesPlaced; only Main's completion handler
        // may publish B's new revision. No documents.refresh()/reference.refresh() here.
        const result = sliceWorkspace.commitExport(plan.planId, "error", "skip")
        verify(result.ok, result.error)
        verify(result.placementOk, result.placeError)
        compare(result.placedTargets, data.includeA ? 2 : 1)
        compare(slicePlacements.count, 1)
        verify(mainFixture.fileExists(plan.outputFile))
        verify(referenceContents.count > 0, "Existing inactive B must publish contentChanged")
        compare(split.referenceSession, reference)
        compare(documents.activeId(), a)
        compare(activeChanges.count, 0)
        const state = mainFixture.bridgeState(reference)
        verify(state.chart && state.timing, "New chart and matching timing must be readable immediately")
        compare(state.notes, 2)
        compare(state.samples, 1)
        compare(state.measureCount, 7)
        verify(state.tailTimeUs > 0 && state.tailBpm > 0)
        verify(reference.wavFileOfId(10).length > 0)
        tryVerify(() => view.contentHeight > oldHeight)
        if (data.overlay) {
            compare(view.referenceOverlayInfo.available, true)
            compare(view.referenceOverlayInfo.total, 2)
            compare(view.referenceOverlayInfo.referenceMeasureCount, 7)
            compare(view.referenceOverlayInfo.extentExtended, !data.includeA)
            verify(view.referenceOverlayProbe(0).valid, "Existing B notes remain projectable after export")
        } else {
            fuzzyCompare(view.secondsForPosition(6, 0), state.tailTimeUs / 1000000, 0.000001)
        }
        const afterB = mainFixture.snapshot(b)
        compare(afterB.undo, beforeB.undo + 1)
        compare(afterB.redo, 0)
        compare(afterB.dirty, true)
        compare(afterB.meta.TITLE, beforeB.meta.TITLE)
        const afterA = mainFixture.snapshot(a)
        if (data.includeA) {
            compare(afterA.undo, beforeA.undo + 1)
            compare(afterA.redo, 0)
            compare(afterA.notes.length, 2)
            verify(mainFixture.bridgeState(documents.currentSession).chart)
        } else compare(afterA, beforeA)
        compare(mainFixture.disk(a), diskA)
        compare(mainFixture.disk(b), diskB)
    }
    function test_slicePlacement_revision_guard_rejects_changed_reference() {
        start(true)
        const reference = split.referenceSession
        const plan = mainFixture.prepareSliceExport(false)
        verify(plan.ok, plan.error)
        slicePlacements.clear()
        const edit = JSON.parse(beatbench.dispatch(JSON.stringify({command: "meta.edit", args: {
            session_id: b, edits: [{key: "TITLE", value: "Changed after preview"}]
        }})))
        verify(edit.ok)
        // A core revision without publication must still reject stale chart/timing.
        const stale = mainFixture.bridgeState(reference)
        compare(stale.chart, false)
        compare(stale.timing, false)
        const beforeA = mainFixture.snapshot(a), beforeB = mainFixture.snapshot(b)
        const result = sliceWorkspace.commitExport(plan.planId, "error", "skip")
        compare(result.ok, false)
        verify(result.error.length > 0)
        compare(slicePlacements.count, 0)
        compare(mainFixture.fileExists(plan.outputFile), false)
        unchanged(beforeA, beforeB)
        compare(documents.activeId(), a)
    }
    Timer {
        id: publicationHeartbeat
        interval: 10; repeat: true
        property int ticks: 0
        property double last: 0
        property double largestGap: 0
        function begin() { ticks = 0; last = Date.now(); largestGap = 0; start() }
        onTriggered: {
            const now = Date.now()
            largestGap = Math.max(largestGap, now - last)
            last = now
            ++ticks
        }
    }
    function test_project_large_snapshot_publication_keeps_visible_models_and_details_live() {
        start(true)
        const beforeA = mainFixture.snapshot(a), beforeB = mainFixture.snapshot(b)
        main.currentPage = 3
        const page = mainFixture.findProperty(main, "selectedChart")
        const charts = mainFixture.objectNamed(main, "projectChartList")
        const resources = mainFixture.objectNamed(main, "projectResourceList")
        const uses = mainFixture.objectNamed(main, "projectResourceUsesList")
        verify(page !== null && charts !== null && resources !== null && uses !== null)
        let indexMs = -1, readyMs = -1, chartDelegates = -1, resourceDelegates = -1
        publicationHeartbeat.begin()
        try {
            tryVerify(() => publicationHeartbeat.ticks >= 2)
            const started = Date.now()
            mainFixture.startLargeProjectScan()
            tryCompare(project, "state", "parsing", 10000)
            indexMs = Date.now() - started
            compare(project.chartCount, 120)
            compare(project.resourceCount, 2000)
            tryCompare(charts, "count", 120)
            tryCompare(resources, "count", 2000)
            tryVerify(() => mainFixture.liveModelDelegates(charts) > 0
                         && mainFixture.liveModelDelegates(resources) > 0)
            chartDelegates = mainFixture.liveModelDelegates(charts)
            resourceDelegates = mainFixture.liveModelDelegates(resources)
            verify(chartDelegates < 100 && resourceDelegates < 100,
                   "Only viewport delegates should exist, not the full project snapshot")
            // Select through real row click handlers before the parsed snapshot arrives.
            page.chartSearch = "chart_119"; page.refreshCharts()
            tryCompare(charts, "count", 1)
            // count updates before ListView polishes/rebinds its old delegates.
            tryVerify(() => charts.itemAtIndex(0) !== null
                         && charts.itemAtIndex(0).modelData.path === "chart_119.bms")
            mouseClick(charts.itemAtIndex(0), 180, 12)
            compare(page.selectedChart, "chart_119.bms")
            compare(page.selectedChartInfo.parsed, false)
            page.searchText = "resource_0006"; page.refreshResources()
            tryCompare(resources, "count", 1)
            tryVerify(() => resources.itemAtIndex(0) !== null
                         && resources.itemAtIndex(0).modelData.path === "resource_0006.wav")
            mouseClick(resources.itemAtIndex(0), 180, 12)
            compare(page.selectedResource.path, "resource_0006.wav")
            compare(uses.count, 0)
            // Keep both complete tables visible during the parsed publication; selected
            // details must follow their stable paths across the model reset.
            page.chartSearch = ""; page.refreshCharts()
            page.searchText = ""; page.refreshResources()
            tryCompare(charts, "count", 120)
            tryCompare(resources, "count", 2000)
            const parsingStarted = Date.now()
            mainFixture.releaseProjectScan()
            tryCompare(project, "state", "ready", 10000)
            readyMs = Date.now() - parsingStarted
            tryCompare(uses, "count", 16)
            compare(page.selectedChartInfo.title, "Stress Chart 119")
            compare(page.selectedChartInfo.notes, 219)
            compare(page.selectedResource.state, "缺失")
            compare(page.selectedResource.refs, 16)
            tryVerify(() => uses.itemAtIndex(0) !== null)
            verify(uses.itemAtIndex(0).text.indexOf("chart_000.bms") >= 0)
            verify(uses.itemAtIndex(0).text.indexOf("refs=1") >= 0)
            // Case-insensitive path filtering plus open state, kind and missingness.
            page.chartSearch = "CHART_119"; page.refreshCharts()
            tryCompare(charts, "count", 1)
            page.onlyOpen = true; page.refreshCharts()
            tryCompare(charts, "count", 0)
            page.onlyOpen = false; page.refreshCharts()
            tryCompare(charts, "count", 1)
            page.onlyMissing = true; page.kindFilter = "wav"
            page.searchText = "resource_0006"; page.refreshResources()
            tryCompare(resources, "count", 1)
            page.kindFilter = "bmp"; page.refreshResources()
            tryCompare(resources, "count", 0)
            page.onlyMissing = false; page.onlyUnreferenced = true
            page.kindFilter = "wav"; page.searchText = "resource_0000"; page.refreshResources()
            tryCompare(resources, "count", 1)
            page.kindFilter = ""; page.onlyUnreferenced = false
            page.searchText = ""; page.refreshResources()
            tryCompare(resources, "count", 2000)
            wait(40) // Let the timer observe the last GUI publication/layout callback too.
            verify(mainFixture.liveModelDelegates(resources) < 100)
            verify(publicationHeartbeat.ticks >= 3)
            // Generous guard for catastrophic GUI stalls, not a platform microbenchmark.
            verify(publicationHeartbeat.largestGap < 2000, "Project publication stalled the GUI heartbeat")
            unchanged(beforeA, beforeB)
        } finally {
            publicationHeartbeat.stop()
            mainFixture.releaseProjectScan()
            console.log("Main project snapshot: charts=120 resources=2000 usesPerReferencedResource=16"
                        + " indexMs=" + indexMs + " readyMs=" + readyMs
                        + " largestHeartbeatGapMs=" + publicationHeartbeat.largestGap
                        + " chartDelegates=" + chartDelegates + " resourceDelegates=" + resourceDelegates)
        }
    }
    function projectAction(path, label) {
        main.currentPage = 3
        wait(0)
        const page = mainFixture.findProperty(main, "selectedChart")
        verify(page !== null)
        page.selectedChart = path
        clickButton(page, label)
    }
    function primaryState() {
        return { document: mainFixture.snapshot(a), path: main.chartPath,
                 meta: JSON.stringify(main.chartMeta), draft: main.metaDraftDirty,
                 edits: JSON.stringify(mainFixture.findProperty(main, "primaryKeys").collectEdits()),
                 audioPath: audioEngine.chartPath, audioChanges: audioEngine.pathChanges }
    }
    function verifyPrimary(state) {
        compare(documents.activeId(), a)
        compare(chartSession.sessionId, a)
        compare(primaryState(), state)
        compare(activeChanges.count, 0) // Even a transient activation would rebind Main/audio.
        compare(main.pendingDocAction, "")
    }
    function test_projectB_reusesDirtyDocumentAndExpandsWithoutDraftConfirmation() {
        start(true)
        draft()
        const state = primaryState(), sb = mainFixture.snapshot(b)
        const count = documents.instances.length, bridge = documents.session(b)
        split.collapse()
        tryCompare(split, "expanded", false)
        activeChanges.clear()
        projectAction(sb.path, "对照 B")
        compare(main.currentPage, 3)
        main.currentPage = 0 // Assert the visible result, not hidden SplitView geometry.
        tryCompare(split, "expanded", true)
        compare(split.referenceDocumentId, b)
        compare(split.referenceView.documentSession, bridge)
        compare(documents.instances.length, count)
        compare(mainFixture.snapshot(b), sb)
        verifyPrimary(state)
    }
    function test_projectB_loadsNewInstanceWithoutActivatingA() {
        start(true)
        // Keep the original temporary on-disk B, but move its loaded instance to a
        // second temporary path. The first path is now genuinely unopened; no
        // repository testdata or fixture implementation is modified.
        const path = mainFixture.snapshot(b).path
        verify(documents.save(b, path + ".moved.bms").ok)
        verify(project.sessionIdForPath(path) === "")
        draft()
        const state = primaryState(), sb = mainFixture.snapshot(b)
        const count = documents.instances.length
        split.collapse()
        tryCompare(split, "expanded", false)
        activeChanges.clear()
        projectAction(path, "对照 B")
        compare(main.currentPage, 3)
        main.currentPage = 0 // Assert the visible result, not hidden SplitView geometry.
        tryCompare(split, "expanded", true)
        const id = split.referenceDocumentId
        verify(id !== a && id !== b && id !== "")
        compare(documents.instances.length, count + 1)
        compare(project.sessionIdForPath(path), id)
        compare(split.referenceView.documentSession, documents.session(id))
        compare(mainFixture.snapshot(id).dirty, false)
        compare(mainFixture.snapshot(b), sb)
        verifyPrimary(state)
        // Repeated project opens reuse this exact newly loaded bridge too.
        const bridge = documents.session(id)
        projectAction(path, "对照 B")
        compare(documents.instances.length, count + 1)
        compare(split.referenceView.documentSession, bridge)
        verifyPrimary(state)
    }
    function test_projectB_failurePreservesBothRolesAndCollapsedState() {
        start(true)
        draft()
        const state = primaryState(), sb = mainFixture.snapshot(b)
        const count = documents.instances.length
        split.collapse()
        tryCompare(split, "expanded", false)
        activeChanges.clear()
        projectAction(sb.path + ".missing.bms", "对照 B")
        compare(main.currentPage, 3)
        compare(split.referenceDocumentId, b)
        compare(split.expanded, false)
        compare(documents.instances.length, count)
        compare(mainFixture.snapshot(b), sb)
        verify(main.statusText.indexOf("打开对照失败") >= 0)
        verifyPrimary(state)
    }
    function test_projectListDoubleClick_navigatesAfterConfirmation_data() {
        return [{tag: "clean", confirm: ""}, {tag: "cancel", confirm: "取消"},
                {tag: "discard", confirm: "不保存"}]
    }
    function test_projectListDoubleClick_navigatesAfterConfirmation(data) {
        start(true)
        if (data.confirm) draft()
        const path = mainFixture.snapshot(b).path
        project.scan(path.slice(0, path.lastIndexOf("/")))
        tryCompare(project, "state", "ready")
        main.currentPage = 3
        const rowPath = project.charts.find(c => project.absolutePath(c.path) === path).path
        let row = null
        tryVerify(function() { row = mainFixture.objectNamed(main, "projectChartRow:" + rowPath); return row !== null })
        verify(waitForRendering(row))
        mouseDoubleClickSequence(row, 180, row.height / 2, Qt.LeftButton)
        if (data.confirm) {
            compare(main.pendingDocAction, "switch")
            compare(main.currentPage, 3)
            confirmButton(data.confirm)
        }
        compare(main.currentPage, data.confirm === "取消" ? 3 : 0)
        compare(documents.activeId(), data.confirm === "取消" ? a : b)
        project.clear()
    }
    function test_projectA_usesExistingDraftConfirmation_data() {
        return [{tag: "cancel", button: "取消"}, {tag: "discard", button: "不保存"},
                {tag: "save", button: "保存"}]
    }
    function test_projectA_usesExistingDraftConfirmation(data) {
        start(true)
        const title = draft(), sb = mainFixture.snapshot(b)
        const state = primaryState()
        activeChanges.clear()
        projectAction(sb.path, "编辑 A")
        compare(main.pendingDocAction, "assign")
        verifyPrimaryBeforeConfirm(state)
        confirmButton(data.button)
        if (data.button === "取消") {
            verifyPrimary(state)
            compare(main.currentPage, 3)
        } else {
            tryCompare(chartSession, "sessionId", b)
            compare(main.currentPage, 3)
            compare(main.metaDraftDirty, false)
            compare(mainFixture.snapshot(b), sb)
            if (data.button === "保存") {
                compare(mainFixture.snapshot(a).meta.TITLE, title)
                verify(mainFixture.disk(a).indexOf("#TITLE " + title) >= 0)
            } else compare(mainFixture.snapshot(a), state.document)
        }
    }
    function verifyPrimaryBeforeConfirm(state) {
        compare(documents.activeId(), a)
        compare(primaryState(), state)
        compare(activeChanges.count, 0)
    }
    function test_viewMenu_hidesBSecondaryChannelsByDefaultAndToggles() {
        start(false)
        const sa = mainFixture.snapshot(a), sb = mainFixture.snapshot(b)
        compare(main.hideReferenceSecondaryChannels, true)
        compare(split.hideSecondaryChannels, true)
        const menu = main.menuBar.menuAt(2)
        for (let i = 0; i < 2; ++i) {
            menu.open()
            tryCompare(menu, "visible", true)
            const item = findChild(main, "hideReferenceSecondaryChannelsMenuItem")
            verify(item !== null)
            compare(item.checkable, true)
            compare(item.checked, i === 0)
            mouseClick(item, item.width / 2, item.height / 2)
            compare(main.hideReferenceSecondaryChannels, i !== 0)
            compare(split.hideSecondaryChannels, i !== 0)
        }
        unchanged(sa, sb)
    }
    function test_referenceFollowsToolbarToolWithIndependentEditingContext() {
        start(false)
        const sa = mainFixture.snapshot(a), sb = mainFixture.snapshot(b)
        for (const tool of ["pan", "select", "note", "ln", "mine"]) {
            main.editorTool = tool
            compare(split.editorTool, tool)
            compare(split.referenceView.editorTool, tool)
            compare(split.referenceView.editable, true)
            compare(split.referenceView.ownsPlayback, false)
        }
        unchanged(sa, sb)
    }
    function enterOverlay() {
        clickButton(main, "同屏叠加")
        tryCompare(split, "overlayMode", true)
        tryCompare(mainFixture.primaryView(main), "referenceOverlayEnabled", true)
    }
    function test_openReference_defaults_to_half_and_tracks_window_resize() {
        start(false)
        split.collapse()
        tryCompare(split, "expanded", false)
        verify(split.openReference(b))
        tryCompare(split, "expanded", true)
        compare(split.referenceShare, 0.5)
        compare(split.referenceSession, documents.session(b))

        const initialWindowWidth = main.width
        const initialSplitWidth = split.width
        tryVerify(() => Math.abs(split.referenceWidth - (initialSplitWidth - 4) / 2) <= 1)

        main.width = initialWindowWidth - 160
        tryVerify(() => split.width < initialSplitWidth
                  && Math.abs(split.referenceWidth - (split.width - 4) / 2) <= 1)
        compare(split.referenceShare, 0.5)

        main.width = initialWindowWidth
        tryCompare(split, "width", initialSplitWidth)
        tryVerify(() => Math.abs(split.referenceWidth - (initialSplitWidth - 4) / 2) <= 1)
        compare(split.referenceShare, 0.5)
        compare(documents.activeId(), a)
    }
    function test_overlayMode_switch_stays_at_header_left() {
        start(false)
        const enter = mainFixture.objectNamed(main, "overlayModeButton")
        verify(enter !== null && enter.visible)
        // Both split and full-width presentations share the same left edge.
        for (let i = 0; i < 2; ++i) {
            const before = enter.mapToItem(main.contentItem, 0, 0)
            const primary = mainFixture.primaryView(main)
            fuzzyCompare(before.x - primary.mapToItem(main.contentItem, 0, 0).x, 4, 1)
            wait(Qt.styleHints.mouseDoubleClickInterval + 20)
            enterOverlay()
            const back = mainFixture.objectNamed(main, "returnToSplitButton")
            verify(back !== null && back.visible)
            tryVerify(() => Math.abs(back.mapToItem(main.contentItem, 0, 0).x - before.x) < 1)
            // Native controls can differ slightly in implicit height; both stay in the same header.
            verify(Math.abs(back.mapToItem(main.contentItem, 0, 0).y - before.y) <= 3)
            // Same-position clicks must be separate clicks, not a synthesized double-click.
            wait(Qt.styleHints.mouseDoubleClickInterval + 20)
            mouseClick(back, back.width / 2, back.height / 2)
            tryCompare(split, "overlayMode", false)
            tryVerify(() => enter.visible && Math.abs(enter.mapToItem(main.contentItem, 0, 0).x - before.x) < 1)
        }
    }
    function test_overlayMode_controls_preserve_documents_and_primary_input() {
        start(false)
        mainFixture.prepareOverlayExample()
        const primary = mainFixture.primaryView(main)
        primary.seekTo(0.5)
        wait(40)
        const anchor = primary.viewportItem.viewportAnchor
        const width = primary.width, height = primary.height, splitWidth = split.referenceWidth
        const sa = mainFixture.snapshot(a), sb = mainFixture.snapshot(b)
        primary.viewportItem.playheadSec = 1.5 // paused head need not equal the viewport cursor
        enterOverlay()
        tryVerify(() => primary.width > width + 300)
        tryCompare(primary, "height", height)
        tryCompare(primary, "playheadSec", 1.5)
        compare(primary.referenceSession, documents.session(b))
        compare(split.referenceView.visible, false)
        compare(split.syncActive, false)
        tryVerify(() => Math.abs(primary.viewportItem.viewportAnchor.measure
                  + primary.viewportItem.viewportAnchor.fraction - anchor.measure - anchor.fraction) < 0.00001)
        const toggle = mainFixture.objectNamed(main, "overlayVisibleCheck")
        verify(toggle !== null && toggle.visible)
        mouseClick(toggle, toggle.width / 2, toggle.height / 2)
        tryCompare(primary, "referenceOverlayEnabled", false)
        mouseClick(toggle, toggle.width / 2, toggle.height / 2)
        tryCompare(primary, "referenceOverlayEnabled", true)
        const slider = mainFixture.objectNamed(main, "overlayOpacitySlider")
        verify(slider !== null && slider.visible)
        mouseClick(slider, slider.width * 0.8, slider.height / 2)
        tryVerify(() => split.overlayOpacity !== 0.45)
        fuzzyCompare(primary.viewportItem.referenceOverlayOpacity, split.overlayOpacity, 0.00001)
        verify(split.selectReference(a))
        tryCompare(primary, "referenceSession", documents.session(a))
        verify(split.selectReference(b))
        tryCompare(primary, "referenceSession", documents.session(b))
        unchanged(sa, sb)
        main.editorTool = "select"
        const sourceIndex = sb.notes.findIndex(n => n.measure === 1 && n.num === 0 && n.lane === 1)
        verify(sourceIndex >= 0)
        const cap = primary.viewportItem.referenceOverlayProbe(sourceIndex).rect
        tryVerify(() => primary.viewportItem.noteAt(cap.x + cap.width / 2, cap.y + cap.height - 2).valid)
        mouseClick(primary, cap.x + cap.width / 2, cap.y + cap.height - 2)
        compare(split.referenceFocused, false)
        tryCompare(main, "chartEditingAllowed", true)
        compare(main.selectionRefs.length, 1)
        compare(main.selectionRefs[0].sample, 1)
        unchanged(sa, sb)
        primary.selection = []; main.selectionRefs = []
        verify(mainFixture.screenshot())
        clickButton(split, "A/B 分栏")
        tryCompare(split, "overlayMode", false)
        tryCompare(primary, "referenceOverlayEnabled", false)
        tryVerify(() => Math.abs(split.referenceWidth - splitWidth) < 1)
        compare(split.referenceDocumentId, b)
        unchanged(sa, sb)
    }
    function test_overlayMode_swap_draft_confirmation_preserves_anchor() {
        start(true)
        const primary = mainFixture.primaryView(main)
        primary.measureHeight = 288
        primary.seekTo(1.5)
        wait(40)
        enterOverlay()
        const before = primary.viewportItem.viewportAnchor
        draft()
        swap(); confirmButton("取消")
        compare(documents.activeId(), a)
        compare(primary.viewportItem.viewportAnchor, before)
        swap(); confirmButton("不保存")
        tryCompare(documents, "currentSession", documents.session(b))
        tryCompare(split, "referenceDocumentId", a)
        const current = mainFixture.primaryView(main)
        tryCompare(current, "referenceSession", documents.session(a))
        tryVerify(() => Math.abs(current.viewportItem.viewportAnchor.measure
                    + current.viewportItem.viewportAnchor.fraction - before.measure - before.fraction) < 0.00001)
        compare(split.overlayMode, true)
        compare(current.ownsPlayback, true)
        compare(split.referenceView.ownsPlayback, false)
    }
    function test_overlayMode_project_assignment_and_close() {
        start(true)
        enterOverlay()
        const original = documents.activeId()
        main.currentPage = 3
        const id = documents.create().id
        verify(documents.activate(original).ok)
        main.adoptChart(mainFixture.snapshot(a).path)
        verify(split.openReference(id))
        compare(main.currentPage, 3)
        compare(split.overlayMode, true)
        main.currentPage = 0
        tryCompare(mainFixture.primaryView(main), "referenceSession", documents.session(id))
        split.collapse()
        tryCompare(split, "overlayMode", false)
        tryCompare(split, "expanded", false)
        compare(split.referenceDocumentId, id)
        compare(documents.activeId(), original)
    }
    function test_compactReferenceHidesRenderedWaveformAndReclaimsWidth() {
        start(false)
        const reference = split.referenceView
        verify(reference.documentSession.renderAsync(8000, false))
        tryVerify(() => reference.documentSession.waveformInfo().valid === true, 10000)
        tryCompare(reference, "waveformReady", true)
        const wave = mainFixture.findProperty(reference, "scrubbing")
        verify(wave !== null)
        compare(main.hideReferenceSecondaryChannels, true)
        compare(wave.visible, false)
        tryCompare(reference.viewportItem, "width", reference.width - 12)
        compare(mainFixture.findProperty(reference, "maxY").x, reference.viewportItem.width)
        main.hideReferenceSecondaryChannels = false
        tryCompare(wave, "visible", true)
        tryCompare(reference.viewportItem, "width", reference.width - 12 - wave.width)
        compare(mainFixture.findProperty(reference, "maxY").x, reference.viewportItem.width)
        main.hideReferenceSecondaryChannels = true
        tryCompare(wave, "visible", false)
        tryCompare(reference.viewportItem, "width", reference.width - 12)
        verify(mainFixture.screenshot())
    }
    function test_synchronizedPlayback_usesOwnTiming_andBSeekNeverSeeksAudio() {
        start(false)
        const primary = mainFixture.primaryView(main)
        const reference = split.referenceView
        primary.measureHeight = 288
        tryVerify(() => split.syncActive && reference.measureHeight === 288)
        tryCompare(primary.viewportItem, "height", reference.viewportItem.height)
        compare(primary.mapToItem(main.contentItem, 0, 0).y, reference.mapToItem(main.contentItem, 0, 0).y)
        audioEngine.hasPcm = true
        audioEngine.playing = true
        audioEngine.positionSec = 1.5
        audioEngine.playbackChanged()
        tryVerify(() => Math.abs(reference.viewportItem.viewportAnchor.measure
                    + reference.viewportItem.viewportAnchor.fraction - 0.75) < 0.00001)
        compare(primary.playheadSec, 1.5)
        fuzzyCompare(reference.playheadSec, 1.0, 0.00001) // 120 BPM A -> 180 BPM B
        compare(reference.ownsPlayback, false)
        // B navigation changes only the view, not the global clock or source.
        reference.seekTo(0.4)
        tryVerify(() => Math.abs(primary.viewportItem.viewportAnchor.measure
                    + primary.viewportItem.viewportAnchor.fraction - 0.3) < 0.00001)
        compare(audioEngine.positionSec, 1.5)
        compare(audioEngine.playbackCalls, 0)
        audioEngine.playbackChanged() // existing playing+follow rule reasserts A position
        tryVerify(() => Math.abs(primary.viewportItem.viewportAnchor.measure
                    + primary.viewportItem.viewportAnchor.fraction - 0.75) < 0.00001)
        primary.followPlayhead = false
        const anchor = JSON.stringify(primary.viewportItem.viewportAnchor)
        audioEngine.positionSec = 2
        audioEngine.playbackChanged()
        compare(JSON.stringify(primary.viewportItem.viewportAnchor), anchor)
        fuzzyCompare(reference.playheadSec, 4 / 3, 0.00001)
        audioEngine.playing = false
        audioEngine.hasPcm = false
    }
    function test_syncSwapKeepsAnchorAfterRealDraftConfirmation() {
        start(true)
        const primary = mainFixture.primaryView(main)
        primary.measureHeight = 288
        primary.seekToSeconds(1.5)
        tryVerify(() => split.referenceView.measureHeight === 288)
        const before = primary.viewportItem.viewportAnchor
        draft()
        swap()
        confirmButton("取消")
        compare(primary.viewportItem.viewportAnchor, before)
        swap()
        confirmButton("不保存")
        tryCompare(documents, "currentSession", documents.session(b))
        const current = mainFixture.primaryView(main)
        tryVerify(() => Math.abs(current.viewportItem.viewportAnchor.measure
                    + current.viewportItem.viewportAnchor.fraction - before.measure - before.fraction) < 0.00001)
        tryVerify(() => Math.abs(split.referenceView.viewportItem.viewportAnchor.measure
                    + split.referenceView.viewportItem.viewportAnchor.fraction - before.measure - before.fraction) < 0.00001)
        compare(audioEngine.playbackCalls, 0)
    }
    function test_lnPlacementPolicy_data() {
        return [
            {tag: "LNOBJ-without-LNTYPE", type: "", obj: "ZZ", lnobj: true},
            {tag: "LNTYPE1-with-LNOBJ", type: "1", obj: "ZZ", lnobj: false},
            {tag: "LNTYPE2-with-LNOBJ", type: "2", obj: "ZZ", lnobj: false},
            {tag: "no-LNOBJ", type: "", obj: "", lnobj: false},
            {tag: "LNTYPE2-no-LNOBJ", type: "2", obj: "", lnobj: false},
            {tag: "zero-LNOBJ", type: "", obj: "00", lnobj: false},
            {tag: "malformed-LNOBJ", type: "", obj: "!Z", lnobj: false},
            {tag: "short-LNOBJ", type: "", obj: "Z", lnobj: false}
        ]
    }
    function test_lnPlacementPolicy(data) {
        start(false)
        const edited = JSON.parse(beatbench.dispatch(JSON.stringify({command: "meta.edit", args: {
            session_id: a, edits: [{key: "LNTYPE", value: data.type}, {key: "LNOBJ", value: data.obj}]
        }})))
        verify(edited.ok)
        documents.refresh()
        compare(chartSession.useLnObjPlacement(), data.lnobj)
        main.currentSampleId = "01"
        main.editorTool = "ln"
        // Both policies must reject BGM without editing any existing note.
        const before = mainFixture.snapshot(a)
        main.placeNote({measure: 0, num: 1, den: 4, lanePlayer: 0, laneKind: "bgm", laneIndex: 0})
        compare(mainFixture.snapshot(a), before)
        compare(main.pendingLnHead, null)
        main.placeNote({measure: 0, num: 1, den: 4, lanePlayer: 0, laneKind: "key", laneIndex: 3})
        compare(!!main.pendingLnHead, data.lnobj)
        const viewport = mainFixture.primaryView(main).viewportItem
        viewport.scrollToStart()
        const refs = viewport.notesInRect(0, 18, viewport.width, viewport.height)
        const placed = refs.find(n => n.measure === 0 && n.lane.kind === "key" && n.lane.index === 3)
        verify(placed !== undefined, "Placed note must be exposed as a view ref")
        compare(placed.ln_channel, !data.lnobj)
        compare(placed.note_kind, "normal")
        compare(placed.sample, 1)
        // Complete both representations with a different current sample. LNOBJ must
        // use its marker; LN-channel placement must preserve the independently chosen ID.
        main.currentSampleId = "02"
        main.placeNote({measure: 0, num: 3, den: 4, lanePlayer: 0, laneKind: "key", laneIndex: 3})
        compare(main.pendingLnHead, null)
        const completed = viewport.notesInRect(0, 18, viewport.width, viewport.height)
                                  .filter(n => n.measure === 0 && n.lane.kind === "key" && n.lane.index === 3)
        compare(completed.length, 2)
        const tail = completed.find(n => n.pos.num === 3 && n.pos.den === 4)
        verify(tail !== undefined)
        compare(tail.sample, data.lnobj ? chartSession.lnobjSample() : 2)
        compare(tail.ln_channel, !data.lnobj)
        compare(tail.note_kind, "normal")
        main.lnSelectMode = true
        tryCompare(viewport, "lnSelectMode", true)
        const column = viewport.probe(0, 40).columns[viewport.columnIndexForRef(placed)]
        verify(column !== undefined)
        const x = column.x + column.w / 2
        let headHit = null, tailHit = null
        for (let y = 19; y < viewport.height; ++y) {
            const hit = viewport.noteAt(x, y)
            if (!hit.valid || hit.measure !== 0) continue
            if (hit.pos.num === 1 && hit.pos.den === 4) headHit = hit
            if (hit.pos.num === 3 && hit.pos.den === 4) tailHit = hit
            if (headHit && tailHit) break
        }
        verify(headHit !== null && tailHit !== null, "Both completed LN endpoints are hittable")
        verify(headHit.lnPartner !== undefined && tailHit.lnPartner !== undefined)
        compare(headHit.sample, 1)
        compare(headHit.ln_channel, !data.lnobj)
        compare(headHit.lnPartner.sample, tail.sample)
        compare(headHit.lnPartner.pos, tail.pos)
        compare(headHit.lnPartner.ln_channel, !data.lnobj)
        compare(headHit.lnPartner.note_kind, "normal")
        compare(tailHit.lnPartner.sample, 1)
        compare(tailHit.lnPartner.pos, placed.pos)
        compare(tailHit.lnPartner.ln_channel, !data.lnobj)
        compare(tailHit.lnPartner.note_kind, "normal")
        // Deletion is endpoint-local even with LN partner selection enabled.
        main.deleteNoteAt(placed)
        const remaining = viewport.notesInRect(0, 18, viewport.width, viewport.height)
                                  .filter(n => n.measure === 0 && n.lane.kind === "key" && n.lane.index === 3)
        compare(remaining.length, 1)
        compare(remaining[0].sample, tail.sample)
        compare(remaining[0].pos, tail.pos)
        compare(remaining[0].ln_channel, tail.ln_channel)
        main.deleteNoteAt(tail)
        compare(mainFixture.snapshot(a).notes, before.notes)
    }
    function test_coincidentNoteRefs_preserveIdentityOnMoveAndDelete() {
        start(false)
        const lane = {player: 0, kind: "key", index: 3}
        for (const kind of ["normal", "ln", "mine"]) {
            verify(main.sessionCmd("note.put", {measure: 0, pos: {num: 1, den: 4},
                                               lane: lane, sample: 1, kind: kind}) !== null)
        }
        const viewport = mainFixture.primaryView(main).viewportItem
        viewport.scrollToStart()
        function refs() {
            return viewport.notesInRect(0, 18, viewport.width, viewport.height)
                           .filter(n => n.lane.kind === "key" && (n.lane.index === 3 || n.lane.index === 4))
        }
        const all = refs()
        compare(all.length, 3)
        const ordinary = all.find(n => !n.ln_channel && n.note_kind === "normal")
        const ln = all.find(n => n.ln_channel && n.note_kind === "normal")
        const mine = all.find(n => !n.ln_channel && n.note_kind === "mine")
        verify(ordinary !== undefined && ln !== undefined && mine !== undefined)
        main.selectionRefs = [ln]
        main.moveSelection(0.25, null, lane, -1)
        compare(main.selectionRefs.length, 1)
        compare(main.selectionRefs[0].ln_channel, true)
        compare(main.selectionRefs[0].note_kind, "normal")
        const moved = refs()
        compare(moved.find(n => n.ln_channel).pos, {num: 1, den: 2})
        compare(moved.find(n => n.note_kind === "mine").pos, mine.pos)
        compare(moved.find(n => !n.ln_channel && n.note_kind === "normal").pos, ordinary.pos)
        const beforeCrossKey = mainFixture.snapshot(a).notes
        // Use the actual laneAtX target schema, not a Lane/ref-shaped stand-in.
        main.moveSelection(0, {valid: true, laneKind: "key", laneIndex: 4, lanePlayer: 0}, lane, -1)
        compare(main.selectionRefs.length, 1)
        compare(main.selectionRefs[0].lane, {player: 0, kind: "key", index: 4})
        compare(main.selectionRefs[0].ln_channel, true)
        compare(main.selectionRefs[0].note_kind, "normal")
        const crossed = refs()
        compare(crossed.length, 3)
        const crossedLn = crossed.find(n => n.ln_channel)
        compare(crossedLn.lane, {player: 0, kind: "key", index: 4})
        compare(crossedLn.pos, {num: 1, den: 2})
        compare(crossedLn.sample, ln.sample)
        compare(crossedLn.note_kind, "normal")
        compare(crossed.find(n => n.note_kind === "mine").lane, mine.lane)
        compare(crossed.find(n => n.note_kind === "mine").pos, mine.pos)
        compare(crossed.find(n => !n.ln_channel && n.note_kind === "normal").lane, ordinary.lane)
        compare(crossed.find(n => !n.ln_channel && n.note_kind === "normal").pos, ordinary.pos)
        const afterCrossKey = mainFixture.snapshot(a).notes
        main.undoEdit()
        compare(mainFixture.snapshot(a).notes, beforeCrossKey)
        const undoneLn = refs().find(n => n.ln_channel)
        compare(undoneLn.lane, lane)
        compare(undoneLn.pos, {num: 1, den: 2})
        compare(undoneLn.note_kind, "normal")
        main.redoEdit()
        compare(mainFixture.snapshot(a).notes, afterCrossKey)
        const redoneLn = refs().find(n => n.ln_channel)
        compare(redoneLn.lane, crossedLn.lane)
        compare(redoneLn.pos, crossedLn.pos)
        compare(redoneLn.note_kind, "normal")
        // The sample dialog must address the mine, not its coincident ordinary alias.
        main.editNoteSample(mine)
        const sampleDialog = mainFixture.objectNamed(main, "noteSampleDialog")
        tryCompare(sampleDialog, "opened", true)
        mainFixture.objectNamed(main, "noteSampleIdInput").text = "02"
        sampleDialog.accept()
        tryCompare(sampleDialog, "visible", false)
        const sampled = refs()
        compare(sampled.find(n => n.note_kind === "mine").sample, 2)
        compare(sampled.find(n => !n.ln_channel && n.note_kind === "normal").sample, 1)
        main.deleteNoteAt(ordinary)
        const remaining = refs()
        compare(remaining.length, 2)
        verify(remaining.some(n => n.ln_channel))
        verify(remaining.some(n => n.note_kind === "mine"))
    }
    function test_quantizeBoxMixedMeasureEnd_data() {
        return [{tag: "no-backup", backup: false}, {tag: "with-backup", backup: true}]
    }
    function test_quantizeBoxMixedMeasureEnd(data) {
        start(true)
        split.collapse()
        tryVerify(function() { return split.referenceWidth < 120 })
        const path = mainFixture.snapshot(a).path
        // Loading installs the real BMS crash-backup hook; the test fixture otherwise omits it.
        paneProtocol(a, "session.load", {path: path})
        main.adoptChart(path)
        const key = {player: 0, kind: "key", index: 7}
        const bgm = {player: 0, kind: "bgm", index: 0}
        const seeds = [{lane: key, sample: 2, sub_line: 0, num: 14},
                       {lane: bgm, sample: 3, sub_line: 0, num: 15},
                       {lane: bgm, sample: 4, sub_line: 1, num: 14}]
        for (const n of seeds) {
            verify(main.sessionCmd("note.put", {measure: 0, pos: {num: n.num, den: 16},
                                                lane: n.lane, sample: n.sample,
                                                sub_line: n.sub_line}) !== null)
        }
        compare(paneProtocol(a, "session.autosave", {backup: data.backup}).backup, data.backup)
        verify(!mainFixture.fileExists(path + ".bak"))
        const before = mainFixture.snapshot(a), beforeB = mainFixture.snapshot(b)
        main.editorTool = "select"
        main.snapNum = 1
        main.snapDen = 16
        const primary = mainFixture.primaryView(main)
        primary.bgmExpanded = false
        primary.measureHeight = 160
        const view = primary.viewportItem
        wait(0)
        view.scrollToStart()
        view.scrollX = Math.max(0, view.contentWidth - view.width)
        const columns = view.probe(0, 40).columns
        function centerX(ref) {
            const index = view.columnIndexForRef(ref)
            verify(index >= 0 && index < columns.length)
            const c = columns[index]
            return Math.round(c.x + c.w / 2)
        }
        function yNear(measure) {
            for (let y = 20; y < view.height - 2; ++y) {
                if (Math.abs(view.measureAtY(y) - measure) < 0.008) return y
            }
            fail("Position outside primary viewport: " + measure)
        }
        const x0 = centerX({lane: {player: 0, kind: "key", index: 5}})
        const x1 = centerX(seeds[1])
        const y0 = yNear(13 / 16), y1 = yNear(17 / 16)
        verify(x0 > view.rulerWidth && x1 > x0 && x1 < view.width)
        verify(!view.objectAt(x0, y0).valid, "Box drag must start on blank space")
        const hits = view.notesInRect(x0, y0, x1, y1)
        compare(hits.length, seeds.length, JSON.stringify({hits: hits, keyX: centerX(seeds[0]),
            x0: x0, x1: x1, y0: y0, y1: y1, columns: columns}))
        mouseDrag(primary, x0, y0, x1 - x0, y1 - y0, Qt.LeftButton)
        compare(main.referenceActionTarget, false)
        compare(main.selectionRefs.length, seeds.length,
                "Selected: " + JSON.stringify(main.selectionRefs) + "; model: " + JSON.stringify(before.notes))
        const originalRefs = main.selectionRefs.slice()
        for (const n of seeds) {
            const ref = originalRefs.find(r => r.sample === n.sample)
            verify(ref !== undefined)
            compare(ref.measure, 0)
            compare(ref.pos.num / ref.pos.den, n.num / 16)
            compare(ref.lane, n.lane)
            compare(ref.sub_line, n.sub_line)
        }

        main.snapDen = 4
        verify(uiActions.invoke("tool.quantize", {}))
        const after = mainFixture.snapshot(a)
        compare(after.undo, before.undo + 1)
        compare(after.notes.length, before.notes.length)
        compare(main.selectionRefs.length, seeds.length)
        for (const n of seeds) {
            const note = after.notes.find(r => r.sample === n.sample)
            verify(note !== undefined)
            compare(note.measure, 0)
            compare(note.num, 3)
            compare(note.den, 4)
            const ref = main.selectionRefs.find(r => r.sample === n.sample)
            verify(ref !== undefined)
            compare(ref.measure, note.measure)
            compare(ref.pos, {num: note.num, den: note.den})
            compare(ref.lane, n.lane)
            compare(ref.sub_line, n.sub_line)
        }
        if (data.backup) verify(mainFixture.fileExists(path + ".bak"))
        verify(uiActions.invoke("edit.undo", {}))
        compare(mainFixture.snapshot(a).notes, before.notes)
        compare(mainFixture.snapshot(a).undo, before.undo)
        compare(mainFixture.snapshot(b), beforeB)
    }
    function test_loadDoesNotEditModeFieldsOrCreateDraft() {
        start(false, true)
        compare(mainFixture.snapshot(a).meta.LNTYPE, "2")
        compare(mainFixture.snapshot(a).meta.LNOBJ, "ZZ")
        compare(mainFixture.snapshot(a).meta.PLAYER, "4")
        compare(main.metaDraftDirty, false)
    }
    function assertPmsPrimary(mode, id) {
        compare(main.chartMode, mode)
        compare(main.isPmsChart, true)
        compare(main.metaDraftDirty, false)
        compare(chartSession.sessionId, id)
        compare(chartSession.chartMode(), mode)
        const primary = mainFixture.primaryView(main)
        compare(primary.documentSession.sessionId, id)
        const viewport = primary.viewportItem
        tryVerify(() => viewport.columnIndexForRef({lane: {player: 0, kind: "key", index: 9}}) >= 0)
        compare(viewport.columnIndexForRef({lane: {player: 0, kind: "key", index: 10}}), -1)
        compare(mainFixture.objectNamed(main, "saveAsDialog").defaultSuffix, "pms")
        compare(split.referenceDocumentId, b)
        compare(split.referenceView.documentSession.sessionId, b)
        compare(split.referenceView.ownsPlayback, false)
    }
    function test_newModeDialog_createsIndependentPms_andRetainsModeAfterSwitch() {
        start(false)
        const sa = mainFixture.snapshot(a), sb = mainFixture.snapshot(b)
        // 调用Ctrl+N同源处理函数，再操作真实弹窗和新建按钮；不等同于原生快捷键验收。
        main.newChart()
        const dialog = mainFixture.dialog(main, "新建谱面")
        verify(dialog !== null)
        const combo = mainFixture.objectNamed(main, "newModeCombo")
        verify(combo !== null)
        let index = -1
        for (let i = 0; i < combo.model.length; ++i)
            if (combo.model[i].value === "pms9k") index = i
        verify(index >= 0)
        combo.currentIndex = index
        clickButton(dialog, "新建")
        const id = documents.activeId()
        verify(id !== a && id !== b)
        compare(main.chartPath, "")
        assertPmsPrimary("pms9k", id)
        unchanged(sa, sb)
        verify(documents.activate(a).ok)
        main.adoptChart(mainFixture.snapshot(a).path)
        compare(documents.activeId(), a)
        verify(documents.activate(id).ok)
        main.adoptChart(mainFixture.snapshot(id).path)
        assertPmsPrimary("pms9k", id)
        unchanged(sa, sb)
    }
    function test_pmsProfileSwitch_retainsMemoryMode_data() {
        return [{tag: "standard", bmesp: false, mode: "pms9k"},
                {tag: "bme-sp", bmesp: true, mode: "pms9k-bmesp"}]
    }
    function test_pmsProfileSwitch_retainsMemoryMode(data) {
        start(false)
        const sa = mainFixture.snapshot(a), sb = mainFixture.snapshot(b)
        main.requestDocumentAction("open", mainFixture.pmsFixture(data.bmesp))
        const id = documents.activeId()
        verify(id !== a && id !== b)
        assertPmsPrimary(data.mode, id)
        verify(documents.activate(a).ok)
        main.adoptChart(mainFixture.snapshot(a).path)
        verify(documents.activate(id).ok)
        main.adoptChart(mainFixture.snapshot(id).path)
        assertPmsPrimary(data.mode, id)
        unchanged(sa, sb)
    }
    // PMS suffix tests exercise production Main and its real modal. The accepted
    // handler is the FileDialog's shared entry, not a test of the native chooser.
    function pmsProfileData() {
        return [{tag: "standard", bmesp: false, mode: "pms9k"},
                {tag: "bme-sp", bmesp: true, mode: "pms9k-bmesp"}]
    }
    function openPmsFixture(profile) {
        const path = mainFixture.pmsFixture(profile.bmesp)
        verify(path.length > 0)
        main.requestDocumentAction("open", path)
        const id = documents.activeId()
        verify(id !== a && id !== b)
        assertPmsPrimary(profile.mode, id)
        return id
    }
    function test_statusShowsAsciiThenCp932AfterSavingJapanese() {
        start(true)
        const id = openPmsFixture(pmsProfileData()[0])
        const label = mainFixture.objectNamed(main, "chartTextFormatLabel")
        verify(label !== null)
        compare(label.text, "ASCII  LF")
        const response = JSON.parse(beatbench.dispatch(JSON.stringify({command: "meta.edit", args: {
            session_id: id, edits: [{key: "TITLE", value: "日本語"}]
        }})))
        verify(response.ok)
        main.adoptChart(main.chartPath)
        verify(main.saveChart())
        tryCompare(label, "text", "CP932  LF")
    }
    function seedPmsHistory(id) {
        for (const edit of [{key: "GENRE", value: "PMS history"},
                            {key: "ARTIST", value: "PMS redo"}]) {
            const result = JSON.parse(beatbench.dispatch(JSON.stringify({command: "meta.edit", args: {
                session_id: id, edits: [edit]
            }})))
            verify(result.ok)
        }
        const undone = JSON.parse(beatbench.dispatch(JSON.stringify({command: "session.undo", args: {
            session_id: id
        }})))
        verify(undone.ok)
        documents.refresh()
        const state = mainFixture.snapshot(id)
        verify(state.dirty && state.undo > 0 && state.redo > 0)
    }
    function draftTitleFor(id) {
        const title = mainFixture.snapshot(id).meta.TITLE
        const field = mainFixture.textEditor(main, title)
        verify(field !== null, "Missing TITLE editor for " + id)
        main.requestActivate()
        mouseClick(field, field.width / 2, field.height / 2)
        field.forceActiveFocus()
        tryCompare(field, "activeFocus", true)
        wait(100) // 原生Windows首次激活异步完成，不能把按键注入到尚未激活的窗口。
        field.selectAll()
        keyClick(Qt.Key_D)
        tryCompare(main, "metaDraftDirty", true)
        compare(mainFixture.snapshot(id).meta.TITLE, title)
        verify(field.text !== title)
        return field.text
    }
    function pmsSaveState(id) {
        return {document: mainFixture.snapshot(id), a: mainFixture.snapshot(a), b: mainFixture.snapshot(b),
                disk: mainFixture.disk(id), diskA: mainFixture.disk(a), diskB: mainFixture.disk(b),
                path: main.chartPath, mode: main.chartMode, draft: main.metaDraftDirty,
                edits: JSON.stringify(mainFixture.findProperty(main, "primaryKeys").collectEdits())}
    }
    function pmsDialog() {
        let dialog = null
        // saveChart[As] queues its modal with Qt.callLater; do not assert synchronously.
        tryVerify(function() {
            dialog = mainFixture.objectNamed(main, "pmsSuffixConfirmDialog")
            return dialog !== null && dialog.visible
        })
        tryCompare(dialog, "opened", true)
        compare(dialog.title, "PMS 保存后缀警告")
        compare(dialog.modal, true)
        verify(mainFixture.button(dialog, "仍按此后缀保存") !== null)
        verify(mainFixture.button(dialog, "取消保存") !== null)
        return dialog
    }
    function acceptPmsSave() { clickButton(pmsDialog(), "仍按此后缀保存") }
    function acceptedSaveAs(path) {
        // Simulate only the captured ID from openSaveAsDialog, without opening a
        // platform-native chooser in an automated QML test. Production consumes it.
        main.saveAsDocumentId = documents.activeId()
        const result = main.handleSaveAsAccepted(path)
        compare(main.saveAsDocumentId, "")
        return result
    }
    function savePmsToDangerousPath(id, path) {
        compare(documents.activeId(), id)
        compare(main.saveChartAs(path), undefined)
        acceptPmsSave()
        tryCompare(main, "chartPath", path)
        compare(mainFixture.snapshot(id).dirty, false)
        verify(mainFixture.fileExists(path))
    }
    function test_pmsDangerousSaveAs_cancelThenConfirm_data() {
        const rows = []
        for (const profile of pmsProfileData())
            for (const suffix of ["bms", "bme", "bml"])
                rows.push({tag: profile.tag + "-" + suffix, bmesp: profile.bmesp,
                           mode: profile.mode, suffix: suffix})
        return rows
    }
    function test_pmsDangerousSaveAs_cancelThenConfirm(data) {
        start(true)
        const id = openPmsFixture(data)
        seedPmsHistory(id)
        const title = draftTitleFor(id)
        const before = pmsSaveState(id), output = before.path + ".save." + data.suffix
        compare(mainFixture.fileExists(output), false)
        // At least one row enters through the exact FileDialog accepted handler.
        if (data.suffix === "bms") compare(acceptedSaveAs(output), undefined)
        else compare(main.saveChartAs(output), undefined)
        compare(mainFixture.fileExists(output), false)
        compare(pmsSaveState(id), before) // No draft commit before the queued modal.
        const dialog = pmsDialog()
        compare(mainFixture.fileExists(output), false)
        compare(pmsSaveState(id), before)
        if (data.mode === "pms9k" && data.suffix === "bms") {
            wait(50)
            verify(mainFixture.screenshot())
        }
        clickButton(dialog, "取消保存")
        tryCompare(dialog, "visible", false)
        compare(main.pendingDocAction, "")
        compare(mainFixture.fileExists(output), false)
        compare(pmsSaveState(id), before) // Complete snapshot/history/revision, both disks and draft.
        compare(documents.activeId(), id)
        compare(main.saveChartAs(output), undefined)
        pmsDialog()
        compare(mainFixture.fileExists(output), false)
        compare(pmsSaveState(id), before)
        acceptPmsSave()
        tryCompare(main, "chartPath", output)
        const saved = mainFixture.snapshot(id)
        verify(mainFixture.fileExists(output))
        compare(saved.meta.TITLE, title)
        compare(saved.notes, before.document.notes)
        compare(saved.dirty, false)
        compare(saved.undo, before.document.undo + 1)
        compare(saved.redo, 0)
        verify(mainFixture.disk(id).indexOf("#TITLE " + title) >= 0)
        assertPmsPrimary(data.mode, id) // Saving .bms/.bme/.bml must NOT reparse/downgrade memory.
        unchanged(before.a, before.b)
        compare(mainFixture.disk(a), before.diskA)
        compare(mainFixture.disk(b), before.diskB)
    }
    function test_pmsSuffixEscape_cancelsBeforeAnyWrite() {
        start(true)
        const id = openPmsFixture(pmsProfileData()[0])
        seedPmsHistory(id)
        draftTitleFor(id)
        const before = pmsSaveState(id), output = before.path + ".escape.bms"
        compare(main.saveChartAs(output), undefined)
        const dialog = pmsDialog()
        keyClick(Qt.Key_Escape)
        tryCompare(dialog, "visible", false)
        compare(main.pmsSaveConfirmationPending, false)
        compare(mainFixture.fileExists(output), false)
        compare(pmsSaveState(id), before)
    }
    function test_pmsUppercaseSuffix_doesNotWarn_data() { return pmsProfileData() }
    function test_pmsUppercaseSuffix_doesNotWarn(data) {
        start(true)
        const id = openPmsFixture(data)
        const title = draftTitleFor(id), before = pmsSaveState(id)
        const output = before.path + ".PMS"
        verify(main.saveChartAs(output))
        wait(0)
        const dialog = mainFixture.objectNamed(main, "pmsSuffixConfirmDialog")
        verify(dialog !== null && !dialog.visible)
        verify(mainFixture.fileExists(output))
        compare(mainFixture.snapshot(id).meta.TITLE, title)
        compare(mainFixture.snapshot(id).dirty, false)
        assertPmsPrimary(data.mode, id)
        unchanged(before.a, before.b)
        compare(mainFixture.disk(a), before.diskA)
        compare(mainFixture.disk(b), before.diskB)
    }
    function test_spSaveAs_doesNotWarn_data() {
        return ["bms", "bme", "bml"].map(s => ({tag: s, suffix: s}))
    }
    function test_spSaveAs_doesNotWarn(data) {
        start(true)
        const title = draftTitleFor(a), sb = mainFixture.snapshot(b), diskB = mainFixture.disk(b)
        const output = main.chartPath + ".save." + data.suffix
        acceptedSaveAs(output)
        wait(0)
        const dialog = mainFixture.objectNamed(main, "pmsSuffixConfirmDialog")
        verify(dialog !== null && !dialog.visible)
        verify(mainFixture.fileExists(output))
        compare(mainFixture.snapshot(a).meta.TITLE, title)
        compare(mainFixture.snapshot(a).dirty, false)
        compare(main.chartMode, "sp7k")
        compare(main.metaDraftDirty, false)
        compare(mainFixture.snapshot(b), sb)
        compare(mainFixture.disk(b), diskB)
    }
    function test_pmsDangerousCurrentPath_saveChart_data() { return pmsProfileData() }
    function test_pmsDangerousCurrentPath_saveChart(data) {
        start(true)
        const id = openPmsFixture(data), output = main.chartPath + ".save.bms"
        savePmsToDangerousPath(id, output)
        seedPmsHistory(id)
        const title = draftTitleFor(id), before = pmsSaveState(id)
        compare(main.saveChart(), undefined)
        pmsDialog()
        compare(pmsSaveState(id), before)
        clickButton(pmsDialog(), "取消保存")
        compare(pmsSaveState(id), before)
        compare(main.saveChart(), undefined)
        acceptPmsSave()
        compare(mainFixture.snapshot(id).meta.TITLE, title)
        compare(mainFixture.snapshot(id).dirty, false)
        verify(mainFixture.disk(id).indexOf("#TITLE " + title) >= 0)
        assertPmsPrimary(data.mode, id)
        unchanged(before.a, before.b)
        compare(mainFixture.disk(a), before.diskA)
        compare(mainFixture.disk(b), before.diskB)
    }
    function test_pmsPendingAction_cancelOrSaveBeforeContinuing_data() {
        const rows = []
        for (const action of ["new", "close"])
            for (const accept of [false, true])
                rows.push({tag: action + (accept ? "-confirm" : "-cancel"), action: action, accept: accept})
        return rows
    }
    function test_pmsPendingAction_cancelOrSaveBeforeContinuing(data) {
        start(true)
        // start(true) deliberately leaves A/B dirty with history. Close must not
        // accidentally test their background saves instead of the PMS decision.
        verify(documents.save(a).ok)
        verify(documents.save(b).ok)
        const id = openPmsFixture(pmsProfileData()[0]), output = main.chartPath + ".save.bms"
        savePmsToDangerousPath(id, output)
        const title = draftTitleFor(id), before = pmsSaveState(id)
        const count = documents.instances.length
        if (data.action === "new") main.newChartAs("sp7k")
        else main.close() // Actual Window.onClosing gate, no allowClose test shortcut.
        compare(main.pendingDocAction, data.action)
        confirmButton("保存")
        const dialog = pmsDialog()
        compare(main.pendingDocAction, data.action)
        compare(documents.activeId(), id)
        compare(documents.instances.length, count)
        compare(main.visible, true)
        compare(main.allowClose, false)
        compare(pmsSaveState(id), before)
        clickButton(dialog, data.accept ? "仍按此后缀保存" : "取消保存")
        tryCompare(main, "pendingDocAction", "")
        if (!data.accept) {
            compare(documents.activeId(), id)
            compare(documents.instances.length, count)
            compare(main.visible, true)
            compare(main.allowClose, false)
            compare(pmsSaveState(id), before)
        } else {
            compare(mainFixture.snapshot(id).meta.TITLE, title)
            compare(mainFixture.snapshot(id).dirty, false)
            verify(mainFixture.disk(id).indexOf("#TITLE " + title) >= 0)
            if (data.action === "new") {
                compare(documents.instances.length, count + 1)
                verify(documents.activeId() !== id)
                compare(main.chartMode, "sp7k")
                compare(main.visible, true)
            } else tryCompare(main, "visible", false)
            unchanged(before.a, before.b)
            compare(mainFixture.disk(a), before.diskA)
            compare(mainFixture.disk(b), before.diskB)
        }
    }
    function test_pmsSuffixConfirm_rejectsChangedDocumentId() {
        start(true)
        const id = openPmsFixture(pmsProfileData()[0])
        draftTitleFor(id)
        const original = mainFixture.snapshot(id), disk = mainFixture.disk(id)
        const output = main.chartPath + ".save.bms"
        acceptedSaveAs(output)
        const dialog = pmsDialog()
        verify(documents.activate(b).ok)
        main.adoptChart(mainFixture.snapshot(b).path)
        const sa = mainFixture.snapshot(a), sb = mainFixture.snapshot(b)
        const diskA = mainFixture.disk(a), diskB = mainFixture.disk(b)
        clickButton(dialog, "仍按此后缀保存")
        tryCompare(dialog, "visible", false)
        compare(mainFixture.fileExists(output), false)
        compare(mainFixture.snapshot(id), original)
        compare(mainFixture.disk(id), disk)
        unchanged(sa, sb)
        compare(mainFixture.disk(a), diskA)
        compare(mainFixture.disk(b), diskB)
        compare(documents.activeId(), b)
        compare(main.pendingDocAction, "")
    }
    function test_pmsQueuedConfirmation_freezesTargetAndBlocksReentry() {
        start(true)
        const id = openPmsFixture(pmsProfileData()[0])
        const title = draftTitleFor(id), before = pmsSaveState(id)
        const output = main.chartPath + ".save.bms", other = main.chartPath + ".PMS"
        compare(acceptedSaveAs(output), undefined)
        // Lock exists even BEFORE the deferred modal is visible.
        compare(main.pmsSaveConfirmationPending, true)
        compare(main.pendingPmsSaveDocumentId, id)
        compare(main.pendingPmsSavePath, output)
        compare(main.pendingPmsSaveAs, true)
        main.saveChartAs(other)
        main.saveChart()
        main.saveAllDocuments()
        main.newChartAs("sp7k")
        main.close()
        compare(main.pendingDocAction, "")
        compare(main.pendingPmsSaveDocumentId, id)
        compare(main.pendingPmsSavePath, output)
        compare(main.pendingPmsSaveAs, true)
        compare(main.visible, true)
        compare(mainFixture.fileExists(output), false)
        compare(mainFixture.fileExists(other), false)
        compare(pmsSaveState(id), before)
        acceptPmsSave()
        compare(main.pmsSaveConfirmationPending, false)
        compare(main.chartPath, output)
        compare(mainFixture.snapshot(id).meta.TITLE, title)
        verify(mainFixture.fileExists(output))
        compare(mainFixture.fileExists(other), false)
        const saved = pmsSaveState(id)
        // Stale accept callbacks cannot reuse the authorization or commit twice.
        compare(main.confirmPmsSuffixSave(), false)
        compare(main.handleSaveAsAccepted(other), false)
        compare(pmsSaveState(id), saved)
        compare(mainFixture.fileExists(other), false)
    }
    function test_saveAsAccepted_rejectsFrozenIdAfterSwitch() {
        start(true)
        const id = openPmsFixture(pmsProfileData()[0])
        const original = mainFixture.snapshot(id), disk = mainFixture.disk(id)
        const output = main.chartPath + ".save.bms"
        // Same capture/consume contract as the native dialog, without testing its UI.
        main.saveAsDocumentId = id
        verify(documents.activate(b).ok)
        main.adoptChart(mainFixture.snapshot(b).path)
        const sa = mainFixture.snapshot(a), sb = mainFixture.snapshot(b)
        const diskA = mainFixture.disk(a), diskB = mainFixture.disk(b)
        compare(main.handleSaveAsAccepted(output), false)
        compare(main.saveAsDocumentId, "")
        wait(0)
        compare(main.pmsSaveConfirmationPending, false)
        const dialog = mainFixture.objectNamed(main, "pmsSuffixConfirmDialog")
        verify(dialog !== null && !dialog.visible)
        compare(mainFixture.fileExists(output), false)
        compare(mainFixture.snapshot(id), original)
        compare(mainFixture.disk(id), disk)
        unchanged(sa, sb)
        compare(mainFixture.disk(a), diskA)
        compare(mainFixture.disk(b), diskB)
        compare(documents.activeId(), b)
    }
    function test_saveAll_inactivePms_requiresEachConfirmation_data() {
        return [{tag: "cancel-first", cancel: true}, {tag: "confirm-both", cancel: false}]
    }
    function test_saveAll_inactivePms_requiresEachConfirmation(data) {
        start(true)
        verify(documents.save(a).ok)
        verify(documents.save(b).ok)
        const ids = []
        for (const profile of pmsProfileData()) {
            const id = openPmsFixture(profile)
            savePmsToDangerousPath(id, main.chartPath + ".save.bms")
            seedPmsHistory(id)
            ids.push(id)
        }
        verify(documents.activate(a).ok)
        main.adoptChart(mainFixture.snapshot(a).path)
        const sa = mainFixture.snapshot(a), sb = mainFixture.snapshot(b)
        const diskA = mainFixture.disk(a), diskB = mainFixture.disk(b)
        const snapshots = ids.map(id => mainFixture.snapshot(id))
        const disks = ids.map(id => mainFixture.disk(id))
        main.saveAllDocuments()
        pmsDialog()
        compare(main.pendingDocAction, "saveAll")
        for (let i = 0; i < ids.length; ++i) {
            compare(mainFixture.snapshot(ids[i]), snapshots[i])
            compare(mainFixture.disk(ids[i]), disks[i])
        }
        if (data.cancel) {
            clickButton(pmsDialog(), "取消保存")
            tryCompare(main, "pendingDocAction", "")
            for (let i = 0; i < ids.length; ++i) {
                compare(mainFixture.snapshot(ids[i]), snapshots[i])
                compare(mainFixture.disk(ids[i]), disks[i])
            }
        } else {
            acceptPmsSave()
            pmsDialog()
            compare(main.pendingDocAction, "saveAll")
            compare(ids.filter(id => !mainFixture.snapshot(id).dirty).length, 1)
            const remaining = ids.findIndex(id => mainFixture.snapshot(id).dirty)
            compare(mainFixture.snapshot(ids[remaining]), snapshots[remaining])
            compare(mainFixture.disk(ids[remaining]), disks[remaining])
            acceptPmsSave()
            tryCompare(main, "pendingDocAction", "")
            for (let i = 0; i < ids.length; ++i) {
                const saved = mainFixture.snapshot(ids[i])
                compare(saved.dirty, false)
                compare(saved.meta, snapshots[i].meta)
                compare(saved.notes, snapshots[i].notes)
                compare(saved.undo, snapshots[i].undo)
                compare(saved.redo, snapshots[i].redo)
                compare(documents.session(ids[i]).chartMode(), pmsProfileData()[i].mode)
                verify(mainFixture.disk(ids[i]).indexOf("#GENRE PMS history") >= 0)
            }
        }
        compare(documents.activeId(), a)
        unchanged(sa, sb)
        compare(mainFixture.disk(a), diskA)
        compare(mainFixture.disk(b), diskB)
        compare(main.visible, true)
        compare(main.allowClose, false)
    }
    function test_newDirectModes_doNotInjectPlayerOrChangeOldDocuments_data() {
        return [{tag: "sp", mode: "sp7k"}, {tag: "dp", mode: "dp"}, {tag: "battle", mode: "battle"}]
    }
    function test_newDirectModes_doNotInjectPlayerOrChangeOldDocuments(data) {
        start(false)
        const sa = mainFixture.snapshot(a), sb = mainFixture.snapshot(b)
        main.newChartAs(data.mode)
        const id = documents.activeId()
        verify(id !== a && id !== b)
        compare(main.chartMode, data.mode)
        compare(chartSession.chartMode(), data.mode)
        compare(main.metaDraftDirty, false)
        verify(main.chartMeta.PLAYER === undefined)
        compare(mainFixture.primaryView(main).documentSession.sessionId, id)
        verify(documents.activate(a).ok)
        main.adoptChart(mainFixture.snapshot(a).path)
        verify(documents.activate(id).ok)
        main.adoptChart(mainFixture.snapshot(id).path)
        compare(main.chartMode, data.mode)
        unchanged(sa, sb)
    }
    function test_unnamedSwap_preservesIdentityAndHistory() {
        start(false)
        const sa = mainFixture.snapshot(a), sb = mainFixture.snapshot(b)
        compare(sa.path, ""); compare(sb.path, "")
        swap()
        tryCompare(chartSession, "sessionId", b)
        compare(documents.activeId(), b)
        compare(split.referenceDocumentId, a)
        compare(split.referenceView.documentSession.sessionId, a)
        compare(main.chartMeta.TITLE, "Fixture B")
        compare(main.chartPath, "")
        unchanged(sa, sb)
        swap()
        compare(documents.activeId(), a)
        compare(split.referenceDocumentId, b)
        unchanged(sa, sb)
        verify(mainFixture.screenshot())
    }
    function test_draftCancel() {
        start(true)
        draft()
        const sa = mainFixture.snapshot(a), sb = mainFixture.snapshot(b)
        swap()
        compare(main.pendingDocAction, "swap")
        compare(documents.activeId(), a)
        confirmButton("取消")
        tryCompare(main, "pendingDocAction", "")
        compare(documents.activeId(), a)
        compare(split.referenceDocumentId, b)
        compare(main.metaDraftDirty, true)
        unchanged(sa, sb)
    }
    function test_draftDiscard() {
        start(true)
        draft()
        const sa = mainFixture.snapshot(a), sb = mainFixture.snapshot(b)
        swap(); confirmButton("不保存")
        tryVerify(function() { return documents.activeId() === b })
        compare(split.referenceDocumentId, a)
        compare(main.metaDraftDirty, false)
        unchanged(sa, sb)
        swap()
        compare(main.chartMeta.TITLE, "Fixture A")
        compare(main.metaDraftDirty, false)
    }
    function test_draftSave() {
        start(true)
        const title = draft(), sb = mainFixture.snapshot(b)
        // This must persist the real typed draft, not merely save the old chart.
        swap(); confirmButton("保存")
        tryVerify(function() { return documents.activeId() === b })
        const saved = mainFixture.snapshot(a)
        compare(saved.meta.TITLE, title)
        compare(saved.dirty, false)
        verify(mainFixture.disk(a).indexOf("#TITLE " + title) >= 0)
        compare(mainFixture.snapshot(b), sb)
        compare(split.referenceDocumentId, a)
    }
    function findCombo(item, key) {
        if (item.source && item.source.key === key && item.popup) return item
        const children = item.children || []
        for (let i = 0; i < children.length; ++i) {
            const found = findCombo(children[i], key)
            if (found) return found
        }
        return null
    }
    function test_metadataDraft_allFieldKindsUseAuthoritativeState() {
        start(true)
        const panel = mainFixture.findProperty(main, "primaryKeys")
        verify(panel !== null)
        panel.expandSecondary = true
        wait(0)
        const secondary = mainFixture.textEditor(main, "Secondary fixture")
        verify(secondary !== null)
        secondary.forceActiveFocus()
        secondary.selectAll()
        keyClick(Qt.Key_E)
        const combo = findCombo(panel, "PLAYER")
        verify(combo !== null)
        combo.forceActiveFocus()
        keyClick(Qt.Key_End)
        tryVerify(function() { return panel.collectEdits().some(e => e.key === "PLAYER" && e.value === "4") })
        verify(panel.collectEdits().some(e => e.key === "COMMENT" && e.value === "e"))
        main.saveMetaEdits()
        compare(mainFixture.snapshot(a).meta.COMMENT, "e")
        compare(mainFixture.snapshot(a).meta.PLAYER, "4")
        compare(main.metaDraftDirty, false)
        verify(main.saveChart())
        verify(mainFixture.disk(a).indexOf("#COMMENT e") >= 0)
        verify(mainFixture.disk(a).indexOf("#PLAYER 4") >= 0)
    }
    function paneProtocol(id, command, args) {
        const explicit = Object.assign({}, args || {}, {session_id: id})
        const response = JSON.parse(beatbench.dispatch(JSON.stringify({command: command, args: explicit})))
        verify(response.ok, command + ": " + JSON.stringify(response.error))
        documents.refresh()
        return response.result
    }
    function seedPaneMeta(id, kind) {
        const object = {kind: kind, measure: 2, pos: {num: 0, den: 1}, layer: 0,
                        sample: id === a ? 1 : 2, value: kind === "bpm" ? 180 : 96}
        paneProtocol(id, kind === "bga" ? "bga.put" : "timing.put", object)
        return object
    }
    function paneMetaEvents(id, kind) {
        return paneProtocol(id, kind === "bga" ? "bga.list" : "timing.list", {kind: kind}).events
    }
    function test_referenceClipboard_realShortcutsUseBAndOneUndo_data() {
        return [{tag: "different-doc", sameDocument: false}, {tag: "same-doc", sameDocument: true}]
    }
    function test_referenceClipboard_realShortcutsUseBAndOneUndo(data) {
        start(false)
        if (data.sameDocument) verify(split.selectReference(a))
        const id = data.sameDocument ? a : b, other = data.sameDocument ? b : a
        const sample = data.sameDocument ? 1 : 2
        main.editorTool = "select"
        main.selectionRefs = [{measure: 1, pos: {num: 0, den: 1}, sample: 1,
                              lane: {player: 0, kind: "key", index: 1}}]
        const before = mainFixture.snapshot(id), untouched = mainFixture.snapshot(other)
        const point = auditionPoint(split.referenceView, true, sample)
        mouseClick(split.referenceView, point.x, point.y)
        keySequence("Ctrl+C")
        verify(clipboard.text().indexOf(sample === 2 ? "02" : "01") >= 0,
               "Copy must encode the selected B document sample")
        compare(clipboard.text(), main.clipboardLines.join("\n"))
        verify(clipboard.text().toUpperCase().indexOf("#WAV") < 0)
        compare(mainFixture.snapshot(id), before)
        compare(mainFixture.snapshot(other), untouched)
        const view = split.referenceView
        split.synchronizeViewports = false
        for (let i = 0; i < 8; ++i) view.extendMeasures()
        // Editable tail measures have no timing index; navigate them by musical position, not seconds.
        view.viewportItem.applyViewportAnchor({valid: true, measure: 6, fraction: 0})
        tryVerify(() => Math.floor(view.centerMeasure()) > 1)
        const target = Math.floor(view.centerMeasure())
        verify(target > 1)
        keySequence("Ctrl+V")
        tryVerify(() => mainFixture.snapshot(id).notes.length === before.notes.length + 1)
        const pasted = mainFixture.snapshot(id)
        compare(pasted.undo, before.undo + 1)
        compare(pasted.notes.length, before.notes.length + 1)
        verify(pasted.notes.some(n => n.measure === target && n.sample === sample))
        compare(pasted.definitions, before.definitions)
        compare(mainFixture.snapshot(other), untouched)
        if (data.sameDocument)
            compare(mainFixture.bridgeState(mainFixture.primaryView(main).documentSession).notes, pasted.notes.length)
        compare(documents.activeId(), a)
        compare(view.ownsPlayback, false)
        verify(uiActions.invoke("edit.undo", {}))
        compare(mainFixture.snapshot(id).notes, before.notes)
    }
    function test_referencePaste_rejectsDefinitionsAndLowercaseWithoutFallback_data() {
        return [{tag: "raw-definitions", text: "#WAVZZ new.wav\n#00011:ZZ"},
                {tag: "literal-lowercase", text: "#00011:0a"},
                {tag: "base62-declaration", text: "#BASE 62\n#00011:10"}]
    }
    function test_referencePaste_rejectsDefinitionsAndLowercaseWithoutFallback(data) {
        start(false)
        split.referenceFocused = true
        const sa = mainFixture.snapshot(a), sb = mainFixture.snapshot(b)
        clipboard.setText(data.text)
        main.pasteClipboard()
        unchanged(sa, sb)
        const dialog = mainFixture.objectNamed(main, "base62PasteDialog")
        compare(dialog.visible, false, "B must not expose a case-fold confirmation bypass")
        const response = main.referenceEditOperations.pasteCaptured(
            main.referenceEditContext.capture(), data.text, 3, {})
        compare(response.ok, false)
        unchanged(sa, sb)
    }
    function test_primaryPaste_preservesExplicitRawDefinitionsAndInternalFallback() {
        start(false)
        split.referenceFocused = false
        const sb = mainFixture.snapshot(b)
        const before = mainFixture.snapshot(a)
        clipboard.setText("#WAVZZ explicit.wav\n#00011:ZZ")
        main.pasteClipboard()
        const first = mainFixture.snapshot(a)
        compare(first.undo, before.undo + 1)
        verify(Object.values(first.definitions).indexOf("explicit.wav") >= 0)
        verify(first.notes.some(n => n.sample === 1295))
        compare(main.selectionRefs.length, 1)
        compare(mainFixture.snapshot(b), sb)
        main.clipboardLines = ["#00012:02"]
        clipboard.setText("not BMS text")
        main.pasteClipboard()
        const after = mainFixture.snapshot(a)
        compare(after.undo, first.undo + 1)
        verify(after.notes.some(n => n.sample === 2 && n.lane === 2))
        compare(mainFixture.snapshot(b), sb)
    }
    function test_primaryBase62Confirm_freezesOwnerTextAndTarget_data() {
        return [{tag: "focus-only", change: "focus"}, {tag: "active-document", change: "active"},
                {tag: "revision-changed", change: "revision"}, {tag: "cancel", change: "cancel"}]
    }
    function test_primaryBase62Confirm_freezesOwnerTextAndTarget(data) {
        start(false)
        split.referenceFocused = false
        clipboard.setText("#00011:0a")
        main.pasteClipboard()
        const dialog = mainFixture.objectNamed(main, "base62PasteDialog")
        tryCompare(dialog, "opened", true)
        const target = dialog.pendingTarget
        compare(dialog.captureToken.sessionId, a)
        compare(dialog.pendingText, "#00011:0a")
        clipboard.setText("#00012:ZZ")
        const primary = mainFixture.primaryView(main)
        for (let i = 0; i < 8; ++i) primary.extendMeasures()
        primary.viewportItem.applyViewportAnchor({valid: true, measure: 6, fraction: 0})
        tryVerify(() => Math.floor(primary.centerMeasure()) !== target)
        split.referenceFocused = true
        if (data.change === "active") {
            verify(documents.activate(b).ok)
            main.adoptChart(mainFixture.snapshot(b).path)
        } else if (data.change === "revision") {
            paneProtocol(a, "note.put", {measure: 5, pos: {num: 0, den: 1}, sample: 1,
                lane: {player: 0, kind: "key", index: 2}})
        }
        const sa = mainFixture.snapshot(a), sb = mainFixture.snapshot(b)
        if (data.change === "cancel") clickButton(dialog, "取消")
        else clickButton(dialog, "仍要粘贴")
        tryCompare(dialog, "visible", false)
        compare(mainFixture.snapshot(b), sb)
        if (data.change === "focus") {
            const after = mainFixture.snapshot(a)
            compare(after.undo, sa.undo + 1)
            verify(after.notes.some(n => n.measure === target && n.sample === 10))
            verify(!after.notes.some(n => n.sample === 1295))
        } else compare(mainFixture.snapshot(a), sa)
    }
    function test_metaModal_bothPanesFreezeStopUnitsAndDestination_data() {
        const result = []
        for (const reference of [false, true]) for (const kind of ["bpm", "stop", "bga"])
            result.push({tag: (reference ? "B-" : "A-") + kind, reference: reference, kind: kind})
        return result
    }
    function test_metaModal_bothPanesFreezeStopUnitsAndDestination(data) {
        start(false)
        const id = data.reference ? b : a, other = data.reference ? a : b
        const object = seedPaneMeta(id, data.kind)
        const before = mainFixture.snapshot(id), untouched = mainFixture.snapshot(other)
        const original = paneMetaEvents(id, data.kind)
        main.stopUnit = 1
        if (data.reference) split.referenceView.metaEditRequested(object)
        else main.editMetaObject(object)
        const dialog = mainFixture.objectNamed(main, "metaEditDialog")
        tryCompare(dialog, "opened", true)
        compare(dialog.displaySession, documents.session(id))
        const frozenBpm = data.reference ? 180 : 120
        if (data.kind === "stop") {
            compare(dialog.frozenSettings.stopUnit, 1)
            compare(dialog.frozenSettings.stopBpm, frozenBpm)
            fuzzyCompare(Number(dialog.displayText), 96 * 1250 / frozenBpm, 0.000001)
        }
        main.stopUnit = 0
        main.currentBpmValue = 999
        split.referenceFocused = !data.reference
        const field = mainFixture.objectNamed(main, "metaEditValueInput")
        field.text = data.kind === "bpm" ? "240" : data.kind === "stop" ? "1250" : "ZZ"
        dialog.accept()
        tryCompare(dialog, "visible", false)
        const events = paneMetaEvents(id, data.kind)
        compare(events.length, original.length)
        if (data.kind === "bga") compare(events[0].sample, 1295)
        else compare(events[0].value, data.kind === "bpm" ? 240 : frozenBpm)
        compare(mainFixture.snapshot(id).undo, before.undo + 1)
        compare(mainFixture.snapshot(other), untouched)
        compare(documents.activeId(), a)
    }
    function test_referenceMetaBga_usesStrictDestinationLiteral_data() {
        return [{tag: "base36-rejects-lowercase", base: 36, text: "0a", accepted: false},
                {tag: "base62-case-sensitive", base: 62, text: "0a", accepted: true, sample: 36},
                {tag: "base36-unbound", base: 36, text: "ZZ", accepted: true, sample: 1295},
                {tag: "no-prefix", base: 36, text: "#BMP02", accepted: false},
                {tag: "no-trim", base: 36, text: " 02", accepted: false}]
    }
    function test_referenceMetaBga_usesStrictDestinationLiteral(data) {
        start(false)
        verify(mainFixture.setDocumentBase(b, data.base))
        const object = seedPaneMeta(b, "bga")
        const sa = mainFixture.snapshot(a), sb = mainFixture.snapshot(b)
        const original = paneMetaEvents(b, "bga")
        main.editReferenceMetaObject(object)
        const dialog = mainFixture.objectNamed(main, "metaEditDialog")
        tryCompare(dialog, "opened", true)
        compare(mainFixture.objectNamed(main, "metaEditValueInput").text, "02")
        mainFixture.objectNamed(main, "metaEditValueInput").text = data.text
        dialog.accept()
        tryCompare(dialog, "visible", false)
        compare(mainFixture.snapshot(a), sa)
        compare(mainFixture.snapshot(b).definitions, sb.definitions)
        if (data.accepted) {
            compare(mainFixture.snapshot(b).undo, sb.undo + 1)
            compare(paneMetaEvents(b, "bga")[0].sample, data.sample)
        } else {
            compare(mainFixture.snapshot(b), sb)
            compare(paneMetaEvents(b, "bga"), original)
        }
    }
    function test_referenceMetaModal_closedOwnerNeverFallsBackToA() {
        start(false)
        const id = documents.create().id
        verify(documents.activate(a).ok)
        main.adoptChart(mainFixture.snapshot(a).path)
        verify(split.selectReference(id))
        const object = seedPaneMeta(id, "bpm")
        main.editReferenceMetaObject(object)
        const dialog = mainFixture.objectNamed(main, "metaEditDialog")
        tryCompare(dialog, "opened", true)
        verify(JSON.parse(beatbench.dispatch(JSON.stringify({command: "session.close", args: {id: id}}))).ok)
        documents.refresh()
        const sa = mainFixture.snapshot(a), sb = mainFixture.snapshot(b)
        compare(mainFixture.objectNamed(main, "metaEditValueInput").text, "180")
        mainFixture.objectNamed(main, "metaEditValueInput").text = "240"
        dialog.accept()
        tryCompare(dialog, "visible", false)
        unchanged(sa, sb)
    }
    function test_metaModal_rejectsOwnerOrRevisionChanges_data() {
        return [{tag: "A-active-changed", reference: false, change: "active"},
                {tag: "A-revision", reference: false, change: "revision"},
                {tag: "B-document-changed", reference: true, change: "document"},
                {tag: "B-revision", reference: true, change: "revision"}]
    }
    function test_metaModal_rejectsOwnerOrRevisionChanges(data) {
        start(false)
        const id = data.reference ? b : a
        const object = seedPaneMeta(id, "bpm")
        if (data.reference) main.editReferenceMetaObject(object)
        else main.editMetaObject(object)
        const dialog = mainFixture.objectNamed(main, "metaEditDialog")
        tryCompare(dialog, "opened", true)
        const displayOwner = dialog.displaySession
        if (data.change === "active") {
            verify(documents.activate(b).ok)
            main.adoptChart(mainFixture.snapshot(b).path)
        } else if (data.change === "document") verify(split.selectReference(a))
        else paneProtocol(id, "note.put", {measure: 5, pos: {num: 0, den: 1}, sample: 1,
            lane: {player: 0, kind: "key", index: 2}})
        compare(dialog.displaySession, displayOwner)
        compare(mainFixture.objectNamed(main, "metaEditValueInput").text, "180")
        const sa = mainFixture.snapshot(a), sb = mainFixture.snapshot(b)
        mainFixture.objectNamed(main, "metaEditValueInput").text = "240"
        dialog.accept()
        tryCompare(dialog, "visible", false)
        unchanged(sa, sb)
        compare(paneMetaEvents(id, "bpm")[0].value, 180)
    }
    function test_referenceFocus_routesRealDeleteAndUndoWithoutChangingA() {
        start(false)
        const primary = mainFixture.primaryView(main)
        const titleA = mainFixture.objectNamed(main, "primaryPaneTitle")
        const titleB = mainFixture.objectNamed(main, "referencePaneTitle")
        verify(titleA !== null && titleA.visible)
        verify(titleB !== null && titleB.visible)
        const toggle = mainFixture.objectNamed(main, "overlayModeButton")
        verify(toggle !== null)
        compare(titleA.font.family, toggle.contentItem.font.family)
        compare(titleA.font.family, titleB.font.family)
        compare(titleA.font.pixelSize, titleB.font.pixelSize)
        tryVerify(() => Math.abs(titleA.mapToItem(split, 0, titleA.baselineOffset).y
                               - titleB.mapToItem(split, 0, titleB.baselineOffset).y) <= 1,
                  2000, "The pane titles share a text baseline")
        compare(titleA.paneActive, true)
        compare(titleB.paneActive, false)
        compare(main.referenceActionTarget, false)
        main.editorTool = "select"
        main.selectionRefs = [{measure: 1, pos: {num: 0, den: 1}, sample: 1,
                              lane: {player: 0, kind: "key", index: 1}}]
        const heldA = main.selectionRefs
        const sa = mainFixture.snapshot(a), sb = mainFixture.snapshot(b)
        const note = auditionPoint(split.referenceView, true, 2)
        mouseClick(split.referenceView, note.x, note.y)
        tryCompare(split, "referenceFocused", true)
        compare(main.referenceActionTarget, true)
        compare(titleA.paneActive, false)
        compare(titleB.paneActive, true)
        compare(main.chartEditingAllowed, true)
        compare(main.referenceEditContext.sessionId, b)
        compare(main.referenceEditContext.selection.length, 1)
        compare(main.selectionRefs, heldA)
        for (const id of ["tool.addMeasure"]) {
            verify(!uiActions.enabled(id), id)
            verify(!uiActions.invoke(id, {}), id)
        }
        for (const id of ["edit.copy", "edit.paste", "edit.delete", "edit.undo", "edit.redo", "tool.quantize", "tool.mirror", "tool.rotate", "tool.toggleLn"])
            verify(uiActions.enabled(id), id)
        keyClick(Qt.Key_Delete)
        tryVerify(() => mainFixture.snapshot(b).notes.length === sb.notes.length - 1)
        compare(mainFixture.snapshot(b).undo, sb.undo + 1)
        compare(mainFixture.snapshot(a), sa)
        compare(main.selectionRefs, heldA)
        compare(documents.activeId(), a)
        verify(uiActions.invoke("edit.undo", {}))
        compare(mainFixture.snapshot(b).notes, sb.notes)
        compare(mainFixture.snapshot(a), sa)
        compare(main.referenceEditContext.selection.length, 0)
        compare(primary.documentSession, chartSession)
        compare(split.referenceView.documentSession, documents.session(b))

        const primaryNote = auditionPoint(primary, true)
        mouseClick(primary, primaryNote.x, primaryNote.y)
        tryCompare(split, "referenceFocused", false)
        compare(main.referenceActionTarget, false)
        compare(titleA.paneActive, true)
        compare(titleB.paneActive, false)
        compare(mainFixture.snapshot(a), sa)
        compare(audioEngine.previewFiles.length, 0)
    }
    function test_referencePlacement_preservesLiteralInDestinationBase_data() {
        return [{tag: "62-to36-lowercase-rejected", sourceBase: 62, targetBase: 36, text: "0a", accepted: false},
                {tag: "62-to36-uppercase", sourceBase: 62, targetBase: 36, text: "10", accepted: true, sample: 36},
                {tag: "36-to62-same-literal", sourceBase: 36, targetBase: 62, text: "10", accepted: true, sample: 62},
                {tag: "62-unbound-lowercase", sourceBase: 62, targetBase: 62, text: "0a", accepted: true, sample: 36},
                {tag: "reserved-zero", sourceBase: 36, targetBase: 36, text: "00", accepted: false}]
    }
    function test_referencePlacement_preservesLiteralInDestinationBase(data) {
        start(false)
        verify(mainFixture.setDocumentBase(a, data.sourceBase))
        verify(mainFixture.setDocumentBase(b, data.targetBase))
        split.synchronizeViewports = false
        main.editorTool = "note"
        main.currentSampleId = data.text
        const sa = mainFixture.snapshot(a), sb = mainFixture.snapshot(b)
        const view = split.referenceView
        verify(waitForRendering(view))
        const point = auditionPoint(view, false)
        mouseClick(view, point.x, point.y)
        if (data.accepted) {
            tryVerify(() => mainFixture.snapshot(b).notes.length === sb.notes.length + 1)
            compare(mainFixture.snapshot(b).undo, sb.undo + 1)
            verify(mainFixture.snapshot(b).notes.some(n => n.sample === data.sample))
            compare(mainFixture.snapshot(b).definitions, sb.definitions)
        } else compare(mainFixture.snapshot(b), sb)
        compare(mainFixture.snapshot(a), sa)
        compare(main.currentSampleId, data.text)
        compare(documents.activeId(), a)
    }
    function test_referencePlacement_sameDocumentRefreshAndUndo() {
        start(false)
        split.synchronizeViewports = false
        verify(split.selectReference(a))
        main.editorTool = "note"
        main.currentSampleId = "ZZ" // valid unbound slot must not import/create a definition
        const before = mainFixture.snapshot(a), other = mainFixture.snapshot(b)
        const view = split.referenceView
        verify(waitForRendering(view))
        const point = auditionPoint(view, false)
        mouseClick(view, point.x, point.y)
        tryVerify(() => mainFixture.snapshot(a).notes.length === before.notes.length + 1)
        compare(mainFixture.snapshot(a).undo, before.undo + 1)
        compare(mainFixture.snapshot(a).definitions, before.definitions)
        compare(view.documentSession, chartSession)
        compare(view.ownsPlayback, false)
        compare(mainFixture.bridgeState(mainFixture.primaryView(main).documentSession).notes, before.notes.length + 1)
        compare(mainFixture.bridgeState(view.documentSession).notes, before.notes.length + 1)
        compare(documents.activeId(), a)
        verify(uiActions.invoke("edit.undo", {}))
        compare(mainFixture.snapshot(a).notes, before.notes)
        compare(mainFixture.snapshot(b), other)
    }
    function test_referenceNoteModal_keepsCapturedDestination_data() {
        return [{tag: "focus-only", switchDocument: false}, {tag: "changed-B", switchDocument: true}]
    }
    function test_referenceNoteModal_keepsCapturedDestination(data) {
        start(false)
        main.editorTool = "select"
        const view = split.referenceView
        const point = auditionPoint(view, true, 2)
        const sa = mainFixture.snapshot(a), sb = mainFixture.snapshot(b)
        mouseDoubleClickSequence(view, point.x, point.y)
        const dialog = mainFixture.objectNamed(main, "noteSampleDialog")
        tryCompare(dialog, "opened", true)
        compare(dialog.displaySession, documents.session(b))
        const field = mainFixture.objectNamed(main, "noteSampleIdInput")
        field.text = "ZZ"
        if (data.switchDocument) verify(split.selectReference(a))
        else split.referenceFocused = false
        compare(dialog.displaySession, documents.session(b))
        dialog.accept()
        tryCompare(dialog, "visible", false)
        compare(mainFixture.snapshot(a), sa)
        if (data.switchDocument) compare(mainFixture.snapshot(b), sb)
        else {
            compare(mainFixture.snapshot(b).undo, sb.undo + 1)
            verify(mainFixture.snapshot(b).notes.some(n => n.sample === 1295))
        }
        compare(documents.activeId(), a)
    }
    function test_referenceEdit_preservesDraftDocksAndCtrlSSavesA() {
        start(true)
        const text = draft()
        const sa = mainFixture.snapshot(a)
        const owner = chartSession
        main.currentSampleId = "02"
        main.editorTool = "note"
        const point = auditionPoint(split.referenceView, false)
        mouseClick(split.referenceView, point.x, point.y)
        tryCompare(split, "referenceFocused", true)
        const editedB = mainFixture.snapshot(b)
        compare(mainFixture.snapshot(a), sa)
        compare(main.metaDraftDirty, true)
        compare(chartSession, owner)
        compare(documents.activeId(), a)
        compare(main.currentSampleId, "02")
        keySequence("Ctrl+S")
        tryCompare(main, "metaDraftDirty", false)
        tryVerify(() => !mainFixture.snapshot(a).dirty)
        verify(mainFixture.disk(a).indexOf("#TITLE " + text) >= 0)
        compare(mainFixture.snapshot(b), editedB)
        compare(split.referenceView.documentSession, documents.session(b))
        compare(chartSession, owner)
    }
    function test_referenceLnPendingAndCancel_areIndependentOfA() {
        start(false)
        for (const id of [a, b]) {
            const changed = JSON.parse(beatbench.dispatch(JSON.stringify({command: "meta.edit", args: {
                session_id: id, edits: [{key: "LNOBJ", value: "ZZ"}]
            }})))
            verify(changed.ok)
        }
        documents.refresh()
        split.synchronizeViewports = false
        main.editorTool = "ln"
        main.currentSampleId = "01"
        const primary = mainFixture.primaryView(main)
        const ap = auditionPoint(primary, false)
        mouseClick(primary, ap.x, ap.y)
        verify(main.pendingLnHead !== null)
        const pendingA = main.pendingLnHead
        const sa = mainFixture.snapshot(a), sb = mainFixture.snapshot(b)
        const bp = auditionPoint(split.referenceView, false)
        mouseClick(split.referenceView, bp.x, bp.y)
        verify(main.referenceEditContext.pendingLnHead.measure !== undefined)
        compare(main.pendingLnHead, pendingA)
        compare(main.primaryEditContext.pendingLnHead, pendingA)
        compare(mainFixture.snapshot(a), sa)
        compare(mainFixture.snapshot(b).notes.length, sb.notes.length + 1)
        keyClick(Qt.Key_Escape)
        compare(Object.keys(main.referenceEditContext.pendingLnHead).length, 0)
        compare(mainFixture.snapshot(b).notes, sb.notes)
        compare(main.pendingLnHead, pendingA)
        compare(mainFixture.snapshot(a), sa)
    }
    function test_sameDocumentReferenceUndo_invalidatesPrimaryPendingHead() {
        start(false)
        verify(JSON.parse(beatbench.dispatch(JSON.stringify({command: "meta.edit", args: {
            session_id: a, edits: [{key: "LNOBJ", value: "ZZ"}]
        }}))).ok)
        documents.refresh()
        verify(split.selectReference(a))
        main.editorTool = "ln"
        main.currentSampleId = "01"
        const primary = mainFixture.primaryView(main)
        const before = mainFixture.snapshot(a)
        primary.hitPlaceRequested({valid: true, measure: 2, num: 0, den: 1,
            lanePlayer: 0, laneKind: "key", laneIndex: 1})
        verify(main.pendingLnHead !== null)
        compare(main.pendingLnHead.measure, 2)
        main.selectionRefs = [main.pendingLnHead]
        split.referenceFocused = true
        verify(uiActions.invoke("edit.undo", {}))
        compare(mainFixture.snapshot(a).notes, before.notes)
        compare(main.pendingLnHead, null)
        compare(main.selectionRefs.length, 0)
        compare(main.metaSelection.length, 0)
        split.referenceFocused = false
        primary.hitPlaceRequested({valid: true, measure: 3, num: 0, den: 1,
            lanePlayer: 0, laneKind: "key", laneIndex: 1})
        verify(main.pendingLnHead !== null, "A click must create a fresh head, not an orphan LNOBJ tail")
        compare(main.pendingLnHead.measure, 3)
        const after = mainFixture.snapshot(a)
        compare(after.notes.length, before.notes.length + 1)
        verify(after.notes.some(n => n.measure === 3 && n.sample === 1))
        verify(!after.notes.some(n => n.measure === 3 && n.sample === 1295))
    }
    function test_referenceToolSwitch_doesNotDeletePrimaryPendingNote() {
        start(false)
        for (const id of [a, b]) {
            verify(JSON.parse(beatbench.dispatch(JSON.stringify({command: "meta.edit", args: {
                session_id: id, edits: [{key: "LNOBJ", value: "ZZ"}]
            }}))).ok)
        }
        documents.refresh()
        main.editorTool = "ln"
        main.currentSampleId = "01"
        mainFixture.primaryView(main).hitPlaceRequested({valid: true, measure: 2, num: 0, den: 1,
            lanePlayer: 0, laneKind: "key", laneIndex: 1})
        verify(main.pendingLnHead !== null)
        const sa = mainFixture.snapshot(a), sb = mainFixture.snapshot(b)
        split.referenceView.hitPlaceRequested({valid: true, measure: 2, num: 0, den: 1,
            lanePlayer: 0, laneKind: "key", laneIndex: 1})
        verify(main.referenceEditContext.pendingLnHead.measure !== undefined)
        split.referenceFocused = true
        verify(uiActions.invoke("tool.note", {}))
        compare(main.editorTool, "note")
        compare(mainFixture.snapshot(a), sa, "Switching B's tool must not delete A's actual head")
        compare(main.pendingLnHead, null) // inactive pane state is invalidated, not deleted
        compare(mainFixture.snapshot(b).notes, sb.notes) // only B's pending head was cancelled
        compare(Object.keys(main.referenceEditContext.pendingLnHead).length, 0)
    }
    function test_noteSampleModal_clearsStaleSelectionsInBothSameDocumentPanes_data() {
        return [{tag: "A-modal", reference: false}, {tag: "B-modal", reference: true}]
    }
    function test_noteSampleModal_clearsStaleSelectionsInBothSameDocumentPanes(data) {
        start(false)
        verify(split.selectReference(a))
        main.editorTool = "select"
        const ref = {measure: 1, pos: {num: 0, den: 1}, sample: 1,
                     lane: {player: 0, kind: "key", index: 1}, ln_channel: false, note_kind: "normal"}
        main.selectionRefs = [ref]
        main.referenceEditContext.selection = [ref]
        if (data.reference) main.editReferenceNoteSample(ref)
        else main.editNoteSample(ref)
        const dialog = mainFixture.objectNamed(main, "noteSampleDialog")
        tryCompare(dialog, "opened", true)
        mainFixture.objectNamed(main, "noteSampleIdInput").text = "02"
        dialog.accept()
        tryCompare(dialog, "visible", false)
        verify(mainFixture.snapshot(a).notes.some(n => n.measure === 1 && n.sample === 2))
        compare(main.selectionRefs.length, 0)
        compare(main.referenceEditContext.selection.length, 0)
        split.referenceFocused = true
        verify(!uiActions.invoke("edit.delete", {}), "Del must not reuse pre-setSample identity")
    }
    function test_referenceTransformsAndMove_useLocalSelection() {
        start(false)
        main.editorTool = "select"
        const sa = mainFixture.snapshot(a), sb = mainFixture.snapshot(b)
        const view = split.referenceView
        const point = auditionPoint(view, true, 2)
        mouseClick(view, point.x, point.y)
        verify(uiActions.invoke("tool.mirror", {}))
        compare(mainFixture.snapshot(b).undo, sb.undo + 1)
        compare(mainFixture.snapshot(a), sa)
        verify(uiActions.invoke("edit.undo", {}))
        compare(mainFixture.snapshot(b).notes, sb.notes)
        mouseClick(view, point.x, point.y)
        const picked = main.referenceEditContext.selection[0]
        verify(picked !== undefined)
        view.moveSelectionRequested(0.25, null, picked.lane, -1)
        compare(mainFixture.snapshot(b).undo, sb.undo + 1)
        compare(mainFixture.snapshot(a), sa)
        compare(main.referenceEditContext.selection.length, 1)
        compare(main.referenceEditContext.selection[0].sample, picked.sample)
        verify(uiActions.invoke("edit.undo", {}))
        compare(mainFixture.snapshot(b).notes, sb.notes)
    }
    function test_referenceMissingDocument_neverFallsBackToA() {
        start(false)
        const sa = mainFixture.snapshot(a)
        verify(split.selectReference(""))
        split.referenceFocused = true
        compare(split.referenceView.editable, false)
        compare(main.chartEditingAllowed, false)
        for (const id of ["edit.delete", "edit.undo", "edit.redo", "edit.copy", "edit.paste", "tool.quantize"])
            verify(!uiActions.invoke(id, {}), id)
        split.referenceView.hitPlaceRequested({valid: true, measure: 0, num: 0, den: 1,
            lanePlayer: 0, laneKind: "key", laneIndex: 1})
        main.undoEdit()
        main.pasteClipboard()
        main.uiActionDelete()
        compare(mainFixture.snapshot(a), sa)
        const temporaryId = documents.create().id
        verify(documents.activate(a).ok)
        main.adoptChart(sa.path)
        verify(split.selectReference(temporaryId))
        split.referenceFocused = true
        const closed = JSON.parse(beatbench.dispatch(JSON.stringify({command: "session.close", args: {id: temporaryId}})))
        verify(closed.ok)
        documents.refresh()
        compare(main.referenceEditContext.available, false)
        compare(split.referenceView.editable, false)
        main.undoEdit()
        compare(mainFixture.snapshot(a), sa)
    }
    function test_editPlayback_geometryStableAcrossStateChanges() {
        start(false)
        const button = mainFixture.objectNamed(main, "editPlaybackButton")
        verify(button !== null)
        const geometry = function() {
            const p = button.mapToItem(main.contentItem, 0, 0)
            return [p.x, p.y, button.width, button.height,
                    button.implicitWidth, button.implicitHeight, button.parent.height]
        }
        const stopped = geometry()
        compare(button.text, "") // No platform emoji/font fallback metrics.
        compare(button.playing, false)
        for (let i = 0; i < 4; ++i) {
            audioEngine.playing = true
            tryCompare(button, "playing", true)
            wait(0)
            compare(geometry(), stopped)
            audioEngine.playing = false
            tryCompare(button, "playing", false)
            wait(0)
            compare(geometry(), stopped)
        }
        // Only publish fake audio state: do not start playback/rendering in a layout test.
        compare(audioEngine.playbackCalls, 0)
    }
    function test_pageTabs_projectFirst_preserveLogicalIndices() {
        start(false)
        main.clearTextFocus()
        const stack = mainFixture.objectNamed(main, "mainPageStack")
        verify(stack !== null)
        const order = [3, 0, 1, 2]
        const tabs = order.map(i => mainFixture.objectNamed(main, "pageTab" + i))
        for (let i = 0; i < tabs.length; ++i) {
            verify(tabs[i] !== null)
            if (i > 0) verify(tabs[i - 1].x < tabs[i].x)
        }
        compare(main.currentPage, 0)
        compare(stack.currentIndex, 0)
        compare(tabs[1].highlighted, true)
        for (let i = 0; i < order.length; ++i) {
            mouseClick(tabs[i], tabs[i].width / 2, tabs[i].height / 2)
            tryCompare(main, "currentPage", order[i])
            compare(stack.currentIndex, order[i])
            compare(main.chartEditingAllowed, order[i] === 0)
            for (let j = 0; j < tabs.length; ++j)
                compare(tabs[j].highlighted, j === i)
            // Stack children remain Edit / Slice / Test / Project, not visual tab order.
            compare(stack.children[0].visible, order[i] === 0)
            compare(stack.children[1].visible, order[i] === 1)
            compare(stack.children[2].visible, order[i] === 2)
            compare(stack.children[3].visible, order[i] === 3)
            compare(stack.children[1].kbdPageActive, order[i] === 1)
        }
        // Programmatic logical navigation must also highlight the correct visual button.
        main.currentPage = 0
        tryCompare(tabs[1], "highlighted", true)
    }
    function assertDialogContentBounds(dialog) {
        verify(dialog !== null)
        tryCompare(dialog, "opened", true)
        const body = dialog.contentItem
        waitForRendering(body) // Width changes reflow text before the layout's next polish.
        verify(body.height + 1 >= body.implicitHeight, "Dialog body squeezed below content height")
        let textCount = 0
        function inspect(item) {
            if (!item.visible) return
            if (item.text !== undefined && item.font !== undefined && item.text !== "") {
                ++textCount
                const p = item.mapToItem(body, 0, 0)
                verify(p.x >= -1 && p.y >= -1, "Text begins outside dialog body: " + item.text)
                verify(p.x + item.width <= body.width + 1, "Text crosses body right edge: " + item.text)
                verify(p.y + item.height <= body.height + 1, "Text overlaps footer: " + item.text)
                if (item.contentHeight !== undefined)
                    verify(item.contentHeight <= item.height + 1, "Text vertically clipped: " + item.text
                           + " (content=" + item.contentHeight + ", allocated=" + item.height
                           + ", implicit=" + item.implicitHeight + ", width=" + item.width
                           + ", dialog width=" + dialog.width + ")")
                if (item.contentWidth !== undefined && item.selectByMouse === undefined)
                    verify(item.contentWidth <= item.width + 1, "Label horizontally clipped: " + item.text)
                verify(item.text.indexOf("©") < 0 && item.text.indexOf("wufe8") < 0)
            }
            const children = item.children || []
            for (let i = 0; i < children.length; ++i) inspect(children[i])
        }
        inspect(body)
        verify(textCount >= 2, "Expected actual production dialog text")
        const bottom = body.mapToItem(dialog.footer, 0, body.height)
        verify(bottom.y <= 1, "Body overlaps footer")
    }
    function test_editObjectDialog_contentBounds_data() {
        return [{tag: "note", kind: "note"}, {tag: "bpm", kind: "bpm"},
                {tag: "stop", kind: "stop"}, {tag: "bga", kind: "bga"}]
    }
    function test_editObjectDialog_contentBounds(data) {
        start(false)
        const ref = {measure: 1, pos: {num: 0, den: 1}, sample: 1,
                     lane: {player: 0, kind: "key", index: 1},
                     kind: data.kind, layer: 3, value: data.kind === "bpm" ? 180 : 96}
        if (data.kind !== "note") {
            const seeded = JSON.parse(beatbench.dispatch(JSON.stringify({
                command: data.kind === "bga" ? "bga.put" : "timing.put",
                args: {session_id: a, kind: data.kind, measure: ref.measure, pos: ref.pos,
                       layer: ref.layer, sample: ref.sample, value: ref.value}
            })))
            verify(seeded.ok)
            documents.refresh()
        }
        const sa = mainFixture.snapshot(a), sb = mainFixture.snapshot(b)
        if (data.kind === "note") {
            main.selectionRefs = [ref]
            wait(0)
            clickButton(main, "修改采样…")
        } else {
            main.metaSelection = [ref]
            wait(0)
            clickButton(main, "编辑事件…")
        }
        const dialog = mainFixture.objectNamed(main, data.kind === "note" ? "noteSampleDialog" : "metaEditDialog")
        assertDialogContentBounds(dialog)
        verify(mainFixture.screenshot()) // BB_MAIN_SCREENSHOT opt-in: production popup evidence.
        // Also exercise wrapped hint/position text without relying on a specific system font.
        dialog.width = 260
        assertDialogContentBounds(dialog)
        keyClick(Qt.Key_Escape)
        tryCompare(dialog, "visible", false)
        unchanged(sa, sb)
    }
    function auditionPoint(view, wantNote, expectedSample = 1) {
        const columns = view.probe(0, 40).columns
        for (let c = 0; c < columns.length; ++c) {
            const x = columns[c].x + columns[c].w / 2
            if (x <= view.viewportItem.rulerWidth || x >= view.width - 10) continue
            for (let y = 30; y < view.height - 25; y += 2) {
                const hit = view.viewportItem.hitTest(x, y)
                if (!hit.valid || hit.laneKind !== "key") continue
                const object = view.viewportItem.objectAt(x, y)
                if (wantNote ? object.valid && object.sample === expectedSample : !object.valid)
                    return {x: x, y: y}
            }
        }
        fail("No audition fixture point: " + wantNote)
    }
    function test_noteAudition_deselectAndBlankStopOnlyPreview_data() {
        return [{tag: "default-pan", tool: "pan"}, {tag: "select", tool: "select"}]
    }
    function test_noteAudition_deselectAndBlankStopOnlyPreview(data) {
        start(false)
        const bound = JSON.parse(beatbench.dispatch(JSON.stringify({command: "sample.setFile", args: {
            session_id: a, id: "01", file: "preview-test.wav", kind: "wav"
        }})))
        verify(bound.ok)
        documents.refresh()
        audioEngine.allowPreview = true
        audioEngine.playing = true
        audioEngine.hasPcm = true
        main.editorTool = data.tool
        const primary = mainFixture.primaryView(main)
        primary.viewportItem.scrollToStart()
        wait(0)
        const sa = mainFixture.snapshot(a), sb = mainFixture.snapshot(b)
        const note = auditionPoint(primary, true), blank = auditionPoint(primary, false)
        mouseClick(primary, note.x, note.y)
        compare(audioEngine.previewFiles.length, 1)
        compare(main.selectionRefs.length, 1)
        wait(Qt.styleHints.mouseDoubleClickInterval + 20)
        const beforeCtrl = audioEngine.previewStops
        mouseClick(primary, note.x, note.y, Qt.LeftButton, Qt.ControlModifier)
        compare(main.selectionRefs.length, 0)
        verify(audioEngine.previewStops > beforeCtrl)
        compare(audioEngine.previewFiles.length, 1) // Deselection must not re-audition.
        wait(Qt.styleHints.mouseDoubleClickInterval + 20)
        mouseClick(primary, note.x, note.y)
        compare(audioEngine.previewFiles.length, 2)
        const beforeBlank = audioEngine.previewStops
        mousePress(primary, blank.x, blank.y)
        verify(audioEngine.previewStops > beforeBlank) // Stop on press, not delayed to release.
        mouseRelease(primary, blank.x, blank.y)
        compare(main.selectionRefs.length, 0)
        // Also cancel when selection was already empty (e.g. sample-panel audition).
        const beforeEmpty = audioEngine.previewStops
        mouseClick(primary, blank.x, blank.y)
        verify(audioEngine.previewStops > beforeEmpty)
        const beforeB = audioEngine.previewStops
        const refBlank = auditionPoint(split.referenceView, false)
        mouseClick(split.referenceView, refBlank.x, refBlank.y)
        compare(audioEngine.previewStops, beforeB) // B cannot silence A audio.
        compare(audioEngine.playing, true)
        compare(audioEngine.playbackCalls, 0)
        unchanged(sa, sb)
    }
    function test_referenceNoteAudition_usesBSessionAndStopsOnlyB_data() {
        return [{tag: "pan", tool: "pan"}, {tag: "select", tool: "select"}]
    }
    function test_referenceNoteAudition_usesBSessionAndStopsOnlyB(data) {
        start(true, false, true)
        split.synchronizeViewports = false
        main.editorTool = data.tool
        const primary = mainFixture.primaryView(main), reference = split.referenceView
        verify(waitForRendering(reference))
        primary.viewportItem.scrollToStart()
        reference.viewportItem.scrollToStart()
        wait(0)
        const sa = mainFixture.snapshot(a), sb = mainFixture.snapshot(b)
        verify(sa.path.endsWith("/main-integration-a/audition.bms"))
        verify(sb.path.endsWith("/main-integration-b/audition.bms"))
        compare(primary.documentSession.wavFileOfId(2), "a-collision.wav")
        compare(primary.documentSession.wavFileOfId(3), "a-missing-in-b.wav")
        const aNote = auditionPoint(primary, true, 1)
        const bNote = auditionPoint(reference, true, 2)
        const silent = auditionPoint(reference, true, 3)
        const blank = auditionPoint(reference, false)

        audioEngine.allowPreview = true
        audioEngine.playing = true
        audioEngine.hasPcm = true
        audioEngine.positionSec = 0.75
        mouseClick(primary, aNote.x, aNote.y)
        compare(audioEngine.previewFiles, ["a-note.wav"])
        compare(audioEngine.previewFile, "a-note.wav")
        compare(audioEngine.referenceNotePreviewCalls, [])
        const ordinaryStops = audioEngine.previewStops
        const pathChanges = audioEngine.pathChanges
        compare(audioEngine.chartPath, sa.path)
        const aSelection = main.selectionRefs
        mouseClick(reference, blank.x, blank.y)
        compare(audioEngine.previewFile, "a-note.wav")
        compare(audioEngine.previewStops, ordinaryStops)

        // 真实 B 按压只选中，原地松开恰好试听一次。
        mousePress(reference, bNote.x, bNote.y)
        compare(audioEngine.referenceNotePreviewCalls, [])
        mouseRelease(reference, bNote.x, bNote.y)
        const expected = {path: sb.path, sampleId: 2, file: "b-note.wav"}
        compare(audioEngine.referenceNotePreviewCalls, [expected])
        compare(audioEngine.referenceNotePreview, expected)
        tryCompare(split, "referenceFocused", true)
        compare(main.referenceEditContext.selection.length, 1)
        compare(main.referenceEditContext.selection[0].sample, 2)

        // B 空白在按下时停止单音试听；A 的整曲播放继续。
        const referenceStops = audioEngine.referenceNotePreviewStops
        mousePress(reference, blank.x, blank.y)
        verify(audioEngine.referenceNotePreviewStops > referenceStops)
        compare(audioEngine.referenceNotePreview, null)
        compare(audioEngine.previewFile, "")
        compare(audioEngine.previewStops, ordinaryStops)
        compare(audioEngine.playing, true)
        mouseRelease(reference, blank.x, blank.y)
        compare(main.referenceEditContext.selection.length, 0)

        // 无定义的 note 仍然调用 C++ API；不能在 QML 预查后跳过，更不能查 A 的同 ID。
        mouseClick(reference, silent.x, silent.y)
        compare(audioEngine.referenceNotePreviewCalls, [expected, {path: sb.path, sampleId: 3, file: ""}])
        compare(audioEngine.referenceNotePreview, null)
        compare(main.referenceEditContext.selection.length, 1)
        compare(main.referenceEditContext.selection[0].sample, 3)
        compare(audioEngine.previewFiles, ["a-note.wav"])
        compare(audioEngine.previewFile, "")
        compare(audioEngine.previewStops, ordinaryStops)
        compare(audioEngine.chartPath, sa.path)
        compare(audioEngine.pathChanges, pathChanges)
        compare(audioEngine.playing, true)
        compare(audioEngine.hasPcm, true)
        compare(audioEngine.positionSec, 0.75)
        compare(audioEngine.playbackCalls, 0)
        compare(main.selectionRefs, aSelection)
        compare(documents.activeId(), a)
        compare(split.referenceDocumentId, b)
        unchanged(sa, sb)
    }
    function test_about_genericContentBounds() {
        start(false)
        main.debugOpenDialog = "about"
        const dialog = mainFixture.objectNamed(main, "aboutDialog")
        assertDialogContentBounds(dialog)
        verify(mainFixture.screenshot())
        compare(dialog.showCancel, false)
        dialog.close()
    }
    function test_draftSaveFailure_doesNotSwap() {
        start(true)
        draft()
        const sb = mainFixture.snapshot(b)
        mainFixture.failSave()
        swap(); confirmButton("保存")
        tryCompare(main, "pendingDocAction", "")
        compare(documents.activeId(), a)
        compare(split.referenceDocumentId, b)
        verify(main.statusText.indexOf("保存失败") >= 0)
        compare(mainFixture.snapshot(a).dirty, true)
        compare(main.metaDraftDirty, true)
        compare(mainFixture.snapshot(b), sb)
    }
    function test_mainMenuCursors_preserveEnabledAndDisabledSemantics() {
        start(false)
        const bar = main.menuBar
        verify(bar !== null)
        const title = bar.itemAt(0)
        verify(title !== null)
        const titleCursor = mainFixture.objectNamed(title, "semanticCursor")
        verify(titleCursor !== null)
        compare(titleCursor.cursorShape, Qt.PointingHandCursor)
        // The fixture registers edit actions only; the production View menu has static items.
        const menu = bar.menuAt(2)
        verify(menu.count > 0)
        let enabledCount = 0
        for (let i = 0; i < menu.count; ++i) {
            const item = menu.itemAt(i)
            if (!item) continue
            const cursor = mainFixture.objectNamed(item, "semanticCursor")
            verify(cursor !== null)
            compare(cursor.cursorShape, item.enabled ? Qt.PointingHandCursor : Qt.ArrowCursor)
            if (item.enabled) enabledCount++
        }
        verify(enabledCount > 0)
        const action = menu.itemAt(0)
        const cursor = mainFixture.objectNamed(action, "semanticCursor")
        action.enabled = false
        compare(cursor.cursorShape, Qt.ArrowCursor)
        action.enabled = true
        compare(cursor.cursorShape, Qt.PointingHandCursor)
    }

}
