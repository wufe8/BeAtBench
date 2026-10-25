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
function extractFunction(name) {
    const match = controller.match(new RegExp(`^    function ${name}\\([^]*?^    }`, 'm'));
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
    const context = vm.createContext({
        window: { selectionRefs: structuredClone(refs), snapNum: 1, snapDen: 4,
                  chartMode: mode, isPmsChart: isPms(mode), verticalMoveMode: 2,
                  // SessionController 读 Main 的派生别名（readonly：mode === 0）
                  get verticalMoveLocked() { return this.verticalMoveMode === 0; } },
        root: { editPage: null },
        chartSession: { playKeyCount: () => isPms(mode) ? 9 : 7 },
        qsTr: text => text,
        setStatus: text => statuses.push(text),
        sessionCmd: (name, args) => {
            calls.push({ name, args });
            return name === 'session.save' ? { output: args.path || 'existing.bme', bytes: 123 }
                                          : { moved: args.moves?.length || 0 };
        },
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
    for (const name of ['moveSelection', 'addPosDelta', 'laneEquals', 'pmsSuffixWarning', 'saveChart', 'saveChartAs']) {
        vm.runInContext(extractFunction(name), context);
    }
    return { context, calls, statuses };
}
function ref(num, den, measure = 0, kind = 'bgm', index = 0) {
    return { measure, pos: { num, den }, lane: { player: 0, kind, index }, sample: 1, sub_line: 0 };
}
function plain(value) { return JSON.parse(JSON.stringify(value)); }
function mf(value) { return value.measure + value.pos.num / value.pos.den; }

// 按 C++ 数值语义求产品 ghost/snap 标量表达式的结果。
// 只验证当前预览公式，不验证渲染、命中测试或 Qt 事件链。
const ghostExpression = view.match(/const qreal mf = (qFuzzyIsNull\(m_moveDeltaF\)[^;]+);/)?.[1];
const snapBody = view.match(/qreal ChartViewItem::snapMf\(qreal mf\) const \{([^]*?)\n}/)?.[1];
assert.ok(ghostExpression && snapBody, 'production ghost/snap expressions exist');
// 同样压平空白后再抽取：格式化会把 const int nslots = / return 拆行（2026-10 实测）。
const snapFlat = snapBody.replace(/\s+/g, ' ');
const slotsExpression = snapFlat.match(/const int nslots = (.+?);/)[1]
    .replace('std::max', 'Math.max').replace('m_snapDen / m_snapNum', 'Math.trunc(m_snapDen / m_snapNum)');
const snapExpression = snapFlat.match(/return (.+?);/)[1].replace('std::round', 'cppRound');
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

test('vertical lock zeroes time delta but keeps the target lane', () => {
    const source = ref(1, 8);
    const { context, calls } = makeContext([source]);
    context.window.verticalMoveMode = 0;
    context.moveSelection(1 / 8,
                          { valid: true, laneKind: 'key', lanePlayer: 0, laneIndex: 3 },
                          source.lane, 0);
    const move = plain(calls[0].args.moves[0]);
    assert.deepEqual(move.to.pos, source.pos);
    assert.equal(move.to.measure, source.measure);
    assert.equal(move.to.lane.index, 3);
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
        const { context, calls, statuses } = makeContext([], mode);
        assert.equal(vm.runInNewContext(suffixExpression, { window: context.window }), pms ? 'pms' : 'bms');
        for (const output of ['song.bms', 'song.bme', 'song.bml', 'song.txt', 'song', 'song.PMS.bms',
                              'song.pms', 'song.PMS', 'C:/folder.pms/song.bme']) {
            assert.equal(context.saveChartAs(output), true);
            assert.equal(calls.at(-1).name, 'session.save');
            assert.equal(calls.at(-1).args.path, output);
            const warn = pms && !output.toLowerCase().endsWith('.pms');
            assert.equal(statuses.at(-1).includes('无法自动恢复 PMS 9key'), warn, output);
            assert.equal(statuses.at(-1).includes('按内容推断'), false);
        }
    });
}

test('ordinary save also warns for the actual non-pms output, not only Save As', () => {
    const { context, statuses } = makeContext([], 'pms9k-bmesp');
    context.window.chartPath = 'existing.bme';
    assert.equal(context.saveChart(), true);
    assert.ok(statuses.at(-1).includes('无法自动恢复 PMS 9key'));
});
