// SPDX-License-Identifier: GPL-3.0-only
// LN/地雷放置测试：note.put 的 kind 语义（normal/ln/mine）。
// 用户确认（2026-09）：BMS 中 LN 与单点文件层无本质区别，识别是前端职责；
// core 提供 kind 语义扩展性 + LN 自动配对辅助。
#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <vector>

#include "beatbench/core/Chart.hpp"
#include "beatbench/core/codec/BmsChannelMaps.hpp"
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
    c.meta["LNTYPE"] = "1";
    return c;
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

// —— LN 放置（自动配对） ——

TEST(LnMinePut, PutLnHeadThenTailPairs) {
    EditorSession s;
    s.load(make_chart());
    // 放头（m1 pos0 key1 sample1，ln_kind）→ 无候选，作为未配对候选头
    ASSERT_TRUE(s.exec(std::make_unique<PutNoteCommand>(
        1, Rational(0, 1), Lane{0, LaneKind::Key, 1}, 1, true)));
    ASSERT_EQ(s.chart().notes.size(), 1u);
    EXPECT_FALSE(s.chart().notes[0].value.ln_pair.has_value());
    EXPECT_TRUE(ln_consistent(s.chart().notes));
    // 放尾（m1 pos1/2 key1 sample1，ln_kind）→ 自动配对
    ASSERT_TRUE(s.exec(std::make_unique<PutNoteCommand>(
        1, Rational(1, 2), Lane{0, LaneKind::Key, 1}, 1, true)));
    ASSERT_EQ(s.chart().notes.size(), 2u);
    EXPECT_TRUE(ln_consistent(s.chart().notes));
    // 头尾互指
    const auto& a = s.chart().notes[0];
    const auto& b = s.chart().notes[1];
    EXPECT_EQ(a.value.ln_pair.value_or(999), 1u);
    EXPECT_EQ(b.value.ln_pair.value_or(999), 0u);
}

TEST(LnMinePut, PutLnUndoRestoresUnpaired) {
    EditorSession s;
    s.load(make_chart());
    ASSERT_TRUE(s.exec(std::make_unique<PutNoteCommand>(
        1, Rational(0, 1), Lane{0, LaneKind::Key, 1}, 1, true)));
    ASSERT_TRUE(s.exec(std::make_unique<PutNoteCommand>(
        1, Rational(1, 2), Lane{0, LaneKind::Key, 1}, 1, true)));
    ASSERT_TRUE(ln_consistent(s.chart().notes));
    // undo 尾 → 头解除配对，尾消失
    ASSERT_TRUE(s.undo());
    ASSERT_EQ(s.chart().notes.size(), 1u);
    EXPECT_FALSE(s.chart().notes[0].value.ln_pair.has_value());
    EXPECT_TRUE(ln_consistent(s.chart().notes));
    // undo 头 → 空
    ASSERT_TRUE(s.undo());
    EXPECT_TRUE(s.chart().notes.empty());
}

TEST(LnMinePut, PutLnDifferentSampleNoPair) {
    EditorSession s;
    s.load(make_chart());
    // 头 sample1，尾 sample2 → 不配对（不同采样 id 不是一对）
    ASSERT_TRUE(s.exec(std::make_unique<PutNoteCommand>(
        1, Rational(0, 1), Lane{0, LaneKind::Key, 1}, 1, true)));
    ASSERT_TRUE(s.exec(std::make_unique<PutNoteCommand>(
        1, Rational(1, 2), Lane{0, LaneKind::Key, 1}, 2, true)));
    ASSERT_EQ(s.chart().notes.size(), 2u);
    for (const auto& e : s.chart().notes) {
        EXPECT_FALSE(e.value.ln_pair.has_value());
    }
    EXPECT_TRUE(ln_consistent(s.chart().notes));
}

// —— 地雷放置 ——

TEST(LnMinePut, PutMineKind) {
    EditorSession s;
    s.load(make_chart());
    ASSERT_TRUE(s.exec(std::make_unique<PutNoteCommand>(
        1, Rational(0, 1), Lane{0, LaneKind::Key, 1}, 7, false, NoteKind::Landmine)));
    ASSERT_EQ(s.chart().notes.size(), 1u);
    EXPECT_EQ(s.chart().notes[0].value.kind, NoteKind::Landmine);
    // undo
    ASSERT_TRUE(s.undo());
    EXPECT_TRUE(s.chart().notes.empty());
}

// —— 写回验证（LN 走 5x 通道、地雷走 D 通道） ——

TEST(LnMinePut, WriterLnAndMineChannels) {
    using beatbench::bms::bms_channel_for_mode;
    // LN（LNTYPE 1）：1P key1 → 51
    EXPECT_EQ(bms_channel_for_mode("sp7k", Lane{0, LaneKind::Key, 1}, true, NoteKind::Normal),
              "51");
    // 地雷：1P key1 → D1
    EXPECT_EQ(bms_channel_for_mode("sp7k", Lane{0, LaneKind::Key, 1}, false,
                                   NoteKind::Landmine),
              "D1");
    // 2P 地雷 key2 → E2
    EXPECT_EQ(bms_channel_for_mode("sp7k", Lane{1, LaneKind::Key, 2}, false,
                                   NoteKind::Landmine),
              "E2");
}

// —— 协议 dispatch ——

TEST(LnMinePut, ProtocolPutMine) {
    auto& session = beatbench::edit::global_editor_session();
    session.load(make_chart());
    Json req = Json::object();
    req.set("command", "note.put");
    Json args = Json::object();
    args.set("measure", 1);
    Json pos = Json::object();
    pos.set("num", 0);
    pos.set("den", 1);
    args.set("pos", std::move(pos));
    Json lane = Json::object();
    lane.set("player", 0);
    lane.set("kind", "key");
    lane.set("index", 1);
    args.set("lane", std::move(lane));
    args.set("sample", 7);
    args.set("kind", "mine");
    req.set("args", std::move(args));
    const Json resp = global_registry().dispatch(req);
    ASSERT_TRUE(resp.at("ok").as_bool());
    EXPECT_EQ(resp.at("result").at("kind").as_str(), "mine");
    EXPECT_EQ(session.chart().notes[0].value.kind, NoteKind::Landmine);
}

TEST(LnMinePut, ProtocolPutLnPairs) {
    auto& session = beatbench::edit::global_editor_session();
    session.load(make_chart());
    // 头
    Json req = Json::object();
    req.set("command", "note.put");
    Json args = Json::object();
    args.set("measure", 1);
    Json pos = Json::object();
    pos.set("num", 0);
    pos.set("den", 1);
    args.set("pos", std::move(pos));
    Json lane = Json::object();
    lane.set("player", 0);
    lane.set("kind", "key");
    lane.set("index", 1);
    args.set("lane", std::move(lane));
    args.set("sample", 1);
    args.set("kind", "ln");
    req.set("args", std::move(args));
    global_registry().dispatch(req);
    // 尾
    req = Json::object();
    req.set("command", "note.put");
    args = Json::object();
    args.set("measure", 1);
    pos = Json::object();
    pos.set("num", 1);
    pos.set("den", 2);
    args.set("pos", std::move(pos));
    lane = Json::object();
    lane.set("player", 0);
    lane.set("kind", "key");
    lane.set("index", 1);
    args.set("lane", std::move(lane));
    args.set("sample", 1);
    args.set("kind", "ln");
    req.set("args", std::move(args));
    const Json resp2 = global_registry().dispatch(req);
    ASSERT_TRUE(resp2.at("ok").as_bool());
    EXPECT_TRUE(ln_consistent(session.chart().notes));
    ASSERT_EQ(session.chart().notes.size(), 2u);
    EXPECT_EQ(session.chart().notes[0].value.ln_pair.value_or(999), 1u);
}

TEST(LnMinePut, ProtocolPutBadKind) {
    auto& session = beatbench::edit::global_editor_session();
    session.load(make_chart());
    Json req = Json::object();
    req.set("command", "note.put");
    Json args = Json::object();
    args.set("measure", 1);
    Json pos = Json::object();
    pos.set("num", 0);
    pos.set("den", 1);
    args.set("pos", std::move(pos));
    args.set("lane", 1);
    args.set("sample", 1);
    args.set("kind", "bogus");
    req.set("args", std::move(args));
    const Json resp = global_registry().dispatch(req);
    EXPECT_FALSE(resp.at("ok").as_bool());
    EXPECT_EQ(resp.at("error").at("code").as_str(), "bad_args");
}

// —— LN 配对（最终规则 2026-09）：向前找最近一个未配对同 lane 同 sample Normal 头；
//    忽略中间其它通道/sample 的 note。遇到同通道但已配对/地雷 → 停止。 ——

// 核心（问题2）：头 A → key2 普通 note（中间）→ 尾 A，忽略中间其它通道 note 仍配对。

TEST(LnMinePut, CrossLaneMiddleStillPairs) {
    EditorSession s;
    s.load(make_chart());
    ASSERT_TRUE(s.exec(std::make_unique<PutNoteCommand>(
        1, Rational(0, 1), Lane{0, LaneKind::Key, 1}, 1, true)));   // 头 A
    ASSERT_TRUE(s.exec(std::make_unique<PutNoteCommand>(
        1, Rational(1, 2), Lane{0, LaneKind::Key, 2}, 1, true)));   // B（不同通道，普通候选）
    ASSERT_TRUE(s.exec(std::make_unique<PutNoteCommand>(
        1, Rational(3, 4), Lane{0, LaneKind::Key, 1}, 1, true)));   // 尾 A：忽略中间 B，配到 A 头
    ASSERT_EQ(s.chart().notes.size(), 3u);
    EXPECT_TRUE(ln_consistent(s.chart().notes));
    // 头 A（pos0）与尾 A（pos3/4）配对；B（key2）保持未配对
    bool a_paired_pair = false, a_tail = false, b_unpaired = false;
    for (const auto& e : s.chart().notes) {
        if (e.pos == Rational(0, 1) && e.value.lane == Lane{0, LaneKind::Key, 1})
            a_paired_pair = e.value.ln_pair.has_value();
        if (e.pos == Rational(3, 4) && e.value.lane == Lane{0, LaneKind::Key, 1})
            a_tail = e.value.ln_pair.has_value();
        if (e.pos == Rational(1, 2) && e.value.lane == Lane{0, LaneKind::Key, 2})
            b_unpaired = !e.value.ln_pair.has_value();
    }
    EXPECT_TRUE(a_paired_pair);
    EXPECT_TRUE(a_tail);
    EXPECT_TRUE(b_unpaired);
}

// 对照：同通道相邻头尾仍正常配对。

TEST(LnMinePut, AdjacentSameLaneStillPairs) {
    EditorSession s;
    s.load(make_chart());
    ASSERT_TRUE(s.exec(std::make_unique<PutNoteCommand>(
        1, Rational(0, 1), Lane{0, LaneKind::Key, 1}, 1, true)));
    ASSERT_TRUE(s.exec(std::make_unique<PutNoteCommand>(
        1, Rational(1, 2), Lane{0, LaneKind::Key, 1}, 1, true)));
    ASSERT_EQ(s.chart().notes.size(), 2u);
    EXPECT_TRUE(ln_consistent(s.chart().notes));
    EXPECT_EQ(s.chart().notes[0].value.ln_pair.value_or(999), 1u);
    EXPECT_EQ(s.chart().notes[1].value.ln_pair.value_or(999), 0u);
}

// 对照：中间隔一个不同通道普通 note（非 ln），忽略它仍配对（问题2 场景）。

TEST(LnMinePut, MiddleOtherLaneOrdinaryNoteStillPairs) {
    EditorSession s;
    s.load(make_chart());
    ASSERT_TRUE(s.exec(std::make_unique<PutNoteCommand>(
        1, Rational(0, 1), Lane{0, LaneKind::Key, 1}, 1, true)));
    ASSERT_TRUE(s.exec(std::make_unique<PutNoteCommand>(
        1, Rational(1, 2), Lane{0, LaneKind::Key, 2}, 1, false)));  // 普通 note（非 ln）
    ASSERT_TRUE(s.exec(std::make_unique<PutNoteCommand>(
        1, Rational(3, 4), Lane{0, LaneKind::Key, 1}, 1, true)));
    ASSERT_EQ(s.chart().notes.size(), 3u);
    EXPECT_TRUE(ln_consistent(s.chart().notes));
    // 两个 key1 note 配对（头 pos0 + 尾 pos3/4）
    auto find_pair = [&](const Rational& p) {
        for (const auto& e : s.chart().notes)
            if (e.pos == p && e.value.lane == Lane{0, LaneKind::Key, 1})
                return e.value.ln_pair.has_value();
        return false;
    };
    EXPECT_TRUE(find_pair(Rational(0, 1)));
    EXPECT_TRUE(find_pair(Rational(3, 4)));
}

// 对照：同通道已形成 LN（头尾配对）后再放第 3 个 → 停止不重复配（作为新头）。

TEST(LnMinePut, AlreadyPairedStopsRescan) {
    EditorSession s;
    s.load(make_chart());
    ASSERT_TRUE(s.exec(std::make_unique<PutNoteCommand>(
        1, Rational(0, 1), Lane{0, LaneKind::Key, 1}, 1, true)));   // 头
    ASSERT_TRUE(s.exec(std::make_unique<PutNoteCommand>(
        1, Rational(1, 2), Lane{0, LaneKind::Key, 1}, 1, true)));   // 尾（配成 LN）
    ASSERT_TRUE(s.exec(std::make_unique<PutNoteCommand>(
        1, Rational(3, 4), Lane{0, LaneKind::Key, 1}, 1, true)));   // 再放：向前遇到已配对 → 新头
    ASSERT_EQ(s.chart().notes.size(), 3u);
    EXPECT_TRUE(ln_consistent(s.chart().notes));
    // 第 3 个（pos3/4）未配对（不重复配）
    for (const auto& e : s.chart().notes)
        if (e.pos == Rational(3, 4) && e.value.lane == Lane{0, LaneKind::Key, 1})
            EXPECT_FALSE(e.value.ln_pair.has_value());
}


