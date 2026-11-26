// SPDX-License-Identifier: GPL-3.0-only
// Real registry, fixed bridges and view geometry; no QML, audio or OS drag/drop.
#include <gtest/gtest.h>

#include <QGuiApplication>
#include <QJsonDocument>
#include <QJsonObject>
#include <QUuid>
#include <algorithm>
#include <cmath>
#include <limits>
#include <memory>
#include <string>
#include <vector>

#include "beatbench/core/edit/SessionRegistry.hpp"
#include "bridge/PaneDragController.hpp"

namespace {
using namespace beatbench::app;
using beatbench::edit::session_registry;
bool ok(const QVariantMap& r) { return r.value("ok").toBool(); }
QString code(const QVariantMap& r) { return r.value("error").toMap().value("code").toString(); }
QVariantMap result(const QVariantMap& r) { return r.value("result").toMap(); }

class PaneDragControllerTest : public ::testing::Test {
   protected:
    void SetUp() override {
        auto& registry = session_registry();
        previous_ = registry.active_id();
        oldIds_ = registry.ids();
        for (auto* id : {&a_, &b_}) {
            *id = QUuid::createUuid().toString(QUuid::WithoutBraces);
            ASSERT_TRUE(registry.create(id->toStdString(), false));
            auto* s = core(*id);
            beatbench::Chart chart;
            chart.mode_id = "sp7k";
            chart.meta["BPM"] = "120";
            for (const auto measure : {1u, 8u}) {
                beatbench::Event<beatbench::Note> note{measure, {1, 4}, {}};
                note.value.lane = {0, beatbench::LaneKind::Key, 1};
                note.value.sample.id = measure;
                chart.notes.push_back(note);
            }
            s->load(std::move(chart));
            s->set_backup_enabled(false);
            s->set_autosave_enabled(false);
        }
        ASSERT_TRUE(registry.activate(b_.toStdString()));  // Neither pane follows global active.
        documents_ = std::make_unique<DocumentService>();
        left_ = std::make_unique<PaneEditContext>(documents_.get());
        right_ = std::make_unique<PaneEditContext>(documents_.get());
        for (auto* context : {left_.get(), right_.get()}) {
            context->setSessionId(a_);
            context->setEditable(true);
        }
        leftOps_ = std::make_unique<PaneEditOperations>(left_.get());
        rightOps_ = std::make_unique<PaneEditOperations>(right_.get());
        root_ = std::make_unique<QQuickItem>();
        root_->setSize({2000, 1000});
        leftSurface_ = std::make_unique<QQuickItem>(root_.get());
        rightSurface_ = std::make_unique<QQuickItem>(root_.get());
        rightSurface_->setPosition({850, 35});
        rightSurface_->setScale(0.9);
        leftView_ = std::make_unique<ChartViewItem>(leftSurface_.get());
        rightView_ = std::make_unique<ChartViewItem>(rightSurface_.get());
        for (auto* view : {leftView_.get(), rightView_.get()}) {
            view->setSize({780, 650});
            view->setTopHigh(false);
            view->setMeasureHeight(120);
            view->setEditableMeasures(12);
            view->setSession(left_->session());
            view->setScrollY(0);
        }
        controller_ = std::make_unique<PaneDragController>();
        ASSERT_TRUE(controller_->registerPane(left_.get(), leftSurface_.get(), leftView_.get(),
                                              leftOps_.get()));
        ASSERT_TRUE(controller_->registerPane(right_.get(), rightSurface_.get(), rightView_.get(),
                                              rightOps_.get()));
        press_ = notePoint(*leftView_);
        ref_ = leftView_->objectAt(press_.x(), press_.y());
        ASSERT_EQ(ref_.value("kind").toString(), "note");
        left_->setSelection({ref_});
    }
    void TearDown() override {
        controller_.reset();
        rightView_.reset();
        leftView_.reset();
        rightSurface_.reset();
        leftSurface_.reset();
        root_.reset();
        rightOps_.reset();
        leftOps_.reset();
        right_.reset();
        left_.reset();
        documents_.reset();
        auto& registry = session_registry();
        registry.activate(previous_);
        for (const auto& id : registry.ids())
            if (std::find(oldIds_.begin(), oldIds_.end(), id) == oldIds_.end()) registry.close(id);
    }
    static beatbench::edit::EditorSession* core(const QString& id) {
        return session_registry().by_id(id.toStdString());
    }
    // Find centers through public hit geometry, not duplicated private column widths.
    static qreal columnX(const ChartViewItem& view, const QString& kind = "key", int index = 2,
                         const QString& meta = {}, int bga = -1) {
        qreal first = -1, last = -1;
        for (int x = 0; x < view.width(); ++x) {
            const auto c = view.laneAtX(x);
            if (!c.value("valid").toBool() || c.value("metaKind").toString() != meta ||
                c.value("bgaLayer", -1).toInt() != bga)
                continue;
            if (meta.isEmpty() && bga < 0 &&
                (c.value("laneKind") != kind || c.value("laneIndex").toInt() != index))
                continue;
            if (first < 0) first = x;
            last = x;
        }
        return first < 0 ? -1 : (first + last) / 2;
    }
    static QPointF notePoint(const ChartViewItem& view) {
        const auto x = columnX(view, "key", 1);
        for (int y = 18; y < view.height(); ++y)
            if (view.objectAt(x, y).value("kind") == "note") return {x, qreal(y + 2)};
        return {-1, -1};
    }
    static QPointF at(const ChartViewItem& view, double f, qreal x) {
        qreal bestY = -1;
        double best = std::numeric_limits<double>::max();
        for (qreal y = 18; y < view.height(); y += 0.125) {
            const double mf = view.measureAtY(y);
            if (mf >= 0 && std::abs(mf - f) < best) {
                best = std::abs(mf - f);
                bestY = y;
            }
        }
        return {x, bestY};
    }
    QPointF target(double f = 1.43, int lane = 2) const {
        const auto local = at(*rightView_, f, columnX(*rightView_, "key", lane));
        return leftView_->mapFromScene(rightView_->mapToScene(local));
    }
    bool begin(const QPointF& current) {
        return controller_->begin(leftSurface_.get(), press_, current, ref_, left_->selection());
    }
    QString a_, b_;
    std::string previous_;
    std::vector<std::string> oldIds_;
    std::unique_ptr<DocumentService> documents_;
    std::unique_ptr<PaneEditContext> left_, right_;
    std::unique_ptr<PaneEditOperations> leftOps_, rightOps_;
    std::unique_ptr<QQuickItem> root_, leftSurface_, rightSurface_;
    std::unique_ptr<ChartViewItem> leftView_, rightView_;
    std::unique_ptr<PaneDragController> controller_;
    QPointF press_;
    QVariantMap ref_;
};

TEST_F(PaneDragControllerTest, ViewReferencesAndColumnsSerializeLaneBytesAsJsonNumbers) {
    const auto reference = QJsonDocument::fromVariant(ref_).object();
    const auto lane = reference.value("lane").toObject();
    EXPECT_TRUE(lane.value("player").isDouble());
    EXPECT_TRUE(lane.value("index").isDouble());
    EXPECT_EQ(lane.value("player").toInt(-1), 0);
    EXPECT_EQ(lane.value("index").toInt(-1), 1);
    const auto cols = leftView_->editColumns();
    for (const auto& c : cols) {
        const auto json = QJsonDocument::fromVariant(c).object();
        EXPECT_TRUE(json.value("lanePlayer").isDouble());
        EXPECT_TRUE(json.value("laneIndex").isDouble());
    }
}

TEST_F(PaneDragControllerTest, SceneMappedRawSampleUsesTargetSnapAndCommitsExactPreviewOnce) {
    rightView_->setMeasureHeight(200);
    rightView_->setScrollY(110);
    leftSurface_->setProperty("snapDen", 16);
    rightSurface_->setProperty("snapDen", 4);
    right_->setSelection({QVariantMap{{"old", true}}});
    right_->setMetaSelection({QVariantMap{{"kind", "bpm"}}});
    right_->setPendingLnHead(ref_);
    const auto point = target();
    int committed = 0;
    QObject::connect(controller_.get(), &PaneDragController::selectionCommitted, controller_.get(),
                     [&](PaneEditContext* source, PaneEditContext* dest, const QVariantList& refs) {
                         ++committed;
                         EXPECT_EQ(source, left_.get());
                         EXPECT_EQ(dest, right_.get());
                         EXPECT_TRUE(source->selection().isEmpty());
                         EXPECT_EQ(dest->selection(), refs);
                     });
    ASSERT_TRUE(begin(point));
    const auto preview = rightView_->plannedMovePreview();
    ASSERT_EQ(preview.size(), 1);
    EXPECT_EQ(preview.front().toMap().value("pos").toMap().value("den").toInt(), 2);
    EXPECT_TRUE(leftView_->plannedMovePreview().isEmpty());
    EXPECT_TRUE(leftView_->plannedMovePreviewActive());
    const auto response = controller_->finish(leftSurface_.get(), point);
    ASSERT_TRUE(ok(response)) << QJsonDocument::fromVariant(response).toJson().toStdString();
    EXPECT_EQ(right_->selection(), preview);
    EXPECT_EQ(core(a_)->undo_depth(), 1u);
    EXPECT_EQ(committed, 1);
    EXPECT_FALSE(controller_->active());
    EXPECT_FALSE(rightView_->plannedMovePreviewActive());
    EXPECT_TRUE(right_->metaSelection().isEmpty());
    EXPECT_TRUE(right_->pendingLnHead().isEmpty());
    EXPECT_EQ(session_registry().active_id(), b_.toStdString());
    EXPECT_EQ(core(b_)->undo_depth(), 0u);
    EXPECT_FALSE(ok(controller_->finish(leftSurface_.get(), point)));
    EXPECT_EQ(core(a_)->undo_depth(), 1u);
    ASSERT_TRUE(core(a_)->undo());
    EXPECT_EQ(core(a_)->chart().notes.front().value.lane.index, 1);
    EXPECT_EQ(core(a_)->chart().notes.front().pos, beatbench::Rational(1, 4));
}

TEST_F(PaneDragControllerTest, DifferentDocumentOrOutsideIsOwnedButNeverFallsBack) {
    right_->setSessionId(b_);
    rightView_->setSession(right_->session());
    const auto point = target();
    ASSERT_TRUE(begin(point));
    auto response = controller_->update(leftSurface_.get(), point);
    EXPECT_EQ(code(response), "different_document");
    EXPECT_TRUE(result(response).value("handled").toBool());
    EXPECT_FALSE(ok(controller_->finish(leftSurface_.get(), point)));
    EXPECT_EQ(core(a_)->undo_depth(), 0u);
    EXPECT_EQ(core(b_)->undo_depth(), 0u);
    ASSERT_TRUE(begin({-40, -40}));
    response = controller_->finish(leftSurface_.get(), {-40, -40});
    EXPECT_EQ(code(response), "invalid_target");
    EXPECT_TRUE(result(response).value("wasDrag").toBool());
    EXPECT_EQ(core(a_)->undo_depth(), 0u);
}

TEST_F(PaneDragControllerTest, ForeignSurfaceCannotStealGrabAndForgedIdentityCannotBegin) {
    auto forged = ref_;
    forged.insert("sample", 9);
    left_->setSelection({forged});
    EXPECT_FALSE(controller_->begin(leftSurface_.get(), press_, target(), forged, {forged}));
    left_->setSelection({ref_});
    ASSERT_TRUE(begin(target()));
    EXPECT_FALSE(
        result(controller_->finish(rightSurface_.get(), target())).value("handled").toBool());
    controller_->cancel(rightSurface_.get(), "foreign");
    EXPECT_TRUE(controller_->active());
    ASSERT_TRUE(ok(controller_->finish(leftSurface_.get(), target())));
}

TEST_F(PaneDragControllerTest, SelectionLayoutAndRevisionInvalidateFrozenSource) {
    ASSERT_TRUE(begin(target()));
    left_->setSelection({});
    EXPECT_FALSE(controller_->active());
    EXPECT_FALSE(ok(controller_->finish(leftSurface_.get(), target())));
    left_->setSelection({ref_});
    ASSERT_TRUE(begin(target()));
    leftView_->setShowExtras(true);
    EXPECT_FALSE(ok(controller_->finish(leftSurface_.get(), target())));
    EXPECT_EQ(core(a_)->undo_depth(), 0u);
    leftView_->setShowExtras(false);
    ASSERT_TRUE(begin(target()));
    ASSERT_TRUE(ok(right_->dispatch("note.put", {{"measure", 4},
                                                 {"pos", QVariantMap{{"num", 0}, {"den", 1}}},
                                                 {"lane", ref_.value("lane")},
                                                 {"sample", 7},
                                                 {"kind", "normal"}})));
    ASSERT_TRUE(core(a_)->undo());
    EXPECT_FALSE(ok(controller_->finish(leftSurface_.get(), target())));
    EXPECT_EQ(core(a_)->undo_depth(), 0u);
}

TEST_F(PaneDragControllerTest, HiddenReadOnlyDestroyedAndReloadedEndpointsCancelSafely) {
    ASSERT_TRUE(begin(target()));
    rightSurface_->setVisible(false);
    EXPECT_FALSE(controller_->active());
    rightSurface_->setVisible(true);
    ASSERT_TRUE(begin(target()));
    right_->setEditable(false);
    EXPECT_FALSE(controller_->active());
    right_->setEditable(true);
    ASSERT_TRUE(begin(target()));
    rightView_.reset();
    EXPECT_FALSE(controller_->active());
    EXPECT_FALSE(ok(controller_->finish(leftSurface_.get(), {900, 200})));
    EXPECT_EQ(core(a_)->undo_depth(), 0u);
}

TEST_F(PaneDragControllerTest, ReloadAndDocumentServiceDestructionCannotReviveGesture) {
    ASSERT_TRUE(begin(target()));
    core(a_)->load(beatbench::Chart{});
    EXPECT_FALSE(ok(controller_->finish(leftSurface_.get(), target())));
    EXPECT_EQ(core(a_)->undo_depth(), 0u);
    documents_.reset();
    EXPECT_FALSE(controller_->active());
    EXPECT_FALSE(ok(controller_->finish(leftSurface_.get(), target())));
}

TEST_F(PaneDragControllerTest, CommittingPrecedesSignalsAndBlocksSynchronousRefreshReentry) {
    const auto point = target();
    ASSERT_TRUE(begin(point));
    int reentries = 0;
    QObject::connect(left_.get(), &PaneEditContext::edited, controller_.get(), [&](const QString&) {
        ++reentries;
        EXPECT_FALSE(controller_->active());
        EXPECT_EQ(code(controller_->finish(leftSurface_.get(), point)), "already_committing");
        EXPECT_FALSE(begin(point));
        controller_->cancelActive("refresh");
    });
    ASSERT_TRUE(ok(controller_->finish(leftSurface_.get(), point)));
    EXPECT_EQ(reentries, 1);
    EXPECT_EQ(core(a_)->undo_depth(), 1u);
    EXPECT_EQ(right_->selection().size(), 1);
}

TEST_F(PaneDragControllerTest, TargetRebindDuringFinishSignalPreventsCommit) {
    const auto point = target();
    ASSERT_TRUE(begin(point));
    QObject::connect(controller_.get(), &PaneDragController::activeChanged, controller_.get(), [&] {
        if (!controller_->active()) right_->setSessionId(b_);
    });
    EXPECT_EQ(code(controller_->finish(leftSurface_.get(), point)), "stale_context");
    EXPECT_EQ(core(a_)->undo_depth(), 0u);
    EXPECT_EQ(core(b_)->undo_depth(), 0u);
}

TEST_F(PaneDragControllerTest, SnapNoopDoesNotCreateUndoOrTransferSelection) {
    leftSurface_->setProperty("snapDen", 4);
    const auto point = press_ + QPointF(0, 6);
    ASSERT_TRUE(begin(point));
    ASSERT_TRUE(ok(controller_->finish(leftSurface_.get(), point)));
    EXPECT_EQ(core(a_)->undo_depth(), 0u);
    EXPECT_EQ(left_->selection(), QVariantList{ref_});
    EXPECT_TRUE(right_->selection().isEmpty());
}

TEST_F(PaneDragControllerTest, OrdinaryMoveDoesNotCarryUnselectedLnPartnerAndPreservesIdentity) {
    auto chart = core(a_)->chart();
    chart.notes.front().value.ln_channel = true;
    auto partner = chart.notes.front();
    partner.measure = 2;
    partner.value.sample.id = 3;
    chart.notes.insert(chart.notes.begin() + 1, partner);
    core(a_)->load(std::move(chart));
    documents_->refresh();
    ref_ = leftView_->objectAt(press_.x(), press_.y());
    ref_.insert("lnPartner", QVariantMap{{"sample", 3}});
    left_->setSelection({ref_});
    ASSERT_TRUE(begin(target()));
    ASSERT_TRUE(ok(controller_->finish(leftSurface_.get(), target())));
    ASSERT_EQ(right_->selection().size(), 1);
    const auto selected = right_->selection().front().toMap();
    EXPECT_TRUE(selected.value("ln_channel").toBool());
    EXPECT_EQ(selected.value("sample").toInt(), 1);
    EXPECT_EQ(selected.value("note_kind"), "normal");
    EXPECT_FALSE(selected.contains("lnPartner"));
    for (const auto& note : core(a_)->chart().notes)
        if (note.value.sample.id == 3) {
            EXPECT_EQ(note.measure, 2u);
            EXPECT_EQ(note.value.lane.index, 1);
            EXPECT_TRUE(note.value.ln_channel);
        }
    EXPECT_EQ(core(a_)->undo_depth(), 1u);
}

TEST_F(PaneDragControllerTest, LocalTimingConversionUsesCapturedSharedPolicyAndOneUndo) {
    const auto x = columnX(*leftView_, "key", 0, "bpm");
    ASSERT_GE(x, 0);
    const auto point = QPointF(x, press_.y());
    ASSERT_TRUE(begin(point));
    EXPECT_TRUE(
        result(controller_->update(leftSurface_.get(), point)).value("conversion").toBool());
    ASSERT_TRUE(ok(controller_->finish(leftSurface_.get(), point)));
    EXPECT_EQ(core(a_)->chart().notes.size(), 1u);
    EXPECT_EQ(core(a_)->chart().bpm_events.size(), 1u);
    EXPECT_EQ(core(a_)->undo_depth(), 1u);
    EXPECT_TRUE(left_->selection().isEmpty());
    ASSERT_TRUE(core(a_)->undo());
    EXPECT_EQ(core(a_)->chart().notes.size(), 2u);
}

TEST_F(PaneDragControllerTest, LocalBgaConversionPreservesIdAndIsOneUndo) {
    leftView_->setShowExtras(true);
    press_ = notePoint(*leftView_);
    ref_ = leftView_->objectAt(press_.x(), press_.y());
    left_->setSelection({ref_});
    const auto x = columnX(*leftView_, "key", 0, {}, 2);
    ASSERT_GE(x, 0);
    const auto point = QPointF(x, press_.y());
    ASSERT_TRUE(begin(point));
    ASSERT_TRUE(ok(controller_->finish(leftSurface_.get(), point)));
    ASSERT_EQ(core(a_)->chart().bga_events.size(), 1u);
    EXPECT_EQ(core(a_)->chart().bga_events.front().value.image.id, 1u);
    EXPECT_EQ(core(a_)->chart().bga_events.front().value.layer, 2);
    EXPECT_EQ(core(a_)->undo_depth(), 1u);
    EXPECT_TRUE(left_->selection().isEmpty());
    ASSERT_TRUE(core(a_)->undo());
    EXPECT_TRUE(core(a_)->chart().bga_events.empty());
    EXPECT_EQ(core(a_)->chart().notes.size(), 2u);
}

TEST_F(PaneDragControllerTest, MineMoveRetainsKindSampleAndExplicitEndpointIdentity) {
    auto chart = core(a_)->chart();
    chart.notes.front().value.kind = beatbench::NoteKind::Landmine;
    chart.notes.front().value.sample.id = 12;
    core(a_)->load(std::move(chart));
    documents_->refresh();
    ref_ = leftView_->objectAt(press_.x(), press_.y());
    left_->setSelection({ref_});
    ASSERT_TRUE(begin(target()));
    ASSERT_TRUE(ok(controller_->finish(leftSurface_.get(), target())));
    ASSERT_EQ(right_->selection().size(), 1);
    const auto moved = right_->selection().front().toMap();
    EXPECT_EQ(moved.value("sample").toInt(), 12);
    EXPECT_EQ(moved.value("note_kind").toString(), "mine");
    EXPECT_FALSE(moved.value("ln_channel").toBool());
    EXPECT_EQ(core(a_)->undo_depth(), 1u);
}

TEST_F(PaneDragControllerTest, CrossPaneBgmCollapsedExpandedMatrixPreservesHiddenSubLineGaps) {
    const auto baseChart = core(a_)->chart();
    for (const bool sourceExpanded : {false, true}) {
        for (const bool targetExpanded : {false, true}) {
            SCOPED_TRACE(::testing::Message() << "sourceExpanded=" << sourceExpanded
                                              << " targetExpanded=" << targetExpanded);
            auto chart = baseChart;
            for (const auto line : {2u, 4u}) {
                beatbench::Event<beatbench::Note> note{1u, {1, 7}, {}};
                note.value.lane = {0, beatbench::LaneKind::Bgm, 0};
                note.value.sample.id = 19 + line;
                note.value.sub_line = line;
                chart.notes.push_back(note);
            }
            std::stable_sort(
                chart.notes.begin(), chart.notes.end(), [](const auto& a, const auto& b) {
                    return a.measure != b.measure ? a.measure < b.measure : a.pos < b.pos;
                });
            core(a_)->load(std::move(chart));
            documents_->refresh();
            leftView_->setBgmExpanded(sourceExpanded);
            rightView_->setBgmExpanded(targetExpanded);
            const auto bgmX = [](const ChartViewItem& view, int line) {
                qreal first = -1, last = -1;
                for (int x = 0; x < view.width(); ++x) {
                    const auto c = view.laneAtX(x);
                    if (!c.value("valid").toBool() || c.value("laneKind") != "bgm" ||
                        c.value("bgaLayer", -1).toInt() >= 0 ||
                        c.value("sub_line", -1).toInt() != line)
                        continue;
                    if (first < 0) first = x;
                    last = x;
                }
                return first < 0 ? -1 : (first + last) / 2;
            };
            const auto sourceX = bgmX(*leftView_, sourceExpanded ? 2 : -1);
            const auto targetX = bgmX(*rightView_, targetExpanded ? 1 : -1);
            ASSERT_GE(sourceX, 0);
            ASSERT_GE(targetX, 0);
            press_ = {-1, -1};
            for (int y = 18; y < leftView_->height(); ++y) {
                const auto hit = leftView_->objectAt(sourceX, y);
                if (hit.value("kind") == "note" && hit.value("sample").toInt() == 21) {
                    press_ = {sourceX, qreal(y + 2)};
                    break;
                }
            }
            ASSERT_GE(press_.y(), 18);
            ref_ = leftView_->objectAt(press_.x(), press_.y());
            QVariantList refs;
            for (const auto& value :
                 leftView_->notesInRect(0, 18, leftView_->width(), leftView_->height())) {
                if (value.toMap().value("lane").toMap().value("kind") == "bgm") refs.append(value);
            }
            ASSERT_EQ(refs.size(), 2);
            left_->setSelection(refs);
            rightSurface_->setProperty("snapDen", 4);
            const auto local = at(*rightView_, 1.49, targetX);
            const auto point = leftView_->mapFromScene(rightView_->mapToScene(local));
            ASSERT_TRUE(begin(point));
            const auto preview = rightView_->plannedMovePreview();
            ASSERT_EQ(preview.size(), 2);
            const int base = targetExpanded ? 1 : 2;
            for (qsizetype i = 0; i < preview.size(); ++i) {
                const auto moved = preview[i].toMap();
                EXPECT_EQ(moved.value("lane").toMap().value("kind").toString(), "bgm");
                EXPECT_EQ(moved.value("sub_line").toInt(), base + 2 * i);
                EXPECT_EQ(moved.value("sample").toInt(), 21 + 2 * i);
                EXPECT_EQ(moved.value("note_kind").toString(), "normal");
                EXPECT_FALSE(moved.value("ln_channel").toBool());
            }
            const auto response = controller_->finish(leftSurface_.get(), point);
            ASSERT_TRUE(ok(response))
                << QJsonDocument::fromVariant(response).toJson().toStdString();
            EXPECT_TRUE(left_->selection().isEmpty());
            EXPECT_EQ(right_->selection(), preview);
            EXPECT_EQ(core(a_)->undo_depth(), 1u);
            ASSERT_TRUE(core(a_)->undo());
            for (const auto& note : core(a_)->chart().notes) {
                if (note.value.lane.kind != beatbench::LaneKind::Bgm) continue;
                EXPECT_EQ(note.value.sub_line, note.value.sample.id - 19);
                EXPECT_EQ(note.pos, beatbench::Rational(1, 7));
            }
            EXPECT_EQ(core(b_)->undo_depth(), 0u);
        }
    }
}

TEST_F(PaneDragControllerTest, CrossPanePureHorizontalMovePreservesOffGridRationalExactly) {
    auto chart = core(a_)->chart();
    chart.notes.front().pos = {1, 7};
    core(a_)->load(std::move(chart));
    documents_->refresh();
    press_ = notePoint(*leftView_);
    ref_ = leftView_->objectAt(press_.x(), press_.y());
    left_->setSelection({ref_});
    leftSurface_->setProperty("moveMode", true);
    rightSurface_->setProperty("snapDen", 4);
    const auto point = target(1.6);
    ASSERT_TRUE(begin(point));
    const auto preview = rightView_->plannedMovePreview();
    ASSERT_EQ(preview.size(), 1);
    EXPECT_EQ(preview.front().toMap().value("pos"), ref_.value("pos"));
    EXPECT_EQ(preview.front().toMap().value("measure"), ref_.value("measure"));
    EXPECT_EQ(preview.front().toMap().value("lane").toMap().value("index").toInt(), 2);
    const auto response = controller_->finish(leftSurface_.get(), point);
    ASSERT_TRUE(ok(response)) << QJsonDocument::fromVariant(response).toJson().toStdString();
    EXPECT_EQ(right_->selection(), preview);
    EXPECT_EQ(core(a_)->chart().notes.front().pos, beatbench::Rational(1, 7));
    EXPECT_EQ(core(a_)->chart().notes.front().value.lane.index, 2);
    EXPECT_EQ(core(a_)->undo_depth(), 1u);
}

TEST_F(PaneDragControllerTest, PreviewObserverCanDestroyTargetWithoutCommitOrDanglingPaint) {
    const auto point = target();
    QObject::connect(rightView_.get(), &ChartViewItem::plannedMovePreviewChanged, controller_.get(),
                     [&] {
                         if (rightView_ && !rightView_->plannedMovePreview().isEmpty())
                             rightView_.reset();
                     });
    ASSERT_TRUE(begin(point));
    EXPECT_FALSE(controller_->active());
    EXPECT_EQ(rightView_.get(), nullptr);
    EXPECT_FALSE(ok(controller_->finish(leftSurface_.get(), point)));
    EXPECT_EQ(core(a_)->undo_depth(), 0u);
    EXPECT_EQ(core(b_)->undo_depth(), 0u);
}

TEST_F(PaneDragControllerTest, ClosingCapturedSharedOwnerCannotCommitToActiveDocument) {
    const auto point = target();
    ASSERT_TRUE(begin(point));
    ASSERT_TRUE(session_registry().close(a_.toStdString()));
    const auto response = controller_->finish(leftSurface_.get(), point);
    EXPECT_FALSE(ok(response));
    EXPECT_TRUE(result(response).value("handled").toBool());
    EXPECT_EQ(core(b_)->undo_depth(), 0u);
    EXPECT_EQ(session_registry().active_id(), b_.toStdString());
    EXPECT_FALSE(controller_->active());
}

TEST_F(PaneDragControllerTest, CrossPaneMetaConversionIsRejectedWithoutUndo) {
    const auto local = QPointF(columnX(*rightView_, "key", 0, "bpm"), press_.y());
    const auto point = leftView_->mapFromScene(rightView_->mapToScene(local));
    ASSERT_TRUE(begin(point));
    EXPECT_EQ(code(controller_->finish(leftSurface_.get(), point)), "invalid_target");
    EXPECT_EQ(core(a_)->undo_depth(), 0u);
}
}  // namespace

int main(int argc, char** argv) {
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QGuiApplication app(argc, argv);
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
