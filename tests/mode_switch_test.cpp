// SPDX-License-Identifier: GPL-3.0-only
// chart.set_mode（issue #26 阶段二安全子集）：可表示性预检、dry-run、apply/invert、拒绝不丢数据。
#include <gtest/gtest.h>

#include "beatbench/core/Chart.hpp"
#include "beatbench/core/command/Command.hpp"
#include "beatbench/core/edit/SessionRegistry.hpp"

using namespace beatbench;
using beatbench::cmd::global_registry;
using beatbench::edit::global_editor_session;
using beatbench::json::Json;

namespace {

Event<Note> note(std::uint32_t measure, Rational pos, Lane lane, std::uint32_t sample) {
    Event<Note> e{measure, pos, {}};
    e.value.lane = lane;
    e.value.sample.id = sample;
    return e;
}

Chart chart_with(Lane extra) {
    Chart c;
    c.mode_id = "sp7k";
    c.meta["BPM"] = "130";
    c.notes.push_back(note(1, Rational(0, 1), {0, LaneKind::Key, 1}, 1));
    c.notes.push_back(note(1, Rational(1, 2), extra, 2));
    return c;
}

Json req(const char* command, Json args) {
    Json r = Json::object();
    r.set("command", command);
    r.set("args", std::move(args));
    return r;
}

}  // namespace

TEST(ModeSwitch, DryRunCountsUnrepresentableNotes) {
    global_editor_session().load(chart_with({0, LaneKind::Scratch, 0}));
    Json args = Json::object();
    args.set("mode", "pms9k");
    args.set("dry_run", true);
    const Json resp = global_registry().dispatch(req("chart.set_mode", std::move(args)));
    ASSERT_TRUE(resp.at("ok").as_bool()) << resp.dump();
    EXPECT_EQ(resp.at("result").at("unsupported").as_i64(), 1);
    EXPECT_EQ(resp.at("result").at("supported").as_i64(), 1);
}

TEST(ModeSwitch, SwitchesAndUndoRestores) {
    global_editor_session().load(chart_with({0, LaneKind::Key, 7}));
    Json args = Json::object();
    args.set("mode", "pms9k");
    const Json resp = global_registry().dispatch(req("chart.set_mode", std::move(args)));
    ASSERT_TRUE(resp.at("ok").as_bool()) << resp.dump();
    EXPECT_EQ(global_editor_session().chart().mode_id.value_or(""), std::string("pms9k"));
    ASSERT_TRUE(global_editor_session().undo());
    EXPECT_EQ(global_editor_session().chart().mode_id.value_or(""), std::string("sp7k"));
}

TEST(ModeSwitch, UnrepresentableIsRejectedAtomically) {
    global_editor_session().load(chart_with({0, LaneKind::Scratch, 0}));
    const auto before_mode = global_editor_session().chart().mode_id.value_or("");
    const auto undo_before = global_editor_session().undo_depth();
    Json args = Json::object();
    args.set("mode", "pms9k");
    const Json resp = global_registry().dispatch(req("chart.set_mode", std::move(args)));
    ASSERT_FALSE(resp.at("ok").as_bool()) << resp.dump();
    EXPECT_EQ(resp.at("error").at("code").as_str(), std::string("mode_unrepresentable"));
    EXPECT_EQ(global_editor_session().chart().mode_id.value_or(""), before_mode);
    EXPECT_EQ(global_editor_session().undo_depth(), undo_before);
}

TEST(ModeSwitch, UpdatesPlayerAndUndoRestores) {
    Chart c = chart_with({0, LaneKind::Key, 7});
    c.meta["PLAYER"] = "1";
    global_editor_session().load(std::move(c));
    Json args = Json::object();
    args.set("mode", "dp");
    const Json resp = global_registry().dispatch(req("chart.set_mode", std::move(args)));
    ASSERT_TRUE(resp.at("ok").as_bool()) << resp.dump();
    EXPECT_EQ(global_editor_session().chart().meta.at("PLAYER"), std::string("3"));
    ASSERT_TRUE(global_editor_session().undo());
    EXPECT_EQ(global_editor_session().chart().meta.at("PLAYER"), std::string("1"));
    EXPECT_EQ(global_editor_session().chart().mode_id.value_or(""), std::string("sp7k"));

    Json battle = Json::object();
    battle.set("mode", "battle");
    const Json bresp = global_registry().dispatch(req("chart.set_mode", std::move(battle)));
    ASSERT_TRUE(bresp.at("ok").as_bool()) << bresp.dump();
    EXPECT_EQ(global_editor_session().chart().meta.at("PLAYER"), std::string("4"));
}

TEST(ModeSwitch, RestoresMissingPlayerOnUndo) {
    Chart c = chart_with({0, LaneKind::Key, 7});
    c.meta.erase("PLAYER");
    global_editor_session().load(std::move(c));
    Json args = Json::object();
    args.set("mode", "dp");
    ASSERT_TRUE(
        global_registry().dispatch(req("chart.set_mode", std::move(args))).at("ok").as_bool());
    EXPECT_EQ(global_editor_session().chart().meta.at("PLAYER"), std::string("3"));
    ASSERT_TRUE(global_editor_session().undo());
    EXPECT_EQ(global_editor_session().chart().meta.count("PLAYER"), 0u);
}

TEST(ModeSwitch, NoOpDoesNotPushUndo) {
    global_editor_session().load(chart_with({0, LaneKind::Key, 7}));
    const auto undo_before = global_editor_session().undo_depth();
    Json args = Json::object();
    args.set("mode", "sp7k");
    const Json resp = global_registry().dispatch(req("chart.set_mode", std::move(args)));
    ASSERT_TRUE(resp.at("ok").as_bool()) << resp.dump();
    EXPECT_TRUE(resp.at("result").at("unchanged").as_bool());
    EXPECT_EQ(global_editor_session().undo_depth(), undo_before);
}
