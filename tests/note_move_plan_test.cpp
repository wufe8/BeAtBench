// SPDX-License-Identifier: GPL-3.0-only
#include <gtest/gtest.h>

#include <cmath>
#include <limits>

#include "bridge/NoteMovePlan.hpp"

namespace {
using namespace beatbench::app;
QVariantMap ref(int key = 1, int measure = 0, int num = 1, int den = 24,
                const QString& kind = "key", int subLine = 0) {
    return {{"measure", measure},
            {"pos", QVariantMap{{"num", num}, {"den", den}}},
            {"lane", QVariantMap{{"player", 0}, {"kind", kind}, {"index", key}}},
            {"sample", 1},
            {"sub_line", subLine},
            {"ln_channel", false},
            {"note_kind", "normal"}};
}
QVariantMap col(const QString& kind, int index, int subLine = -1) {
    return {{"valid", true},      {"lanePlayer", 0},     {"laneKind", kind},
            {"laneIndex", index}, {"sub_line", subLine}, {"bgaLayer", -1}};
}
QVariantList columns() {
    auto bpm = col("key", 0);
    bpm.insert("metaKind", "bpm");
    return {bpm, col("scratch", 1), col("key", 1), col("key", 2), col("key", 3), col("bgm", 1)};
}
NoteMoveSource source() { return {{ref(1), ref(2)}, ref(1), {2, 3}, 2, columns()}; }
NoteMoveDrop drop(int rank = 3) {
    NoteMoveDrop result;
    result.columns = columns();
    result.targetRank = rank;
    result.targetColumn = result.columns[rank].toMap();
    result.timeLocked = true;
    return result;
}
void failedAtomic(const NoteMovePlan& plan, const QString& code) {
    EXPECT_FALSE(plan.ok);
    EXPECT_FALSE(plan.changed);
    EXPECT_EQ(plan.errorCode, code);
    EXPECT_TRUE(plan.moves.isEmpty());
    EXPECT_TRUE(plan.selection.isEmpty());
}
TEST(NoteMovePlan, HorizontalMovementKeepsExactFractionAndFullIdentity) {
    auto src = source();
    auto first = src.refs[0].toMap();
    first.insert("ln_channel", true);
    first.insert("note_kind", "mine");
    first.insert("lnPartner", ref(1, 2));
    src.refs[0] = first;
    src.grabRef = first;
    const auto plan = buildNoteMovePlan(src, drop());
    ASSERT_TRUE(plan.ok);
    EXPECT_TRUE(plan.changed);
    ASSERT_EQ(plan.moves.size(), 2);
    EXPECT_EQ(plan.moves[0].toMap().value("from").toMap(), first);
    const auto selected = plan.selection[0].toMap();
    EXPECT_EQ(selected.value("pos"), first.value("pos"));
    EXPECT_TRUE(selected.value("ln_channel").toBool());
    EXPECT_EQ(selected.value("note_kind").toString(), "mine");
    EXPECT_FALSE(selected.contains("lnPartner"));
    EXPECT_EQ(selected.value("lane").toMap().value("index").toInt(), 2);
    EXPECT_EQ(plan.selection[1].toMap().value("lane").toMap().value("index").toInt(), 3);
    EXPECT_EQ(src.refs[0].toMap(), first);  // No mutation of the caller snapshot.
}
TEST(NoteMovePlan, PureTimeMovementNeedsNoColumnsAndKeepsBgmSubline) {
    NoteMoveSource src{{ref(1, 0, 1, 24, "bgm", 7)}, ref(1, 0, 1, 24, "bgm", 7), {}, -1, {}};
    NoteMoveDrop dst;
    dst.laneLocked = true;
    dst.deltaF = 1.0 / 16.0;
    const auto plan = buildNoteMovePlan(src, dst);
    ASSERT_TRUE(plan.ok);
    const auto selected = plan.selection.front().toMap();
    EXPECT_EQ(selected.value("pos").toMap().value("num").toInt(), 1);
    EXPECT_EQ(selected.value("pos").toMap().value("den").toInt(), 8);
    EXPECT_EQ(selected.value("sub_line").toInt(), 7);
    EXPECT_EQ(selected.value("lane"), src.grabRef.value("lane"));
}
TEST(NoteMovePlan, FrozenPressToTargetMusicalDeltaDoesNotUseGrabRefTimeAsPress) {
    NoteMoveSource src{{ref(1, 5, 1, 4)}, ref(1, 5, 1, 4), {}, -1, {}};
    NoteMoveDrop dst;
    dst.laneLocked = true;
    dst.deltaF = 8.7 - 5.2;  // Controller supplies independently mapped viewport positions.
    const auto plan = buildNoteMovePlan(src, dst);
    ASSERT_TRUE(plan.ok);
    const auto selected = plan.selection.front().toMap();
    EXPECT_EQ(selected.value("measure").toInt(), 8);
    EXPECT_EQ(selected.value("pos").toMap().value("num").toInt(), 3);
    EXPECT_EQ(selected.value("pos").toMap().value("den").toInt(), 4);
}
TEST(NoteMovePlan, TargetRanksUseTheirOwnLayoutIncludingMetaPlaceholders) {
    auto dst = drop();
    auto stop = col("key", 0);
    stop.insert("metaKind", "stop");
    dst.columns.prepend(stop);
    dst.targetRank = 4;
    dst.targetColumn = dst.columns[4].toMap();
    const auto plan = buildNoteMovePlan(source(), dst);
    ASSERT_TRUE(plan.ok);
    EXPECT_EQ(plan.selection[0].toMap().value("lane").toMap().value("index").toInt(), 2);
    EXPECT_EQ(plan.selection[1].toMap().value("lane").toMap().value("index").toInt(), 3);
}
TEST(NoteMovePlan, CollapsedBgmSourceKeepsInvisibleSublineSpacingWhenExpandedOrCollapsed) {
    NoteMoveSource src{{ref(1, 0, 1, 24, "bgm", 4), ref(1, 0, 1, 24, "bgm", 6)},
                       ref(1, 0, 1, 24, "bgm", 4),
                       {5, 5},
                       5,
                       columns()};
    auto dst = drop(5);
    auto plan = buildNoteMovePlan(src, dst);
    ASSERT_TRUE(plan.ok);
    EXPECT_FALSE(plan.changed);
    EXPECT_EQ(plan.selection[0].toMap().value("sub_line").toInt(), 4);
    EXPECT_EQ(plan.selection[1].toMap().value("sub_line").toInt(), 6);
    dst.columns[5] = col("bgm", 1, 1);
    dst.targetColumn = dst.columns[5].toMap();
    plan = buildNoteMovePlan(src, dst);
    ASSERT_TRUE(plan.ok);
    EXPECT_EQ(plan.selection[0].toMap().value("sub_line").toInt(), 1);
    EXPECT_EQ(plan.selection[1].toMap().value("sub_line").toInt(), 3);
    dst = drop(2);
    plan = buildNoteMovePlan(src, dst);
    ASSERT_TRUE(plan.ok);
    EXPECT_EQ(plan.selection[0].toMap().value("lane").toMap().value("index").toInt(), 1);
    EXPECT_EQ(plan.selection[1].toMap().value("lane").toMap().value("index").toInt(), 3);
    EXPECT_EQ(plan.selection[0].toMap().value("sub_line").toInt(), 0);
}
TEST(NoteMovePlan, PlayGroupToBgmKeepsColumnGapAndNeverChangesSampleFromColumnHint) {
    auto dst = drop(5);
    dst.targetColumn.insert("sampleHint", 999);
    const auto plan = buildNoteMovePlan(source(), dst);
    ASSERT_TRUE(plan.ok);
    EXPECT_EQ(plan.selection[0].toMap().value("sub_line").toInt(), 0);
    EXPECT_EQ(plan.selection[1].toMap().value("sub_line").toInt(), 1);
    EXPECT_EQ(plan.selection[0].toMap().value("sample").toInt(), 1);
}
TEST(NoteMovePlan, InvalidMemberTargetRejectsWholePlanInsteadOfClamping) {
    failedAtomic(buildNoteMovePlan(source(), drop(4)), "bad_target");
    auto dst = drop();
    dst.targetRank = 2;  // Metadata still says key2 rather than key1.
    failedAtomic(buildNoteMovePlan(source(), dst), "bad_target");
    auto src = source();
    src.ranks[1] = 99;
    failedAtomic(buildNoteMovePlan(src, drop()), "missing_columns");
    dst = drop();
    dst.laneLocked = true;
    dst.timeLocked = false;
    dst.deltaF = -2;
    failedAtomic(buildNoteMovePlan(source(), dst), "bad_target");
    dst.deltaF = std::numeric_limits<double>::infinity();
    failedAtomic(buildNoteMovePlan(source(), dst), "bad_delta");
}
TEST(NoteMovePlan, GrabMustMatchCompleteSourceIdentityNotOnlyLane) {
    auto src = source();
    src.grabRef.insert("sample", 9);
    failedAtomic(buildNoteMovePlan(src, drop()), "bad_grab");
    src = source();
    src.grabRef.insert("ln_channel", true);
    failedAtomic(buildNoteMovePlan(src, drop()), "bad_grab");
    src = source();
    src.grabRef.insert("note_kind", "mine");
    failedAtomic(buildNoteMovePlan(src, drop()), "bad_grab");
    src = source();
    src.grabRef.insert("sub_line", 4);
    failedAtomic(buildNoteMovePlan(src, drop()), "bad_grab");
    src = source();
    src.refs.append(src.refs.front());
    src.ranks.append(2);
    failedAtomic(buildNoteMovePlan(src, drop()), "duplicate_source");
}
TEST(NoteMovePlan, ZeroChangeAndAxisLocksHaveExplicitSideEffectFreeResult) {
    auto dst = drop(2);
    dst.deltaF = 3;  // timeLocked ignores movement in time.
    auto plan = buildNoteMovePlan(source(), dst);
    ASSERT_TRUE(plan.ok);
    EXPECT_FALSE(plan.changed);
    dst.laneLocked = true;
    plan = buildNoteMovePlan(source(), dst);
    ASSERT_TRUE(plan.ok);
    EXPECT_FALSE(plan.changed);
    EXPECT_EQ(plan.selection[0].toMap().value("pos"), source().refs[0].toMap().value("pos"));
}
TEST(NoteMovePlan, TimeSnapCannotSilentlyMergeDifferentSourceIdentities) {
    auto first = ref(1, 0, 1, 100);
    auto second = ref(1, 0, 2, 100);
    NoteMoveSource src{{first, second}, first, {}, -1, {}};
    NoteMoveDrop dst;
    dst.laneLocked = true;
    dst.deltaF = 0.001;
    failedAtomic(buildNoteMovePlan(src, dst), "duplicate_target");
}
TEST(NoteMovePlan, SamePositionDifferentNoteKindKeepsSeparateCompleteIdentities) {
    auto normal = ref();
    auto mine = normal;
    mine.insert("note_kind", "mine");
    NoteMoveSource src{{normal, mine}, normal, {2, 2}, 2, columns()};
    const auto plan = buildNoteMovePlan(src, drop());
    ASSERT_TRUE(plan.ok);
    EXPECT_EQ(plan.selection.size(), 2);
    EXPECT_EQ(plan.selection[0].toMap().value("note_kind").toString(), "normal");
    EXPECT_EQ(plan.selection[1].toMap().value("note_kind").toString(), "mine");
}
}  // namespace
