// SPDX-License-Identifier: GPL-3.0-only
// Function-level regression of production Main.qml + SessionController.qml; no Qt/GUI.
// Run: node --test tests/qml_pms_save_guard_test.js
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');
const { test } = require('node:test');
const root = path.resolve(__dirname, '..');
const main = fs.readFileSync(path.join(root, 'app/qml/Main.qml'), 'utf8');
const controller = fs.readFileSync(path.join(root, 'app/qml/SessionController.qml'), 'utf8');
function extract(source, name) {
    const match = source.match(new RegExp(`^    function ${name}\\([^]*?^    }`, 'm'));
    assert.ok(match, `production function ${name}`);
    return match[0];
}
function setup(mode = 'pms9k', chartPath = 'song.bms') {
    const calls = [], later = [], statuses = [];
    const c = vm.createContext({
        chartMode: mode, chartPath, metaDraftDirty: true, pendingDocAction: '', pendingOpenPath: '',
        pendingNewMode: '', allowClose: false, saveAsDocumentId: '', pendingPmsSaveDocumentId: '',
        pendingPmsSavePath: '', pendingPmsSaveAs: false, pmsSaveRequestToken: 0,
        chartSession: { id: 'attached-A', hasChart: true, dirty: true,
            get sessionId() { return this.id; }, chartMode: () => mode },
        chartHistoryController: { busy: false, enabled: false, finishSave: () => {} },
        project: { refreshOpenStates: () => {} },
        pendingOriginalDocument: '', pendingSelectedSaves: [], pendingSelectedSaveIndex: 0,
        documents: {
            activeId: () => 'attached-A', refresh: () => {},
            dirtyDocuments: () => c.chartSession.dirty ? [{ id: 'attached-A' }] : [],
            saveDirtyNamed: () => ({ ok: true, errors: [], confirmation: [], unnamed: [] }),
            save: (id, output) => {
                calls.push('session.save');
                if (c.failSave) return { ok: false, error: 'write failed' };
                c.chartSession.dirty = false;
                return { ok: true, id, path: output };
            }
        },
        Qt: { callLater: fn => later.push(fn) }, qsTr: text => text,
        setStatus: text => statuses.push(text),
        editUtils: { // Qt QFileInfo policy itself is covered by edit_utils_test.cpp.
            pmsSuffixNeedsConfirmation: (m, p) => ['pms9k', 'pms9k-bmesp'].includes(m) && !!p && !/\.pms$/i.test(p)
        },
        pmsSuffixConfirmDialog: { open: () => calls.push('modal') },
        saveAsDialog: { open: () => calls.push('fileDialog') },
        unsavedConfirmDialog: { visible: false, open: () => calls.push('unsaved') },
        newChartDialog: { open: () => calls.push('newDialog') },
        fileDialog: { open: () => calls.push('openDialog') },
        performNewChart: m => calls.push(`new:${m}`), doOpenChart: p => calls.push(`open:${p}`),
        close: () => calls.push('close'), refreshLint: () => {},
        editPage: {
            collectMetaEdits: () => { calls.push('collect'); return [{ key: 'TITLE', value: 'draft' }]; },
            applyRawEdits: () => calls.push('raw'),
            commitMeta: () => { calls.push('commit'); c.metaDraftDirty = false; }
        },
        sessionCmd: (name, args) => {
            calls.push(name);
            if (name === 'session.save') {
                if (c.failSave) return null;
                c.chartSession.dirty = false;
                return { output: args.path || c.chartPath, bytes: 1 };
            }
            return {};
        }
    });
    c.window = c;
    Object.defineProperty(c, 'pmsSaveConfirmationPending', { get: () => c.pendingPmsSaveDocumentId !== '' });
    Object.defineProperty(c, 'isPmsChart', { get: () => ['pms9k', 'pms9k-bmesp'].includes(c.chartMode) });
    vm.runInContext('String.prototype.arg = function(v) { return this.replace(/%[1-9]/, String(v)); };', c);
    for (const name of ['pmsSuffixWarning', 'saveChart', 'saveChartAs']) {
        vm.runInContext(extract(controller, name).replace(`function ${name}(`, `function controller_${name}(`), c);
    }
    // The real controller save functions call the unprefixed warning helper.
    c.pmsSuffixWarning = c.controller_pmsSuffixWarning;
    c.session = { saveChart: () => c.controller_saveChart(), saveChartAs: p => c.controller_saveChartAs(p) };
    for (const name of ['openSaveAsDialog', 'handleSaveAsAccepted', 'completeSaveAttempt',
        'requestPmsSuffixConfirmation', 'clearPmsSuffixConfirmation', 'cancelPmsSuffixSave',
        'confirmPmsSuffixSave', 'documentIsDirty', 'anyDocumentIsDirty', 'restoreDocument',
        'requestDocumentAction', 'finishPendingDocumentAction',
        'cancelPendingDocumentAction', 'saveThenContinue', 'saveChart', 'saveChartAs', 'newChart', 'newChartAs', 'uiActionSaveAs']) {
        vm.runInContext(extract(main, name), c);
    }
    const closing = main.match(/onClosing: function\(close\) \{([^]*?)^    }/m);
    assert.ok(closing);
    vm.runInContext(`function onClosing(close) {${closing[1]}}`, c);
    return { c, calls, statuses, flush: () => { while (later.length) later.shift()(); } };
}
function assertUntouched(c, calls) {
    assert.equal(c.metaDraftDirty, true);
    for (const action of ['collect', 'meta.edit', 'raw', 'session.save', 'commit', 'close'])
        assert.ok(!calls.includes(action), `${action} must wait for confirmation`);
}
for (const mode of ['pms9k', 'pms9k-bmesp']) {
    test(`${mode}: named save waits, cancels without edits, stale callLater cannot reopen`, () => {
        const { c, calls, flush } = setup(mode);
        assert.equal(c.saveChart(), undefined);
        assert.equal(c.pendingPmsSavePath, 'song.bms');
        assert.equal(c.pendingPmsSaveAs, false);
        assert.deepEqual(calls, []);
        c.cancelPmsSuffixSave(); flush();
        assertUntouched(c, calls);
        assert.equal(c.pmsSaveConfirmationPending, false);
    });
    test(`${mode}: native accepted defers modal, locks reentry, confirms exactly once`, () => {
        const { c, calls, flush } = setup(mode, '');
        c.pendingDocAction = 'new'; c.pendingNewMode = 'dp';
        c.saveThenContinue();
        assert.deepEqual(calls, ['fileDialog']);
        assert.equal(c.saveAsDocumentId, 'attached-A');
        assert.equal(c.handleSaveAsAccepted('chosen.bms'), undefined);
        assert.equal(c.pendingPmsSaveAs, true);
        assert.equal(c.pendingDocAction, 'new');
        assertUntouched(c, calls);
        c.saveChart(); c.saveChartAs('other.bms'); c.uiActionSaveAs(); c.newChartAs('battle');
        c.requestDocumentAction('close');
        assert.equal(c.pendingNewMode, 'dp');
        assert.equal(c.pendingPmsSavePath, 'chosen.bms');
        const close = { accepted: true }; c.onClosing(close); assert.equal(close.accepted, false);
        flush(); assert.deepEqual(calls, ['fileDialog', 'modal']);
        assert.equal(c.confirmPmsSuffixSave(), true);
        assert.equal(c.chartPath, 'chosen.bms');
        assert.equal(c.metaDraftDirty, false);
        assert.equal(calls.filter(x => x === 'session.save').length, 1);
        assert.ok(calls.indexOf('meta.edit') < calls.indexOf('session.save'));
        assert.ok(calls.indexOf('raw') < calls.indexOf('session.save'));
        assert.ok(calls.includes('new:dp'));
        assert.equal(c.confirmPmsSuffixSave(), false);
        assert.equal(c.handleSaveAsAccepted('duplicate.bms'), false);
        assert.equal(calls.filter(x => x === 'session.save').length, 1);
    });
}
for (const action of ['close', 'new', 'open']) {
    test(`${action}: cancel keeps drafts and blocks pending action; success releases only after save`, () => {
        const { c, calls, flush } = setup();
        c.requestDocumentAction(action, 'next.bms'); c.saveThenContinue(); flush();
        assert.equal(c.pendingDocAction, action); assertUntouched(c, calls);
        c.cancelPmsSuffixSave();
        assert.equal(c.pendingDocAction, ''); assert.equal(c.allowClose, false); assertUntouched(c, calls);
        c.requestDocumentAction(action, 'next.bms'); c.saveThenContinue();
        assert.equal(c.confirmPmsSuffixSave(), true);
        assert.equal(c.pendingDocAction, '');
        const continuation = { close: 'close', new: 'newDialog', open: 'open:next.bms' }[action];
        assert.ok(calls.indexOf(continuation) > calls.indexOf('session.save'));
    });
}
for (const change of ['session', 'path', 'detached']) {
    test(`confirmation rejects changed ${change}`, () => {
        const { c, calls } = setup(); c.pendingDocAction = 'close'; c.saveChart();
        if (change === 'session') c.chartSession.id = 'attached-B';
        if (change === 'path') c.chartPath = 'other.bms';
        if (change === 'detached') c.chartSession.hasChart = false;
        assert.equal(c.confirmPmsSuffixSave(), false); assertUntouched(c, calls);
        assert.equal(c.pendingDocAction, ''); assert.equal(c.allowClose, false);
    });
}
test('native accepted rejects changed session and consumes its identity', () => {
    const { c, calls } = setup(); c.openSaveAsDialog(); c.pendingDocAction = 'close';
    c.chartSession.id = 'attached-B';
    assert.equal(c.handleSaveAsAccepted('chosen.bms'), false);
    assert.equal(c.saveAsDocumentId, ''); assert.equal(c.pendingDocAction, ''); assertUntouched(c, calls);
});
test('native rejection clears lock, preserves drafts, and cancels pending close', () => {
    const { c, calls } = setup('pms9k', '');
    c.pendingDocAction = 'close'; c.saveThenContinue();
    const rejected = main.match(/onRejected: \{\s*chartHistoryController\.finishSave\(window\.saveAsDocumentId, false\)\s*window\.saveAsDocumentId = ""([^]*?)^        }/m);
    assert.ok(rejected);
    vm.runInContext(`window.saveAsDocumentId = ""; ${rejected[1]}`, c);
    assert.equal(c.saveAsDocumentId, ''); assert.equal(c.pendingDocAction, '');
    assert.equal(c.allowClose, false); assertUntouched(c, calls);
});
test('safe native accepted completes pending close without a warning', () => {
    const { c, calls, flush } = setup('pms9k', '');
    c.pendingDocAction = 'close'; c.saveThenContinue();
    assert.equal(c.handleSaveAsAccepted('chosen.PMS'), true); flush();
    assert.equal(c.pendingDocAction, ''); assert.ok(!calls.includes('modal'));
    assert.ok(calls.indexOf('close') > calls.indexOf('session.save'));
});
test('closing stays blocked during native file dialog even when document is clean', () => {
    const { c } = setup(); c.metaDraftDirty = false; c.chartSession.dirty = false;
    c.openSaveAsDialog(); const close = { accepted: true }; c.onClosing(close);
    assert.equal(close.accepted, false);
});
test('confirmed write failure preserves draft and cancels pending close', () => {
    const { c, calls } = setup(); c.failSave = true; c.pendingDocAction = 'close'; c.saveThenContinue();
    assert.equal(c.confirmPmsSuffixSave(), false);
    assert.equal(c.metaDraftDirty, true); assert.equal(c.pendingDocAction, ''); assert.equal(c.allowClose, false);
    assert.ok(calls.includes('session.save')); assert.ok(!calls.includes('commit')); assert.ok(!calls.includes('close'));
});
for (const [mode, target] of [['sp7k', 'song.bms'], ['dp', 'song.bme'], ['pms9k', 'song.PMS'], ['pms9k-bmesp', 'song.pms']]) {
    test(`${mode}/${target}: safe save bypasses modal`, () => {
        const { c, calls, flush } = setup(mode, target);
        assert.equal(c.saveChart(), true); flush(); assert.ok(!calls.includes('modal'));
        assert.equal(calls.filter(x => x === 'session.save').length, 1);
    });
}
test('production dialog wiring and public-only interface stay in place', () => {
    assert.match(main, /onAccepted: window\.handleSaveAsAccepted\(urlToPath\(selectedFile\)\)/);
    assert.match(main, /onRejected: \{\s*chartHistoryController\.finishSave\(window\.saveAsDocumentId, false\)\s*window\.saveAsDocumentId = ""/);
    assert.match(main, /onAccepted: window\.confirmPmsSuffixSave\(\)/);
    assert.match(main, /onRejected: window\.cancelPmsSuffixSave\(\)/);
    assert.equal((main.match(/saveAsDialog\.open\(\)/g) || []).length, 1);
    assert.match(extract(main, 'confirmPmsSuffixSave'), /id !== documents\.activeId\(\) \|\| id !== chartSession\.sessionId/);
    assert.match(extract(main, 'saveChart'), /chartSession\.chartMode\(\)/);
    const header = fs.readFileSync(path.join(root, 'app/bridge/ChartSession.hpp'), 'utf8');
    assert.match(header, /Q_INVOKABLE QString activeSessionId\(\) const/);
});
