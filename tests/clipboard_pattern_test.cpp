// SPDX-License-Identifier: GPL-3.0-only
// 配置复制（issue #47）：clipboard.copy pattern + clipboard.applyPattern 的原子预检/部分套用/LN。
#include <gtest/gtest.h>

#include <algorithm>
#include <string>

#include "beatbench/core/Chart.hpp"
#include "beatbench/core/command/Command.hpp"
#include "beatbench/core/edit/SessionRegistry.hpp"

using namespace beatbench;
using beatbench::cmd::global_registry;
using beatbench::edit::global_editor_session;
using beatbench::json::Json;

namespace {

Event<Note> note(std::uint32_t measure, Rational pos, Lane lane, std::uint32_t sample,
                 bool ln = false) {
    Event<Note> e{measure, pos, {}};
    e.value.lane = lane;
    e.value.sample.id = sample;
    e.value.ln_channel = ln;
    return e;
}

Json sel_item(const Event<Note>& e) {
    Json o = Json::object();
    o.set("measure", static_cast<std::int64_t>(e.measure));
    Json p = Json::object();
    p.set("num", e.pos.num);
    p.set("den", e.pos.den);
    o.set("pos", std::move(p));
    Json l = Json::object();
    l.set("player", static_cast<std::int64_t>(e.value.lane.player));
    l.set("kind", e.value.lane.kind == LaneKind::Scratch ? "scratch"
                  : e.value.lane.kind == LaneKind::Bgm   ? "bgm"
                  : e.value.lane.kind == LaneKind::Pedal ? "pedal"
                                                         : "key");
    l.set("index", static_cast<std::int64_t>(e.value.lane.index));
    o.set("lane", std::move(l));
    o.set("sample", static_cast<std::int64_t>(e.value.sample.id));
    o.set("sub_line", static_cast<std::int64_t>(e.value.sub_line));
    return o;
}

Json request(std::string_view command, Json args) {
    Json req = Json::object();
    req.set("command", std::string(command));
    req.set("args", std::move(args));
    return req;
}

Json selection_for(const Chart& c, std::initializer_list<std::size_t> idxs) {
    Json arr = Json::array();
    for (const auto i : idxs) arr.push_back(sel_item(c.notes[i]));
    return arr;
}

Chart target_chart() {
    Chart c;
    c.mode_id = "sp7k";
    c.meta["BPM"] = "130";
    c.notes.push_back(note(5, Rational(0, 1), {0, LaneKind::Key, 3}, 3));
    c.notes.push_back(note(5, Rational(1, 2), {0, LaneKind::Key, 4}, 4));
    return c;
}

Chart source_chart() {
    Chart c;
    c.mode_id = "sp7k";
    c.meta["BPM"] = "130";
    c.notes.push_back(note(1, Rational(0, 1), {0, LaneKind::Key, 1}, 1));
    c.notes.push_back(note(1, Rational(1, 2), {0, LaneKind::Key, 2}, 2));
    return c;
}

Json copy_pattern(const Chart& source) {
    global_editor_session().load(source);
    Json args = Json::object();
    args.set("selection", selection_for(source, {0, 1}));
    const Json resp = global_registry().dispatch(request("clipboard.copy", std::move(args)));
    EXPECT_TRUE(resp.at("ok").as_bool()) << resp.dump();
    return resp.at("result").at("pattern");
}

Chart ln_chart(std::uint8_t lane) {
    Chart c;
    c.mode_id = "sp7k";
    c.meta["BPM"] = "130";
    c.notes.push_back(note(1, Rational(0, 1), {0, LaneKind::Key, lane}, 7, true));
    c.notes.push_back(note(2, Rational(0, 1), {0, LaneKind::Key, lane}, 7, true));
    c.notes[0].value.ln_pair = 1;
    c.notes[1].value.ln_pair = 0;
    return c;
}

}  // namespace

Json pattern_item(Lane lane, bool ln = false, std::uint32_t sub_line = 0) {
    Json item = Json::object();
    item.set("measure", static_cast<std::int64_t>(1));
    Json p = Json::object();
    p.set("num", static_cast<std::int64_t>(0));
    p.set("den", static_cast<std::int64_t>(1));
    item.set("pos", std::move(p));
    Json l = Json::object();
    l.set("player", static_cast<std::int64_t>(lane.player));
    l.set("kind", lane.kind == LaneKind::Scratch ? "scratch"
                  : lane.kind == LaneKind::Bgm   ? "bgm"
                  : lane.kind == LaneKind::Pedal ? "pedal"
                                                 : "key");
    l.set("index", static_cast<std::int64_t>(lane.index));
    item.set("lane", std::move(l));
    item.set("sub_line", static_cast<std::int64_t>(sub_line));
    item.set("ln", ln);
    return item;
}

TEST(ClipboardPattern, CopyEmitsOrderedLanePattern) {
    const Json pattern = copy_pattern(source_chart());
    ASSERT_TRUE(pattern.is_array());
    ASSERT_EQ(pattern.size(), 2u);
    EXPECT_EQ(pattern.as_array()[0].at("lane").at("kind").as_str(), std::string("key"));
    EXPECT_EQ(pattern.as_array()[0].at("lane").at("index").as_i64(), 1);
    EXPECT_EQ(pattern.as_array()[1].at("lane").at("index").as_i64(), 2);
}

TEST(ClipboardPattern, AppliesLaneSequenceAndPreservesTimeSample) {
    const Json pattern = copy_pattern(source_chart());
    global_editor_session().load(target_chart());
    Json args = Json::object();
    args.set("selection", selection_for(global_editor_session().chart(), {0, 1}));
    args.set("pattern", pattern);
    const Json resp =
        global_registry().dispatch(request("clipboard.applyPattern", std::move(args)));
    ASSERT_TRUE(resp.at("ok").as_bool()) << resp.dump();
    const auto& notes = global_editor_session().chart().notes;
    ASSERT_EQ(notes.size(), 2u);
    EXPECT_EQ(notes[0].value.lane.index, 1);
    EXPECT_EQ(notes[1].value.lane.index, 2);
    EXPECT_EQ(notes[0].value.sample.id, 3u);
    EXPECT_EQ(notes[1].value.sample.id, 4u);
    EXPECT_EQ(notes[0].measure, 5u);
    EXPECT_EQ(notes[1].pos, Rational(1, 2));
    EXPECT_TRUE(global_editor_session().undo());
    EXPECT_EQ(global_editor_session().chart().notes[0].value.lane.index, 3);
}

TEST(ClipboardPattern, PartialTargetsOnlyApplyFirstN) {
    const Json pattern = copy_pattern(source_chart());
    global_editor_session().load(target_chart());
    Json args = Json::object();
    args.set("selection", selection_for(global_editor_session().chart(), {1}));
    args.set("pattern", pattern);
    const Json resp =
        global_registry().dispatch(request("clipboard.applyPattern", std::move(args)));
    ASSERT_TRUE(resp.at("ok").as_bool()) << resp.dump();
    EXPECT_EQ(resp.at("result").at("moved").as_i64(), 1);
    const auto& notes = global_editor_session().chart().notes;
    ASSERT_EQ(notes.size(), 2u);
    EXPECT_EQ(notes[0].value.lane.index, 3);  // 未选目标不动
    EXPECT_EQ(notes[1].value.lane.index, 1);  // 第一个目标套 pattern[0]
}

TEST(ClipboardPattern, CollisionFailsAtomically) {
    const Json pattern = copy_pattern(source_chart());
    Chart target = target_chart();
    target.notes.push_back(note(5, Rational(0, 1), {0, LaneKind::Key, 1}, 9));
    global_editor_session().load(target);
    const auto before = global_editor_session().chart().notes.size();
    const auto undo_before = global_editor_session().undo_depth();
    Json args = Json::object();
    args.set("selection", selection_for(global_editor_session().chart(), {0, 1}));
    args.set("pattern", pattern);
    const Json resp =
        global_registry().dispatch(request("clipboard.applyPattern", std::move(args)));
    ASSERT_FALSE(resp.at("ok").as_bool()) << resp.dump();
    EXPECT_EQ(resp.at("error").at("code").as_str(), std::string("target_occupied"));
    EXPECT_EQ(global_editor_session().chart().notes.size(), before);
    EXPECT_EQ(global_editor_session().undo_depth(), undo_before);
}

TEST(ClipboardPattern, ExternalTextPatternMovesTarget) {
    global_editor_session().load(target_chart());
    Json args = Json::object();
    args.set("selection", selection_for(global_editor_session().chart(), {0}));
    args.set("text", "#00111:0100");
    const Json resp =
        global_registry().dispatch(request("clipboard.applyPattern", std::move(args)));
    ASSERT_TRUE(resp.at("ok").as_bool()) << resp.dump();
    EXPECT_EQ(global_editor_session().chart().notes[0].value.lane.index, 1);
}

TEST(ClipboardPattern, ExternalTextLnObjImplicitTailPairsCorrectly) {
    // LNOBJ 隐式尾：parser 生成完整 ln_pair，pattern 侧两两配对不再错位
    global_editor_session().load(ln_chart(1));
    Json args = Json::object();
    args.set("selection", selection_for(global_editor_session().chart(), {0, 1}));
    // 普通通道两个同 sample 物件 + #LNOBJ 01：第二个物件是前一个的尾。
    args.set("text", std::string("#LNOBJ 01\n#00111:0100\n#00211:0100"));
    const Json resp =
        global_registry().dispatch(request("clipboard.applyPattern", std::move(args)));
    ASSERT_TRUE(resp.at("ok").as_bool()) << resp.dump();
    const auto& notes = global_editor_session().chart().notes;
    ASSERT_EQ(notes.size(), 2u);
    ASSERT_TRUE(notes[0].value.ln_pair.has_value());
    ASSERT_TRUE(notes[1].value.ln_pair.has_value());
    EXPECT_EQ(notes[0].value.lane.index, 1);
    EXPECT_EQ(notes[1].value.lane.index, 1);
}

TEST(ClipboardPattern, LnPairGroupsMoveTogetherAndUndoRestores) {
    const Json pattern = copy_pattern(ln_chart(1));
    global_editor_session().load(ln_chart(3));
    Json args = Json::object();
    args.set("selection", selection_for(global_editor_session().chart(), {0, 1}));
    args.set("pattern", pattern);
    const Json resp =
        global_registry().dispatch(request("clipboard.applyPattern", std::move(args)));
    ASSERT_TRUE(resp.at("ok").as_bool()) << resp.dump();
    const auto& notes = global_editor_session().chart().notes;
    ASSERT_EQ(notes.size(), 2u);
    EXPECT_EQ(notes[0].value.lane.index, 1);
    EXPECT_EQ(notes[1].value.lane.index, 1);
    ASSERT_TRUE(notes[0].value.ln_pair.has_value());
    ASSERT_TRUE(notes[1].value.ln_pair.has_value());

    ASSERT_TRUE(global_editor_session().undo());
    EXPECT_EQ(global_editor_session().chart().notes[0].value.lane.index, 3);
    EXPECT_EQ(global_editor_session().chart().notes[1].value.lane.index, 3);
}
TEST(ClipboardPattern, StationarySelectedTargetBlocksAtomically) {
    Chart c;
    c.mode_id = "sp7k";
    c.meta["BPM"] = "130";
    c.notes.push_back(note(5, Rational(0, 1), {0, LaneKind::Key, 1}, 3));
    c.notes.push_back(note(5, Rational(0, 1), {0, LaneKind::Key, 3}, 4));
    global_editor_session().load(c);
    const auto undo_before = global_editor_session().undo_depth();
    Json pattern = Json::array();
    pattern.push_back(pattern_item({0, LaneKind::Key, 3}));
    Json args = Json::object();
    args.set("selection", selection_for(global_editor_session().chart(), {0, 1}));
    args.set("pattern", std::move(pattern));
    const Json resp =
        global_registry().dispatch(request("clipboard.applyPattern", std::move(args)));
    ASSERT_FALSE(resp.at("ok").as_bool()) << resp.dump();
    EXPECT_EQ(resp.at("error").at("code").as_str(), std::string("target_occupied"));
    EXPECT_EQ(global_editor_session().undo_depth(), undo_before);
    EXPECT_EQ(global_editor_session().chart().notes[0].value.lane.index, 1);
    EXPECT_EQ(global_editor_session().chart().notes[1].value.lane.index, 3);
}

TEST(ClipboardPattern, BgmTargetAutoPicksFreeSubLine) {
    Chart c;
    c.mode_id = "sp7k";
    c.meta["BPM"] = "130";
    c.notes.push_back(note(5, Rational(0, 1), {0, LaneKind::Key, 1}, 3));
    c.notes.push_back(note(5, Rational(0, 1), {0, LaneKind::Bgm, 0}, 9));
    c.notes[1].value.sub_line = 0;
    global_editor_session().load(c);
    Json pattern = Json::array();
    pattern.push_back(pattern_item({0, LaneKind::Bgm, 0}));
    Json args = Json::object();
    args.set("selection", selection_for(global_editor_session().chart(), {0}));
    args.set("pattern", std::move(pattern));
    const Json resp =
        global_registry().dispatch(request("clipboard.applyPattern", std::move(args)));
    ASSERT_TRUE(resp.at("ok").as_bool()) << resp.dump();
    EXPECT_EQ(resp.at("result").at("moved").as_i64(), 1);
    const auto& notes = global_editor_session().chart().notes;
    bool found = false;
    for (const auto& e : notes) {
        if (e.value.sample.id == 3) {
            found = true;
            EXPECT_EQ(e.value.lane.kind, LaneKind::Bgm);
            EXPECT_EQ(e.value.sub_line, 1u);
        }
    }
    EXPECT_TRUE(found);
}

TEST(ClipboardPattern, HiddenTargetBlocksAtomically) {
    global_editor_session().load(target_chart());
    Json pattern = Json::array();
    pattern.push_back(pattern_item({0, LaneKind::Key, 1}));
    Json args = Json::object();
    args.set("selection", selection_for(global_editor_session().chart(), {0}));
    args.set("pattern", std::move(pattern));
    Json hidden = Json::array();
    hidden.push_back("lane:0:key:1");
    args.set("hidden_lanes", std::move(hidden));
    const Json resp =
        global_registry().dispatch(request("clipboard.applyPattern", std::move(args)));
    ASSERT_FALSE(resp.at("ok").as_bool()) << resp.dump();
    EXPECT_EQ(resp.at("error").at("code").as_str(), std::string("target_hidden"));
}

TEST(ClipboardPattern, LnMismatchSkipsWithoutMoving) {
    global_editor_session().load(ln_chart(3));
    Json pattern = Json::array();
    pattern.push_back(pattern_item({0, LaneKind::Key, 1}, false));
    Json args = Json::object();
    args.set("selection", selection_for(global_editor_session().chart(), {0, 1}));
    args.set("pattern", std::move(pattern));
    const Json resp =
        global_registry().dispatch(request("clipboard.applyPattern", std::move(args)));
    ASSERT_TRUE(resp.at("ok").as_bool()) << resp.dump();
    EXPECT_EQ(resp.at("result").at("moved").as_i64(), 0);
    EXPECT_EQ(resp.at("result").at("skipped").as_i64(), 1);
    EXPECT_EQ(global_editor_session().chart().notes[0].value.lane.index, 3);
}

TEST(ClipboardPattern, OddLnPatternRejectsAtomically) {
    global_editor_session().load(target_chart());
    const auto before = global_editor_session().chart().notes.size();
    Json args = Json::object();
    args.set("selection", selection_for(global_editor_session().chart(), {0}));
    // 3 个 LN 槽：头、尾、孤立头 → 无法成组，必须整批拒绝。
    args.set("text", std::string("#00151:01\n#00251:01\n#00351:01"));
    const Json resp =
        global_registry().dispatch(request("clipboard.applyPattern", std::move(args)));
    ASSERT_FALSE(resp.at("ok").as_bool()) << resp.dump();
    EXPECT_EQ(resp.at("error").at("code").as_str(), std::string("incomplete_ln"));
    EXPECT_EQ(global_editor_session().chart().notes.size(), before);
}
