// SPDX-License-Identifier: GPL-3.0-only
#include <gtest/gtest.h>

#include <QCoreApplication>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QSettings>
#include <QTemporaryDir>
#include <QTimer>
#include <algorithm>
#include <functional>

#include "beatbench/core/bms/BmsCodec.hpp"
#include "beatbench/core/edit/SessionRegistry.hpp"
#include "bridge/DocumentService.hpp"
#include "bridge/ProjectController.hpp"
#include "bridge/ResourceRelocationController.hpp"

namespace {
using namespace beatbench;
using namespace beatbench::app;
using namespace beatbench::edit;
bool ok(const QVariantMap& value) { return value.value("ok").toBool(); }
QString planId(const QVariantMap& value) { return value.value("planId").toString(); }
bool waitFor(const std::function<bool()>& done) {
    if (done()) return true;
    QEventLoop loop;
    QTimer tick, timeout;
    tick.setInterval(1);
    timeout.setSingleShot(true);
    QObject::connect(&tick, &QTimer::timeout, &loop, [&] {
        if (done()) loop.quit();
    });
    QObject::connect(&timeout, &QTimer::timeout, &loop, &QEventLoop::quit);
    tick.start();
    timeout.start(15000);
    loop.exec();
    return done();
}
class RelocationTest : public ::testing::Test {
   protected:
    DocumentService documents;
    ProjectController project;
    ResourceRelocationController controller{&documents, &project};
    std::unique_ptr<QTemporaryDir> temporary;
    QString root, a, b, c;
    std::vector<std::string> previous;
    std::string active;
    QSettings::Format oldFormat = QSettings::NativeFormat;
    QString oldOrganization, oldApplication;
    void SetUp() override {
        previous = session_registry().ids();
        active = session_registry().active_id();
        const auto base = qEnvironmentVariable("BB_TEST_TMPDIR", QDir::tempPath());
        ASSERT_TRUE(QDir().mkpath(base));
        temporary = std::make_unique<QTemporaryDir>(QDir(base).filePath("relocation-XXXXXX"));
        ASSERT_TRUE(temporary->isValid());
        root = temporary->path();
        oldFormat = QSettings::defaultFormat();
        oldOrganization = QCoreApplication::organizationName();
        oldApplication = QCoreApplication::applicationName();
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, root);
        QCoreApplication::setOrganizationName("BeAtBenchTest");
        QCoreApplication::setApplicationName("Relocation");
        a = QDir(root).filePath("A.bms");
        b = QDir(root).filePath("sub/B.bms");
        c = QDir(root).filePath("C.bms");
        ASSERT_TRUE(
            write(a,
                  "#TITLE Original\n#BPM 120\n#WAV0A old/shared.wav\n#BMP00 "
                  "old/stage.png\n#STAGEFILE old/stage.png\n#BANNER old/stage.png\n#BACKBMP "
                  "old/stage.png\n#PREVIEW old/preview.ogg\n#00111:0A\n"));
        ASSERT_TRUE(write(b,
                          "#BASE 62\n#BPM 120\n#WAV0a ../old/shared.wav\n#BMP0z "
                          "../old/stage.png\n#STAGEFILE ../old/stage.png\n#00111:0a\n"));
        ASSERT_TRUE(write(c, "#BPM 120\n#WAV01 old2/shared.wav\n"));
        ASSERT_TRUE(write(QDir(root).filePath("media/shared.wav"), "audio"));
        ASSERT_TRUE(write(QDir(root).filePath("media/stage.png"), "image"));
        ASSERT_TRUE(write(QDir(root).filePath("media/preview.ogg"), "audio"));
        ASSERT_TRUE(scan());
    }
    void TearDown() override {
        controller.cancel();
        project.clear();
        session_registry().activate(active);
        for (const auto& id : session_registry().ids())
            if (std::find(previous.begin(), previous.end(), id) == previous.end())
                session_registry().close(id);
        QCoreApplication::setOrganizationName(oldOrganization);
        QCoreApplication::setApplicationName(oldApplication);
        QSettings::setDefaultFormat(oldFormat);
    }
    static bool write(const QString& path, const QByteArray& data) {
        QDir().mkpath(QFileInfo(path).absolutePath());
        QFile f(path);
        return f.open(QIODevice::WriteOnly) && f.write(data) == data.size();
    }
    static QByteArray bytes(const QString& path) {
        QFile f(path);
        return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray{};
    }
    bool scan() {
        project.scan(root);
        return waitFor([&] { return project.state() == "ready" || project.state() == "error"; }) &&
               project.state() == "ready";
    }
    QVariantMap row(const QString& relative) {
        for (const auto& v : controller.rows()) {
            auto r = v.toMap();
            if (r.value("oldPath").toString() == QDir(root).filePath(relative)) return r;
        }
        return {};
    }
    bool choose(const QString& relative, const QString& replacement) {
        return ok(controller.chooseCandidate(row(relative).value("key").toString(),
                                             QDir(root).filePath(replacement)));
    }
    void chooseAll() {
        ASSERT_TRUE(choose("old/shared.wav", "media/shared.wav"));
        ASSERT_TRUE(choose("old2/shared.wav", "media/shared.wav"));
        ASSERT_TRUE(choose("old/stage.png", "media/stage.png"));
        ASSERT_TRUE(choose("old/preview.ogg", "media/preview.ogg"));
    }
    EditorSession* session(const QString& path) {
        return session_registry().by_id(
            DocumentService::findSessionId(path).toUtf8().toStdString());
    }
    static std::string sample(EditorSession* s, SampleKind kind, unsigned id) {
        return s->chart().samples.at({kind, id}).file;
    }
};
TEST_F(RelocationTest, StartsOnlyReadyAndGroupsPathsNotBasenamesWithAllReferencesByDefault) {
    const auto activeId = session_registry().active_id();
    ASSERT_TRUE(ok(controller.start()));
    ASSERT_EQ(controller.rows().size(), 4);
    const auto shared = row("old/shared.wav");
    ASSERT_EQ(shared.value("uses").toList().size(), 2);
    EXPECT_EQ(shared.value("selectedCount").toInt(), 2);
    EXPECT_NE(shared.value("key"), row("old2/shared.wav").value("key"));
    EXPECT_EQ(row("old/stage.png").value("uses").toList().size(), 6);
    EXPECT_EQ(session_registry().active_id(), activeId);
    EXPECT_TRUE(DocumentService::findSessionId(a).isEmpty());
    project.clear();
    EXPECT_FALSE(ok(controller.start()));
}
TEST_F(RelocationTest, ExplicitTargetsSelectUsesAndSingleResourceLimitsRows) {
    ASSERT_TRUE(ok(controller.start({}, {b})));
    EXPECT_EQ(row("old/shared.wav").value("selectedCount").toInt(), 1);
    EXPECT_EQ(row("old/stage.png").value("selectedCount").toInt(), 2);
    EXPECT_EQ(row("old/preview.ogg").value("selectedCount").toInt(), 0);
    ASSERT_TRUE(ok(controller.start("old/shared.wav", {a})));
    ASSERT_EQ(controller.rows().size(), 1);
    EXPECT_EQ(controller.rows().front().toMap().value("selectedCount").toInt(), 1);
    EXPECT_FALSE(ok(controller.start({}, {"../escape.bms"})));
    EXPECT_FALSE(ok(controller.start({}, {a, a})));
    EXPECT_FALSE(ok(controller.start({}, {"not-scanned.bms"})));
}
TEST_F(RelocationTest, SelectionHeadersMixedBasesUndoRedoKeepOtherDirtyChangesAndNeverWrite) {
    ASSERT_TRUE(ok(documents.open(a)));
    auto* sa = session(a);
    ASSERT_NE(sa, nullptr);
    sa->set_backup_enabled(false);
    ASSERT_TRUE(sa->exec(std::make_unique<MetaEditCommand>("TITLE", "Unrelated dirty")));
    const auto depth = sa->undo_depth();
    const auto diskA = bytes(a), diskB = bytes(b);
    const auto activeId = session_registry().active_id();
    ASSERT_TRUE(ok(controller.start()));
    chooseAll();
    // Deliberately leave C untouched although it has the same basename.
    auto other = row("old2/shared.wav");
    controller.setUseSelected(other.value("key").toString(),
                              other.value("uses").toList().front().toMap().value("key").toString(),
                              false);
    int writes = 0;
    sa->set_backup_enabled(true);
    sa->set_autosave_enabled(true);
    sa->set_persist_hook([&](const Chart&, const std::string&, std::string*) {
        ++writes;
        return true;
    });
    const auto p = controller.prepare();
    ASSERT_TRUE(ok(p)) << p.value("error").toString().toStdString();
    EXPECT_EQ(p.value("targetCount").toInt(), 2);
    EXPECT_EQ(p.value("changedCount").toInt(), 9);
    int appliedSignals = 0;
    QStringList changed;
    QObject::connect(&controller, &ResourceRelocationController::relocated, &controller,
                     [&](const QStringList& paths) {
                         ++appliedSignals;
                         changed = paths;
                     });
    ASSERT_TRUE(ok(controller.commit(planId(p))));
    EXPECT_EQ(writes, 0);
    EXPECT_EQ(appliedSignals, 1);
    EXPECT_EQ(changed.size(), 2);
    EXPECT_EQ(bytes(a), diskA);
    EXPECT_EQ(bytes(b), diskB);
    EXPECT_FALSE(QFileInfo::exists(a + ".bak"));
    EXPECT_FALSE(QFileInfo::exists(b + ".bak"));
    EXPECT_EQ(session_registry().active_id(), activeId);
    EXPECT_EQ(sa->undo_depth(), depth + 1);
    EXPECT_EQ(sample(sa, SampleKind::Wav, 10), "media/shared.wav");
    EXPECT_EQ(sample(session(b), SampleKind::Wav, 36), "../media/shared.wav");
    EXPECT_EQ(sample(sa, SampleKind::Bmp, 0), "media/stage.png");
    EXPECT_EQ(sample(session(b), SampleKind::Bmp, 61), "../media/stage.png");
    for (const auto* field : {"STAGEFILE", "BANNER", "BACKBMP"})
        EXPECT_EQ(sa->chart().meta.at(field), "media/stage.png");
    EXPECT_EQ(sa->chart().meta.at("PREVIEW"), "media/preview.ogg");
    EXPECT_EQ(sa->chart().meta.at("TITLE"), "Unrelated dirty");
    EXPECT_TRUE(sa->backup_enabled());
    EXPECT_TRUE(sa->autosave_enabled());
    EXPECT_FALSE(ok(controller.commit(planId(p))));
    sa->set_backup_enabled(false);
    sa->set_autosave_enabled(false);
    session(b)->set_backup_enabled(false);
    session(b)->set_autosave_enabled(false);
    ASSERT_TRUE(sa->undo());
    ASSERT_TRUE(session(b)->undo());
    EXPECT_EQ(sample(sa, SampleKind::Wav, 10), "old/shared.wav");
    EXPECT_EQ(sa->chart().meta.at("TITLE"), "Unrelated dirty");
    ASSERT_TRUE(sa->redo());
    ASSERT_TRUE(session(b)->redo());
    EXPECT_EQ(sample(session(b), SampleKind::Wav, 36), "../media/shared.wav");
    EXPECT_EQ(bytes(a), diskA);
    EXPECT_EQ(writes, 0);
}
TEST_F(RelocationTest, BulkResolvedSelectionIsExplicitAndUnresolvedSelectedUsesBlock) {
    ASSERT_TRUE(ok(controller.start()));
    EXPECT_FALSE(ok(controller.prepare()));
    ASSERT_TRUE(choose("old/shared.wav", "media/shared.wav"));
    controller.setAllSelected(true, true);
    EXPECT_EQ(row("old/shared.wav").value("selectedCount").toInt(), 2);
    EXPECT_EQ(row("old2/shared.wav").value("selectedCount").toInt(), 0);
    const auto p = controller.prepare();
    ASSERT_TRUE(ok(p));
    EXPECT_EQ(p.value("changedCount").toInt(), 2);
    controller.setAllSelected(false);
    EXPECT_FALSE(ok(controller.commit(planId(p))));
    const auto empty = controller.prepare();
    ASSERT_TRUE(ok(empty));
    EXPECT_EQ(empty.value("changedCount").toInt(), 0);
    EXPECT_TRUE(ok(controller.commit(planId(empty))));
}
TEST_F(RelocationTest, RestoredOriginalNeedsNoReplacementAndAddsNoUndo) {
    ASSERT_TRUE(ok(controller.start("old/shared.wav")));
    ASSERT_TRUE(write(QDir(root).filePath("old/shared.wav"), "restored"));
    const auto p = controller.prepare();
    ASSERT_TRUE(ok(p));
    EXPECT_EQ(p.value("changedCount").toInt(), 0);
    const auto result = controller.commit(planId(p));
    ASSERT_TRUE(ok(result));
    EXPECT_TRUE(result.value("changedTargets").toStringList().isEmpty());
    EXPECT_EQ(session(a)->undo_depth(), 0u);
    EXPECT_EQ(session(b)->undo_depth(), 0u);
}
TEST_F(RelocationTest, RestorationAfterPrepareAlsoAvoidsRewrite) {
    ASSERT_TRUE(ok(controller.start("old/shared.wav")));
    ASSERT_TRUE(choose("old/shared.wav", "media/shared.wav"));
    const auto p = controller.prepare();
    ASSERT_TRUE(ok(p));
    ASSERT_TRUE(write(QDir(root).filePath("old/shared.wav"), "restored"));
    const auto result = controller.commit(planId(p));
    ASSERT_TRUE(ok(result));
    EXPECT_EQ(result.value("changedCount").toInt(), 0);
    EXPECT_EQ(sample(session(a), SampleKind::Wav, 10), "old/shared.wav");
}
TEST_F(RelocationTest, RejectsSelectedDefinitionChangedInMemoryBeforeOrAfterStart) {
    ASSERT_TRUE(ok(documents.openReference(a)));
    auto* sa = session(a);
    sa->set_backup_enabled(false);
    ASSERT_TRUE(sa->exec(std::make_unique<SetSampleFileCommand>(SampleKind::Wav, 10, "dirty.wav")));
    ASSERT_TRUE(ok(controller.start("old/shared.wav")));
    ASSERT_TRUE(choose("old/shared.wav", "media/shared.wav"));
    EXPECT_FALSE(ok(controller.prepare()));
    EXPECT_EQ(sample(sa, SampleKind::Wav, 10), "dirty.wav");
    ASSERT_TRUE(sa->undo());
    ASSERT_TRUE(ok(controller.start("old/shared.wav")));
    ASSERT_TRUE(choose("old/shared.wav", "media/shared.wav"));
    ASSERT_TRUE(sa->exec(std::make_unique<SetSampleFileCommand>(SampleKind::Wav, 10, "later.wav")));
    EXPECT_FALSE(ok(controller.prepare()));
}
TEST_F(RelocationTest, ValidatesEveryTargetRevisionBeforeAnyMutationAndConsumesPlan) {
    ASSERT_TRUE(ok(controller.start("old/shared.wav")));
    ASSERT_TRUE(choose("old/shared.wav", "media/shared.wav"));
    const auto p = controller.prepare();
    ASSERT_TRUE(ok(p));
    auto* sb = session(b);
    sb->set_backup_enabled(false);
    ASSERT_TRUE(sb->exec(std::make_unique<MetaEditCommand>("TITLE", "late")));
    EXPECT_FALSE(ok(controller.commit(planId(p))));
    EXPECT_FALSE(ok(controller.commit(planId(p))));
    EXPECT_EQ(sample(session(a), SampleKind::Wav, 10), "old/shared.wav");
    EXPECT_EQ(session(a)->undo_depth(), 0u);
}
TEST_F(RelocationTest, ReplacementDeletionAndTargetPathChangeRejectWholeBatch) {
    ASSERT_TRUE(ok(controller.start("old/shared.wav")));
    ASSERT_TRUE(choose("old/shared.wav", "media/shared.wav"));
    auto p = controller.prepare();
    ASSERT_TRUE(ok(p));
    ASSERT_TRUE(QFile::remove(QDir(root).filePath("media/shared.wav")));
    EXPECT_FALSE(ok(controller.commit(planId(p))));
    EXPECT_EQ(session(a)->undo_depth(), 0u);
    ASSERT_TRUE(write(QDir(root).filePath("media/shared.wav"), "new"));
    p = controller.prepare();
    ASSERT_TRUE(ok(p));
    session(b)->set_path("renamed.bms");
    EXPECT_FALSE(ok(controller.commit(planId(p))));
    EXPECT_EQ(session(a)->undo_depth(), 0u);
}
TEST_F(RelocationTest, InputChangesCancelAndRescanInvalidatePlans) {
    ASSERT_TRUE(ok(controller.start("old/shared.wav")));
    ASSERT_TRUE(choose("old/shared.wav", "media/shared.wav"));
    auto p = controller.prepare();
    ASSERT_TRUE(ok(p));
    EXPECT_FALSE(
        ok(controller.chooseCandidate("unknown", QDir(root).filePath("media/shared.wav"))));
    EXPECT_FALSE(ok(controller.commit(planId(p))));
    p = controller.prepare();
    ASSERT_TRUE(ok(p));
    controller.cancel();
    EXPECT_FALSE(ok(controller.commit(planId(p))));
    EXPECT_TRUE(controller.rows().isEmpty());
    ASSERT_TRUE(ok(controller.start("old/shared.wav")));
    ASSERT_TRUE(choose("old/shared.wav", "media/shared.wav"));
    p = controller.prepare();
    ASSERT_TRUE(ok(p));
    ASSERT_TRUE(scan());
    EXPECT_FALSE(ok(controller.commit(planId(p))));
    EXPECT_FALSE(ok(controller.prepare()));
    ASSERT_TRUE(ok(controller.start("old/shared.wav")));
}
TEST_F(RelocationTest, WrongPlanIdDoesNotConsumeUnrelatedValidPlan) {
    ASSERT_TRUE(ok(controller.start("old/shared.wav")));
    ASSERT_TRUE(choose("old/shared.wav", "media/shared.wav"));
    const auto p = controller.prepare();
    ASSERT_TRUE(ok(p));
    EXPECT_FALSE(ok(controller.commit("wrong-id")));
    EXPECT_TRUE(ok(controller.commit(planId(p))));
}
TEST_F(RelocationTest, RejectsPathEscapesFamiliesControlCharactersAndUnknownResources) {
    ASSERT_TRUE(ok(controller.start("old/shared.wav")));
    const auto key = row("old/shared.wav").value("key").toString();
    EXPECT_FALSE(
        ok(controller.chooseCandidate(key, QDir(root).absoluteFilePath("../external.wav"))));
    EXPECT_FALSE(ok(controller.chooseCandidate(key, "media/shared.wav")));
    EXPECT_FALSE(ok(controller.chooseCandidate(key, QDir(root).filePath("media/stage.png"))));
    EXPECT_FALSE(ok(
        controller.chooseCandidate(key, QDir(root).filePath("media/shared.wav\n#TITLE inject"))));
    EXPECT_FALSE(
        ok(controller.chooseCandidate("unknown", QDir(root).filePath("media/shared.wav"))));
    EXPECT_FALSE(ok(controller.mapDirectory("old", "../external")));
    EXPECT_FALSE(ok(controller.mapDirectory("", "media")));
}
TEST_F(RelocationTest, DirectoryMappingPreservesSuffixAndIsAtomicWhenFileMissing) {
    ASSERT_TRUE(write(QDir(root).filePath("newold/sub/extra.wav"), "new"));
    ASSERT_TRUE(write(a, "#BPM 120\n#WAV01 old/sub/extra.wav\n#WAV02 old/missing.wav\n"));
    ASSERT_TRUE(scan());
    ASSERT_TRUE(ok(controller.start({}, {a})));
    EXPECT_FALSE(ok(controller.mapDirectory("old", "newold")));
    EXPECT_TRUE(row("old/sub/extra.wav").value("replacement").toString().isEmpty());
    ASSERT_TRUE(write(QDir(root).filePath("newold/missing.wav"), "new"));
    ASSERT_TRUE(ok(controller.mapDirectory("old", "newold")));
    EXPECT_EQ(row("old/sub/extra.wav").value("replacement").toString(),
              QDir(root).filePath("newold/sub/extra.wav"));
    const auto p = controller.prepare();
    ASSERT_TRUE(ok(p));
    ASSERT_TRUE(ok(controller.commit(planId(p))));
    EXPECT_EQ(sample(session(a), SampleKind::Wav, 1), "newold/sub/extra.wav");
}
TEST_F(RelocationTest, SearchExactUniquePrefillAmbiguityAndRecursiveOption) {
    ASSERT_TRUE(ok(controller.start("old/shared.wav")));
    controller.search(root, false);
    ASSERT_TRUE(waitFor([&] { return !controller.busy(); }));
    EXPECT_TRUE(row("old/shared.wav").value("replacement").toString().isEmpty());
    controller.search(QDir(root).filePath("media"), false);
    ASSERT_TRUE(waitFor([&] { return !controller.busy(); }));
    EXPECT_EQ(row("old/shared.wav").value("candidates").toStringList().size(), 1);
    EXPECT_EQ(row("old/shared.wav").value("replacement").toString(),
              QDir(root).filePath("media/shared.wav"));
    ASSERT_TRUE(write(QDir(root).filePath("other/shared.wav"), "other"));
    controller.search(root, true);
    ASSERT_TRUE(waitFor([&] { return !controller.busy(); }));
    EXPECT_EQ(row("old/shared.wav").value("candidates").toStringList().size(), 2);
    EXPECT_EQ(row("old/shared.wav").value("state").toString(), "ambiguous");
    EXPECT_TRUE(row("old/shared.wav").value("replacement").toString().isEmpty());
    EXPECT_FALSE(ok(controller.prepare()));
}
TEST_F(RelocationTest, SearchRequiresExplicitFolderAndInputChangeDiscardsQueuedResults) {
    ASSERT_TRUE(ok(controller.start("old/shared.wav")));
    controller.search({}, true);
    EXPECT_FALSE(controller.busy());
    controller.search(QDir(root).absoluteFilePath(".."), true);
    EXPECT_FALSE(controller.busy());
    controller.search(root, true);
    controller.cancel();
    QCoreApplication::processEvents();
    EXPECT_FALSE(controller.busy());
    EXPECT_TRUE(controller.rows().isEmpty());
    ASSERT_TRUE(ok(controller.start("old/shared.wav")));
    controller.search(root, true);
    ASSERT_TRUE(choose("old/shared.wav", "media/shared.wav"));
    QCoreApplication::processEvents();
    EXPECT_EQ(row("old/shared.wav").value("replacement").toString(),
              QDir(root).filePath("media/shared.wav"));
}
TEST_F(RelocationTest, RejectsConditionalTargetsAndChangedDiskReferences) {
    ASSERT_TRUE(write(a, "#BPM 120\n#RANDOM 2\n#IF 1\n#WAV01 absent.wav\n#ENDIF\n#ENDRANDOM\n"));
    ASSERT_TRUE(scan());
    EXPECT_FALSE(ok(controller.start({}, {a})));
    ASSERT_TRUE(write(a, "#BPM 120\n#WAV01 old.wav\n"));
    ASSERT_TRUE(scan());
    ASSERT_TRUE(write(a, "#BPM 120\n#WAV01 different.wav\n"));
    ASSERT_TRUE(ok(controller.start({}, {a})));
    controller.search(QDir(root).filePath("media"), false);
    ASSERT_TRUE(waitFor([&] { return !controller.busy(); }));
    EXPECT_FALSE(ok(controller.prepare()));
}
TEST_F(RelocationTest, RejectsReloadedClosedAndChangedBaseTargets) {
    ASSERT_TRUE(ok(controller.start("old/shared.wav")));
    ASSERT_TRUE(choose("old/shared.wav", "media/shared.wav"));
    auto p = controller.prepare();
    ASSERT_TRUE(ok(p));
    auto* sb = session(b);
    const auto chart = sb->chart();
    const auto path = sb->path();
    sb->load(chart, path);
    EXPECT_FALSE(ok(controller.commit(planId(p))));
    p = controller.prepare();
    ASSERT_TRUE(ok(p));
    const auto id = DocumentService::findSessionId(b).toUtf8().toStdString();
    ASSERT_TRUE(session_registry().close(id));
    EXPECT_FALSE(ok(controller.commit(planId(p))));
    ASSERT_TRUE(ok(documents.openReference(b)));
    session(b)->chart_mut().id_base = IdBase::Base36;
    EXPECT_FALSE(ok(controller.prepare()));
    EXPECT_EQ(session(a)->undo_depth(), 0u);
}
TEST_F(RelocationTest, BulkActionsCannotWidenExplicitTargetScope) {
    ASSERT_TRUE(ok(controller.start("old/shared.wav", {a})));
    ASSERT_EQ(row("old/shared.wav").value("uses").toList().size(), 1);
    controller.setAllSelected(false);
    controller.setAllSelected(true);
    ASSERT_TRUE(choose("old/shared.wav", "media/shared.wav"));
    const auto p = controller.prepare();
    ASSERT_TRUE(ok(p));
    EXPECT_EQ(p.value("targetCount").toInt(), 1);
    ASSERT_TRUE(ok(controller.commit(planId(p))));
    EXPECT_TRUE(DocumentService::findSessionId(b).isEmpty());
    EXPECT_EQ(bytes(b).contains("../old/shared.wav"), true);
}
TEST_F(RelocationTest, DeselectedIncompatibleUsesDoNotConstrainRepair) {
    ASSERT_TRUE(write(a, "#BPM 120\n#WAV01 old/shared.wav\n#BMP01 old/shared.wav\n"));
    ASSERT_TRUE(scan());
    ASSERT_TRUE(ok(controller.start("old/shared.wav", {a})));
    auto r = row("old/shared.wav");
    EXPECT_FALSE(choose("old/shared.wav", "media/shared.wav"));
    for (const auto& v : r.value("uses").toList()) {
        const auto use = v.toMap();
        if (use.value("kind").toString() == "bmp")
            controller.setUseSelected(r.value("key").toString(), use.value("key").toString(),
                                      false);
    }
    ASSERT_TRUE(choose("old/shared.wav", "media/shared.wav"));
    controller.setAllSelected(true, true);
    EXPECT_EQ(row("old/shared.wav").value("selectedCount").toInt(), 1);
    const auto p = controller.prepare();
    ASSERT_TRUE(ok(p));
    ASSERT_TRUE(ok(controller.commit(planId(p))));
    EXPECT_EQ(sample(session(a), SampleKind::Wav, 1), "media/shared.wav");
    EXPECT_EQ(sample(session(a), SampleKind::Bmp, 1), "old/shared.wav");
}
TEST_F(RelocationTest, LeadingWhitespacePathsRoundTripForDefinitionsAndHeaders) {
    ASSERT_TRUE(write(QDir(root).filePath(" leading.wav"), "audio"));
    ASSERT_TRUE(write(QDir(root).filePath(" leading.png"), "image"));
    ASSERT_TRUE(ok(controller.start({}, {a})));
    ASSERT_TRUE(choose("old/shared.wav", " leading.wav"));
    ASSERT_TRUE(choose("old/stage.png", " leading.png"));
    controller.setAllSelected(true, true);
    const auto p = controller.prepare();
    ASSERT_TRUE(ok(p));
    ASSERT_TRUE(ok(controller.commit(planId(p))));
    EXPECT_EQ(sample(session(a), SampleKind::Wav, 10), "./ leading.wav");
    EXPECT_EQ(session(a)->chart().meta.at("STAGEFILE"), "./ leading.png");
    const auto roundtrip = bms::read_bms(bms::write_bms(session(a)->chart())).chart;
    EXPECT_EQ(roundtrip.samples.at({SampleKind::Wav, 10}).file, "./ leading.wav");
    EXPECT_EQ(roundtrip.meta.at("STAGEFILE"), "./ leading.png");
}
TEST_F(RelocationTest, ScanProvenanceRemainsImmutableUntilRescan) {
    const auto snapshot = project.missingResourceSnapshot();
    ASSERT_FALSE(snapshot.isEmpty());
    QVariantMap use;
    for (const auto& v : snapshot.value("resources").toList())
        if (v.toMap().value("path").toString() == "old/shared.wav")
            use = v.toMap().value("uses").toList().front().toMap();
    EXPECT_EQ(use.value("oldValue").toString(), "old/shared.wav");
    EXPECT_EQ(use.value("number").toUInt(), 10u);
    EXPECT_EQ(use.value("base").toUInt(), 36u);
    ASSERT_TRUE(write(a, "#BPM 120\n#WAV0A changed.wav\n"));
    ASSERT_TRUE(ok(controller.start("old/shared.wav", {a})));
    ASSERT_TRUE(choose("old/shared.wav", "media/shared.wav"));
    EXPECT_FALSE(ok(controller.prepare()));
    EXPECT_EQ(project.missingResourceSnapshot().value("generation"), snapshot.value("generation"));
    ASSERT_TRUE(scan());
    EXPECT_NE(project.missingResourceSnapshot().value("generation"), snapshot.value("generation"));
}
TEST_F(RelocationTest, SearchBudgetsBoundDuplicatedBasenameAssignmentsAndNeverInferUniqueness) {
    for (int i = 0; i < 99; ++i)
        ASSERT_TRUE(write(QDir(root).filePath(QString("matches/%1/shared.wav").arg(i)), "audio"));
    beatbench::project::Project synthetic;
    synthetic.root = root.toUtf8().toStdString();
    beatbench::project::ChartRef chart;
    chart.path = "A.bms";
    chart.parsed = true;
    synthetic.charts.push_back(chart);
    for (int i = 0; i < 205; ++i) {
        beatbench::project::ResourceRef resource;
        resource.path = QString("absent/%1/shared.wav").arg(i).toStdString();
        resource.kind = "wav";
        beatbench::project::ResourceUse use;
        use.chart = "A.bms";
        use.kind = "wav";
        use.id = "0A";
        use.original_value = resource.path;
        use.definition_id = 10;
        use.id_base = 36;
        resource.uses.push_back(use);
        synthetic.resources.push_back(resource);
    }
    ProjectController::ScanFunctions functions;
    functions.index = [synthetic](const std::string&, const beatbench::project::ScanOptions&) {
        return synthetic;
    };
    functions.parse = [](beatbench::project::Project&, const beatbench::project::ScanOptions&) {};
    ProjectController localProject(functions);
    localProject.scan(root);
    ASSERT_TRUE(waitFor([&] { return localProject.state() == "ready"; }));
    ResourceRelocationController local(&documents, &localProject);
    ASSERT_TRUE(ok(local.start()));
    local.search(root, true);
    ASSERT_TRUE(waitFor([&] { return !local.busy(); }));
    int total = 0;
    for (const auto& v : local.rows()) {
        const auto r = v.toMap();
        const auto candidates = r.value("candidates").toStringList();
        EXPECT_LE(candidates.size(), 100);
        total += int(candidates.size());
        EXPECT_TRUE(r.value("replacement").toString().isEmpty());
        EXPECT_EQ(r.value("state").toString(), "limited");
    }
    EXPECT_LE(total, 20000);
    EXPECT_TRUE(local.message().startsWith("search_limit"));
}
TEST_F(RelocationTest, WorkerLifetimeAndServiceLifetimeAreOwnedAndSafe) {
    {
        auto local = std::make_unique<ResourceRelocationController>(&documents, &project);
        ASSERT_TRUE(ok(local->start()));
        local->search(root, true);
    }
    QCoreApplication::processEvents();
    auto localProject = std::make_unique<ProjectController>();
    auto localDocuments = std::make_unique<DocumentService>();
    ResourceRelocationController local(localDocuments.get(), localProject.get());
    localProject.reset();
    localDocuments.reset();
    EXPECT_FALSE(ok(local.start()));
    EXPECT_FALSE(ok(local.prepare()));
    local.search(root, true);
    EXPECT_FALSE(local.busy());
}
TEST_F(RelocationTest, SymlinkCandidatesAndSearchRootsAreRejectedWithoutTraversal) {
    const auto link = QDir(root).filePath("linked.wav");
    if (!QFile::link(QDir(root).filePath("media/shared.wav"), link) || !QFileInfo(link).isSymLink())
        GTEST_SKIP() << "Native symlink creation unavailable";
    ASSERT_TRUE(ok(controller.start("old/shared.wav")));
    EXPECT_FALSE(
        ok(controller.chooseCandidate(row("old/shared.wav").value("key").toString(), link)));
    controller.search(root, true);
    ASSERT_TRUE(waitFor([&] { return !controller.busy(); }));
    EXPECT_FALSE(row("old/shared.wav").value("candidates").toStringList().contains(link));
}
}  // namespace
int main(int argc, char** argv) {
    QCoreApplication application(argc, argv);
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
