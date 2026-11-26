// SPDX-License-Identifier: GPL-3.0-only
#include <gtest/gtest.h>

#include <algorithm>
#include <string>
#include <vector>

#include "beatbench/core/bms/BmsCodec.hpp"
#include "beatbench/core/command/Command.hpp"
#include "beatbench/core/edit/Selection.hpp"
#include "beatbench/core/edit/SessionRegistry.hpp"

using namespace beatbench;
using namespace beatbench::edit;
using beatbench::json::Json;

namespace {
Chart identity_chart() {
    Chart chart;
    chart.mode_id = "sp7k";
    chart.meta["BPM"] = "130";
    chart.meta["LNTYPE"] = "1";
    Event<Note> ordinary{1, Rational(0, 1), {}};
    ordinary.value.lane = {0, LaneKind::Key, 1};
    ordinary.value.sample.id = 1;
    auto ln = ordinary;
    ln.value.ln_channel = true;
    auto mine = ordinary;
    mine.value.kind = NoteKind::Landmine;
    chart.notes = {ordinary, ln, mine};
    return chart;
}

Json identity_ref(bool ln, const char* kind) {
    Json ref = Json::object();
    ref.set("measure", 1);
    ref.set("sample", 1);
    Json lane = Json::object();
    lane.set("player", 0);
    lane.set("kind", "key");
    lane.set("index", 1);
    ref.set("lane", std::move(lane));
    ref.set("ln_channel", ln);
    ref.set("note_kind", kind);
    return ref;
}

Json selection_args(Json ref) {
    Json args = Json::object();
    Json selection = Json::array();
    selection.push_back(std::move(ref));
    args.set("selection", std::move(selection));
    return args;
}

Json dispatch(const char* command, Json args) {
    Json request = Json::object();
    request.set("command", command);
    request.set("args", std::move(args));
    return cmd::global_registry().dispatch(request);
}
Json note_position(std::uint32_t measure, Rational pos, unsigned key = 1) {
    auto value = identity_ref(true, "normal");
    value.set("measure", static_cast<std::int64_t>(measure));
    Json p = Json::object();
    p.set("num", pos.num);
    p.set("den", pos.den);
    value.set("pos", std::move(p));
    value.as_object().at("lane").set("index", static_cast<int>(key));
    return value;
}

Json move_item(Json from, Json to) {
    Json item = Json::object();
    item.set("from", std::move(from));
    item.set("to", std::move(to));
    return item;
}

Chart paired_identity_chart() {
    auto chart = identity_chart();
    chart.notes.pop_back();  // Ordinary + LN head at the same position/sample.
    auto ordinary_tail = chart.notes[0];
    ordinary_tail.pos = Rational(1, 2);
    auto ln_tail = ordinary_tail;
    ln_tail.value.ln_channel = true;
    chart.notes.push_back(ordinary_tail);
    chart.notes.push_back(ln_tail);
    chart.notes[1].value.ln_pair = 3;
    chart.notes[3].value.ln_pair = 1;
    return chart;
}

void expect_notes_exact(const std::vector<Event<Note>>& actual,
                        const std::vector<Event<Note>>& expected) {
    ASSERT_EQ(actual.size(), expected.size());
    for (std::size_t i = 0; i < actual.size(); ++i) {
        SCOPED_TRACE(i);
        EXPECT_EQ(actual[i].measure, expected[i].measure);
        EXPECT_EQ(actual[i].pos, expected[i].pos);
        EXPECT_EQ(actual[i].value.lane, expected[i].value.lane);
        EXPECT_EQ(actual[i].value.sample, expected[i].value.sample);
        EXPECT_EQ(actual[i].value.kind, expected[i].value.kind);
        EXPECT_EQ(actual[i].value.ln_channel, expected[i].value.ln_channel);
        EXPECT_EQ(actual[i].value.ln_pair, expected[i].value.ln_pair);
        EXPECT_EQ(actual[i].value.sub_line, expected[i].value.sub_line);
    }
}
}  // namespace

TEST(LnIdentity, SelectionRetainsAllSamePositionKinds) {
    Selection selection;
    NoteRef ordinary{1, Rational(0, 1), {0, LaneKind::Key, 1}, 1};
    ordinary.ln_channel = false;
    ordinary.note_kind = NoteKind::Normal;
    auto ln = ordinary;
    ln.ln_channel = true;
    auto mine = ordinary;
    mine.note_kind = NoteKind::Landmine;
    selection.add(ordinary);
    selection.add(ln);
    selection.add(mine);
    EXPECT_EQ(selection.size(), 3u);
    EXPECT_FALSE(ordinary == ln);
    EXPECT_FALSE(ordinary == mine);
}

TEST(LnIdentity, ProtocolMoveTargetsLnWithoutMovingOrdinaryOrMine) {
    auto& session = global_editor_session();
    session.load(identity_chart());
    Json args = Json::object();
    args.set("from", identity_ref(true, "normal"));
    Json to = Json::object();
    to.set("measure", 2);
    args.set("to", std::move(to));
    const auto response = dispatch("note.move", std::move(args));
    ASSERT_TRUE(response.at("ok").as_bool());
    ASSERT_TRUE(response.at("result").at("ok").as_bool());
    for (const auto& note : session.chart().notes)
        EXPECT_EQ(note.measure, note.value.ln_channel ? 2u : 1u);
    ASSERT_TRUE(session.undo());
    for (const auto& note : session.chart().notes) EXPECT_EQ(note.measure, 1u);
    ASSERT_TRUE(session.redo());
    for (const auto& note : session.chart().notes)
        EXPECT_EQ(note.measure, note.value.ln_channel ? 2u : 1u);
}

TEST(LnIdentity, ProtocolMoveRejectsAmbiguousLegacySourceWithoutMutation) {
    auto& session = global_editor_session();
    session.load(identity_chart());
    auto source = identity_ref(false, "normal");
    source.as_object().erase("ln_channel");
    source.as_object().erase("note_kind");
    Json args = Json::object();
    args.set("from", std::move(source));
    Json to = Json::object();
    to.set("measure", 2);
    args.set("to", std::move(to));
    const auto response = dispatch("note.move", std::move(args));
    ASSERT_FALSE(response.at("ok").as_bool());
    EXPECT_EQ(response.at("error").at("code").as_str(), "bad_args");
    EXPECT_EQ(session.undo_depth(), 0u);
    for (const auto& note : session.chart().notes) EXPECT_EQ(note.measure, 1u);
}

TEST(LnIdentity, ProtocolMoveRegionTargetsOnlyMine) {
    auto& session = global_editor_session();
    session.load(identity_chart());
    auto args = selection_args(identity_ref(false, "mine"));
    Json delta = Json::object();
    delta.set("measure", 1);
    args.set("delta", std::move(delta));
    const auto response = dispatch("note.moveRegion", std::move(args));
    ASSERT_TRUE(response.at("ok").as_bool());
    ASSERT_TRUE(response.at("result").at("ok").as_bool());
    for (const auto& note : session.chart().notes)
        EXPECT_EQ(note.measure, note.value.kind == NoteKind::Landmine ? 2u : 1u);
}

TEST(LnIdentity, ProtocolQuantizeTargetsOnlyMine) {
    auto& session = global_editor_session();
    auto chart = identity_chart();
    for (auto& note : chart.notes) note.pos = Rational(1, 3);
    session.load(std::move(chart));
    auto ref = identity_ref(false, "mine");
    Json pos = Json::object();
    pos.set("num", 1);
    pos.set("den", 3);
    ref.set("pos", std::move(pos));
    auto response = dispatch("note.quantize", selection_args(std::move(ref)));
    ASSERT_TRUE(response.at("ok").as_bool());
    ASSERT_TRUE(response.at("result").at("ok").as_bool());
    for (const auto& note : session.chart().notes)
        EXPECT_EQ(note.pos,
                  note.value.kind == NoteKind::Landmine ? Rational(5, 16) : Rational(1, 3));
}

TEST(LnIdentity, ProtocolDeleteTargetsOnlyMine) {
    auto& session = global_editor_session();
    session.load(identity_chart());
    const auto response = dispatch("note.delete", selection_args(identity_ref(false, "mine")));
    ASSERT_TRUE(response.at("ok").as_bool());
    ASSERT_TRUE(response.at("result").at("ok").as_bool());
    ASSERT_EQ(session.chart().notes.size(), 2u);
    for (const auto& note : session.chart().notes) EXPECT_EQ(note.value.kind, NoteKind::Normal);
    ASSERT_TRUE(session.undo());
    EXPECT_EQ(session.chart().notes.size(), 3u);
}

TEST(LnIdentity, ProtocolSetSampleTargetsOnlyLn) {
    auto& session = global_editor_session();
    session.load(identity_chart());
    auto args = identity_ref(true, "normal");
    args.set("to", "02");
    const auto response = dispatch("note.setSample", std::move(args));
    ASSERT_TRUE(response.at("ok").as_bool());
    ASSERT_TRUE(response.at("result").at("ok").as_bool());
    for (const auto& note : session.chart().notes)
        EXPECT_EQ(note.value.sample.id, note.value.ln_channel ? 2u : 1u);
}

TEST(LnIdentity, ProtocolIdentityFieldsRejectWrongTypesAndNames) {
    global_editor_session().load(identity_chart());
    for (const auto& invalid : {Json(1), Json("true"), Json()}) {
        auto ref = identity_ref(false, "normal");
        ref.set("ln_channel", invalid);
        auto response = dispatch("note.delete", selection_args(std::move(ref)));
        ASSERT_FALSE(response.at("ok").as_bool());
        EXPECT_EQ(response.at("error").at("code").as_str(), "bad_args");
    }
    for (const auto& invalid : {Json(true), Json("ln"), Json("landmine"), Json()}) {
        auto ref = identity_ref(false, "normal");
        ref.set("note_kind", invalid);
        auto response = dispatch("clipboard.copy", selection_args(std::move(ref)));
        ASSERT_FALSE(response.at("ok").as_bool());
        EXPECT_EQ(response.at("error").at("code").as_str(), "bad_args");
    }
}

TEST(LnIdentity, ClipboardCopyDisambiguatesMineAndRejectsAmbiguousLegacyRef) {
    global_editor_session().load(identity_chart());
    auto response = dispatch("clipboard.copy", selection_args(identity_ref(false, "mine")));
    ASSERT_TRUE(response.at("ok").as_bool());
    const auto& lines = response.at("result").at("lines").as_array();
    ASSERT_EQ(lines.size(), 1u);
    EXPECT_EQ(lines[0].as_str(), "#001D1:01");
    Json legacy = Json::object();
    legacy.set("measure", 1);
    legacy.set("sample", 1);
    legacy.set("lane", identity_ref(false, "normal").at("lane"));
    response = dispatch("clipboard.copy", selection_args(std::move(legacy)));
    ASSERT_FALSE(response.at("ok").as_bool());
    EXPECT_EQ(response.at("error").at("code").as_str(), "bad_args");
}

TEST(LnIdentity, PasteReturnsActualChannelAndKindIdentity) {
    global_editor_session().load(identity_chart());
    Json args = Json::object();
    args.set("text", "#00211:01\n#00251:01\n#002D1:01");
    const auto response = dispatch("clipboard.paste", std::move(args));
    ASSERT_TRUE(response.at("ok").as_bool());
    ASSERT_TRUE(response.at("result").at("ok").as_bool());
    const auto& refs = response.at("result").at("selection").as_array();
    ASSERT_EQ(refs.size(), 3u);
    EXPECT_FALSE(refs[0].at("ln_channel").as_bool());
    EXPECT_EQ(refs[0].at("note_kind").as_str(), "normal");
    EXPECT_TRUE(refs[1].at("ln_channel").as_bool());
    EXPECT_EQ(refs[1].at("note_kind").as_str(), "normal");
    EXPECT_FALSE(refs[2].at("ln_channel").as_bool());
    EXPECT_EQ(refs[2].at("note_kind").as_str(), "mine");
}

TEST(LnIdentity, TransformReturnsIdentityWithoutAliasingOtherKinds) {
    auto& session = global_editor_session();
    session.load(identity_chart());
    auto args = selection_args(identity_ref(true, "normal"));
    args.set("mirror", true);
    const auto response = dispatch("note.transform", std::move(args));
    ASSERT_TRUE(response.at("ok").as_bool());
    ASSERT_TRUE(response.at("result").at("ok").as_bool());
    const auto& ref = response.at("result").at("selection").as_array()[0];
    EXPECT_TRUE(ref.at("ln_channel").as_bool());
    EXPECT_EQ(ref.at("note_kind").as_str(), "normal");
    for (const auto& note : session.chart().notes)
        EXPECT_EQ(note.value.lane.index, note.value.ln_channel ? 7 : 1);
}

TEST(LnIdentity, CrossKeyMoveKeepsLnFlagsAndUnselectedPartnerUnchanged) {
    auto& session = global_editor_session();
    session.load(paired_identity_chart());
    const auto before = session.chart().notes;
    auto response = dispatch("note.move", move_item(note_position(1, Rational(0, 1)),
                                                    note_position(1, Rational(0, 1), 2)));
    ASSERT_TRUE(response.at("ok").as_bool());
    ASSERT_TRUE(response.at("result").at("ok").as_bool());
    const auto after = session.chart().notes;
    ASSERT_EQ(after.size(), 4u);
    std::size_t ln_count = 0;
    bool moved_head = false, untouched_tail = false;
    for (const auto& note : after) {
        if (!note.value.ln_channel) {
            EXPECT_EQ(note.value.lane.index, 1);
            continue;
        }
        ++ln_count;
        EXPECT_EQ(note.value.sample.id, 1u);
        if (note.pos == Rational(0, 1)) {
            EXPECT_EQ(note.value.lane.index, 2);
            moved_head = true;
        } else {
            EXPECT_EQ(note.pos, Rational(1, 2));
            EXPECT_EQ(note.measure, 1u);
            EXPECT_EQ(note.value.lane.index, 1);
            untouched_tail = true;
        }
    }
    EXPECT_EQ(ln_count, 2u);
    EXPECT_TRUE(moved_head);
    EXPECT_TRUE(untouched_tail);
    const auto written = bms::write_bms(session.chart());
    EXPECT_NE(written.find("#00152:"), std::string::npos);
    EXPECT_NE(written.find("#00151:"), std::string::npos);
    ASSERT_TRUE(session.undo());
    expect_notes_exact(session.chart().notes, before);
    ASSERT_TRUE(session.redo());
    expect_notes_exact(session.chart().notes, after);
}

TEST(LnIdentity, BatchMovesBothLnEndsAcrossLaneWithoutMovingOrdinaryAliases) {
    auto& session = global_editor_session();
    session.load(paired_identity_chart());
    const auto before = session.chart().notes;
    Json moves = Json::array();
    moves.push_back(
        move_item(note_position(1, Rational(0, 1)), note_position(2, Rational(0, 1), 2)));
    moves.push_back(
        move_item(note_position(1, Rational(1, 2)), note_position(2, Rational(1, 2), 2)));
    Json args = Json::object();
    args.set("moves", std::move(moves));
    const auto response = dispatch("note.move", std::move(args));
    ASSERT_TRUE(response.at("ok").as_bool());
    ASSERT_TRUE(response.at("result").at("ok").as_bool());
    const auto after = session.chart().notes;
    ASSERT_EQ(after.size(), 4u);
    std::size_t ln_count = 0;
    for (std::size_t i = 0; i < after.size(); ++i) {
        const auto& note = after[i];
        EXPECT_EQ(note.value.sample.id, 1u);
        if (note.value.ln_channel) {
            ++ln_count;
            EXPECT_EQ(note.measure, 2u);
            EXPECT_EQ(note.value.lane.index, 2);
            ASSERT_TRUE(note.value.ln_pair.has_value());
            ASSERT_LT(*note.value.ln_pair, after.size());
            EXPECT_EQ(after[*note.value.ln_pair].value.ln_pair, i);
            EXPECT_TRUE(after[*note.value.ln_pair].value.ln_channel);
        } else {
            EXPECT_EQ(note.measure, 1u);
            EXPECT_EQ(note.value.lane.index, 1);
        }
    }
    EXPECT_EQ(ln_count, 2u);
    EXPECT_NE(bms::write_bms(session.chart()).find("#00252:"), std::string::npos);
    ASSERT_TRUE(session.undo());
    expect_notes_exact(session.chart().notes, before);
    ASSERT_TRUE(session.redo());
    expect_notes_exact(session.chart().notes, after);
}

TEST(LnIdentity, BatchSameIdChainAndSwapBindOriginalSourcesWithoutRecapture) {
    auto& session = global_editor_session();
    for (const bool swap : {false, true}) {
        SCOPED_TRACE(swap);
        auto chart = identity_chart();
        chart.notes.resize(1);
        auto second = chart.notes.front();
        second.measure = 2;
        chart.notes.push_back(second);
        session.load(std::move(chart));
        const auto before = session.chart().notes;
        auto first_ref = identity_ref(false, "normal");
        auto second_ref = first_ref;
        second_ref.set("measure", 2);
        auto third_ref = first_ref;
        third_ref.set("measure", 3);
        Json moves = Json::array();
        moves.push_back(move_item(first_ref, second_ref));
        moves.push_back(move_item(second_ref, swap ? first_ref : third_ref));
        Json args = Json::object();
        args.set("moves", std::move(moves));
        const auto response = dispatch("note.move", std::move(args));
        ASSERT_TRUE(response.at("ok").as_bool());
        ASSERT_TRUE(response.at("result").at("ok").as_bool());
        const auto after = session.chart().notes;
        ASSERT_EQ(after.size(), 2u);
        std::vector<std::uint32_t> measures;
        for (const auto& note : after) measures.push_back(note.measure);
        std::sort(measures.begin(), measures.end());
        EXPECT_EQ(measures,
                  (swap ? std::vector<std::uint32_t>{1, 2} : std::vector<std::uint32_t>{2, 3}));
        ASSERT_TRUE(session.undo());
        expect_notes_exact(session.chart().notes, before);
        ASSERT_TRUE(session.redo());
        expect_notes_exact(session.chart().notes, after);
    }
}

TEST(LnIdentity, MoveUndoDoesNotInvalidateEarlierPutOrSetSampleUndo) {
    auto& session = global_editor_session();
    for (const bool change_sample : {false, true}) {
        SCOPED_TRACE(change_sample);
        session.load(identity_chart());
        const auto original = session.chart().notes;
        auto source = identity_ref(true, "normal");
        if (change_sample) {
            auto args = source;
            args.set("to", "02");
            const auto response = dispatch("note.setSample", std::move(args));
            ASSERT_TRUE(response.at("ok").as_bool());
            ASSERT_TRUE(response.at("result").at("ok").as_bool());
            source.set("sample", 2);
        } else {
            source = note_position(3, Rational(0, 1), 2);
            source.set("kind", "ln");
            const auto response = dispatch("note.put", source);
            ASSERT_TRUE(response.at("ok").as_bool());
            ASSERT_TRUE(response.at("result").at("ok").as_bool());
        }
        const auto before_move = session.chart().notes;
        auto target = note_position(4, Rational(0, 1), 3);
        const auto response = dispatch("note.move", move_item(source, std::move(target)));
        ASSERT_TRUE(response.at("ok").as_bool());
        ASSERT_TRUE(response.at("result").at("ok").as_bool());
        const auto after_move = session.chart().notes;
        ASSERT_TRUE(session.undo());
        expect_notes_exact(session.chart().notes, before_move);
        ASSERT_TRUE(session.undo());
        expect_notes_exact(session.chart().notes, original);
        ASSERT_TRUE(session.redo());
        expect_notes_exact(session.chart().notes, before_move);
        ASSERT_TRUE(session.redo());
        expect_notes_exact(session.chart().notes, after_move);
    }
}

TEST(LnIdentity, TransformRejectsWildcardCrossKindAliasesWithoutMutation) {
    auto& session = global_editor_session();
    for (const bool keep_ln_identity : {false, true}) {
        session.load(identity_chart());
        const auto before = session.chart().notes;
        auto ref = identity_ref(false, "normal");
        ref.as_object().erase("note_kind");
        if (!keep_ln_identity) ref.as_object().erase("ln_channel");
        auto args = selection_args(std::move(ref));
        args.set("mirror", true);
        const auto response = dispatch("note.transform", std::move(args));
        ASSERT_FALSE(response.at("ok").as_bool());
        EXPECT_EQ(response.at("error").at("code").as_str(), "bad_args");
        EXPECT_EQ(session.undo_depth(), 0u);
        expect_notes_exact(session.chart().notes, before);
    }
}

TEST(LnIdentity, ConvertUndoPreservesEarlierSamePositionPutIdentityAndOrder) {
    auto& session = global_editor_session();
    auto chart = identity_chart();
    chart.notes.resize(1);
    session.load(std::move(chart));
    const auto original = session.chart().notes;
    auto placed = identity_ref(true, "normal");
    placed.set("kind", "ln");
    const auto put = dispatch("note.put", std::move(placed));
    ASSERT_TRUE(put.at("ok").as_bool());
    ASSERT_TRUE(put.at("result").at("ok").as_bool());
    const auto after_put = session.chart().notes;
    auto args = selection_args(identity_ref(false, "normal"));
    args.set("target", "bga_base");
    const auto converted = dispatch("note.convert", std::move(args));
    ASSERT_TRUE(converted.at("ok").as_bool());
    ASSERT_TRUE(converted.at("result").at("ok").as_bool());
    const auto after_convert = session.chart().notes;
    ASSERT_TRUE(session.undo());
    expect_notes_exact(session.chart().notes, after_put);
    ASSERT_TRUE(session.undo());
    expect_notes_exact(session.chart().notes, original);
    ASSERT_TRUE(session.redo());
    expect_notes_exact(session.chart().notes, after_put);
    ASSERT_TRUE(session.redo());
    expect_notes_exact(session.chart().notes, after_convert);
}

TEST(LnIdentity, DeleteUndoWithOrdinaryAliasAtOldLnPartnerRebuildsIdentity) {
    auto& session = global_editor_session();
    session.load(paired_identity_chart());
    const auto before = session.chart().notes;
    const auto response = dispatch("note.delete", selection_args(identity_ref(true, "normal")));
    ASSERT_TRUE(response.at("ok").as_bool());
    ASSERT_TRUE(response.at("result").at("ok").as_bool());
    const auto after = session.chart().notes;
    ASSERT_EQ(after.size(), 3u);
    ASSERT_NO_THROW(ASSERT_TRUE(session.undo()));
    expect_notes_exact(session.chart().notes, before);
    ASSERT_NO_THROW(ASSERT_TRUE(session.redo()));
    expect_notes_exact(session.chart().notes, after);
    ASSERT_NO_THROW(ASSERT_TRUE(session.undo()));
    expect_notes_exact(session.chart().notes, before);
}

TEST(LnIdentity, ConvertUndoWithOrdinaryAliasAtOldLnPartnerRebuildsIdentity) {
    auto& session = global_editor_session();
    auto chart = paired_identity_chart();
    // Keep the converted source first so this test isolates old-partner lookup,
    // rather than the separate original-index restoration requirement.
    std::swap(chart.notes[0], chart.notes[1]);
    chart.notes[0].value.ln_pair = 3;
    chart.notes[3].value.ln_pair = 0;
    session.load(std::move(chart));
    const auto before = session.chart().notes;
    auto args = selection_args(identity_ref(true, "normal"));
    args.set("target", "bga_base");
    const auto response = dispatch("note.convert", std::move(args));
    ASSERT_TRUE(response.at("ok").as_bool());
    ASSERT_TRUE(response.at("result").at("ok").as_bool());
    const auto after = session.chart().notes;
    ASSERT_EQ(after.size(), 3u);
    ASSERT_EQ(session.chart().bga_events.size(), 1u);
    ASSERT_NO_THROW(ASSERT_TRUE(session.undo()));
    expect_notes_exact(session.chart().notes, before);
    EXPECT_TRUE(session.chart().bga_events.empty());
    ASSERT_NO_THROW(ASSERT_TRUE(session.redo()));
    expect_notes_exact(session.chart().notes, after);
    ASSERT_EQ(session.chart().bga_events.size(), 1u);
    ASSERT_NO_THROW(ASSERT_TRUE(session.undo()));
    expect_notes_exact(session.chart().notes, before);
    EXPECT_TRUE(session.chart().bga_events.empty());
}

TEST(LnIdentity, LnMoveToBgmRejectsWithoutMutationOrUndoEntry) {
    auto& session = global_editor_session();
    session.load(paired_identity_chart());
    const auto before = session.chart().notes;
    auto target = note_position(2, Rational(0, 1));
    target.as_object().at("lane").set("kind", "bgm");
    target.as_object().at("lane").set("index", 0);
    const auto response =
        dispatch("note.move", move_item(note_position(1, Rational(0, 1)), std::move(target)));
    ASSERT_FALSE(response.at("ok").as_bool());
    EXPECT_EQ(response.at("error").at("code").as_str(), "bad_args");
    EXPECT_EQ(session.undo_depth(), 0u);
    expect_notes_exact(session.chart().notes, before);
}
