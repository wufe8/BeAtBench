// SPDX-License-Identifier: GPL-3.0-only
#include <gtest/gtest.h>

#include <QUuid>
#include <algorithm>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "beatbench/core/edit/SessionRegistry.hpp"
#include "bridge/PaneEditOperations.hpp"

namespace {
using beatbench::app::DocumentService;
using beatbench::app::PaneEditContext;
using beatbench::app::PaneEditOperations;
using beatbench::edit::session_registry;
bool ok(const QVariantMap& r) { return r.value("ok").toBool(); }
QString error(const QVariantMap& r) { return r.value("error").toMap().value("code").toString(); }
QVariantMap ref(int measure = 0, int num = 0, int den = 1, const QString& kind = "key",
                int index = 1, int sample = 1, int sub = 0) {
    return {{"measure", measure},
            {"pos", QVariantMap{{"num", num}, {"den", den}}},
            {"lane", QVariantMap{{"player", 0}, {"kind", kind}, {"index", index}}},
            {"sample", sample},
            {"sub_line", sub},
            {"ln_channel", false},
            {"note_kind", "normal"}};
}
QVariantMap hit(const QVariantMap& r) {
    const auto lane = r.value("lane").toMap();
    const auto pos = r.value("pos").toMap();
    return {{"valid", true},
            {"measure", r.value("measure")},
            {"num", pos.value("num")},
            {"den", pos.value("den")},
            {"lanePlayer", lane.value("player")},
            {"laneKind", lane.value("kind")},
            {"laneIndex", lane.value("index")},
            {"sub_line", r.value("sub_line")},
            {"bgaLayer", -1}};
}
QVariantMap settings(const QString& tool = "note", const QString& sample = "01") {
    return {{"tool", tool},    {"sampleText", sample}, {"bmpText", "02"}, {"bpmValue", 180},
            {"stopValue", 96}, {"snapNum", 1},         {"snapDen", 16}};
}
QVariantMap column(const QString& kind, int index, int line = -1) {
    return {{"valid", true},      {"lanePlayer", 0},  {"laneKind", kind},
            {"laneIndex", index}, {"sub_line", line}, {"bgaLayer", -1}};
}
QVariantList columns() {
    auto bpm = column("key", 0);
    bpm.insert("metaKind", "bpm");
    return {bpm,
            column("scratch", 1),
            column("key", 1),
            column("key", 2),
            column("key", 3),
            column("bgm", 1, 0),
            column("bgm", 1, 1),
            column("bgm", 1, 2)};
}
class PaneEditOperationsTest : public ::testing::Test {
   protected:
    void SetUp() override {
        auto& registry = session_registry();
        oldActive_ = registry.active_id();
        oldIds_ = registry.ids();
        a_ = create();
        b_ = create();
        ASSERT_TRUE(registry.activate(a_.toStdString()));
        documents_ = std::make_unique<DocumentService>();
        left_ = std::make_unique<PaneEditContext>(documents_.get());
        right_ = std::make_unique<PaneEditContext>(documents_.get());
        left_->setSessionId(a_);
        right_->setSessionId(b_);
        left_->setEditable(true);
        right_->setEditable(true);
        ops_ = std::make_unique<PaneEditOperations>(right_.get());
    }
    void TearDown() override {
        ops_.reset();
        right_.reset();
        left_.reset();
        documents_.reset();
        auto& registry = session_registry();
        registry.activate(oldActive_);
        for (const auto& id : registry.ids())
            if (std::find(oldIds_.begin(), oldIds_.end(), id) == oldIds_.end()) registry.close(id);
    }
    static QString create() {
        const auto id = QUuid::createUuid().toString(QUuid::WithoutBraces);
        auto& registry = session_registry();
        if (!registry.create(id.toStdString(), false)) return {};
        beatbench::Chart chart;
        chart.mode_id = "sp7k";
        chart.meta["BPM"] = "130";
        auto* s = registry.by_id(id.toStdString());
        s->load(std::move(chart));
        s->set_backup_enabled(false);
        s->set_autosave_enabled(false);
        return id;
    }
    static beatbench::edit::EditorSession* core(const QString& id) {
        return session_registry().by_id(id.toStdString());
    }
    bool put(const QVariantMap& r) {
        auto args = r;
        args.insert("kind", "normal");
        return ok(right_->dispatch("note.put", args));
    }
    QString a_, b_;
    std::string oldActive_;
    std::vector<std::string> oldIds_;
    std::unique_ptr<DocumentService> documents_;
    std::unique_ptr<PaneEditContext> left_, right_;
    std::unique_ptr<PaneEditOperations> ops_;
};
TEST_F(PaneEditOperationsTest, PlacementUsesTargetLiteralIdAndDoesNotImportOrActivate) {
    core(a_)->chart_mut().id_base = beatbench::IdBase::Base62;
    EXPECT_EQ(error(ops_->place(hit(ref()), settings("note", "0a"))), "invalid_id");
    ASSERT_TRUE(ok(ops_->place(hit(ref()), settings("note", "10"))));
    ASSERT_EQ(core(b_)->chart().notes.size(), 1u);
    EXPECT_EQ(core(b_)->chart().notes.front().value.sample.id, 36u);
    EXPECT_TRUE(core(b_)->chart().samples.empty());
    EXPECT_TRUE(core(a_)->chart().notes.empty());
    EXPECT_EQ(session_registry().active_id(), a_.toStdString());
    auto bgm = hit(ref(1, 0, 1, "bgm", 1, 1, 4));
    bgm.insert("sampleHint", 3);
    ASSERT_TRUE(ok(ops_->place(bgm, settings("note", ""))));
    EXPECT_EQ(core(b_)->chart().notes.back().value.sub_line, 4u);
    EXPECT_EQ(core(b_)->chart().notes.back().value.sample.id, 3u);
    right_->setEditable(false);
    EXPECT_EQ(error(ops_->place(hit(ref(2)), settings())), "read_only");
}
TEST_F(PaneEditOperationsTest, TimingAndBgaPlacementAndMetaDeleteRemainOnB) {
    auto bpm = hit(ref());
    bpm.insert("metaKind", "bpm");
    ASSERT_TRUE(ok(ops_->place(bpm, settings())));
    auto bga = hit(ref(1));
    bga.insert("bgaLayer", 2);
    ASSERT_TRUE(ok(ops_->place(bga, settings())));
    EXPECT_EQ(core(b_)->chart().bpm_events.size(), 1u);
    EXPECT_EQ(core(b_)->chart().bga_events.size(), 1u);
    const QVariantMap object{{"kind", "bga"},
                             {"measure", 1},
                             {"pos", QVariantMap{{"num", 0}, {"den", 1}}},
                             {"layer", 2},
                             {"sample", 2}};
    ASSERT_TRUE(ok(ops_->selectMeta(object, false)));
    ASSERT_TRUE(ok(ops_->deleteSelection()));
    EXPECT_TRUE(core(b_)->chart().bga_events.empty());
    EXPECT_TRUE(right_->metaSelection().isEmpty());
    EXPECT_TRUE(core(a_)->chart().bpm_events.empty());
}
TEST_F(PaneEditOperationsTest, SelectionIsPaneLocalPreservesGroupAndLnPartner) {
    left_->setSelection({ref(4)});
    ASSERT_TRUE(ok(ops_->selectNote(ref(), false)));
    ASSERT_TRUE(ok(ops_->selectNote(ref(1), true)));
    ASSERT_TRUE(ok(ops_->selectNote(ref(), false)));
    EXPECT_EQ(right_->selection().size(), 2);
    ASSERT_TRUE(ok(ops_->selectNote(ref(), true)));
    EXPECT_EQ(right_->selection().size(), 1);
    auto ln = ref(2);
    ln.insert("lnPartner", ref(3));
    ASSERT_TRUE(ok(ops_->selectNote(ln, false, true)));
    EXPECT_EQ(right_->selection().size(), 2);
    EXPECT_EQ(left_->selection().size(), 1);
    const QVariantMap meta{
        {"kind", "bpm"}, {"measure", 0}, {"pos", QVariantMap{{"num", 0}, {"den", 1}}}};
    ASSERT_TRUE(ok(ops_->selectMeta(meta, false)));
    EXPECT_TRUE(right_->selection().isEmpty());
    ASSERT_TRUE(ok(ops_->clearSelection()));
    EXPECT_TRUE(right_->metaSelection().isEmpty());
}
TEST_F(PaneEditOperationsTest, LnObjHeadTailUsesTargetDefinitionAndMovesHeadAtomically) {
    core(a_)->chart_mut().meta["LNOBJ"] = "02";
    core(b_)->chart_mut().meta["LNOBJ"] = "ZZ";
    ASSERT_TRUE(ok(ops_->place(hit(ref()), settings("ln"))));
    EXPECT_FALSE(right_->pendingLnHead().isEmpty());
    ASSERT_TRUE(ok(ops_->place(hit(ref(0, 1, 4, "key", 2)), settings("ln"))));
    EXPECT_EQ(core(b_)->undo_depth(), 2u);
    ASSERT_EQ(core(b_)->chart().notes.size(), 1u);
    EXPECT_EQ(core(b_)->chart().notes.front().value.lane.index, 2);
    EXPECT_EQ(error(ops_->place(hit(ref(0, 0, 1, "key", 2)), settings("ln"))), "bad_ln_tail");
    ASSERT_TRUE(ok(ops_->place(hit(ref(1, 0, 1, "key", 2)), settings("ln"))));
    EXPECT_TRUE(right_->pendingLnHead().isEmpty());
    EXPECT_EQ(core(b_)->chart().notes.back().value.sample.id, 1295u);
    EXPECT_TRUE(core(a_)->chart().notes.empty());
    ASSERT_TRUE(ok(ops_->undo()));
    ASSERT_EQ(core(b_)->chart().notes.size(), 1u);
    ASSERT_TRUE(ok(ops_->redo()));
    EXPECT_EQ(core(b_)->chart().notes.size(), 2u);
}
TEST_F(PaneEditOperationsTest, LnEscRejectsStaleHeadAndNeverDeletesAnotherDocument) {
    core(b_)->chart_mut().meta["LNOBJ"] = "ZZ";
    ASSERT_TRUE(ok(ops_->place(hit(ref()), settings("ln"))));
    ASSERT_TRUE(ok(ops_->cancelPendingLn()));
    EXPECT_TRUE(core(b_)->chart().notes.empty());
    ASSERT_TRUE(ok(ops_->place(hit(ref()), settings("ln"))));
    ASSERT_TRUE(put(ref(1)));
    EXPECT_EQ(error(ops_->cancelPendingLn()), "stale_ln");
    EXPECT_EQ(core(b_)->chart().notes.size(), 2u);
    ASSERT_TRUE(ok(ops_->place(hit(ref(2)), settings("ln"))));
    right_->setSessionId(a_);
    EXPECT_EQ(error(ops_->cancelPendingLn()), "no_pending_ln");
    EXPECT_TRUE(core(a_)->chart().notes.empty());
}
TEST_F(PaneEditOperationsTest, LnChannelPlacementAndToggleUseTargetLntype) {
    core(b_)->chart_mut().meta["LNTYPE"] = "1";
    core(b_)->chart_mut().meta["LNOBJ"] = "ZZ";
    ASSERT_TRUE(ok(ops_->place(hit(ref()), settings("ln"))));
    ASSERT_EQ(core(b_)->chart().notes.size(), 1u);
    EXPECT_TRUE(core(b_)->chart().notes.front().value.ln_channel);
    EXPECT_TRUE(right_->pendingLnHead().isEmpty());
    EXPECT_EQ(error(ops_->place(hit(ref(1, 0, 1, "bgm")), settings("ln"))), "unsupported_lane");
    auto selected = ref();
    selected.insert("ln_channel", true);
    ASSERT_TRUE(ok(ops_->selectNotes({selected})));
    ASSERT_TRUE(ok(ops_->toggleLn()));
    EXPECT_FALSE(core(b_)->chart().notes.front().value.ln_channel);
}
TEST_F(PaneEditOperationsTest, HorizontalMovePreservesOffGridAndOneUndoStep) {
    const auto source = ref(0, 1, 24);
    ASSERT_TRUE(put(source));
    ASSERT_TRUE(ok(ops_->selectNotes({source})));
    const auto before = core(b_)->undo_depth();
    ASSERT_TRUE(ok(ops_->moveSelection(0, column("key", 2), source.value("lane").toMap(), 0,
                                       columns(), settings())));
    EXPECT_EQ(core(b_)->undo_depth(), before + 1);
    EXPECT_EQ(core(b_)->chart().notes.front().pos, beatbench::Rational(1, 24));
    EXPECT_EQ(core(b_)->chart().notes.front().value.lane.index, 2);
    const auto selected = right_->selection().front().toMap();
    EXPECT_EQ(selected.value("pos"), source.value("pos"));
    EXPECT_EQ(selected.value("note_kind"), source.value("note_kind"));
    ASSERT_TRUE(ok(ops_->undo()));
    EXPECT_EQ(core(b_)->chart().notes.front().value.lane.index, 1);
}
TEST_F(PaneEditOperationsTest, BgmColumnRankMoveKeepsSpacingAndSublineIdentity) {
    const auto one = ref(0, 0, 1, "bgm", 1, 1, 0);
    const auto two = ref(0, 0, 1, "bgm", 1, 2, 1);
    ASSERT_TRUE(put(one));
    ASSERT_TRUE(put(two));
    ASSERT_TRUE(ok(ops_->selectNotes({one, two})));
    const auto before = core(b_)->undo_depth();
    ASSERT_TRUE(ok(ops_->moveSelection(0, column("bgm", 1, 1), one.value("lane").toMap(), 0,
                                       columns(), settings())));
    EXPECT_EQ(core(b_)->undo_depth(), before + 1);
    EXPECT_EQ(core(b_)->chart().notes[0].value.sub_line, 1u);
    EXPECT_EQ(core(b_)->chart().notes[1].value.sub_line, 2u);
    EXPECT_EQ(right_->selection()[1].toMap().value("sub_line").toInt(), 2);
    ASSERT_TRUE(ok(ops_->deleteSelection()));
    EXPECT_TRUE(core(b_)->chart().notes.empty());
}
TEST_F(PaneEditOperationsTest, IncompleteColumnsAndNegativeTargetsFailWithoutPartialMove) {
    ASSERT_TRUE(put(ref()));
    ASSERT_TRUE(ok(ops_->selectNotes({ref()})));
    const auto revision = core(b_)->revision();
    EXPECT_EQ(error(ops_->moveSelection(0, column("key", 2), ref().value("lane").toMap(), 0, {},
                                        settings())),
              "missing_columns");
    EXPECT_EQ(error(ops_->moveSelection(-1, {}, {}, 0, {}, settings())), "bad_target");
    EXPECT_EQ(core(b_)->revision(), revision);
    EXPECT_EQ(core(b_)->chart().notes.size(), 1u);
}
TEST_F(PaneEditOperationsTest, QuantizeAndTransformRetainFullSourceIdentity) {
    core(b_)->chart_mut().mode_id = "pms9k";
    const auto source = ref(0, 1, 24, "key", 1);
    ASSERT_TRUE(put(source));
    ASSERT_TRUE(ok(ops_->selectNotes({source})));
    ASSERT_TRUE(ok(ops_->quantize(settings())));
    EXPECT_EQ(core(b_)->chart().notes.front().pos, beatbench::Rational(1, 16));
    ASSERT_TRUE(ok(ops_->transform(true, 0)));
    EXPECT_EQ(core(b_)->chart().notes.front().value.lane.index, 9);
    EXPECT_EQ(right_->selection().front().toMap().value("lane").toMap().value("index").toInt(), 9);
    ASSERT_TRUE(ok(ops_->deleteNote(right_->selection().front().toMap())));
    EXPECT_TRUE(core(b_)->chart().notes.empty());
}
TEST_F(PaneEditOperationsTest, BgmQuantizeDoesNotTouchSameSampleOnAnotherSubline) {
    const auto zero = ref(0, 1, 24, "bgm", 1, 1, 0);
    const auto four = ref(0, 1, 24, "bgm", 1, 1, 4);
    ASSERT_TRUE(put(zero));
    ASSERT_TRUE(put(four));
    ASSERT_TRUE(ok(ops_->selectNotes({four})));
    ASSERT_TRUE(ok(ops_->quantize(settings())));
    for (const auto& e : core(b_)->chart().notes)
        EXPECT_EQ(e.pos,
                  e.value.sub_line == 4 ? beatbench::Rational(1, 16) : beatbench::Rational(1, 24));
    ASSERT_TRUE(ok(ops_->deleteSelection()));
    ASSERT_EQ(core(b_)->chart().notes.size(), 1u);
    EXPECT_EQ(core(b_)->chart().notes.front().value.sub_line, 0u);
}
TEST_F(PaneEditOperationsTest, SharedDocumentUndoWorksAcrossOperationInstances) {
    left_->setSessionId(b_);
    PaneEditOperations other(left_.get());
    ASSERT_TRUE(ok(ops_->place(hit(ref()), settings())));
    ASSERT_TRUE(ok(other.undo()));
    EXPECT_TRUE(core(b_)->chart().notes.empty());
    ASSERT_TRUE(ok(ops_->redo()));
    EXPECT_EQ(core(b_)->chart().notes.size(), 1u);
    EXPECT_EQ(left_->session(), right_->session());
}
TEST_F(PaneEditOperationsTest, MetaMoveAndConversionKeepAUnchanged) {
    auto bpm = hit(ref());
    bpm.insert("metaKind", "bpm");
    ASSERT_TRUE(ok(ops_->place(bpm, settings())));
    const QVariantMap object{{"kind", "bpm"},
                             {"measure", 0},
                             {"pos", QVariantMap{{"num", 0}, {"den", 1}}},
                             {"value", 180}};
    ASSERT_TRUE(ok(ops_->selectMeta(object, false)));
    ASSERT_TRUE(ok(ops_->moveMeta("bpm", object, 0.25, {}, settings())));
    EXPECT_EQ(core(b_)->chart().bpm_events.front().pos, beatbench::Rational(1, 4));
    const auto moved = right_->metaSelection().front().toMap();
    ASSERT_TRUE(ok(ops_->deleteMeta(moved)));
    EXPECT_TRUE(core(b_)->chart().bpm_events.empty());
    EXPECT_TRUE(core(a_)->chart().bpm_events.empty());
}

TEST_F(PaneEditOperationsTest, ClipboardCopyReturnsProtocolTextWithoutUndoOrSelectionChanges) {
    ASSERT_TRUE(put(ref()));
    ASSERT_TRUE(ok(ops_->selectNotes({ref()})));
    const auto revision = core(b_)->revision();
    const auto selection = right_->selection();
    const auto response = ops_->copySelection();
    ASSERT_TRUE(ok(response));
    const auto result = response.value("result").toMap();
    EXPECT_EQ(result.value("count").toInt(), 1);
    EXPECT_EQ(result.value("text").toString(), result.value("lines").toStringList().join('\n'));
    EXPECT_TRUE(result.value("text").toString().contains("#00011:01"));
    EXPECT_EQ(result.value("selectionDisposition").toString(), "preserve");
    EXPECT_EQ(core(b_)->revision(), revision);
    EXPECT_EQ(right_->selection(), selection);
    EXPECT_TRUE(core(a_)->chart().notes.empty());
}
TEST_F(PaneEditOperationsTest, PasteUsesLiteralTargetIdsAndReplacesOnlyOwnSelection) {
    left_->setSelection({ref(7)});
    right_->setMetaSelection({QVariantMap{{"kind", "bpm"}}});
    right_->setPendingLnHead(ref(4));
    const auto response = ops_->paste("#00011:ZZ", 2);
    ASSERT_TRUE(ok(response));
    ASSERT_EQ(core(b_)->chart().notes.size(), 1u);
    EXPECT_EQ(core(b_)->chart().notes.front().measure, 2u);
    EXPECT_EQ(core(b_)->chart().notes.front().value.sample.id, 1295u);
    EXPECT_TRUE(core(b_)->chart().samples.empty());
    ASSERT_EQ(right_->selection().size(), 1);
    const auto selected = right_->selection().front().toMap();
    EXPECT_EQ(selected.value("note_kind").toString(), "normal");
    EXPECT_FALSE(selected.value("ln_channel").toBool());
    EXPECT_TRUE(right_->metaSelection().isEmpty());
    EXPECT_TRUE(right_->pendingLnHead().isEmpty());
    EXPECT_EQ(left_->selection().size(), 1);
    EXPECT_EQ(response.value("result").toMap().value("selectionDisposition").toString(), "replace");
    EXPECT_EQ(core(b_)->undo_depth(), 1u);
    EXPECT_EQ(session_registry().active_id(), a_.toStdString());
}
TEST_F(PaneEditOperationsTest, DefinitionsFailWholePasteBeforeAnyUndoEvenWhenCaptured) {
    const auto revision = core(b_)->revision();
    const auto token = right_->capture();
    const QStringList definitions{"#WAV01 one.wav", "#BMP01 image.png", "#BPM01 180",
                                  "#STOP01 96",     "#00002:0.5",       "#00008:01"};
    for (const auto& line : definitions) {
        const auto text = line + "\n#00011:01";
        EXPECT_EQ(error(ops_->paste(text, 0)), "definitions_not_allowed");
        EXPECT_EQ(error(ops_->pasteCaptured(token, text, 0)), "definitions_not_allowed");
        EXPECT_EQ(core(b_)->revision(), revision);
        EXPECT_EQ(core(b_)->undo_depth(), 0u);
        EXPECT_TRUE(core(b_)->chart().notes.empty());
        EXPECT_TRUE(core(b_)->chart().samples.empty());
        EXPECT_TRUE(core(b_)->chart().measure_events.empty());
    }
}
TEST_F(PaneEditOperationsTest, ExplicitPrimaryDefinitionCompatibilityIsOptInAndAtomic) {
    PaneEditOperations primary(left_.get());
    const QString text = "#WAV01 clip.wav\n#00002:0.5\n#00011:01";
    EXPECT_EQ(error(primary.paste(text, 3)), "definitions_not_allowed");
    ASSERT_TRUE(ok(primary.paste(text, 3, {{"allowDefinitions", true}})));
    EXPECT_EQ(core(a_)->undo_depth(), 1u);
    EXPECT_EQ(core(a_)->chart().samples.size(), 1u);
    EXPECT_EQ(core(a_)->chart().measure_events.size(), 1u);
    EXPECT_EQ(core(a_)->chart().notes.size(), 1u);
    EXPECT_TRUE(core(b_)->chart().notes.empty());
    ASSERT_TRUE(ok(primary.undo()));
    EXPECT_TRUE(core(a_)->chart().samples.empty());
    EXPECT_TRUE(core(a_)->chart().measure_events.empty());
    EXPECT_TRUE(core(a_)->chart().notes.empty());
}
TEST_F(PaneEditOperationsTest, StrictBase36RejectsLowercaseAndBaseDeclarationOnAllPaths) {
    const auto revision = core(b_)->revision();
    for (const QString& text : {"#00011:0a", "#BASE 62\n#00011:10"}) {
        EXPECT_EQ(error(ops_->paste(text, 0)), "invalid_id");
        EXPECT_EQ(error(ops_->pasteCaptured(right_->capture(), text, 0)), "invalid_id");
        EXPECT_EQ(core(b_)->revision(), revision);
        EXPECT_EQ(core(b_)->chart().id_base, beatbench::IdBase::Base36);
    }
    // Lowercase comments are not ID tokens and do not trigger a blanket-text ban.
    ASSERT_TRUE(ok(ops_->paste("; lowercase comment\n#00011:0A", 0)));
    EXPECT_EQ(core(b_)->chart().notes.front().value.sample.id, 10u);
}
TEST_F(PaneEditOperationsTest, LegacyCaseFoldStillRequiresCapturedConfirmationAndKeepsTargetBase) {
    PaneEditOperations primary(left_.get());
    const QVariantMap legacy{{"legacyBase36CaseFold", true}};
    const QString text = "#BASE 62\n#00011:0a";
    EXPECT_EQ(error(primary.paste(text, 0, legacy)), "base62_confirmation_required");
    EXPECT_EQ(core(a_)->undo_depth(), 0u);
    const auto token = left_->capture();
    ASSERT_TRUE(ok(primary.pasteCaptured(token, text, 0, legacy)));
    EXPECT_EQ(core(a_)->chart().id_base, beatbench::IdBase::Base36);
    EXPECT_EQ(core(a_)->chart().notes.front().value.sample.id, 10u);
    EXPECT_EQ(error(primary.pasteCaptured(token, text, 1, legacy)), "stale_context");
    core(b_)->chart_mut().id_base = beatbench::IdBase::Base62;
    ASSERT_TRUE(ok(ops_->paste("#BASE 36\n#00011:0a", 1)));
    EXPECT_EQ(core(b_)->chart().id_base, beatbench::IdBase::Base62);
    EXPECT_EQ(core(b_)->chart().notes.front().value.sample.id, 36u);
}
TEST_F(PaneEditOperationsTest, CapturedPasteRejectsForeignReboundReloadedAndChangedTargets) {
    const auto token = right_->capture();
    PaneEditOperations primary(left_.get());
    EXPECT_EQ(error(primary.pasteCaptured(token, "#00011:01", 0)), "stale_context");
    ASSERT_TRUE(put(ref(1)));
    const auto revision = core(b_)->revision();
    EXPECT_EQ(error(ops_->pasteCaptured(token, "#00011:01", 2)), "stale_context");
    EXPECT_EQ(core(b_)->revision(), revision);
    const auto second = right_->capture();
    core(b_)->load(beatbench::Chart{});
    const auto reloaded = core(b_)->revision();
    EXPECT_EQ(error(ops_->pasteCaptured(second, "#00011:01", 2)), "stale_context");
    EXPECT_EQ(core(b_)->revision(), reloaded);
    const auto third = right_->capture();
    right_->setSessionId(a_);
    EXPECT_EQ(error(ops_->pasteCaptured(third, "#00011:01", 2)), "stale_context");
    EXPECT_TRUE(core(a_)->chart().notes.empty());
}
TEST_F(PaneEditOperationsTest, CapturedBgaEditUsesStrictTargetBaseAndClearsSelection) {
    auto event = hit(ref(1));
    event.insert("bgaLayer", 2);
    ASSERT_TRUE(ok(ops_->place(event, settings())));
    const QVariantMap object{{"kind", "bga"},
                             {"measure", 1},
                             {"pos", QVariantMap{{"num", 0}, {"den", 1}}},
                             {"layer", 2},
                             {"sample", 2}};
    ASSERT_TRUE(ok(ops_->selectMeta(object, false)));
    const auto info = ops_->metaEditInfo(object, {});
    ASSERT_TRUE(ok(info));
    EXPECT_EQ(info.value("result").toMap().value("text").toString(), "02");
    const auto token = right_->capture();
    const auto revision = core(b_)->revision();
    for (const QString& text : {"0a", " ZZ", "#BMPZZ", "00"})
        EXPECT_EQ(error(ops_->editMetaCaptured(token, object, text, {})), "invalid_id");
    EXPECT_EQ(core(b_)->revision(), revision);
    const auto response = ops_->editMetaCaptured(token, object, "ZZ", {});
    ASSERT_TRUE(ok(response));
    EXPECT_EQ(core(b_)->chart().bga_events.front().value.image.id, 1295u);
    EXPECT_TRUE(core(b_)->chart().samples.empty());
    EXPECT_TRUE(right_->metaSelection().isEmpty());
    EXPECT_EQ(response.value("result").toMap().value("selectionDisposition").toString(), "clear");
    EXPECT_TRUE(core(a_)->chart().bga_events.empty());
}
TEST_F(PaneEditOperationsTest, StopMillisecondsUseTargetEventTempoIncludingSamePositionBpm) {
    core(a_)->chart_mut().meta["BPM"] = "60";
    for (const int bpmMeasure : {1, 2}) {
        // Reload isolated B for each profile: after a tempo change and exactly at it.
        beatbench::Chart chart;
        chart.mode_id = "sp7k";
        chart.meta["BPM"] = "130";
        core(b_)->load(std::move(chart));
        documents_->refresh();
        ASSERT_TRUE(ok(right_->dispatch("timing.put", {{"kind", "bpm"},
                                                       {"measure", bpmMeasure},
                                                       {"pos", QVariantMap{{"num", 0}, {"den", 1}}},
                                                       {"value", 240}})));
        ASSERT_TRUE(ok(right_->dispatch("timing.put", {{"kind", "stop"},
                                                       {"measure", 2},
                                                       {"pos", QVariantMap{{"num", 0}, {"den", 1}}},
                                                       {"value", 96}})));
        const QVariantMap object{{"kind", "stop"},
                                 {"measure", 2},
                                 {"pos", QVariantMap{{"num", 0}, {"den", 1}}},
                                 {"value", 96}};
        const QVariantMap ms{{"stopUnit", 1}, {"stopBpm", 60}};  // Caller cannot spoof the basis.
        const auto info = ops_->metaEditInfo(object, ms);
        ASSERT_TRUE(ok(info));
        const auto values = info.value("result").toMap();
        EXPECT_DOUBLE_EQ(values.value("stopBpm").toDouble(), 240.0);
        EXPECT_DOUBLE_EQ(values.value("text").toString().toDouble(), 500.0);
        const auto depth = core(b_)->undo_depth();
        ASSERT_TRUE(ok(ops_->editMetaCaptured(right_->capture(), object,
                                              values.value("text").toString(), ms)));
        EXPECT_EQ(core(b_)->chart().stop_events.front().value.count, 96);
        EXPECT_EQ(core(b_)->undo_depth(), depth + 1);  // Keep original protocol command semantics.
        ASSERT_TRUE(ok(ops_->editMetaCaptured(right_->capture(), object, "1000", ms)));
        EXPECT_EQ(core(b_)->chart().stop_events.front().value.count, 192);
        EXPECT_EQ(core(b_)->undo_depth(), depth + 2);
        ASSERT_TRUE(ok(ops_->undo()));
        EXPECT_EQ(core(b_)->chart().stop_events.front().value.count, 96);
    }
}
TEST_F(PaneEditOperationsTest, CapturedMetaRejectsStaleOwnerAndInvalidTextWithoutUndo) {
    auto event = hit(ref());
    event.insert("metaKind", "bpm");
    ASSERT_TRUE(ok(ops_->place(event, settings())));
    const QVariantMap object{{"kind", "bpm"},
                             {"measure", 0},
                             {"pos", QVariantMap{{"num", 0}, {"den", 1}}},
                             {"value", 180}};
    const auto token = right_->capture();
    const auto depth = core(b_)->undo_depth();
    for (const QString& text : {"NaN", "Infinity", "180junk", "0", "-1"})
        EXPECT_EQ(error(ops_->editMetaCaptured(token, object, text, {})), "invalid_value");
    EXPECT_EQ(core(b_)->undo_depth(), depth);
    ASSERT_TRUE(put(ref(2)));
    const auto changedDepth = core(b_)->undo_depth();
    EXPECT_EQ(error(ops_->editMetaCaptured(token, object, "190", {})), "stale_context");
    EXPECT_EQ(core(b_)->undo_depth(), changedDepth);
    EXPECT_DOUBLE_EQ(core(b_)->chart().bpm_events.front().value.value, 180);
    auto missing = object;
    missing.insert("measure", 7);
    EXPECT_EQ(error(ops_->metaEditInfo(missing, {})), "not_found");
    EXPECT_TRUE(core(a_)->chart().bpm_events.empty());
}
TEST_F(PaneEditOperationsTest, CapturedMetaCannotRetargetAnotherOverlappingEvent) {
    auto event = hit(ref());
    event.insert("metaKind", "bpm");
    ASSERT_TRUE(ok(ops_->place(event, settings())));
    auto duplicate = core(b_)->chart().bpm_events.front();
    duplicate.value.value = 240;
    core(b_)->chart_mut().bpm_events.push_back(duplicate);
    documents_->refresh();
    const QVariantMap earlier{{"kind", "bpm"},
                              {"measure", 0},
                              {"pos", QVariantMap{{"num", 0}, {"den", 1}}},
                              {"value", 180}};
    const auto depth = core(b_)->undo_depth();
    EXPECT_EQ(error(ops_->editMetaCaptured(right_->capture(), earlier, "190", {})), "not_found");
    EXPECT_EQ(core(b_)->undo_depth(), depth);
    EXPECT_DOUBLE_EQ(core(b_)->chart().bpm_events.front().value.value, 180);
    EXPECT_DOUBLE_EQ(core(b_)->chart().bpm_events.back().value.value, 240);
    auto effective = earlier;
    effective.insert("value", 240);
    ASSERT_TRUE(ok(ops_->editMetaCaptured(right_->capture(), effective, "250", {})));
    EXPECT_DOUBLE_EQ(core(b_)->chart().bpm_events.front().value.value, 180);
    EXPECT_DOUBLE_EQ(core(b_)->chart().bpm_events.back().value.value, 250);
}
}  // namespace
