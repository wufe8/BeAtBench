// SPDX-License-Identifier: GPL-3.0-only
// note.moveRegion（框选整段/多选统一位移）测试：selection + delta（统一时间位移）
// + 可选 to_lane（整组换轨）；各 note 相对位置不变、一个 undo 步。
// 与 note.quantize/note.transform 同族（selection 数组 + 规则 + CompositeCommand 一步 undo）。
// 依据 2026-09 M2/M3 对齐：「框选整段平移」= 区间规则型（非逐个精调，那属 note.move moves）。
#include <gtest/gtest.h>

#include <algorithm>
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

std::vector<std::tuple<std::uint32_t, Rational, Lane, std::uint32_t>> norm_notes(
    const std::vector<Event<Note>>& notes) {
    std::vector<std::tuple<std::uint32_t, Rational, Lane, std::uint32_t>> out;
    for (const auto& e : notes) {
        out.emplace_back(e.measure, e.pos, e.value.lane, e.value.sample.id);
    }
    std::sort(out.begin(), out.end());
    return out;
}

// 生成 selection JSON（模块级 refs；与 clipboard.copy 的 selection 一致）
Json selection_json(const std::vector<NoteRef>& refs) {
    Json arr = Json::array();
    for (const auto& r : refs) {
        Json item = Json::object();
        item.set("measure", static_cast<std::int64_t>(r.measure));
        Json pos = Json::object();
        pos.set("num", r.pos.num);
        pos.set("den", r.pos.den);
        item.set("pos", std::move(pos));
        Json lane = Json::object();
        lane.set("player", static_cast<std::int64_t>(r.lane.player));
        std::string kind = "key";
        if (r.lane.kind == LaneKind::Scratch) kind = "scratch";
        else if (r.lane.kind == LaneKind::Pedal) kind = "pedal";
        else if (r.lane.kind == LaneKind::Bgm) kind = "bgm";
        lane.set("kind", kind);
        lane.set("index", static_cast<std::int64_t>(r.lane.index));
        item.set("lane", std::move(lane));
        item.set("sample", static_cast<std::int64_t>(r.sample));
        arr.push_back(std::move(item));
    }
    return arr;
}

}  // namespace

// —— 统一时间位移（+2 小节 + 1/2 拍）：相对位置不变 ——

TEST(MoveRegion, ShiftAllByDelta) {
    using beatbench::cmd::global_registry;
    auto& session = beatbench::edit::global_editor_session();
    session.load(make_chart());

    Json req = Json::object();
    req.set("command", "note.moveRegion");
    Json args = Json::object();
    // selection：全部 3 个 note
    args.set("selection", selection_json({
        {1, Rational(0, 1), Lane{0, LaneKind::Key, 1}, 1},
        {1, Rational(1, 2), Lane{0, LaneKind::Key, 2}, 2},
        {2, Rational(0, 1), Lane{0, LaneKind::Key, 3}, 3},
    }));
    // delta：+2 小节 + 1/2 拍
    Json delta = Json::object();
    delta.set("measure", 2);
    Json dpos = Json::object();
    dpos.set("num", 1);
    dpos.set("den", 2);
    delta.set("pos", std::move(dpos));
    args.set("delta", std::move(delta));
    req.set("args", std::move(args));

    const Json resp = global_registry().dispatch(req);
    ASSERT_TRUE(resp.at("ok").as_bool()) << resp.dump();
    EXPECT_EQ(resp.at("result").at("notes").as_i64(), 3);
    EXPECT_EQ(session.undo_depth(), 1u);

    // 期望（delta +2m +1/2拍，并归一进位）：
    //   n1 m1 pos0 → m3 pos1/2 key1
    //   n2 m1 pos1/2 → m4 pos0 key2（1/2+1/2=1 进位到 m4）
    //   n3 m2 pos0 → m4 pos1/2 key3
    bool a = false, b = false, c_ok = false;
    for (const auto& e : session.chart().notes) {
        if (e.measure == 3 && e.pos == Rational(1, 2) &&
            e.value.lane == Lane{0, LaneKind::Key, 1} && e.value.sample.id == 1)
            a = true;
        if (e.measure == 4 && e.pos == Rational(0, 1) &&
            e.value.lane == Lane{0, LaneKind::Key, 2} && e.value.sample.id == 2)
            b = true;
        if (e.measure == 4 && e.pos == Rational(1, 2) &&
            e.value.lane == Lane{0, LaneKind::Key, 3} && e.value.sample.id == 3)
            c_ok = true;
    }
    EXPECT_TRUE(a);
    EXPECT_TRUE(b);
    EXPECT_TRUE(c_ok);

    // 一次 undo 全部回原位
    req = Json::object();
    req.set("command", "session.undo");
    const Json resp2 = global_registry().dispatch(req);
    ASSERT_TRUE(resp2.at("ok").as_bool()) << resp2.dump();
    EXPECT_EQ(norm_notes(session.chart().notes), norm_notes(make_chart().notes));
}

// —— 统一位移 + 整组换轨（drag 横向） ——

TEST(MoveRegion, ShiftAndChangeLane) {
    using beatbench::cmd::global_registry;
    auto& session = beatbench::edit::global_editor_session();
    session.load(make_chart());

    Json req = Json::object();
    req.set("command", "note.moveRegion");
    Json args = Json::object();
    args.set("selection", selection_json({
        {1, Rational(0, 1), Lane{0, LaneKind::Key, 1}, 1},
        {1, Rational(1, 2), Lane{0, LaneKind::Key, 2}, 2},
    }));
    Json delta = Json::object();
    delta.set("measure", 1);
    Json dpos = Json::object();
    dpos.set("num", 0);
    dpos.set("den", 1);
    delta.set("pos", std::move(dpos));
    args.set("delta", std::move(delta));
    Json to_lane = Json::object();
    to_lane.set("player", 0);
    to_lane.set("kind", "key");
    to_lane.set("index", 5);
    args.set("to_lane", std::move(to_lane));
    req.set("args", std::move(args));

    const Json resp = global_registry().dispatch(req);
    ASSERT_TRUE(resp.at("ok").as_bool()) << resp.dump();
    EXPECT_EQ(session.undo_depth(), 1u);

    bool a = false, b = false;
    for (const auto& e : session.chart().notes) {
        if (e.measure == 2 && e.pos == Rational(0, 1) &&
            e.value.lane == Lane{0, LaneKind::Key, 5} && e.value.sample.id == 1)
            a = true;
        if (e.measure == 2 && e.pos == Rational(1, 2) &&
            e.value.lane == Lane{0, LaneKind::Key, 5} && e.value.sample.id == 2)
            b = true;
    }
    EXPECT_TRUE(a);
    EXPECT_TRUE(b);
}

// —— 空 selection 拒绝 ——

TEST(MoveRegion, EmptySelectionRejected) {
    using beatbench::cmd::global_registry;
    auto& session = beatbench::edit::global_editor_session();
    session.load(make_chart());
    Json req = Json::object();
    req.set("command", "note.moveRegion");
    Json args = Json::object();
    args.set("selection", Json::array());
    Json delta = Json::object();
    delta.set("measure", 1);
    Json dpos = Json::object();
    dpos.set("num", 0);
    dpos.set("den", 1);
    delta.set("pos", std::move(dpos));
    args.set("delta", std::move(delta));
    req.set("args", std::move(args));
    const Json resp = global_registry().dispatch(req);
    EXPECT_FALSE(resp.at("ok").as_bool());
    EXPECT_EQ(resp.at("error").at("code").as_str(), "empty_selection");
}

// —— 缺 delta 拒绝 ——

TEST(MoveRegion, MissingDeltaRejected) {
    using beatbench::cmd::global_registry;
    auto& session = beatbench::edit::global_editor_session();
    session.load(make_chart());
    Json req = Json::object();
    req.set("command", "note.moveRegion");
    Json args = Json::object();
    args.set("selection", selection_json({
        {1, Rational(0, 1), Lane{0, LaneKind::Key, 1}, 1},
    }));
    req.set("args", std::move(args));
    const Json resp = global_registry().dispatch(req);
    EXPECT_FALSE(resp.at("ok").as_bool());
    EXPECT_EQ(resp.at("error").at("code").as_str(), "bad_args");
}
