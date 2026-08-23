// SPDX-License-Identifier: GPL-3.0-only
// note.move 跨通道（to.lane）测试：时间 + 通道一起移动、invert 精确还原、
// 向后兼容（不传 to.lane = 纯时间）、LN 两种模式、批量 CompositeCommand 跨通道一步 undo。
// 依据 local/doc/handoff-note-move-batch.md（M2→M3 交接，2026-09）。
#include <gtest/gtest.h>

#include <algorithm>
#include <memory>
#include <string>
#include <tuple>
#include <vector>

#include "beatbench/core/Chart.hpp"
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
    // 三个 note：m1 pos0 key1 s1；m1 pos1/2 key2 s2；m2 pos0 key3 s3
    Event<Note> n1{1, Rational(0, 1), {}};
    n1.value.lane = {0, LaneKind::Key, 1};
    n1.value.sample.id = 1;
    Event<Note> n2{1, Rational(1, 2), {}};
    n2.value.lane = {0, LaneKind::Key, 2};
    n2.value.sample.id = 2;
    Event<Note> n3{2, Rational(0, 1), {}};
    n3.value.lane = {0, LaneKind::Key, 3};
    n3.value.sample.id = 3;
    c.notes = {n1, n2, n3};
    return c;
}

// 容器规范化（(measure,pos,lane,sample) 元组排序；不含 ln_pair——配对单独验证）
std::vector<std::tuple<std::uint32_t, Rational, Lane, std::uint32_t>> norm_notes(
    const std::vector<Event<Note>>& notes) {
    std::vector<std::tuple<std::uint32_t, Rational, Lane, std::uint32_t>> out;
    for (const auto& e : notes) {
        out.emplace_back(e.measure, e.pos, e.value.lane, e.value.sample.id);
    }
    std::sort(out.begin(), out.end());
    return out;
}

bool ln_consistent(const std::vector<Event<Note>>& notes) {
    for (std::size_t i = 0; i < notes.size(); ++i) {
        const auto p = notes[i].value.ln_pair;
        if (!p) continue;
        if (*p >= notes.size()) return false;
        const auto q = notes[*p].value.ln_pair;
        if (!q || *q != i) return false;
    }
    return true;
}

}  // namespace

// —— 单 note 跨通道移动 ——

TEST(NoteMoveLane, MoveTimeAndLane) {
    EditorSession s;
    s.load(make_chart());
    // m1 pos0 key1 s1 → m3 pos1/4 scratch（时间 + 通道一起动）
    ASSERT_TRUE(s.exec(std::make_unique<MoveNoteCommand>(
        1, Rational(0, 1), Lane{0, LaneKind::Key, 1}, 1,
        3, Rational(1, 4), false, Lane{0, LaneKind::Scratch, 0})));
    const auto& notes = s.chart().notes;
    ASSERT_EQ(notes.size(), 3u);
    // 找到移动后的 note：m3 pos1/4 scratch s1
    bool found = false;
    for (const auto& e : notes) {
        if (e.measure == 3 && e.pos == Rational(1, 4) &&
            e.value.lane == Lane{0, LaneKind::Scratch, 0} && e.value.sample.id == 1) {
            found = true;
        }
    }
    EXPECT_TRUE(found);
    // 原位 m1 pos0 key1 不再有 s1
    for (const auto& e : notes) {
        const bool still_at_source =
            e.measure == 1 && e.pos == Rational(0, 1) &&
            e.value.lane == Lane{0, LaneKind::Key, 1} && e.value.sample.id == 1;
        EXPECT_FALSE(still_at_source);
    }
    EXPECT_TRUE(ln_consistent(notes));

    // undo → 还原到源位置 + 源 lane
    ASSERT_TRUE(s.undo());
    EXPECT_EQ(norm_notes(s.chart().notes), norm_notes(make_chart().notes));
}

TEST(NoteMoveLane, BackwardCompatNoLane) {
    // 不传 to.lane = 纯时间移动（旧行为完全一致）
    EditorSession s;
    s.load(make_chart());
    ASSERT_TRUE(s.exec(std::make_unique<MoveNoteCommand>(
        1, Rational(0, 1), Lane{0, LaneKind::Key, 1}, 1,
        5, Rational(1, 2))));  // 无 to_lane
    // lane 不变，measure/pos 变
    bool found = false;
    for (const auto& e : s.chart().notes) {
        if (e.measure == 5 && e.pos == Rational(1, 2) &&
            e.value.lane == Lane{0, LaneKind::Key, 1} && e.value.sample.id == 1) {
            found = true;
        }
    }
    EXPECT_TRUE(found);
    ASSERT_TRUE(s.undo());
    EXPECT_EQ(norm_notes(s.chart().notes), norm_notes(make_chart().notes));
}

// —— LN 两种模式跨通道 ——

// 构造 LN 对（key1 头 m1 pos0 + 尾 m1 pos1/2，互指）
Chart ln_chart() {
    Chart c;
    c.meta["TITLE"] = "LN";
    c.meta["PLAYER"] = "1";
    Event<Note> head{1, Rational(0, 1), {}};
    head.value.lane = {0, LaneKind::Key, 1};
    head.value.sample.id = 1;
    Event<Note> tail{1, Rational(1, 2), {}};
    tail.value.lane = {0, LaneKind::Key, 1};
    tail.value.sample.id = 1;
    head.value.ln_pair = 1;
    tail.value.ln_pair = 0;
    c.notes = {head, tail};
    return c;
}

TEST(NoteMoveLane, LnPairMovesBothLanes) {
    EditorSession s;
    s.load(ln_chart());
    // 2026-09 用户最终确认：移动只移动选中 note，ln_pair 保持（不成对随动）。
    // `move_ln_pair=true` 参数保留（旧 API），但语义 = 只移主 note、配对互指保持。
    // 头 m1 pos0 key1 → m2 pos0 key5；尾留 m1 pos1/2 key1；仍互指。
    ASSERT_TRUE(s.exec(std::make_unique<MoveNoteCommand>(
        1, Rational(0, 1), Lane{0, LaneKind::Key, 1}, 1,
        2, Rational(0, 1), true, Lane{0, LaneKind::Key, 5})));
    const auto& notes = s.chart().notes;
    ASSERT_EQ(notes.size(), 2u);
    bool head_ok = false, tail_ok = false;
    for (const auto& e : notes) {
        if (e.measure == 2 && e.value.lane == Lane{0, LaneKind::Key, 5}) {
            head_ok = e.value.ln_pair.has_value();
        }
        if (e.measure == 1 && e.value.lane == Lane{0, LaneKind::Key, 1}) {
            tail_ok = e.value.ln_pair.has_value();
        }
    }
    EXPECT_TRUE(head_ok);
    EXPECT_TRUE(tail_ok);
    EXPECT_TRUE(ln_consistent(notes));
    // undo → 回源位置 + 源 lane + 配对
    ASSERT_TRUE(s.undo());
    EXPECT_EQ(norm_notes(s.chart().notes), norm_notes(ln_chart().notes));
    EXPECT_TRUE(ln_consistent(s.chart().notes));
}

TEST(NoteMoveLane, LnSingleNoteBreaksPairCrossLane) {
    EditorSession s;
    s.load(ln_chart());
    // 单 note 模式跨通道：只移头（key1→key7），配对**保持**（2026-09 用户最终确认：
    // 移动只移动选中 note，ln_pair 保持——不自动重连也不断开，只按伙伴值重定位）。
    ASSERT_TRUE(s.exec(std::make_unique<MoveNoteCommand>(
        1, Rational(0, 1), Lane{0, LaneKind::Key, 1}, 1,
        2, Rational(0, 1), false, Lane{0, LaneKind::Key, 7})));
    const auto& notes = s.chart().notes;
    ASSERT_EQ(notes.size(), 2u);
    // 头在 m2 key7 仍与尾（m1 key1）互指；尾也仍指向头
    bool head_ok = false, tail_ok = false;
    for (const auto& e : notes) {
        if (e.measure == 2 && e.value.lane == Lane{0, LaneKind::Key, 7}) {
            head_ok = e.value.ln_pair.has_value();
        }
        if (e.measure == 1 && e.value.lane == Lane{0, LaneKind::Key, 1}) {
            tail_ok = e.value.ln_pair.has_value();
        }
    }
    EXPECT_TRUE(head_ok);
    EXPECT_TRUE(tail_ok);
    EXPECT_TRUE(ln_consistent(notes));
    // undo → 完全还原（含配对）
    ASSERT_TRUE(s.undo());
    EXPECT_EQ(norm_notes(s.chart().notes), norm_notes(ln_chart().notes));
    EXPECT_TRUE(ln_consistent(s.chart().notes));
}

// —— 同通道单 note 移动 LN（2026-09 用户确认：移动端重连"向前找最近"，原端原位断开） ——

TEST(NoteMoveLane, LnSingleMoveSameLaneReconnectsForward) {
    EditorSession s;
    s.load(ln_chart());  // 头 m1 pos0 + 尾 m1 pos1/2，互指
    // 只移尾（同通道，m1 pos1/2 → m2 pos3/4）：尾在新位置向前找最近同通道未配对 note
    ASSERT_TRUE(s.exec(std::make_unique<MoveNoteCommand>(
        1, Rational(1, 2), Lane{0, LaneKind::Key, 1}, 1,
        2, Rational(3, 4), false)));
    const auto& notes = s.chart().notes;
    ASSERT_EQ(notes.size(), 2u);
    EXPECT_TRUE(ln_consistent(notes));
    // 尾移到 m2 pos3/4；向前找最近同通道未配对 = 头（m1 pos0，被断开后未配对）→ 重连
    bool tail_reconnected = false;
    for (const auto& e : notes) {
        if (e.measure == 2 && e.pos == Rational(3, 4) &&
            e.value.lane == Lane{0, LaneKind::Key, 1})
            tail_reconnected = e.value.ln_pair.has_value();
    }
    EXPECT_TRUE(tail_reconnected);
    // undo → 恢复原配对（头尾互指原位置）
    ASSERT_TRUE(s.undo());
    EXPECT_EQ(norm_notes(s.chart().notes), norm_notes(ln_chart().notes));
    EXPECT_TRUE(ln_consistent(s.chart().notes));
}

// —— 批量跨通道移动（CompositeCommand 一步 undo） ——

TEST(NoteMoveLane, CompositeBatchCrossLaneOneUndo) {
    EditorSession s;
    s.load(make_chart());  // 3 notes
    // 批量：n1 (m1 pos0 key1 s1) → m3 key5；n2 (m1 pos1/2 key2 s2) → m3 scratch
    auto comp = std::make_unique<CompositeCommand>();
    comp->add(std::make_unique<MoveNoteCommand>(
        1, Rational(0, 1), Lane{0, LaneKind::Key, 1}, 1,
        3, Rational(0, 1), false, Lane{0, LaneKind::Key, 5}));
    comp->add(std::make_unique<MoveNoteCommand>(
        1, Rational(1, 2), Lane{0, LaneKind::Key, 2}, 2,
        3, Rational(1, 2), false, Lane{0, LaneKind::Scratch, 0}));
    ASSERT_TRUE(s.exec(std::move(comp)));
    EXPECT_EQ(s.undo_depth(), 1u);  // 一个 undo 步

    // 目标状态：m3 key5 s1 + m3 scratch s2 + m2 key3 s3（未动）
    const auto& notes = s.chart().notes;
    ASSERT_EQ(notes.size(), 3u);
    bool a = false, b = false, c_ok = false;
    for (const auto& e : notes) {
        if (e.measure == 3 && e.value.lane == Lane{0, LaneKind::Key, 5} && e.value.sample.id == 1)
            a = true;
        if (e.measure == 3 && e.value.lane == Lane{0, LaneKind::Scratch, 0} && e.value.sample.id == 2)
            b = true;
        if (e.measure == 2 && e.value.lane == Lane{0, LaneKind::Key, 3} && e.value.sample.id == 3)
            c_ok = true;
    }
    EXPECT_TRUE(a);
    EXPECT_TRUE(b);
    EXPECT_TRUE(c_ok);
    EXPECT_TRUE(ln_consistent(notes));

    // 一次 undo 全部回原位
    ASSERT_TRUE(s.undo());
    EXPECT_EQ(norm_notes(s.chart().notes), norm_notes(make_chart().notes));
    EXPECT_TRUE(ln_consistent(s.chart().notes));
}

// —— 协议 dispatch ——

TEST(NoteMoveLane, ProtocolToLane) {
    using beatbench::cmd::global_registry;
    auto& session = beatbench::edit::global_editor_session();
    session.load(make_chart());

    Json req = Json::object();
    req.set("command", "note.move");
    Json args = Json::object();
    Json from = Json::object();
    from.set("measure", 1);
    Json pos = Json::object();
    pos.set("num", 0);
    pos.set("den", 1);
    from.set("pos", std::move(pos));
    from.set("sample", 1);
    // from.lane 子对象（新契约；与 note.put/delete 一致）
    Json from_lane = Json::object();
    from_lane.set("player", 0);
    from_lane.set("kind", "key");
    from_lane.set("index", 1);
    from.set("lane", std::move(from_lane));
    args.set("from", std::move(from));
    Json to = Json::object();
    to.set("measure", 4);
    pos = Json::object();
    pos.set("num", 1);
    pos.set("den", 4);
    to.set("pos", std::move(pos));
    // to.lane（跨通道）
    Json to_lane = Json::object();
    to_lane.set("player", 0);
    to_lane.set("kind", "pedal");
    to_lane.set("index", 0);
    to.set("lane", std::move(to_lane));
    args.set("to", std::move(to));
    req.set("args", std::move(args));

    const Json resp = global_registry().dispatch(req);
    ASSERT_TRUE(resp.at("ok").as_bool()) << resp.dump();
    // 找到 m4 pos1/4 pedal s1
    bool found = false;
    for (const auto& e : session.chart().notes) {
        if (e.measure == 4 && e.pos == Rational(1, 4) &&
            e.value.lane == Lane{0, LaneKind::Pedal, 0} && e.value.sample.id == 1) {
            found = true;
        }
    }
    EXPECT_TRUE(found);

    // undo（协议）→ 还原
    req = Json::object();
    req.set("command", "session.undo");
    const Json resp2 = global_registry().dispatch(req);
    ASSERT_TRUE(resp2.at("ok").as_bool()) << resp2.dump();
    EXPECT_EQ(norm_notes(session.chart().notes), norm_notes(make_chart().notes));
}

TEST(NoteMoveLane, ProtocolBadToLane) {
    using beatbench::cmd::global_registry;
    auto& session = beatbench::edit::global_editor_session();
    session.load(make_chart());
    Json req = Json::object();
    req.set("command", "note.move");
    Json args = Json::object();
    Json from = Json::object();
    from.set("measure", 1);
    Json pos = Json::object();
    pos.set("num", 0);
    pos.set("den", 1);
    from.set("pos", std::move(pos));
    from.set("sample", 1);
    from.set("player", 0);
    from.set("kind", "key");
    from.set("index", 1);
    args.set("from", std::move(from));
    Json to = Json::object();
    to.set("measure", 4);
    pos = Json::object();
    pos.set("num", 1);
    pos.set("den", 4);
    to.set("pos", std::move(pos));
    to.set("lane", 42);  // 非法：lane 应为对象
    args.set("to", std::move(to));
    req.set("args", std::move(args));
    const Json resp = global_registry().dispatch(req);
    EXPECT_FALSE(resp.at("ok").as_bool());
    EXPECT_EQ(resp.at("error").at("code").as_str(), "bad_args");
}

// —— Y1（2026）：note.move 接受 moves 数组（1..N 项，每项 {from,to}；兼容旧 {from,to}） ——

TEST(NoteMoveLane, ProtocolMovesArraySingle) {
    using beatbench::cmd::global_registry;
    auto& session = beatbench::edit::global_editor_session();
    session.load(make_chart());

    // 单元素 moves 数组（等价于顶层 {from,to}；新契约）
    Json req = Json::object();
    req.set("command", "note.move");
    Json args = Json::object();
    Json moves = Json::array();
    Json m0 = Json::object();
    Json from0 = Json::object();
    from0.set("measure", 1);
    Json pos0 = Json::object();
    pos0.set("num", 0);
    pos0.set("den", 1);
    from0.set("pos", std::move(pos0));
    from0.set("sample", 1);
    Json lane0 = Json::object();
    lane0.set("player", 0);
    lane0.set("kind", "key");
    lane0.set("index", 1);
    from0.set("lane", std::move(lane0));
    m0.set("from", std::move(from0));
    Json to0 = Json::object();
    to0.set("measure", 5);
    Json pos1 = Json::object();
    pos1.set("num", 1);
    pos1.set("den", 2);
    to0.set("pos", std::move(pos1));
    m0.set("to", std::move(to0));
    moves.push_back(std::move(m0));
    args.set("moves", std::move(moves));
    req.set("args", std::move(args));

    const Json resp = global_registry().dispatch(req);
    ASSERT_TRUE(resp.at("ok").as_bool()) << resp.dump();
    EXPECT_EQ(resp.at("result").at("moved").as_i64(), 1);
    EXPECT_EQ(session.undo_depth(), 1u);  // 单元素也一个 undo 步
    bool found = false;
    for (const auto& e : session.chart().notes) {
        if (e.measure == 5 && e.pos == Rational(1, 2) &&
            e.value.lane == Lane{0, LaneKind::Key, 1} && e.value.sample.id == 1)
            found = true;
    }
    EXPECT_TRUE(found);
}

TEST(NoteMoveLane, ProtocolMovesArrayMultiOneUndo) {
    using beatbench::cmd::global_registry;
    auto& session = beatbench::edit::global_editor_session();
    session.load(make_chart());

    // 两个元素，各跨通道：n1 → key5；n2 → scratch
    Json req = Json::object();
    req.set("command", "note.move");
    Json args = Json::object();
    Json moves = Json::array();
    auto push_move = [&](std::uint32_t fm, std::int64_t pn, std::int64_t pd,
                         std::uint32_t sample, const char* kind, std::uint32_t index,
                         std::uint32_t tm, std::int64_t tn, std::int64_t td,
                         const char* to_kind, std::uint32_t to_index) {
        Json m = Json::object();
        Json from = Json::object();
        from.set("measure", static_cast<std::int64_t>(fm));
        Json pos = Json::object();
        pos.set("num", pn);
        pos.set("den", pd);
        from.set("pos", std::move(pos));
        from.set("sample", static_cast<std::int64_t>(sample));
        Json lane = Json::object();
        lane.set("player", 0);
        lane.set("kind", kind);
        lane.set("index", static_cast<std::int64_t>(index));
        from.set("lane", std::move(lane));
        m.set("from", std::move(from));
        Json to = Json::object();
        to.set("measure", static_cast<std::int64_t>(tm));
        pos = Json::object();
        pos.set("num", tn);
        pos.set("den", td);
        to.set("pos", std::move(pos));
        Json tl = Json::object();
        tl.set("player", 0);
        tl.set("kind", to_kind);
        tl.set("index", static_cast<std::int64_t>(to_index));
        to.set("lane", std::move(tl));
        m.set("to", std::move(to));
        moves.push_back(std::move(m));
    };
    push_move(1, 0, 1, 1, "key", 1, 3, 0, 1, "key", 5);
    push_move(1, 1, 2, 2, "key", 2, 3, 1, 2, "scratch", 0);
    args.set("moves", std::move(moves));
    req.set("args", std::move(args));

    const Json resp = global_registry().dispatch(req);
    ASSERT_TRUE(resp.at("ok").as_bool()) << resp.dump();
    EXPECT_EQ(resp.at("result").at("moved").as_i64(), 2);
    EXPECT_EQ(session.undo_depth(), 1u);  // 两个 note 一个 undo 步

    bool a = false, b = false, c_ok = false;
    for (const auto& e : session.chart().notes) {
        if (e.measure == 3 && e.value.lane == Lane{0, LaneKind::Key, 5} && e.value.sample.id == 1)
            a = true;
        if (e.measure == 3 && e.value.lane == Lane{0, LaneKind::Scratch, 0} && e.value.sample.id == 2)
            b = true;
        if (e.measure == 2 && e.value.lane == Lane{0, LaneKind::Key, 3} && e.value.sample.id == 3)
            c_ok = true;
    }
    EXPECT_TRUE(a);
    EXPECT_TRUE(b);
    EXPECT_TRUE(c_ok);
    EXPECT_TRUE(ln_consistent(session.chart().notes));

    // 一次 undo 全部回原位
    req = Json::object();
    req.set("command", "session.undo");
    const Json resp2 = global_registry().dispatch(req);
    ASSERT_TRUE(resp2.at("ok").as_bool()) << resp2.dump();
    EXPECT_EQ(norm_notes(session.chart().notes), norm_notes(make_chart().notes));
}

TEST(NoteMoveLane, ProtocolMovesArrayEmptyRejected) {
    using beatbench::cmd::global_registry;
    auto& session = beatbench::edit::global_editor_session();
    session.load(make_chart());
    Json req = Json::object();
    req.set("command", "note.move");
    Json args = Json::object();
    args.set("moves", Json::array());
    req.set("args", std::move(args));
    const Json resp = global_registry().dispatch(req);
    EXPECT_FALSE(resp.at("ok").as_bool());
    EXPECT_EQ(resp.at("error").at("code").as_str(), "bad_args");
}

// —— 向后兼容：无 moves 时顶层 {from,to} 仍工作（旧契约不会被破坏） ——

TEST(NoteMoveLane, ProtocolLegacyTopLevelFromTo) {
    using beatbench::cmd::global_registry;
    auto& session = beatbench::edit::global_editor_session();
    session.load(make_chart());
    // 与 ProtocolToLane 相同结构，但这里确认旧契约（顶层 from/to，不经数组）仍 ok
    Json req = Json::object();
    req.set("command", "note.move");
    Json args = Json::object();
    Json from = Json::object();
    from.set("measure", 1);
    Json pos = Json::object();
    pos.set("num", 0);
    pos.set("den", 1);
    from.set("pos", std::move(pos));
    from.set("sample", 1);
    from.set("player", 0);
    from.set("kind", "key");
    from.set("index", 1);
    args.set("from", std::move(from));
    Json to = Json::object();
    to.set("measure", 4);
    pos = Json::object();
    pos.set("num", 1);
    pos.set("den", 4);
    to.set("pos", std::move(pos));
    args.set("to", std::move(to));
    req.set("args", std::move(args));
    const Json resp = global_registry().dispatch(req);
    ASSERT_TRUE(resp.at("ok").as_bool()) << resp.dump();
    EXPECT_EQ(resp.at("result").at("moved").as_i64(), 1);
}
