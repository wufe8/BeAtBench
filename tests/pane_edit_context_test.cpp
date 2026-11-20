// SPDX-License-Identifier: GPL-3.0-only
#include <gtest/gtest.h>

#include <QUuid>
#include <algorithm>
#include <memory>
#include <string>
#include <vector>

#include "beatbench/core/edit/SessionRegistry.hpp"
#include "bridge/PaneEditContext.hpp"

namespace {
using beatbench::app::DocumentService;
using beatbench::app::PaneEditContext;
using beatbench::edit::session_registry;

bool ok(const QVariantMap& response) { return response.value("ok").toBool(); }
QString code(const QVariantMap& response) {
    return response.value("error").toMap().value("code").toString();
}
QVariantMap putArgs(int measure = 0, int sample = 1) {
    return {{"measure", measure},
            {"pos", QVariantMap{{"num", 0}, {"den", 1}}},
            {"lane", QVariantMap{{"player", 0}, {"kind", "key"}, {"index", 1}}},
            {"sample", sample},
            {"kind", "normal"}};
}
class PaneEditContextTest : public ::testing::Test {
   protected:
    void SetUp() override {
        auto& reg = session_registry();
        previous_ = reg.active_id();
        ids_ = reg.ids();
        a_ = create();
        b_ = create();
        ASSERT_FALSE(a_.isEmpty());
        ASSERT_FALSE(b_.isEmpty());
        ASSERT_TRUE(reg.activate(a_.toStdString()));
        documents_ = std::make_unique<DocumentService>();
        left_ = std::make_unique<PaneEditContext>(documents_.get());
        right_ = std::make_unique<PaneEditContext>(documents_.get());
        left_->setSessionId(a_);
        right_->setSessionId(b_);
        left_->setEditable(true);
        right_->setEditable(true);
    }
    void TearDown() override {
        right_.reset();
        left_.reset();
        documents_.reset();
        auto& reg = session_registry();
        reg.activate(previous_);
        for (const auto& id : reg.ids())
            if (std::find(ids_.begin(), ids_.end(), id) == ids_.end()) reg.close(id);
    }
    static QString create() {
        const auto id = QUuid::createUuid().toString(QUuid::WithoutBraces);
        auto& reg = session_registry();
        if (!reg.create(id.toStdString(), false)) return {};
        auto* s = reg.by_id(id.toStdString());
        beatbench::Chart chart;
        chart.mode_id = "sp7k";
        s->load(std::move(chart));
        s->set_backup_enabled(false);
        s->set_autosave_enabled(false);
        return id;
    }
    static beatbench::edit::EditorSession* core(const QString& id) {
        return session_registry().by_id(id.toStdString());
    }
    QString a_, b_;
    std::string previous_;
    std::vector<std::string> ids_;
    std::unique_ptr<DocumentService> documents_;
    std::unique_ptr<PaneEditContext> left_, right_;
};

TEST_F(PaneEditContextTest, InactiveBEditsOnlyItsDocumentAndRefreshesFixedBridge) {
    auto* bridge = right_->session();
    ASSERT_NE(bridge, nullptr);
    const auto oldHash = bridge->debugContentHash();
    int edited = 0;
    QString editedId;
    QObject::connect(right_.get(), &PaneEditContext::edited, right_.get(), [&](const QString& id) {
        ++edited;
        editedId = id;
    });
    ASSERT_TRUE(ok(right_->dispatch("note.put", putArgs())));
    EXPECT_TRUE(core(a_)->chart().notes.empty());
    EXPECT_EQ(core(b_)->chart().notes.size(), 1u);
    EXPECT_EQ(session_registry().active_id(), a_.toStdString());
    EXPECT_EQ(documents_->currentSession(), left_->session());
    EXPECT_NE(bridge->debugContentHash(), oldHash);
    EXPECT_EQ(edited, 1);
    EXPECT_EQ(editedId, b_);
    EXPECT_TRUE(core(b_)->is_dirty());
}
TEST_F(PaneEditContextTest, SameDocumentPanesShareUndoAndBridgeButNotSelection) {
    right_->setSessionId(a_);
    ASSERT_EQ(left_->session(), right_->session());
    left_->setSelection({putArgs(1)});
    right_->setSelection({putArgs(2)});
    right_->setMetaSelection({QVariantMap{{"kind", "bpm"}}});
    ASSERT_TRUE(ok(left_->dispatch("note.put", putArgs())));
    ASSERT_TRUE(ok(right_->dispatch("note.put", putArgs(1))));
    EXPECT_EQ(core(a_)->undo_depth(), 2u);
    ASSERT_TRUE(ok(right_->dispatch("session.undo")));
    EXPECT_EQ(core(a_)->chart().notes.size(), 1u);
    ASSERT_TRUE(ok(left_->dispatch("session.redo")));
    EXPECT_EQ(core(a_)->chart().notes.size(), 2u);
    EXPECT_NE(left_->selection(), right_->selection());
    EXPECT_TRUE(left_->metaSelection().isEmpty());
    EXPECT_EQ(right_->metaSelection().size(), 1);
}
TEST_F(PaneEditContextTest, RejectsLifecycleSaveWrongIdAndReadOnlyWithoutFallthrough) {
    const auto revision = core(a_)->revision();
    for (const QString& command :
         {"session.save", "session.load", "session.new", "session.activate", "session.close",
          "sample.setFile", "note.put.extra"})
        EXPECT_EQ(code(right_->dispatch(command)), "command_not_allowed");
    auto args = putArgs();
    args.insert("session_id", a_);
    EXPECT_EQ(code(right_->dispatch("note.put", args)), "wrong_document");
    args.insert("session_id", "");
    EXPECT_EQ(code(right_->dispatch("note.put", args)), "wrong_document");
    right_->setEditable(false);
    EXPECT_EQ(code(right_->dispatch("note.put", putArgs())), "read_only");
    EXPECT_TRUE(right_->capture().isEmpty());
    right_->setEditable(true);
    right_->setSessionId("");
    EXPECT_EQ(code(right_->dispatch("note.put", putArgs())), "no_document");
    right_->setSessionId("missing-pane-document");
    EXPECT_EQ(right_->session(), nullptr);
    EXPECT_EQ(code(right_->dispatch("session.undo")), "no_document");
    EXPECT_EQ(core(a_)->revision(), revision);
    EXPECT_TRUE(core(b_)->chart().notes.empty());
}
TEST_F(PaneEditContextTest, LiteralIdPreservesTextAndUsesOnlyTargetBaseWithoutDefinitions) {
    core(a_)->chart_mut().id_base = beatbench::IdBase::Base62;
    const auto source = left_->decodeLiteralId("0a");
    ASSERT_TRUE(ok(source));
    EXPECT_EQ(source.value("result").toMap().value("sample").toInt(), 36);
    EXPECT_EQ(source.value("result").toMap().value("text").toString(), "0a");
    EXPECT_EQ(code(right_->decodeLiteralId("0a")), "invalid_id");
    EXPECT_EQ(left_->decodeLiteralId("10").value("result").toMap().value("sample").toInt(), 62);
    EXPECT_EQ(right_->decodeLiteralId("10").value("result").toMap().value("sample").toInt(), 36);
    for (const QString& text : {"", "0", "001", " 1", "1 ", "00", "#1", "é1", "Ａ1"})
        EXPECT_EQ(code(right_->decodeLiteralId(text)), "invalid_id");
    const auto decoded = right_->decodeLiteralId("ZZ");
    ASSERT_TRUE(ok(decoded));
    const int sample = decoded.value("result").toMap().value("sample").toInt();
    ASSERT_TRUE(ok(right_->dispatch("note.put", putArgs(0, sample))));
    EXPECT_EQ(core(b_)->chart().notes.front().value.sample.id, 1295u);
    EXPECT_TRUE(core(b_)->chart().samples.empty());
    auto set = putArgs(0, sample);
    set.insert("to", "0a");
    EXPECT_EQ(code(right_->dispatch("note.setSample", set)), "invalid_id");
}
TEST_F(PaneEditContextTest, CaptureCurrentIsNonConsumingAndUnaffectedByFocusOrSelection) {
    const auto token = right_->capture();
    ASSERT_TRUE(right_->captureCurrent(token));
    EXPECT_FALSE(left_->captureCurrent(token));
    auto forged = token;
    forged.insert("sessionId", a_);
    EXPECT_FALSE(right_->captureCurrent(forged));
    EXPECT_FALSE(right_->captureCurrent({}));
    ASSERT_TRUE(session_registry().activate(b_.toStdString()));
    right_->setSelection({putArgs(4)});
    right_->setMetaSelection({QVariantMap{{"kind", "bpm"}}});
    EXPECT_TRUE(right_->captureCurrent(token));
    EXPECT_TRUE(right_->captureCurrent(token));  // Read-only query does not consume.
    ASSERT_TRUE(ok(right_->dispatchCaptured(token, "note.put", putArgs())));
    EXPECT_FALSE(right_->captureCurrent(token));
}
TEST_F(PaneEditContextTest, CaptureCurrentDetectsReplacementRevisionReloadAndPermissionLoss) {
    auto token = right_->capture();
    const auto replacement = right_->capture();
    EXPECT_FALSE(right_->captureCurrent(token));
    EXPECT_TRUE(right_->captureCurrent(replacement));
    token = replacement;
    ASSERT_TRUE(ok(right_->dispatch("note.put", putArgs())));
    ASSERT_TRUE(ok(right_->dispatch("session.undo")));
    EXPECT_FALSE(right_->captureCurrent(token));
    token = right_->capture();
    core(b_)->load(beatbench::Chart{});
    EXPECT_FALSE(right_->captureCurrent(token));
    token = right_->capture();
    right_->setEditable(false);
    right_->setEditable(true);
    EXPECT_FALSE(right_->captureCurrent(token));
    token = right_->capture();
    right_->setSessionId(a_);
    right_->setSessionId(b_);
    EXPECT_FALSE(right_->captureCurrent(token));
    token = right_->capture();
    documents_.reset();
    EXPECT_FALSE(right_->captureCurrent(token));
}
TEST_F(PaneEditContextTest, CapturesAreOpaqueOwnerBoundReplaceableAndSingleUse) {
    const auto first = right_->capture();
    EXPECT_FALSE(first.contains("revision"));
    EXPECT_FALSE(first.contains("owner"));
    EXPECT_EQ(code(left_->dispatchCaptured(first, "note.put", putArgs())), "stale_context");
    const auto second = right_->capture();
    EXPECT_EQ(code(right_->dispatchCaptured(first, "note.put", putArgs())), "stale_context");
    ASSERT_TRUE(ok(right_->dispatchCaptured(second, "note.put", putArgs())));
    EXPECT_EQ(code(right_->dispatchCaptured(second, "note.put", putArgs(1))), "stale_context");
    EXPECT_TRUE(core(a_)->chart().notes.empty());
}
TEST_F(PaneEditContextTest, CapturesRejectRevisionReloadRebindAndPermissionChanges) {
    auto token = right_->capture();
    ASSERT_TRUE(ok(right_->dispatch("note.put", putArgs())));
    ASSERT_TRUE(ok(right_->dispatch("session.undo")));
    EXPECT_EQ(code(right_->dispatchCaptured(token, "note.put", putArgs())), "stale_context");
    token = right_->capture();
    core(b_)->load(beatbench::Chart{});
    EXPECT_EQ(code(right_->dispatchCaptured(token, "note.put", putArgs())), "stale_context");
    token = right_->capture();
    right_->setSessionId(a_);
    right_->setSessionId(b_);
    EXPECT_EQ(code(right_->dispatchCaptured(token, "note.put", putArgs())), "stale_context");
    token = right_->capture();
    right_->setEditable(false);
    right_->setEditable(true);
    EXPECT_EQ(code(right_->dispatchCaptured(token, "note.put", putArgs())), "stale_context");
}
TEST_F(PaneEditContextTest, ClosedAndRecreatedIdCannotReviveCapturedOwner) {
    const auto token = right_->capture();
    EXPECT_TRUE(right_->captureCurrent(token));
    ASSERT_TRUE(session_registry().close(b_.toStdString()));
    EXPECT_FALSE(right_->captureCurrent(token));
    EXPECT_EQ(code(right_->dispatch("note.put", putArgs())), "no_document");
    ASSERT_TRUE(session_registry().create(b_.toStdString(), false));
    core(b_)->load(beatbench::Chart{});
    EXPECT_FALSE(right_->captureCurrent(token));
    EXPECT_EQ(code(right_->dispatchCaptured(token, "note.put", putArgs())), "stale_context");
    EXPECT_TRUE(core(a_)->chart().notes.empty());
}
TEST_F(PaneEditContextTest, RebindingClearsOnlyPaneStateAndNoopDoesNotEmitEdited) {
    ASSERT_TRUE(ok(right_->dispatch("note.put", putArgs())));
    right_->setSelection({putArgs()});
    right_->setMetaSelection({QVariantMap{{"kind", "bpm"}}});
    right_->setPendingLnHead(putArgs());
    right_->setSessionId(a_);
    EXPECT_TRUE(right_->selection().isEmpty());
    EXPECT_TRUE(right_->metaSelection().isEmpty());
    EXPECT_TRUE(right_->pendingLnHead().isEmpty());
    EXPECT_EQ(core(b_)->chart().notes.size(), 1u);
    int edited = 0;
    QObject::connect(right_.get(), &PaneEditContext::edited, right_.get(),
                     [&](const QString&) { ++edited; });
    const auto response = right_->dispatch("session.undo");
    EXPECT_EQ(code(response), "not_applied");
    EXPECT_EQ(edited, 0);
    EXPECT_TRUE(response.contains("result"));
    EXPECT_TRUE(response.contains("error"));
}
TEST_F(PaneEditContextTest, DestroyedDocumentServiceCannotFallBackToActive) {
    const auto token = right_->capture();
    documents_.reset();
    EXPECT_FALSE(right_->available());
    EXPECT_EQ(right_->session(), nullptr);
    EXPECT_EQ(code(right_->dispatch("note.put", putArgs())), "no_document");
    EXPECT_EQ(code(right_->dispatchCaptured(token, "note.put", putArgs())), "stale_context");
    EXPECT_TRUE(core(a_)->chart().notes.empty());
}
}  // namespace
