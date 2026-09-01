// SPDX-License-Identifier: GPL-3.0-only
// LN 多选移动测试（2026-09 用户反馈问题4）：「LN 选取」模式下点任一段自动选两端，
// 拖动后两端一起移动、相对位置不变、**互指保持（写回仍走 51-59/61-69 LN 通道）**。
// 根因：多选统一位移对 LN 两端逐端单 note 模式移动 → 逐端重连不稳定 → 断链变普通 note。
#include <gtest/gtest.h>

#include <algorithm>
#include <memory>
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

// LN 对：头 m1 pos0 + 尾 m1 pos1/2，同 lane key1 同 sample，互指
Chart ln_chart() {
    Chart c;
    c.meta["TITLE"] = "LN";
    c.meta["PLAYER"] = "1";
    c.meta["BPM"] = "130";
    Event<Note> head{1, Rational(0, 1), {}};
    head.value.lane = {0, LaneKind::Key, 1};
    head.value.sample.id = 1;
    head.value.ln_channel = true;
    Event<Note> tail{1, Rational(1, 2), {}};
    tail.value.lane = {0, LaneKind::Key, 1};
    tail.value.sample.id = 1;
    tail.value.ln_channel = true;
    head.value.ln_pair = 1;
    tail.value.ln_pair = 0;
    c.notes = {head, tail};
    return c;
}

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
        if (r.sub_line != 0) item.set("sub_line", static_cast<std::int64_t>(r.sub_line));
        arr.push_back(std::move(item));
    }
    return arr;
}

}  // namespace

// —— 多选（LN 头尾都在 selection）统一位移：两端一起动、互指保持 ——

TEST(LnMoveRegion, BothSelectedKeepsPair) {
    using beatbench::cmd::global_registry;
    auto& session = beatbench::edit::global_editor_session();
    session.load(ln_chart());

    Json req = Json::object();
    req.set("command", "note.moveRegion");
    Json args = Json::object();
    args.set("selection", selection_json({
        {1, Rational(0, 1), Lane{0, LaneKind::Key, 1}, 1},
        {1, Rational(1, 2), Lane{0, LaneKind::Key, 1}, 1},
    }));
    Json delta = Json::object();
    delta.set("measure", 1);
    Json dpos = Json::object();
    dpos.set("num", 0);
    dpos.set("den", 1);
    delta.set("pos", std::move(dpos));
    args.set("delta", std::move(delta));
    req.set("args", std::move(args));

    const Json resp = global_registry().dispatch(req);
    ASSERT_TRUE(resp.at("ok").as_bool()) << resp.dump();
    EXPECT_EQ(session.undo_depth(), 1u);

    // 两端都在 m2（pos 0 与 1/2），互指保持
    const auto& notes = session.chart().notes;
    ASSERT_EQ(notes.size(), 2u);
    EXPECT_TRUE(ln_consistent(notes));
    bool head_ok = false, tail_ok = false;
    for (const auto& e : notes) {
        EXPECT_EQ(e.measure, 2u);
        if (e.pos == Rational(0, 1)) head_ok = e.value.ln_pair.has_value();
        if (e.pos == Rational(1, 2)) tail_ok = e.value.ln_pair.has_value();
    }
    EXPECT_TRUE(head_ok);
    EXPECT_TRUE(tail_ok);

    // 一次 undo 还原
    req = Json::object();
    req.set("command", "session.undo");
    const Json resp2 = global_registry().dispatch(req);
    ASSERT_TRUE(resp2.at("ok").as_bool()) << resp2.dump();
    EXPECT_TRUE(ln_consistent(session.chart().notes));
    EXPECT_EQ(session.chart().notes.size(), 2u);
}

// —— 多选 + 跨通道（LN 头尾都选中，整体换轨到 key5）：两端同轨、互指保持 ——

TEST(LnMoveRegion, BothSelectedCrossLaneKeepsPair) {
    using beatbench::cmd::global_registry;
    auto& session = beatbench::edit::global_editor_session();
    session.load(ln_chart());

    Json req = Json::object();
    req.set("command", "note.moveRegion");
    Json args = Json::object();
    args.set("selection", selection_json({
        {1, Rational(0, 1), Lane{0, LaneKind::Key, 1}, 1},
        {1, Rational(1, 2), Lane{0, LaneKind::Key, 1}, 1},
    }));
    Json delta = Json::object();
    delta.set("measure", 0);
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

    const auto& notes = session.chart().notes;
    ASSERT_EQ(notes.size(), 2u);
    EXPECT_TRUE(ln_consistent(notes));
    for (const auto& e : notes) {
        EXPECT_EQ(e.value.lane, (Lane{0, LaneKind::Key, 5}));  // 两端同轨（LN 头尾恒同轨道）
    }
}
