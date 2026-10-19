// SPDX-License-Identifier: GPL-3.0-only
// 变换/量化命令测试：note.quantize（吸附网格）/ note.transform（镜像/旋转）的
// apply/invert 精确往返、批量 Composite 一个 undo 步、协议 dispatch。
// 依据 doc/01 §D「量化/镜像/旋转（LR2 式 mirror/random）」。
#include <gtest/gtest.h>

#include <algorithm>
#include <memory>
#include <string>
#include <tuple>
#include <vector>

#include "beatbench/core/Chart.hpp"
#include "beatbench/core/bms/BmsCodec.hpp"
#include "beatbench/core/command/Command.hpp"
#include "beatbench/core/edit/EditorSession.hpp"
#include "beatbench/core/edit/SessionRegistry.hpp"

using namespace beatbench;
using namespace beatbench::edit;
using beatbench::json::Json;
using beatbench::cmd::global_registry;

namespace {

Chart make_chart() {
    Chart c;
    c.meta["TITLE"] = "测试";
    c.meta["PLAYER"] = "1";
    c.meta["BPM"] = "130";
    // 三个 note：m1 pos1/3 key1 s1；m1 pos1/2 key2 s2；m1 pos0 key7 s3
    Event<Note> n1{1, Rational(1, 3), {}};
    n1.value.lane = {0, LaneKind::Key, 1};
    n1.value.sample.id = 1;
    Event<Note> n2{1, Rational(1, 2), {}};
    n2.value.lane = {0, LaneKind::Key, 2};
    n2.value.sample.id = 2;
    Event<Note> n3{1, Rational(0, 1), {}};
    n3.value.lane = {0, LaneKind::Key, 7};
    n3.value.sample.id = 3;
    c.notes = {n1, n2, n3};
    return c;
}

std::vector<std::tuple<std::uint32_t, Rational, Lane, std::uint32_t>> norm_notes(
    const std::vector<Event<Note>>& notes) {
    std::vector<std::tuple<std::uint32_t, Rational, Lane, std::uint32_t>> out;
    for (const auto& e : notes) {
        out.emplace_back(e.measure, e.pos, e.value.lane, e.value.sample.id);
    }
    std::sort(out.begin(), out.end());
    return out;
}

}  // namespace

// —— 量化 ——

TEST(TransformQuantize, QuantizeSnapsToGrid) {
    EditorSession s;
    s.load(make_chart());
    // 量化 m1 pos1/3 → 1/16 网格：1/3 ≈ 0.333，round(0.333*16)=5 → 5/16
    ASSERT_TRUE(s.exec(std::make_unique<QuantizeNoteCommand>(
        1, Rational(1, 3), Lane{0, LaneKind::Key, 1}, 1, 1, 16)));
    // 找到该 note（值定位：新 pos 5/16）
    bool found = false;
    for (const auto& e : s.chart().notes) {
        if (e.value.lane == Lane{0, LaneKind::Key, 1} && e.value.sample.id == 1) {
            EXPECT_EQ(e.pos, Rational(5, 16));  // 1/3 → 5/16
            found = true;
        }
    }
    EXPECT_TRUE(found);
    // 其它 note 不动
    EXPECT_EQ(s.chart().notes.size(), 3u);
    // undo 恢复 1/3
    ASSERT_TRUE(s.undo());
    EXPECT_EQ(norm_notes(s.chart().notes), norm_notes(make_chart().notes));
}

TEST(TransformQuantize, QuantizeAlreadyOnGridNoop) {
    EditorSession s;
    s.load(make_chart());
    // pos 1/2 已在 1/16 网格 → 无变化
    ASSERT_TRUE(s.exec(std::make_unique<QuantizeNoteCommand>(
        1, Rational(1, 2), Lane{0, LaneKind::Key, 2}, 2, 1, 16)));
    EXPECT_EQ(norm_notes(s.chart().notes), norm_notes(make_chart().notes));
    // undo 也无变化（空命令）
    ASSERT_TRUE(s.undo());
    EXPECT_EQ(norm_notes(s.chart().notes), norm_notes(make_chart().notes));
}

TEST(TransformQuantize, QuantizeCustomSnap) {
    EditorSession s;
    s.load(make_chart());
    // snap 3/16：1/3 ≈ 0.333，round(0.333*16/3)=round(1.777)=2 → 6/16 = 3/8
    ASSERT_TRUE(s.exec(std::make_unique<QuantizeNoteCommand>(
        1, Rational(1, 3), Lane{0, LaneKind::Key, 1}, 1, 3, 16)));
    bool found = false;
    for (const auto& e : s.chart().notes) {
        if (e.value.lane == Lane{0, LaneKind::Key, 1} && e.value.sample.id == 1) {
            EXPECT_EQ(e.pos, Rational(3, 8));  // 6/16 约分
            found = true;
        }
    }
    EXPECT_TRUE(found);
}

// —— 变换：镜像 ——

TEST(TransformQuantize, MirrorFlipsKeys) {
    EditorSession s;
    s.load(make_chart());
    // 镜像全部 3 个 note
    auto comp = std::make_unique<CompositeCommand>();
    comp->add(std::make_unique<TransformNoteCommand>(
        1, Rational(1, 3), Lane{0, LaneKind::Key, 1}, 1, true, 0));
    comp->add(std::make_unique<TransformNoteCommand>(
        1, Rational(1, 2), Lane{0, LaneKind::Key, 2}, 2, true, 0));
    comp->add(std::make_unique<TransformNoteCommand>(
        1, Rational(0, 1), Lane{0, LaneKind::Key, 7}, 3, true, 0));
    ASSERT_TRUE(s.exec(std::move(comp)));
    EXPECT_EQ(s.undo_depth(), 1u);
    // key1→key7, key2→key6, key7→key1
    const auto& notes = s.chart().notes;
    bool a = false, b = false, c_ok = false;
    for (const auto& e : notes) {
        if (e.pos == Rational(1, 3) && e.value.lane == Lane{0, LaneKind::Key, 7} &&
            e.value.sample.id == 1) a = true;
        if (e.pos == Rational(1, 2) && e.value.lane == Lane{0, LaneKind::Key, 6} &&
            e.value.sample.id == 2) b = true;
        if (e.pos == Rational(0, 1) && e.value.lane == Lane{0, LaneKind::Key, 1} &&
            e.value.sample.id == 3) c_ok = true;
    }
    EXPECT_TRUE(a);
    EXPECT_TRUE(b);
    EXPECT_TRUE(c_ok);
    // 一次 undo 全回原位
    ASSERT_TRUE(s.undo());
    EXPECT_EQ(norm_notes(s.chart().notes), norm_notes(make_chart().notes));
}

// —— 变换：旋转 ——

TEST(TransformQuantize, RotateCyclesKeys) {
    EditorSession s;
    s.load(make_chart());
    // 旋转 +1：key1→key2, key2→key3, key7→key1
    auto comp = std::make_unique<CompositeCommand>();
    comp->add(std::make_unique<TransformNoteCommand>(
        1, Rational(1, 3), Lane{0, LaneKind::Key, 1}, 1, false, 1));
    comp->add(std::make_unique<TransformNoteCommand>(
        1, Rational(1, 2), Lane{0, LaneKind::Key, 2}, 2, false, 1));
    comp->add(std::make_unique<TransformNoteCommand>(
        1, Rational(0, 1), Lane{0, LaneKind::Key, 7}, 3, false, 1));
    ASSERT_TRUE(s.exec(std::move(comp)));
    const auto& notes = s.chart().notes;
    bool a = false, b = false, c_ok = false;
    for (const auto& e : notes) {
        if (e.pos == Rational(1, 3) && e.value.lane == Lane{0, LaneKind::Key, 2} &&
            e.value.sample.id == 1) a = true;
        if (e.pos == Rational(1, 2) && e.value.lane == Lane{0, LaneKind::Key, 3} &&
            e.value.sample.id == 2) b = true;
        if (e.pos == Rational(0, 1) && e.value.lane == Lane{0, LaneKind::Key, 1} &&
            e.value.sample.id == 3) c_ok = true;
    }
    EXPECT_TRUE(a);
    EXPECT_TRUE(b);
    EXPECT_TRUE(c_ok);
    ASSERT_TRUE(s.undo());
    EXPECT_EQ(norm_notes(s.chart().notes), norm_notes(make_chart().notes));
}

// —— 协议 dispatch ——

TEST(TransformQuantize, ProtocolQuantize) {
    using beatbench::cmd::global_registry;
    auto& session = beatbench::edit::global_editor_session();
    session.load(make_chart());

    Json req = Json::object();
    req.set("command", "note.quantize");
    Json args = Json::object();
    Json sel = Json::array();
    Json item = Json::object();
    item.set("measure", 1);
    Json pos = Json::object();
    pos.set("num", 1);
    pos.set("den", 3);
    item.set("pos", std::move(pos));
    Json lane = Json::object();
    lane.set("player", 0);
    lane.set("kind", "key");
    lane.set("index", 1);
    item.set("lane", std::move(lane));
    item.set("sample", 1);
    sel.push_back(std::move(item));
    args.set("selection", std::move(sel));
    Json snap = Json::object();
    snap.set("num", 1);
    snap.set("den", 16);
    args.set("snap", std::move(snap));
    req.set("args", std::move(args));

    const Json resp = global_registry().dispatch(req);
    ASSERT_TRUE(resp.at("ok").as_bool()) << resp.dump();
    EXPECT_EQ(resp.at("result").at("notes").as_i64(), 1);
    bool found = false;
    for (const auto& e : session.chart().notes) {
        if (e.value.lane == Lane{0, LaneKind::Key, 1} && e.value.sample.id == 1) {
            EXPECT_EQ(e.pos, Rational(5, 16));
            found = true;
        }
    }
    EXPECT_TRUE(found);
}

TEST(TransformQuantize, ProtocolTransformMirror) {
    using beatbench::cmd::global_registry;
    auto& session = beatbench::edit::global_editor_session();
    session.load(make_chart());

    Json req = Json::object();
    req.set("command", "note.transform");
    Json args = Json::object();
    Json sel = Json::array();
    Json item = Json::object();
    item.set("measure", 1);
    Json pos = Json::object();
    pos.set("num", 0);
    pos.set("den", 1);
    item.set("pos", std::move(pos));
    Json lane = Json::object();
    lane.set("player", 0);
    lane.set("kind", "key");
    lane.set("index", 7);
    item.set("lane", std::move(lane));
    item.set("sample", 3);
    sel.push_back(std::move(item));
    args.set("selection", std::move(sel));
    args.set("mirror", true);
    req.set("args", std::move(args));

    const Json resp = global_registry().dispatch(req);
    ASSERT_TRUE(resp.at("ok").as_bool()) << resp.dump();
    // key7 → key1
    bool found = false;
    for (const auto& e : session.chart().notes) {
        if (e.value.lane == Lane{0, LaneKind::Key, 1} && e.value.sample.id == 3) {
            found = true;
        }
    }
    EXPECT_TRUE(found);
    // undo 恢复
    req = Json::object();
    req.set("command", "session.undo");
    const Json resp2 = global_registry().dispatch(req);
    ASSERT_TRUE(resp2.at("ok").as_bool()) << resp2.dump();
    EXPECT_EQ(norm_notes(session.chart().notes), norm_notes(make_chart().notes));
}

TEST(TransformQuantize, ProtocolEmptySelection) {
    using beatbench::cmd::global_registry;
    auto& session = beatbench::edit::global_editor_session();
    session.load(make_chart());
    Json req = Json::object();
    req.set("command", "note.quantize");
    Json args = Json::object();
    args.set("selection", Json::array());
    req.set("args", std::move(args));
    const Json resp = global_registry().dispatch(req);
    EXPECT_FALSE(resp.at("ok").as_bool());
    EXPECT_EQ(resp.at("error").at("code").as_str(), "empty_selection");
}

// —— R27-2（上游复核）：批量快照变换，同拍同 sample 互换不拆 LN ——

namespace {

// 复核场景：pms9k，key1 LN 头@pos0 s1、key1 LN 尾@1/4 s1、key9 普通音@pos0 s1
//（三事件同 sample；按 Chart 顺序全选 mirror 时，逐项值定位会把刚移到 key9
// 的头又被普通音命令按同值定位移回 key1，头尾配对拆坏且命令返回 ok）。
Chart make_pms_ln_swap_chart() {
    Chart c;
    c.meta["TITLE"] = "pms ln swap";
    c.meta["BPM"] = "120";
    c.mode_id = "pms9k";
    Event<Note> head{0, Rational(0, 1), {}};
    head.value.lane = {0, LaneKind::Key, 1};
    head.value.sample.id = 1;
    head.value.ln_channel = true;
    Event<Note> tail{0, Rational(1, 4), {}};
    tail.value.lane = {0, LaneKind::Key, 1};
    tail.value.sample.id = 1;
    tail.value.ln_channel = true;
    Event<Note> normal{0, Rational(0, 1), {}};
    normal.value.lane = {0, LaneKind::Key, 9};
    normal.value.sample.id = 1;
    c.notes = {head, tail, normal};
    return c;
}

NoteRef ref_for(const Event<Note>& e) {
    return NoteRef{e.measure, e.pos, e.value.lane, e.value.sample.id, e.value.sub_line};
}

Json transform_request(std::vector<NoteRef> refs, bool mirror, int rotate = 0) {
    Json args = Json::object();
    Json sel = Json::array();
    for (const auto& r : refs) {
        Json item = Json::object();
        item.set("measure", static_cast<std::int64_t>(r.measure));
        Json pos = Json::object();
        pos.set("num", static_cast<std::int64_t>(r.pos.num));
        pos.set("den", static_cast<std::int64_t>(r.pos.den));
        item.set("pos", std::move(pos));
        Json lane = Json::object();
        lane.set("player", static_cast<std::int64_t>(r.lane.player));
        lane.set("index", static_cast<std::int64_t>(r.lane.index));
        lane.set("kind", "key");
        item.set("lane", std::move(lane));
        item.set("sample", static_cast<std::int64_t>(r.sample));
        sel.push_back(std::move(item));
    }
    args.set("selection", std::move(sel));
    if (mirror) args.set("mirror", true);
    if (rotate != 0) args.set("rotate", static_cast<std::int64_t>(rotate));
    Json req = Json::object();
    req.set("command", "note.transform");
    req.set("args", std::move(args));
    return req;
}

}  // namespace

TEST(TransformQuantize, BatchMirrorSwapKeepsLnPaired) {
    auto& s = beatbench::edit::global_editor_session();
    s.load(make_pms_ln_swap_chart());
    std::vector<NoteRef> refs;
    for (const auto& e : s.chart().notes) refs.push_back(ref_for(e));  // Chart 顺序
    const Json resp = global_registry().dispatch(transform_request(refs, true));
    ASSERT_TRUE(resp.at("ok").as_bool()) << resp.dump();
    const auto& notes = s.chart().notes;
    ASSERT_EQ(notes.size(), 3u);
    // LN 头尾整体到 key9，普通音到 key1
    EXPECT_EQ(notes[0].value.lane, (Lane{0, LaneKind::Key, 9}));
    EXPECT_EQ(notes[1].value.lane, (Lane{0, LaneKind::Key, 9}));
    EXPECT_EQ(notes[2].value.lane, (Lane{0, LaneKind::Key, 1}));
    // 配对保持：头尾互指且同 lane
    ASSERT_TRUE(notes[0].value.ln_pair.has_value());
    EXPECT_EQ(*notes[0].value.ln_pair, 1u);
    ASSERT_TRUE(notes[1].value.ln_pair.has_value());
    EXPECT_EQ(*notes[1].value.ln_pair, 0u);
    // undo/redo 精确往返
    ASSERT_TRUE(s.undo());
    EXPECT_EQ(s.chart().notes[0].value.lane, (Lane{0, LaneKind::Key, 1}));
    EXPECT_EQ(s.chart().notes[1].value.lane, (Lane{0, LaneKind::Key, 1}));
    EXPECT_EQ(s.chart().notes[2].value.lane, (Lane{0, LaneKind::Key, 9}));
    ASSERT_TRUE(s.redo());
    EXPECT_EQ(s.chart().notes[0].value.lane, (Lane{0, LaneKind::Key, 9}));
    EXPECT_EQ(s.chart().notes[1].value.lane, (Lane{0, LaneKind::Key, 9}));
    EXPECT_EQ(s.chart().notes[2].value.lane, (Lane{0, LaneKind::Key, 1}));
    EXPECT_TRUE(s.chart().notes[0].value.ln_pair.has_value());
    EXPECT_EQ(s.chart().notes[0].value.lane, s.chart().notes[1].value.lane);
}

TEST(TransformQuantize, BatchMirrorSwapOrderIndependent) {
    // 不同选择顺序（含普通音在前的危险序）结果一致
    const std::vector<std::vector<std::size_t>> orders = {{0, 1, 2}, {2, 0, 1}, {1, 2, 0}};
    for (const auto& order : orders) {
        auto& s = beatbench::edit::global_editor_session();
        s.load(make_pms_ln_swap_chart());
        std::vector<NoteRef> refs;
        for (const auto& i : order) refs.push_back(ref_for(s.chart().notes[i]));
        const Json resp = global_registry().dispatch(transform_request(refs, true));
        ASSERT_TRUE(resp.at("ok").as_bool());
        const auto& notes = s.chart().notes;
        EXPECT_EQ(notes[0].value.lane, (Lane{0, LaneKind::Key, 9})) << "order[0]";
        EXPECT_EQ(notes[1].value.lane, (Lane{0, LaneKind::Key, 9})) << "order[1]";
        EXPECT_EQ(notes[2].value.lane, (Lane{0, LaneKind::Key, 1})) << "order[2]";
        EXPECT_EQ(notes[0].value.lane, notes[1].value.lane);
    }
}

TEST(TransformQuantize, BatchRotateNoTargetCollision) {
    // rotate 同类碰撞：A@key1 s1 → key2，B@key2 s1（同 sample）→ key3。
    // 旧实现 A 先应用后，B 按值定位 key2 命中刚移来的 A，把 A 再推到 key3。
    auto& s = beatbench::edit::global_editor_session();
    Chart c;
    c.meta["TITLE"] = "rotate swap";
    c.meta["BPM"] = "120";
    Event<Note> a{1, Rational(0, 1), {}};
    a.value.lane = {0, LaneKind::Key, 1};
    a.value.sample.id = 1;
    Event<Note> b{1, Rational(0, 1), {}};
    b.value.lane = {0, LaneKind::Key, 2};
    b.value.sample.id = 1;
    c.notes = {a, b};
    s.load(c);
    // 危险序：A 在前
    const Json resp = global_registry().dispatch(transform_request(
        {ref_for(s.chart().notes[0]), ref_for(s.chart().notes[1])}, false, 1));
    ASSERT_TRUE(resp.at("ok").as_bool());
    EXPECT_EQ(s.chart().notes[0].value.lane, (Lane{0, LaneKind::Key, 2}));
    EXPECT_EQ(s.chart().notes[1].value.lane, (Lane{0, LaneKind::Key, 3}));
}

TEST(TransformQuantize, BatchMirrorRoundtripFileStable) {
    // 变换后保存重开：模型语义稳定，LN 两端同 lane（key9 = 标准 65 通道）仍成对
    auto& s = beatbench::edit::global_editor_session();
    s.load(make_pms_ln_swap_chart());
    std::vector<NoteRef> refs;
    for (const auto& e : s.chart().notes) refs.push_back(ref_for(e));
    ASSERT_TRUE(global_registry().dispatch(transform_request(refs, true)).at("ok").as_bool());
    const auto sigs = norm_notes(s.chart().notes);
    beatbench::bms::BmsReadOptions opts;
    opts.mode = "pms9k";
    const auto r2 = beatbench::bms::read_bms(beatbench::bms::write_bms(s.chart()), opts);
    EXPECT_EQ(norm_notes(r2.chart.notes), sigs);
    int key9_count = 0, key9_paired = 0;
    for (const auto& e : r2.chart.notes) {
        if (e.value.lane == (Lane{0, LaneKind::Key, 9})) {
            ++key9_count;
            if (e.value.ln_pair) ++key9_paired;
        }
    }
    EXPECT_EQ(key9_count, 2);
    EXPECT_EQ(key9_paired, 2);
}
