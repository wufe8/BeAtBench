// SPDX-License-Identifier: GPL-3.0-only
// PMS 别名收敛后的编辑/序列化保真与非法 Lane 安全回归。
#include <gtest/gtest.h>

#include <algorithm>
#include <string>
#include <tuple>
#include <vector>

#include "beatbench/core/bms/BmsCodec.hpp"
#include "beatbench/core/codec/BmsChannelMaps.hpp"
#include "beatbench/core/command/Builtins.hpp"
#include "beatbench/core/command/Command.hpp"
#include "beatbench/core/edit/EditorSession.hpp"
#include "beatbench/core/edit/SessionRegistry.hpp"

using namespace beatbench;
using beatbench::json::Json;

namespace {
Chart read_pms_integrity(const std::string& text) {
    bms::BmsReadOptions options;
    options.mode = "pms9k";
    return bms::read_bms(text, options).chart;
}
using Signature = std::tuple<std::uint32_t, Rational, Lane, std::uint32_t, NoteKind, bool>;
std::vector<std::pair<Signature, Signature>> pair_signatures(const Chart& chart) {
    std::vector<std::pair<Signature, Signature>> result;
    const auto sig = [](const auto& e) -> Signature {
        return {e.measure,         e.pos,        e.value.lane,
                e.value.sample.id, e.value.kind, e.value.ln_channel};
    };
    for (std::size_t i = 0; i < chart.notes.size(); ++i) {
        const auto pair = chart.notes[i].value.ln_pair;
        if (!pair || *pair <= i || *pair >= chart.notes.size()) continue;
        auto a = sig(chart.notes[i]), b = sig(chart.notes[*pair]);
        if (b < a) std::swap(a, b);
        result.emplace_back(std::move(a), std::move(b));
    }
    std::sort(result.begin(), result.end());
    return result;
}
std::vector<Signature> signatures(const Chart& chart) {
    std::vector<Signature> result;
    for (const auto& e : chart.notes)
        result.emplace_back(e.measure, e.pos, e.value.lane, e.value.sample.id, e.value.kind,
                            e.value.ln_channel);
    std::sort(result.begin(), result.end());
    return result;
}
Json lane_json(const Lane& lane) {
    Json j = Json::object();
    j.set("player", static_cast<int>(lane.player));
    j.set("index", static_cast<int>(lane.index));
    const char* kind = lane.kind == LaneKind::Key       ? "key"
                       : lane.kind == LaneKind::Scratch ? "scratch"
                       : lane.kind == LaneKind::Pedal   ? "pedal"
                                                        : "bgm";
    j.set("kind", kind);
    return j;
}
Json selection_json(const Chart& chart) {
    Json result = Json::array();
    for (const auto& e : chart.notes) {
        Json j = Json::object();
        j.set("measure", static_cast<std::int64_t>(e.measure));
        Json pos = Json::array();
        pos.push_back(e.pos.num);
        pos.push_back(e.pos.den);
        j.set("pos", std::move(pos));
        j.set("lane", lane_json(e.value.lane));
        j.set("sample", static_cast<std::int64_t>(e.value.sample.id));
        j.set("sub_line", static_cast<std::int64_t>(e.value.sub_line));
        result.push_back(std::move(j));
    }
    return result;
}
Json call(cmd::Registry& registry, const char* command, Json args) {
    Json request = Json::object();
    request.set("command", command);
    request.set("args", std::move(args));
    return registry.dispatch(request);
}
}  // namespace

TEST(PmsIntegrity, MappingRejectsInvalidLanesBeforeIndexing) {
    for (const auto* mode : {"pms9k", "pms9k-bmesp"}) {
        for (const auto player : {0, 1, 2, 255}) {
            for (const auto index : {0, 1, 5, 6, 9, 10, 255}) {
                for (const auto kind : {LaneKind::Key, LaneKind::Scratch, LaneKind::Pedal}) {
                    const Lane lane{static_cast<std::uint8_t>(player), kind,
                                    static_cast<std::uint8_t>(index)};
                    for (const bool ln : {false, true}) {
                        for (const auto note_kind : {NoteKind::Normal, NoteKind::Landmine}) {
                            const bool valid =
                                kind == LaneKind::Key && index >= 1 && index <= 9 &&
                                (player == 0 || (player == 1 && (index == 1 || index >= 6) &&
                                                 (ln || note_kind == NoteKind::Landmine)));
                            const auto channel =
                                bms::bms_channel_for_mode(mode, lane, ln, note_kind);
                            EXPECT_EQ(!channel.empty(), valid)
                                << mode << " player=" << player << " index=" << index;
                            if (valid) {
                                const auto rule = bms::bms_channel_rule_for(mode, channel);
                                ASSERT_TRUE(rule.has_value()) << channel;
                                EXPECT_EQ(rule->lane, lane) << channel;
                            }
                        }
                    }
                }
            }
        }
        EXPECT_EQ(
            bms::bms_channel_for_mode(mode, Lane{0, LaneKind::Bgm, 0}, false, NoteKind::Normal),
            "01");
        EXPECT_TRUE(
            bms::bms_channel_for_mode(mode, Lane{0, LaneKind::Bgm, 0}, true, NoteKind::Normal)
                .empty());
    }
    EXPECT_TRUE(
        bms::bms_channel_for_mode("sp7k", Lane{2, LaneKind::Key, 1}, false, NoteKind::Normal)
            .empty());
}

TEST(PmsIntegrity, PutRejectsInvalidLaneWithoutChangingSession) {
    cmd::Registry registry;
    cmd::register_builtin_commands(registry);
    auto& session = edit::global_editor_session();
    for (const auto* mode : {"pms9k", "pms9k-bmesp"}) {
        Chart chart = read_pms_integrity("#BPM 120\n#00022:01\n");
        chart.mode_id = mode;
        session.load(chart);
        const auto original = signatures(session.chart());
        for (const auto* note_kind : {"normal", "ln", "mine"}) {
            for (const int index : {-1, 0, 10, 255, 256, 257}) {
                Json args = Json::object();
                args.set("measure", 1);
                args.set("sample", 1);
                args.set("kind", note_kind);
                Json lane = lane_json(Lane{0, LaneKind::Key, 1});
                lane.set("index", index);
                args.set("lane", std::move(lane));
                const auto response = call(registry, "note.put", args);
                ASSERT_FALSE(response.at("ok").as_bool()) << response.dump();
                EXPECT_EQ(response.at("error").at("code").as_str(), "bad_args");
                EXPECT_EQ(signatures(session.chart()), original);
                EXPECT_EQ(session.undo_depth(), 0u);
                args.set("lane", index);  // 数字兼容入口也不能回绕成合法键号
                EXPECT_FALSE(call(registry, "note.put", args).at("ok").as_bool());
            }
            for (const auto* kind : {"scratch", "pedal", "invalid"}) {
                Json args = Json::object();
                args.set("measure", 1);
                args.set("sample", 1);
                args.set("kind", note_kind);
                Json lane = lane_json(Lane{0, LaneKind::Key, 0});
                lane.set("kind", kind);
                args.set("lane", std::move(lane));
                EXPECT_FALSE(call(registry, "note.put", args).at("ok").as_bool());
            }
            for (const int player : {-1, 2, 256}) {
                Json args = Json::object();
                args.set("measure", 1);
                args.set("sample", 1);
                args.set("kind", note_kind);
                Json lane = lane_json(Lane{0, LaneKind::Key, 1});
                lane.set("player", player);
                args.set("lane", std::move(lane));
                EXPECT_FALSE(call(registry, "note.put", args).at("ok").as_bool());
            }
        }
        EXPECT_EQ(signatures(session.chart()), original);
        EXPECT_EQ(session.undo_depth(), 0u);
    }
}

TEST(PmsIntegrity, RetainedSecondSideLnAndMinesRoundTripIncludingOrphan) {
    for (const auto* mode : {"pms9k", "pms9k-bmesp"}) {
        for (const auto* header : {"#BPM 120\n", "#BPM 120\n#LNTYPE 2\n#LNOBJ ZZ\n"}) {
            auto chart =
                read_pms_integrity(std::string(header) +
                                   "#00061:0101\n#00166:0101\n#00267:01\n#003E1:01\n#004E6:01\n");
            chart.mode_id = mode;
            const auto expected = signatures(chart);
            const auto expected_pairs = pair_signatures(chart);
            const auto text = bms::write_bms(chart);
            const auto reopened = read_pms_integrity(text);
            EXPECT_EQ(signatures(reopened), expected) << text;
            EXPECT_EQ(pair_signatures(reopened), expected_pairs);
        }
    }
}

TEST(PmsIntegrity, TransformRejectsUnwritableSecondSideAtomically) {
    cmd::Registry registry;
    cmd::register_builtin_commands(registry);
    auto& session = edit::global_editor_session();
    for (const auto* mode : {"pms9k", "pms9k-bmesp"}) {
        for (const auto* channel : {"66", "E6", "61", "E1", "67"}) {
            for (const auto* slots : {"0101", "01"}) {
                SCOPED_TRACE(::testing::Message()
                             << mode << " channel=" << channel << " slots=" << slots);
                const bool mirror = std::string(channel).back() == '6';
                const int rotate = mirror ? 0 : (std::string(channel).back() == '7' ? -2 : 1);
                Chart chart = read_pms_integrity(std::string("#BPM 120\n#00011:01\n#001") +
                                                 channel + ":" + slots + "\n");
                chart.mode_id = mode;
                session.load(chart);
                // Leave both undo and redo history, and a clean document, before rejection.
                ASSERT_TRUE(session.exec(std::make_unique<edit::MetaEditCommand>("TITLE", "keep")));
                ASSERT_TRUE(
                    session.exec(std::make_unique<edit::MetaEditCommand>("ARTIST", "redo")));
                ASSERT_TRUE(session.undo());
                session.mark_clean();
                const auto before = bms::write_bms(session.chart());
                const auto pairs = pair_signatures(session.chart());
                const auto undo = session.undo_depth(), redo = session.redo_depth();
                int persisted = 0;
                session.set_path("transform-test.pms");
                session.set_persist_hook([&](const Chart&, const std::string&, std::string*) {
                    ++persisted;
                    return true;
                });
                Json args = Json::object();
                args.set("selection", selection_json(session.chart()));
                args.set("mirror", mirror);
                args.set("rotate", rotate);
                const auto selection = args.at("selection").dump();
                const auto response = call(registry, "note.transform", args);
                EXPECT_FALSE(response.at("ok").as_bool()) << response.dump();
                if (!response.at("ok").as_bool()) {
                    EXPECT_EQ(response.at("error").at("code").as_str(), "bad_args");
                    EXPECT_EQ(response.find("result"), nullptr);  // no replacement selection
                }
                EXPECT_EQ(args.at("selection").dump(), selection);
                EXPECT_EQ(bms::write_bms(session.chart()), before);
                EXPECT_EQ(pair_signatures(session.chart()), pairs);
                EXPECT_EQ(session.undo_depth(), undo);
                EXPECT_EQ(session.redo_depth(), redo);
                EXPECT_FALSE(session.is_dirty());
                EXPECT_EQ(persisted, 0);
                // Direct EditCommand callers must not bypass the safety check.
                const auto& source = chart.notes.back();
                EXPECT_FALSE(session.exec(std::make_unique<edit::TransformNoteCommand>(
                    source.measure, source.pos, source.value.lane, source.value.sample.id, mirror,
                    rotate, source.value.sub_line)));
                EXPECT_EQ(bms::write_bms(session.chart()), before);
                EXPECT_EQ(session.undo_depth(), undo);
                EXPECT_EQ(session.redo_depth(), redo);
                EXPECT_FALSE(session.is_dirty());
                EXPECT_EQ(persisted, 0);
                session.set_persist_hook({});
                session.set_path("");
            }
        }
    }
}

TEST(PmsIntegrity, WritableSecondSideAndDpTransformsStillRoundTrip) {
    cmd::Registry registry;
    cmd::register_builtin_commands(registry);
    auto& session = edit::global_editor_session();
    for (const auto* mode : {"pms9k", "pms9k-bmesp", "dp"}) {
        for (const bool mirror : {false, true}) {
            for (const auto* header : {"#BPM 120\n", "#BPM 120\n#LNTYPE 2\n#LNOBJ ZZ\n"}) {
                const bool pms = std::string(mode) != "dp";
                if (!pms && std::string(header).find("LNTYPE") != std::string::npos) continue;
                bms::BmsReadOptions opts;
                opts.mode = mode;
                // Include a PMS orphan: its 2P lane needs the writer's LN fallback,
                // even when no pair exists or LNTYPE 2 prefers ordinary channels.
                Chart chart = bms::read_bms(std::string(header) + "#00061:0101\n#001E1:01\n" +
                                                (pms ? "#00261:01\n" : ""),
                                            opts)
                                  .chart;
                session.load(chart);
                Json args = Json::object();
                args.set("selection", selection_json(chart));
                args.set("mirror", mirror);
                args.set("rotate", mirror ? 0 : -1);
                const auto response = call(registry, "note.transform", args);
                ASSERT_TRUE(response.at("ok").as_bool()) << response.dump();
                ASSERT_TRUE(response.at("result").at("ok").as_bool());
                const int key = pms ? 9 : 7;
                for (const auto& e : session.chart().notes) {
                    EXPECT_EQ(e.value.lane.player, 1);
                    EXPECT_EQ(e.value.lane.index, key);
                }
                for (const auto& ref : response.at("result").at("selection").as_array())
                    EXPECT_EQ(ref.at("lane").at("index").as_i64(), key);
                const auto changed = signatures(session.chart());
                const auto changed_pairs = pair_signatures(session.chart());
                const auto reopened = bms::read_bms(bms::write_bms(session.chart()), opts).chart;
                EXPECT_EQ(signatures(reopened), changed);
                EXPECT_EQ(pair_signatures(reopened), changed_pairs);
                ASSERT_TRUE(session.undo());
                EXPECT_EQ(signatures(session.chart()), signatures(chart));
                ASSERT_TRUE(session.redo());
                EXPECT_EQ(signatures(session.chart()), changed);
            }
        }
    }
}

TEST(PmsIntegrity, LaneKindCaseCompatibilityAndPmsSecondSideValidation) {
    cmd::Registry registry;
    cmd::register_builtin_commands(registry);
    auto& session = edit::global_editor_session();
    Chart chart;
    chart.mode_id = "pms9k";
    session.load(chart);
    for (const auto* spelling : {"key", "Key", "KEY", "kEy"}) {
        Json args = Json::object();
        args.set("measure", 0);
        args.set("sample", 1);
        Json lane = lane_json(Lane{0, LaneKind::Key, 9});
        lane.set("kind", spelling);
        args.set("lane", std::move(lane));
        EXPECT_TRUE(call(registry, "note.put", args).at("ok").as_bool()) << spelling;
    }
    for (const int index : {1, 2, 6, 9}) {
        Json args = Json::object();
        args.set("measure", 1);
        args.set("sample", 1);
        args.set("lane", lane_json(Lane{1, LaneKind::Key, static_cast<std::uint8_t>(index)}));
        EXPECT_FALSE(call(registry, "note.put", args).at("ok").as_bool());
        args.set("kind", "ln");
        EXPECT_EQ(call(registry, "note.put", args).at("ok").as_bool(), index != 2);
    }
}

TEST(PmsIntegrity, LnAliasesPreservePairsThroughSaveAndCopyPaste) {
    cmd::Registry registry;
    cmd::register_builtin_commands(registry);
    auto& session = edit::global_editor_session();
    for (const auto* mode : {"pms9k", "pms9k-bmesp"}) {
        auto chart = read_pms_integrity("#BPM 120\n#LNTYPE 1\n#00062:01010000\n#00058:00000202\n");
        chart.mode_id = mode;
        const auto expected = signatures(chart);
        const auto expected_pairs = pair_signatures(chart);
        ASSERT_EQ(expected_pairs.size(), 2u);
        auto reopened = chart;
        for (int round = 0; round < 3; ++round) {
            reopened = read_pms_integrity(bms::write_bms(reopened));
            EXPECT_EQ(signatures(reopened), expected);
            EXPECT_EQ(pair_signatures(reopened), expected_pairs);
        }
        session.load(chart);
        Json args = Json::object();
        args.set("selection", selection_json(chart));
        const auto copied = call(registry, "clipboard.copy", args);
        ASSERT_TRUE(copied.at("ok").as_bool()) << copied.dump();
        Chart empty;
        empty.mode_id = mode;
        session.load(empty);
        args = Json::object();
        args.set("lines", copied.at("result").at("lines"));
        ASSERT_TRUE(call(registry, "clipboard.paste", args).at("ok").as_bool());
        EXPECT_EQ(signatures(session.chart()), expected);
        EXPECT_EQ(pair_signatures(session.chart()), expected_pairs);
        ASSERT_TRUE(session.undo());
        ASSERT_TRUE(session.redo());
        EXPECT_EQ(pair_signatures(session.chart()), expected_pairs);
    }
}

TEST(PmsIntegrity, IdenticalAliasLnReferencesDoNotCauseFalseHalfSelection) {
    cmd::Registry registry;
    cmd::register_builtin_commands(registry);
    auto& session = edit::global_editor_session();
    auto chart = read_pms_integrity("#BPM 120\n#LNTYPE 1\n#00062:01020000\n#00058:01000300\n");
    session.load(chart);
    Json args = Json::object();
    args.set("selection", selection_json(chart));
    const auto copied = call(registry, "clipboard.copy", args);
    ASSERT_TRUE(copied.at("ok").as_bool()) << copied.dump();
    std::string text = "#BPM 120\n";
    for (const auto& line : copied.at("result").at("lines").as_array())
        text += line.as_str() + "\n";
    // 重叠LN的配对仍遵守既有按lane交替规则；这里只锁定全选不能误报半选或丢物件。
    EXPECT_EQ(signatures(read_pms_integrity(text)), signatures(chart));
}

TEST(PmsIntegrity, RepeatedAliasesPreserveSamplesAndPositionsAcrossSaves) {
    for (const auto* input :
         {"#BPM 120\n#00022:01\n#00018:02\n", "#BPM 120\n#00022:0102\n#00022:03\n#00018:0405\n",
          "#BPM 120\n#00022:01\n#00022:01\n#00018:01\n"}) {
        for (const auto* mode : {"pms9k", "pms9k-bmesp"}) {
            auto chart = read_pms_integrity(input);
            chart.mode_id = mode;
            const auto expected = signatures(chart);
            for (int round = 0; round < 3; ++round) {
                const auto text = bms::write_bms(chart);
                chart = read_pms_integrity(text);
                EXPECT_EQ(signatures(chart), expected) << mode << " round=" << round << "\n"
                                                       << text;
            }
        }
    }
}

TEST(PmsIntegrity, AliasCopyPastePreservesAllNotesAndUndoRedo) {
    cmd::Registry registry;
    cmd::register_builtin_commands(registry);
    auto& session = edit::global_editor_session();
    for (const auto* input :
         {"#BPM 120\n#00022:01\n#00018:02\n", "#BPM 120\n#00022:0102\n#00022:03\n#00018:0405\n",
          "#BPM 120\n#00022:01\n#00018:01\n"}) {
        for (const auto* mode : {"pms9k", "pms9k-bmesp"}) {
            auto chart = read_pms_integrity(input);
            chart.mode_id = mode;
            session.load(chart);
            const auto expected = signatures(chart);
            Json copy_args = Json::object();
            copy_args.set("selection", selection_json(chart));
            const auto copied = call(registry, "clipboard.copy", copy_args);
            ASSERT_TRUE(copied.at("ok").as_bool()) << copied.dump();
            EXPECT_EQ(copied.at("result").at("count").as_i64(),
                      static_cast<std::int64_t>(chart.notes.size()));
            std::string text = "#BPM 120\n";
            for (const auto& line : copied.at("result").at("lines").as_array())
                text += line.as_str() + "\n";
            EXPECT_EQ(signatures(read_pms_integrity(text)), expected) << text;
            Chart empty;
            empty.mode_id = mode;
            session.load(empty);
            Json paste_args = Json::object();
            paste_args.set("lines", copied.at("result").at("lines"));
            const auto pasted = call(registry, "clipboard.paste", paste_args);
            ASSERT_TRUE(pasted.at("ok").as_bool()) << pasted.dump();
            EXPECT_EQ(signatures(session.chart()), expected);
            EXPECT_TRUE(session.undo());
            EXPECT_TRUE(session.chart().notes.empty());
            EXPECT_TRUE(session.redo());
            EXPECT_EQ(signatures(session.chart()), expected);
            EXPECT_EQ(signatures(read_pms_integrity(bms::write_bms(session.chart()))), expected);
        }
    }
}
