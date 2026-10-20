// SPDX-License-Identifier: GPL-3.0-only
// 函数级回归，不代替 GUI/鼠标集成测试。运行：node --test tests/qml_session_regression_test.js
// 抽取产品 QML 函数执行，不加载 Qt Quick 或启动 GUI 进程。
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');
const { test } = require('node:test');

const root = path.resolve(__dirname, '..');
const controller = fs.readFileSync(path.join(root, 'app/qml/SessionController.qml'), 'utf8');
const main = fs.readFileSync(path.join(root, 'app/qml/Main.qml'), 'utf8');
const view = fs.readFileSync(path.join(root, 'app/bridge/ChartViewItem.cpp'), 'utf8');
function extractFunction(name, source = controller) {
    const match = source.match(new RegExp(`^    function ${name}\\([^]*?^    }`, 'm'));
    assert.ok(match, `production function ${name} exists`);
    return match[0];
}
const pmsExpression = main.match(/readonly property bool isPmsChart: (.+)/)?.[1];
const suffixExpression = main.match(/defaultSuffix: (.+)/)?.[1];
assert.ok(pmsExpression && suffixExpression, 'production PMS/suffix bindings exist');
function isPms(chartMode) { return vm.runInNewContext(pmsExpression, { chartMode }); }

function makeContext(refs = [], mode = 'pms9k') {
    const calls = [];
    const statuses = [];
    const saved = [];
    const states = { A: { path: 'existing.bme', mode }, B: { path: 'reference.pms', mode: 'pms9k' } };
    const documents = {
        activeId: () => 'A', refresh: () => {},
        save: (id, output) => {
            saved.push({ id, path: output });
            states[id].path = output;
            return { ok: true, id, path: output };
        }
    };
    const context = vm.createContext({
        documents, project: { refreshOpenStates: () => {} },
        refreshLint: () => {},
        beatbench: { dispatch: request => {
            const { command: name, args } = JSON.parse(request);
            calls.push({ name, args });
            return JSON.stringify({ ok: true, result: name === 'session.save'
                ? { output: states[args.session_id].path, bytes: 123 }
                : { moved: args.moves?.length || 0 } });
        } },
        window: { selectionRefs: structuredClone(refs), snapNum: 1, snapDen: 4,
                  chartMode: mode, isPmsChart: isPms(mode) },
        root: { editPage: null },
        chartSession: { sessionId: 'A', playKeyCount: () => isPms(mode) ? 9 : 7 },
        qsTr: text => text,
        setStatus: text => statuses.push(text),
        // 只替代现有有理数桥，不替代本次要测的吸附算法。
        // 整数归一化契约另由 edit_utils_test.cpp 验证。
        editUtils: { addPosDelta: (ref, delta) => {
            const den = ref.pos.den * delta.pos.den;
            const num = ref.pos.num * delta.pos.den + delta.pos.num * ref.pos.den;
            const carry = Math.floor(num / den);
            const rem = num - carry * den;
            const gcd = (a, b) => b ? gcd(b, a % b) : a;
            const g = gcd(Math.abs(rem), den);
            return { measure: ref.measure + delta.measure + carry,
                     pos: { num: rem / g, den: den / g } };
        } }
    });
    vm.runInContext('String.prototype.arg = function(value) { return this.replace(/%[1-9]/, String(value)); };', context);
    for (const name of ['sessionCmd', 'moveSelection', 'addPosDelta', 'laneEquals', 'pmsSuffixWarning', 'saveChart', 'saveChartAs', 'placeNote', 'placeLnObj', 'cancelPendingLn', 'deleteNoteAt', 'quantizeSelection', 'gcd']) {
        vm.runInContext(extractFunction(name), context);
    }
    return { context, calls, statuses, saved, states };
}
function ref(num, den, measure = 0, kind = 'bgm', index = 0) {
    return { measure, pos: { num, den }, lane: { player: 0, kind, index }, sample: 1, sub_line: 0 };
}
test('explicit note identity survives drag and delete dispatch', () => {
    const ordinary = { ...ref(1, 4, 0, 'key', 1), ln_channel: false, note_kind: 'normal' };
    const ln = { ...ordinary, ln_channel: true };
    const mine = { ...ordinary, note_kind: 'mine' };
    const { context, calls } = makeContext([ordinary, ln, mine]);
    context.moveSelection(0.25, null, ordinary.lane, -1);
    assert.deepEqual(calls[0].args.moves.map(m => [m.from.ln_channel, m.from.note_kind]),
        [[false, 'normal'], [true, 'normal'], [false, 'mine']]);
    assert.deepEqual(plain(context.window.selectionRefs).map(r => [r.ln_channel, r.note_kind]),
        [[false, 'normal'], [true, 'normal'], [false, 'mine']]);
    context.deleteNoteAt(ln);
    assert.equal(calls[1].args.ln_channel, true);
    assert.equal(calls[1].args.note_kind, 'normal');
});

for (const useLnobj of [false, true]) {
    test('LN placement uses explicit bridge policy ' + useLnobj, () => {
        const { context, calls } = makeContext();
        Object.assign(context.window, { editorTool: 'ln', currentSampleId: '01', pendingLnHead: null });
        Object.assign(context.chartSession, { useLnObjPlacement: () => useLnobj,
            lnType: () => 2, lnobjSample: () => 1295, sampleValueOf: () => 1 });
        const hit = { measure: 0, num: 1, den: 4, lanePlayer: 0, laneKind: 'key', laneIndex: 1 };
        context.placeNote({ ...hit, laneKind: 'bgm' });
        assert.equal(calls.length, 0, 'BGM rejects either LN mechanism');
        context.placeNote(hit);
        assert.equal(calls[0].args.kind, useLnobj ? 'normal' : 'ln');
        assert.equal(Boolean(context.window.pendingLnHead), useLnobj);
        if (useLnobj) {
            assert.equal(context.window.pendingLnHead.ln_channel, false);
            assert.equal(context.window.pendingLnHead.note_kind, 'normal');
            context.placeNote({ ...hit, num: 3 });
            assert.equal(calls[1].args.sample, 1295);
            assert.equal(calls[1].args.kind, 'normal');
            assert.equal(context.window.pendingLnHead, null);
        }
    });
}

function plain(value) { return JSON.parse(JSON.stringify(value)); }
function mf(value) { return value.measure + value.pos.num / value.pos.den; }

// 按 C++ 数值语义求产品 ghost/snap 标量表达式的结果。
// 只验证当前预览公式，不验证渲染、命中测试或 Qt 事件链。
const ghostExpression = view.match(/const qreal mf = (qFuzzyIsNull\(m_moveDeltaF\)[^;]+);/)?.[1];
const snapBody = view.match(/qreal ChartViewItem::snapMf\(qreal mf\) const \{([^]*?)\n}/)?.[1];
assert.ok(ghostExpression && snapBody, 'production ghost/snap expressions exist');
const slotsExpression = snapBody.match(/const int nslots = (.+);/)[1]
    .replace('std::max', 'Math.max').replace('m_snapDen / m_snapNum', 'Math.trunc(m_snapDen / m_snapNum)');
const snapExpression = snapBody.match(/return (.+);/)[1].replace('std::round', 'cppRound');
function ghostPosition(value, deltaF, snapNum = 1, snapDen = 4) {
    const env = {
        tBase: mf(value), m_moveDeltaF: deltaF, m_snapNum: snapNum, m_snapDen: snapDen,
        qFuzzyIsNull: value => Math.abs(value) <= 1e-12,
        cppRound: value => value < 0 ? -Math.round(-value) : Math.round(value)
    };
    env.nslots = vm.runInNewContext(slotsExpression, env);
    env.snapMf = value => vm.runInNewContext(snapExpression, { ...env, mf: value });
    return vm.runInNewContext(ghostExpression, env);
}

for (const mode of ['pms9k', 'pms9k-bmesp']) {
    for (const key of [8, 9]) {
        test(`${mode}: pure horizontal off-grid BGM -> key${key} preserves all selected times`, () => {
            const refs = [ref(1, 8), ref(1, 3, 2), ref(7, 16, 3)];
            const { context, calls } = makeContext(refs, mode);
            context.moveSelection(0, { valid: true, laneKind: 'key', lanePlayer: 0, laneIndex: key },
                                  refs[0].lane, 0);
            assert.equal(calls.length, 1);
            assert.equal(calls[0].name, 'note.move');
            assert.equal(calls[0].args.session_id, 'A', 'drag dispatch must target fixed A document');
            const moves = plain(calls[0].args.moves);
            moves.forEach((move, i) => {
                assert.deepEqual(move.to.pos, refs[i].pos);
                assert.equal(move.to.measure, refs[i].measure);
                assert.equal(move.to.lane.index, key);
                assert.equal(mf(move.to), ghostPosition(refs[i], 0));
                assert.deepEqual(plain(context.window.selectionRefs[i].pos), move.to.pos);
            });
            assert.equal(mf(moves[2].to) - mf(moves[0].to), mf(refs[2]) - mf(refs[0]));
        });
    }
}

test('pure horizontal playable-note move preserves mixed-lane multi-selection timing', () => {
    const refs = [ref(1, 8, 0, 'key', 2), ref(1, 3, 1, 'key', 4)];
    const { context, calls } = makeContext(refs);
    context.moveSelection(0, { valid: true, laneKind: 'key', lanePlayer: 0, laneIndex: 8 },
                          refs[0].lane, 0);
    const moves = plain(calls[0].args.moves);
    moves.forEach((move, i) => {
        assert.equal(mf(move.to), ghostPosition(refs[i], 0));
        assert.deepEqual(move.to.pos, refs[i].pos);
    });
    assert.equal(moves[0].to.lane.index, 8);
    assert.equal(moves[1].to.lane, undefined); // 无关轨道不能被压到拖起轨
    assert.deepEqual(plain(context.window.selectionRefs[1].lane), refs[1].lane);
});

test('near-zero horizontal delta uses the same threshold as ghost', () => {
    for (const delta of [-1e-13, 1e-13]) {
        const source = ref(1, 8);
        const { context, calls } = makeContext([source]);
        context.moveSelection(delta, null, source.lane, 0);
        assert.deepEqual(plain(calls[0].args.moves[0].to), { measure: 0, pos: source.pos });
        assert.equal(mf(calls[0].args.moves[0].to), ghostPosition(source, delta));
    }
});

for (const [name, source, delta, expected] of [
    ['off-grid upward', ref(1, 8), 1 / 8, { measure: 0, pos: { num: 1, den: 4 } }],
    ['off-grid downward', ref(3, 8), -1 / 8, { measure: 0, pos: { num: 1, den: 4 } }],
    ['cross measure forward', ref(7, 8, 2), 3 / 8, { measure: 3, pos: { num: 1, den: 4 } }],
    ['cross measure backward', ref(1, 8, 2), -3 / 8, { measure: 1, pos: { num: 3, den: 4 } }],
    ['non-grid denominator', ref(1, 3), 1 / 6, { measure: 0, pos: { num: 1, den: 2 } }],
    // core 可能拒绝负拍位，但派发目标仍须与 ghost 一致。
    ['negative half-slot rounding', ref(1, 8), -1 / 4, { measure: -1, pos: { num: 3, den: 4 } }]
]) {
    test(`vertical absolute snap: ${name}, no second rounding of delta`, () => {
        const { context, calls } = makeContext([source]);
        context.moveSelection(delta, null, source.lane, 0);
        assert.deepEqual(plain(calls[0].args.moves[0].to), expected);
        assert.equal(mf(expected), ghostPosition(source, delta));
        assert.deepEqual(plain(context.window.selectionRefs[0].pos), expected.pos);
    });
}

test('multi-selection vertical grid-spaced offsets and ghost/commit agree', () => {
    const refs = [ref(1, 8), ref(3, 8), ref(1, 8, 2)];
    const { context, calls } = makeContext(refs);
    context.moveSelection(1 / 8, null, refs[0].lane, 0);
    const moves = calls[0].args.moves;
    moves.forEach((move, i) => assert.equal(mf(move.to), ghostPosition(refs[i], 1 / 8)));
    assert.equal(mf(moves[1].to) - mf(moves[0].to), 1 / 4);
    assert.equal(mf(moves[2].to) - mf(moves[0].to), 2);
});

test('non-unit snap numerator uses the same slot count as ghost', () => {
    const source = ref(1, 8);
    const { context, calls } = makeContext([source]);
    context.window.snapNum = 3;
    context.window.snapDen = 8;
    context.moveSelection(1 / 4, null, source.lane, 0);
    assert.deepEqual(plain(calls[0].args.moves[0].to), { measure: 0, pos: { num: 1, den: 2 } });
    assert.equal(mf(calls[0].args.moves[0].to), ghostPosition(source, 1 / 4, 3, 8));
});

for (const mode of ['pms9k', 'pms9k-bmesp', 'sp7k', 'dp', 'battle', '']) {
    test(`${mode || 'unset'}: default suffix and every non-pms target warning`, () => {
        const pms = ['pms9k', 'pms9k-bmesp'].includes(mode);
        const { context, calls, statuses, saved, states } = makeContext([], mode);
        const beforeB = structuredClone(states.B);
        assert.equal(vm.runInNewContext(suffixExpression, { window: context.window }), pms ? 'pms' : 'bms');
        for (const output of ['song.bms', 'song.bme', 'song.bml', 'song.txt', 'song', 'song.PMS.bms',
                              'song.pms', 'song.PMS', 'C:/folder.pms/song.bme']) {
            assert.equal(context.saveChartAs(output), true);
            assert.deepEqual(saved.at(-1), { id: 'A', path: output });
            assert.equal(calls.length, 0, 'Save As must use document service, not legacy session.save');
            assert.equal(context.window.chartPath, output);
            assert.deepEqual(states.B, beforeB);
            const warn = pms && !output.toLowerCase().endsWith('.pms');
            assert.equal(statuses.at(-1).includes('无法自动恢复 PMS 9key'), warn, output);
            assert.equal(statuses.at(-1).includes('按内容推断'), false);
        }
    });
}

for (const mode of ['pms9k', 'pms9k-bmesp']) {
    test(`${mode}: ordinary save targets fixed A and warns without touching B`, () => {
        const { context, calls, statuses, states } = makeContext([], mode);
        const beforeB = structuredClone(states.B);
        context.window.chartPath = 'existing.bme';
        assert.equal(context.saveChart(), true);
        assert.equal(calls.at(-1).args.session_id, 'A');
        assert.ok(statuses.at(-1).includes('无法自动恢复 PMS 9key'));
        assert.deepEqual(states.B, beforeB);
    });
}

// 非 GUI 状态机测试：抽取 Main 真实函数，替身只提供文档/面板边界。
function makeMainContext() {
    const events = [];
    const context = vm.createContext({
        window: { pendingDocAction: '', pendingOpenPath: '', pendingOriginalDocument: '',
                  pendingNewMode: '', metaDraftDirty: false,
                  pmsSaveConfirmationPending: false, saveAsDocumentId: '' },
        documents: { activeId: () => 'old-A', create: mode => {
            events.push(['create', mode]); return { ok: true, id: 'new-A' };
        } },
        unsavedConfirmDialog: { visible: false, open: () => events.push(['confirm']) },
        newChartDialog: { open: () => events.push(['mode-dialog']) },
        sampleModel: { clear: () => events.push(['clear-samples']) },
        editPage: {
            activatePrimary: () => events.push(['bind-primary']),
            reloadMeta: () => events.push(['reload-meta']), reloadBga: () => {},
            setEditableMeasures: count => events.push(['measures', count])
        },
        audioEngine: { setChartPath: value => events.push(['audio-path', value]) },
        refreshTiming: () => {}, statusClearTimer: { restart: () => {} },
        setStatus: text => events.push(['status', text]), qsTr: text => text,
        restoreDocument: id => events.push(['restore', id])
    });
    vm.runInContext('String.prototype.arg = function(value) { return this.replace(/%[1-9]/, String(value)); };', context);
    for (const name of ['newChart', 'newChartAs', 'requestDocumentAction', 'finishPendingDocumentAction',
                        'cancelPendingDocumentAction', 'performNewChart'])
        vm.runInContext(extractFunction(name, main), context);
    return { context, events };
}

test('Ctrl+N opens mode dialog, direct mode waits for draft confirmation and cancel clears it', () => {
    const { context, events } = makeMainContext();
    context.newChart();
    assert.deepEqual(events, [['mode-dialog']]);
    events.length = 0;
    context.window.metaDraftDirty = true;
    context.newChartAs('dp');
    assert.deepEqual(events, [['confirm']]);
    assert.equal(context.window.pendingNewMode, 'dp');
    context.cancelPendingDocumentAction();
    assert.equal(context.window.pendingNewMode, '');
    assert.equal(context.window.pendingDocAction, '');
    events.length = 0;
    context.newChart();
    context.finishPendingDocumentAction(); // discard draft, but preserve existing M7 document
    assert.deepEqual(events, [['confirm'], ['mode-dialog']]);
});

for (const mode of ['sp7k', 'dp', 'battle', 'pms9k']) {
    test(`${mode}: creation uses independent DocumentService and resets primary draft/bindings, not PLAYER`, () => {
        const { context, events } = makeMainContext();
        context.newChartAs(mode);
        assert.deepEqual(events[0], ['create', mode]);
        assert.equal(events.filter(event => event[0] === 'create').length, 1);
        assert.ok(events.find(event => event[0] === 'bind-primary'));
        assert.ok(events.find(event => event[0] === 'reload-meta'));
        assert.ok(events.find(event => event[0] === 'clear-samples'));
        assert.equal(context.window.chartMode, mode);
        assert.equal(context.window.chartPath, '');
        assert.equal(context.window.metaDraftDirty, false);
        assert.equal(Object.hasOwn(context.window.chartMeta, 'PLAYER'), false);
        assert.equal(context.window.pendingNewMode, '');
    });
}

test('failed new document leaves current chart/draft/selection untouched', () => {
    const { context, events } = makeMainContext();
    Object.assign(context.window, { chartMode: 'battle', chartPath: 'old.bms', metaDraftDirty: true,
                                    chartMeta: { TITLE: 'unsaved' }, selectionRefs: [ref(1, 8)] });
    const before = plain(context.window);
    context.documents.create = () => ({ ok: false, error: 'invalid_mode' });
    context.performNewChart('invalid');
    assert.deepEqual(plain(context.window), before);
    assert.equal(events.some(event => event[0] === 'bind-primary'), false);
});
