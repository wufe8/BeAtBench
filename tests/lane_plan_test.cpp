// SPDX-License-Identifier: GPL-3.0-only
// LanePlan 验收：选中集归一、LN 成组、模式校验、隐藏、静止目标碰撞、plan 间冲突、BGM 子行。
#include <gtest/gtest.h>

#include <algorithm>
#include <string>
#include <vector>

#include "beatbench/core/Chart.hpp"
#include "beatbench/core/edit/EditorSession.hpp"
#include "beatbench/core/edit/LanePlan.hpp"

using namespace beatbench;
using beatbench::edit::EditorSession;
using beatbench::edit::LaneMove;
using beatbench::edit::LaneMoveReason;
using beatbench::edit::LanePlanOptions;
using beatbench::edit::LaneTarget;
using beatbench::edit::NoteGroup;
using beatbench::edit::NoteRef;

// gtest 打印 LaneMoveReason（enum class 无隐式转换；PrintTo 走 ADL 须在原命名空间）。
namespace beatbench::edit {
inline void PrintTo(LaneMoveReason r, std::ostream* os) { *os << static_cast<int>(r); }
}  // namespace beatbench::edit

/// 断言消息用的人类可读原因名（enum 不能直接流式输出）。
inline const char* reason_name(LaneMoveReason r) {
    switch (r) {
        case LaneMoveReason::None:
            return "none";
        case LaneMoveReason::NotFound:
            return "not_found";
        case LaneMoveReason::InvalidLane:
            return "invalid_lane";
        case LaneMoveReason::Hidden:
            return "hidden";
        case LaneMoveReason::LnMismatch:
            return "ln_mismatch";
        case LaneMoveReason::Occupied:
            return "occupied";
        case LaneMoveReason::DuplicateTarget:
            return "duplicate_target";
    }
    return "?";
}

namespace {

Event<Note> note(std::uint32_t measure, Rational pos, Lane lane, std::uint32_t sample,
                 bool ln = false) {
    Event<Note> e{measure, pos, {}};
    e.value.lane = lane;
    e.value.sample.id = sample;
    e.value.ln_channel = ln;
    return e;
}

NoteRef ref(const Event<Note>& e) {
    return NoteRef{e.measure, e.pos, e.value.lane, e.value.sample.id, e.value.sub_line};
}

Chart make_chart() {
    Chart c;
    c.mode_id = "sp7k";
    c.meta["BPM"] = "130";
    c.notes.push_back(note(1, Rational(0, 1), {0, LaneKind::Key, 1}, 1));
    c.notes.push_back(note(1, Rational(0, 1), {0, LaneKind::Key, 3}, 2));
    c.notes.push_back(note(1, Rational(1, 2), {0, LaneKind::Key, 2}, 3));
    return c;
}

Chart make_ln_chart() {
    Chart c;
    c.mode_id = "sp7k";
    c.meta["BPM"] = "130";
    c.notes.push_back(note(1, Rational(0, 1), {0, LaneKind::Key, 1}, 7, true));
    c.notes.push_back(note(2, Rational(0, 1), {0, LaneKind::Key, 1}, 7, true));
    c.notes[0].value.ln_pair = 1;
    c.notes[1].value.ln_pair = 0;
    return c;
}

LanePlanOptions options() {
    LanePlanOptions o;
    o.allow_bgm = true;
    return o;
}

}  // namespace

TEST(LanePlan, GroupsSingleAndLnWithPartner) {
    Chart c = make_ln_chart();
    auto groups = beatbench::edit::group_selection(c, {ref(c.notes[1]), ref(c.notes[1])});
    ASSERT_EQ(groups.size(), 1u);
    EXPECT_TRUE(groups[0].ln);
    ASSERT_TRUE(groups[0].b.has_value());
    EXPECT_EQ(groups[0].a.measure, 1u);
    EXPECT_EQ(groups[0].b->measure, 2u);

    Chart plain = make_chart();
    auto plain_groups = beatbench::edit::group_selection(
        plain, {ref(plain.notes[0]), ref(plain.notes[0]), ref(plain.notes[2])});
    ASSERT_EQ(plain_groups.size(), 2u);
    EXPECT_FALSE(plain_groups[0].ln);
    EXPECT_EQ(plain_groups[0].a.measure, 1u);
    EXPECT_EQ(plain_groups[1].a.pos, Rational(1, 2));
}

TEST(LanePlan, MovesLaneWithoutTimeChange) {
    Chart c = make_chart();
    auto groups = beatbench::edit::group_selection(c, {ref(c.notes[0])});
    ASSERT_EQ(groups.size(), 1u);
    auto plan = beatbench::edit::plan_lane_moves(
        c, groups, {LaneTarget{{0, LaneKind::Key, 2}, false}}, options());
    ASSERT_EQ(plan.moves.size(), 1u);
    ASSERT_TRUE(plan.moves[0].move) << reason_name(plan.moves[0].reason);

    EditorSession s;
    s.load(c);
    auto cmd = beatbench::edit::make_lane_move_command(plan.moves[0]);
    ASSERT_TRUE(cmd);
    EXPECT_FALSE(cmd->changes_time());
    ASSERT_TRUE(s.exec(std::move(cmd)));
    EXPECT_EQ(s.chart().notes[0].value.lane.index, 2u);
    EXPECT_EQ(s.chart().notes[0].measure, 1u);
    EXPECT_EQ(s.chart().notes[0].pos, Rational(0, 1));
}

TEST(LanePlan, StationarySelectedTargetBlocksCollision) {
    Chart c = make_chart();
    auto groups = beatbench::edit::group_selection(
        c, {ref(c.notes[0]), ref(c.notes[1])});  // key1 与 key3 同拍
    ASSERT_EQ(groups.size(), 2u);
    auto plan = beatbench::edit::plan_lane_moves(
        c, groups, {LaneTarget{{0, LaneKind::Key, 3}, false}}, options());
    ASSERT_EQ(plan.moves.size(), 1u);
    EXPECT_FALSE(plan.moves[0].move);
    EXPECT_EQ(plan.moves[0].reason, LaneMoveReason::Occupied);
    EXPECT_EQ(plan.skipped, 1u);
    EXPECT_EQ(plan.unplanned, 1u);
}

TEST(LanePlan, DuplicateTargetsPlanOnlyFirst) {
    Chart c = make_chart();
    auto groups =
        beatbench::edit::group_selection(c, {ref(c.notes[0]), ref(c.notes[1])});  // 同拍、不同轨
    ASSERT_EQ(groups.size(), 2u);
    auto plan = beatbench::edit::plan_lane_moves(
        c, groups,
        {LaneTarget{{0, LaneKind::Key, 2}, false}, LaneTarget{{0, LaneKind::Key, 2}, false}},
        options());
    ASSERT_EQ(plan.moves.size(), 2u);
    EXPECT_TRUE(plan.moves[0].move) << reason_name(plan.moves[0].reason);
    EXPECT_FALSE(plan.moves[1].move);
    EXPECT_EQ(plan.moves[1].reason, LaneMoveReason::DuplicateTarget);
}

TEST(LanePlan, BgmTargetAutoPicksFreeSubLine) {
    Chart c = make_chart();
    c.notes.push_back(note(1, Rational(0, 1), {0, LaneKind::Bgm, 0}, 9));
    c.notes.back().value.sub_line = 0;
    auto groups = beatbench::edit::group_selection(c, {ref(c.notes[0])});
    auto plan = beatbench::edit::plan_lane_moves(
        c, groups, {LaneTarget{{0, LaneKind::Bgm, 0}, false}}, options());
    ASSERT_EQ(plan.moves.size(), 1u);
    ASSERT_TRUE(plan.moves[0].move) << reason_name(plan.moves[0].reason);
    EXPECT_EQ(plan.moves[0].sub_line, 1u);
}

TEST(LanePlan, HiddenAndModeInvalidTargets) {
    Chart c = make_chart();
    auto groups = beatbench::edit::group_selection(c, {ref(c.notes[0])});
    auto hidden_opts = options();
    hidden_opts.hidden_lanes.insert("lane:0:key:2");
    auto hidden = beatbench::edit::plan_lane_moves(
        c, groups, {LaneTarget{{0, LaneKind::Key, 2}, false}}, hidden_opts);
    ASSERT_EQ(hidden.moves.size(), 1u);
    EXPECT_FALSE(hidden.moves[0].move);
    EXPECT_EQ(hidden.moves[0].reason, LaneMoveReason::Hidden);

    Chart pms = make_chart();
    pms.mode_id = "pms9k";
    auto pms_groups = beatbench::edit::group_selection(pms, {ref(pms.notes[0])});
    auto scratch = beatbench::edit::plan_lane_moves(
        pms, pms_groups, {LaneTarget{{0, LaneKind::Scratch, 0}, false}}, options());
    ASSERT_EQ(scratch.moves.size(), 1u);
    EXPECT_FALSE(scratch.moves[0].move);
    EXPECT_EQ(scratch.moves[0].reason, LaneMoveReason::InvalidLane);

    auto player2 = beatbench::edit::plan_lane_moves(
        c, groups, {LaneTarget{{1, LaneKind::Key, 1}, false}}, options());
    ASSERT_EQ(player2.moves.size(), 1u);
    EXPECT_FALSE(player2.moves[0].move);
    EXPECT_EQ(player2.moves[0].reason, LaneMoveReason::InvalidLane);
}

TEST(LanePlan, LnMismatchSkipsWithoutMoving) {
    Chart c = make_ln_chart();
    auto groups = beatbench::edit::group_selection(c, {ref(c.notes[0])});
    auto opts = options();
    opts.require_ln_match = true;
    auto plan = beatbench::edit::plan_lane_moves(c, groups,
                                                 {LaneTarget{{0, LaneKind::Key, 2}, false}}, opts);
    ASSERT_EQ(plan.moves.size(), 1u);
    EXPECT_FALSE(plan.moves[0].move);
    EXPECT_EQ(plan.moves[0].reason, LaneMoveReason::LnMismatch);
}

TEST(LanePlan, LnPairMovesTogetherAndUndoRestores) {
    Chart c = make_ln_chart();
    auto groups = beatbench::edit::group_selection(c, {ref(c.notes[0])});
    auto opts = options();
    opts.require_ln_match = true;
    auto plan = beatbench::edit::plan_lane_moves(c, groups,
                                                 {LaneTarget{{0, LaneKind::Key, 2}, true}}, opts);
    ASSERT_EQ(plan.moves.size(), 1u);
    ASSERT_TRUE(plan.moves[0].move) << reason_name(plan.moves[0].reason);

    EditorSession s;
    s.load(c);
    ASSERT_TRUE(s.exec(beatbench::edit::make_lane_move_command(plan.moves[0])));
    ASSERT_EQ(s.chart().notes.size(), 2u);
    EXPECT_EQ(s.chart().notes[0].value.lane.index, 2u);
    EXPECT_EQ(s.chart().notes[1].value.lane.index, 2u);
    EXPECT_TRUE(s.chart().notes[0].value.ln_pair.has_value());
    ASSERT_TRUE(s.undo());
    EXPECT_EQ(s.chart().notes[0].value.lane.index, 1u);
    EXPECT_EQ(s.chart().notes[1].value.lane.index, 1u);
}

TEST(LanePlan, ChangesTimeCapability) {
    const Lane lane1{0, LaneKind::Key, 1};
    const Lane lane2{0, LaneKind::Key, 2};

    beatbench::edit::MoveNoteCommand lane_only(NoteRef{1, Rational(0, 1), lane1, 1}, 1,
                                               Rational(0, 1), lane2);
    EXPECT_FALSE(lane_only.changes_time());

    beatbench::edit::MoveNoteCommand time_move(NoteRef{1, Rational(0, 1), lane1, 1}, 2,
                                               Rational(0, 1));
    EXPECT_TRUE(time_move.changes_time());

    beatbench::edit::QuantizeNoteCommand quantize(NoteRef{1, Rational(1, 8), lane1, 1}, 1, 4);
    EXPECT_TRUE(quantize.changes_time());

    beatbench::edit::CompositeCommand composite;
    composite.add(
        std::make_unique<beatbench::edit::PutNoteCommand>(NoteRef{1, Rational(0, 1), lane1, 1}));
    EXPECT_FALSE(composite.changes_time());
    composite.add(std::make_unique<beatbench::edit::MoveNoteCommand>(
        NoteRef{1, Rational(0, 1), lane1, 1}, 2, Rational(0, 1)));
    EXPECT_TRUE(composite.changes_time());

    beatbench::edit::ShiftMarkerCommand shift(
        std::make_unique<beatbench::edit::PutNoteCommand>(NoteRef{1, Rational(0, 1), lane1, 1}),
        true, false);
    EXPECT_TRUE(shift.changes_time());
}
