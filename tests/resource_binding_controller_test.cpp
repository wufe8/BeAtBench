// SPDX-License-Identifier: GPL-3.0-only
#include <gtest/gtest.h>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>
#include <algorithm>
#include "bridge/ResourceBindingController.hpp"
#include "bridge/DocumentService.hpp"
#include "beatbench/core/edit/SessionRegistry.hpp"

namespace {
using namespace beatbench;
using namespace beatbench::edit;
using namespace beatbench::app;
bool ok(const QVariantMap& r) { return r.value("ok").toBool(); }
QString planId(const QVariantMap& r) { return r.value("planId").toString(); }
class ResourceBindingTest : public ::testing::Test {
protected:
    DocumentService documents;
    ResourceBindingController controller{&documents};
    std::unique_ptr<QTemporaryDir> temporary;
    QString root, a, b;
    std::vector<std::string> previousIds;
    std::string active;
    void SetUp() override {
        previousIds = session_registry().ids(); active = session_registry().active_id();
        const QString base = qEnvironmentVariable("BB_TEST_TMPDIR", QDir::tempPath());
        ASSERT_TRUE(QDir().mkpath(base));
        temporary = std::make_unique<QTemporaryDir>(QDir(base).filePath("binding-XXXXXX"));
        ASSERT_TRUE(temporary->isValid()); root = temporary->path();
        a = QDir(root).filePath("A.bms"); b = QDir(root).filePath("sub/B.bms");
        ASSERT_TRUE(write(a, "#BPM 120\n#WAV01 old.wav\n#00111:01\n"));
        ASSERT_TRUE(write(b, "#BASE 62\n#BPM 120\n#WAV01 ../old.wav\n#00111:01\n"));
    }
    void TearDown() override {
        session_registry().activate(active);
        for (const auto& id : session_registry().ids())
            if (std::find(previousIds.begin(), previousIds.end(), id) == previousIds.end()) session_registry().close(id);
    }
    static bool write(const QString& path, const QByteArray& bytes) {
        QDir().mkpath(QFileInfo(path).absolutePath()); QFile file(path);
        return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size();
    }
    static QByteArray bytes(const QString& path) { QFile f(path); if (!f.open(QIODevice::ReadOnly)) return {}; return f.readAll(); }
    EditorSession* session(const QString& path) {
        return session_registry().by_id(DocumentService::findSessionId(path).toUtf8().toStdString());
    }
    QVariantMap prepare(const QString& raw = "#WAV02 media/new.wav\n#BMP02 images/new.png") {
        return controller.prepareBindings(raw, root, {a,b});
    }
    static std::string file(EditorSession* s, SampleKind kind, unsigned id) {
        const auto it = s->chart().samples.find({kind,id});
        return it == s->chart().samples.end() ? "<absent>" : it->second.file;
    }
};
TEST_F(ResourceBindingTest, MultipleDirtyAndUnopenedTargetsKeepActiveNotesDiskAndIndependentUndo) {
    ASSERT_TRUE(ok(documents.open(a)));
    auto* sa = session(a); ASSERT_NE(sa,nullptr); sa->set_backup_enabled(false);
    ASSERT_TRUE(sa->exec(std::make_unique<SetSampleFileCommand>(SampleKind::Wav,3,"draft.wav")));
    const auto depth = sa->undo_depth(); const auto aBytes = bytes(a), bBytes = bytes(b);
    const auto beforeActive = session_registry().active_id();
    const auto notes = sa->chart().notes;
    int signalCount = 0; QStringList changed;
    QObject::connect(&controller, &ResourceBindingController::bindingsApplied, &controller,
        [&](const QStringList& paths) { ++signalCount; changed = paths; });
    const auto p = prepare(); ASSERT_TRUE(ok(p)) << p.value("error").toString().toStdString();
    EXPECT_EQ(p.value("targetCount").toInt(),2); EXPECT_EQ(p.value("changedCount").toInt(),4);
    EXPECT_EQ(p.value("collisionCount").toInt(),0); EXPECT_NE(session(b),nullptr);
    EXPECT_EQ(session_registry().active_id(),beforeActive); EXPECT_EQ(sa->undo_depth(),depth);
    ASSERT_TRUE(ok(controller.commitBindings(planId(p))));
    EXPECT_EQ(signalCount,1); EXPECT_EQ(changed.size(),2); EXPECT_EQ(sa->undo_depth(),depth+1);
    EXPECT_EQ(session(b)->undo_depth(),1u); EXPECT_TRUE(sa->is_dirty()); EXPECT_TRUE(session(b)->is_dirty());
    EXPECT_EQ(file(sa,SampleKind::Wav,2),"media/new.wav");
    EXPECT_EQ(file(session(b),SampleKind::Wav,2),"../media/new.wav");
    EXPECT_EQ(sa->chart().notes,notes); EXPECT_EQ(bytes(a),aBytes); EXPECT_EQ(bytes(b),bBytes);
    EXPECT_EQ(session_registry().active_id(),beforeActive);
    ASSERT_TRUE(sa->undo()); EXPECT_EQ(file(sa,SampleKind::Wav,2),"<absent>");
    EXPECT_EQ(file(sa,SampleKind::Wav,3),"draft.wav"); EXPECT_EQ(file(session(b),SampleKind::Wav,2),"../media/new.wav");
    session(b)->set_backup_enabled(false); ASSERT_TRUE(session(b)->undo());
    EXPECT_EQ(file(session(b),SampleKind::Bmp,2),"<absent>");
    EXPECT_FALSE(ok(controller.commitBindings(planId(p))));
}
TEST_F(ResourceBindingTest, OverwriteRequiresExplicitConsentAndIsSingleUse) {
    auto p = prepare("#WAV01 new.wav"); ASSERT_TRUE(ok(p)); EXPECT_EQ(p.value("collisionCount").toInt(),2);
    EXPECT_FALSE(ok(controller.commitBindings(planId(p)))); EXPECT_EQ(file(session(a),SampleKind::Wav,1),"old.wav");
    EXPECT_FALSE(ok(controller.commitBindings(planId(p),true)));
    p = prepare("#WAV01 new.wav"); ASSERT_TRUE(ok(p)); ASSERT_TRUE(ok(controller.commitBindings(planId(p),true)));
    EXPECT_EQ(file(session(a),SampleKind::Wav,1),"new.wav"); EXPECT_EQ(file(session(b),SampleKind::Wav,1),"../new.wav");
    session(a)->set_backup_enabled(false); ASSERT_TRUE(session(a)->undo()); EXPECT_EQ(file(session(a),SampleKind::Wav,1),"old.wav");
}
TEST_F(ResourceBindingTest, DefinedEmptyFileStillRequiresOverwriteConsent) {
    ASSERT_TRUE(ok(documents.openReference(a)));
    session(a)->chart_mut().samples[{SampleKind::Wav,2}] = SampleDef{};
    const auto p = controller.prepareBindings("#WAV02 missing.wav",root,{a});
    ASSERT_TRUE(ok(p)); EXPECT_EQ(p.value("collisionCount").toInt(),1);
    EXPECT_FALSE(ok(controller.commitBindings(planId(p))));
    EXPECT_EQ(file(session(a),SampleKind::Wav,2),""); EXPECT_EQ(session(a)->undo_depth(),0u);
}
TEST_F(ResourceBindingTest, CancelAndReplacementInvalidatePlansWithoutUndo) {
    auto p = prepare(); ASSERT_TRUE(ok(p)); controller.discardPlan();
    EXPECT_FALSE(ok(controller.commitBindings(planId(p)))); EXPECT_EQ(session(a)->undo_depth(),0u);
    p = prepare(); ASSERT_TRUE(ok(p)); const auto p2 = prepare("#WAV03 foo.wav"); ASSERT_TRUE(ok(p2));
    EXPECT_FALSE(ok(controller.commitBindings(planId(p)))); ASSERT_TRUE(ok(controller.commitBindings(planId(p2))));
}
TEST_F(ResourceBindingTest, SameValuesCreateNoUndoOrSignal) {
    int signalCount = 0;
    QObject::connect(&controller,&ResourceBindingController::bindingsApplied,&controller,[&](const QStringList&){++signalCount;});
    const auto p = prepare("#WAV01 old.wav"); ASSERT_TRUE(ok(p));
    EXPECT_EQ(p.value("changedCount").toInt(),0); EXPECT_EQ(p.value("collisionCount").toInt(),0);
    ASSERT_TRUE(ok(controller.commitBindings(planId(p)))); EXPECT_EQ(signalCount,0);
    EXPECT_EQ(session(a)->undo_depth(),0u); EXPECT_EQ(session(b)->undo_depth(),0u); EXPECT_FALSE(session(a)->is_dirty());
}
TEST_F(ResourceBindingTest, EachTargetDecodesBase36And62WithoutChangingBaseOrNotes) {
    const auto p = prepare("#WAV0a missing.wav"); ASSERT_TRUE(ok(p));
    const auto aNotes = session(a)->chart().notes, bNotes = session(b)->chart().notes;
    ASSERT_TRUE(ok(controller.commitBindings(planId(p))));
    EXPECT_EQ(file(session(a),SampleKind::Wav,10),"missing.wav"); EXPECT_EQ(file(session(b),SampleKind::Wav,36),"../missing.wav");
    EXPECT_EQ(session(a)->chart().id_base,IdBase::Base36); EXPECT_EQ(session(b)->chart().id_base,IdBase::Base62);
    EXPECT_EQ(session(a)->chart().notes,aNotes); EXPECT_EQ(session(b)->chart().notes,bNotes);
}
TEST_F(ResourceBindingTest, DuplicateDecodedIdsRejectedPerTargetBut62CaseRemainsDistinct) {
    EXPECT_FALSE(ok(prepare("#WAV0A a.wav\n#WAV0a b.wav")));
    auto p = controller.prepareBindings("#WAV0A a.wav\n#WAV0a b.wav\n#BMP0A a.png",root,{b});
    ASSERT_TRUE(ok(p)); ASSERT_TRUE(ok(controller.commitBindings(planId(p))));
    EXPECT_EQ(file(session(b),SampleKind::Wav,10),"../a.wav"); EXPECT_EQ(file(session(b),SampleKind::Wav,36),"../b.wav");
    EXPECT_FALSE(ok(controller.prepareBindings("#BMP0a a.png\n#BMP0a b.png",root,{b})));
}
TEST_F(ResourceBindingTest, StrictInputRejectsUnsupportedMaliciousAndIllegalIds) {
    QStringList bad{"", " \n\t", "#TITLE hi", "#BASE 62", "#00111:01", "#RANDOM 2\n#WAV01 a.wav\n#ENDRANDOM",
        "#IF 1", "#WAV01", "#WAV01 ", "#WAV0! a.wav", "#WAV001 a.wav", "#WAV00 a.wav",
        "#WAV01 a.wav\n#WAV01 a.wav", "#BMP01 a.png\n#BMP01 b.png", "#WAV01 ../escape.wav",
        "#WAV01 C:\\a.wav", "#WAV01 /a.wav", "#WAV01 \\\\server\\a.wav", "#WAV01 file:a.wav",
        "#WAV01 a.wav\r#TITLE pwn", "#WAV01 a.wav\nnot-definition", "#BPM01 120", "#STOP01 12"};
    bad.push_back(QString("#WAV0")+QChar(0x212a)+" a.wav"); // Unicode Kelvin sign 不可经忽略大小写混入 ASCII ID
    bad.push_back(QString("#WAV01 a")+QChar(0)+".wav");
    bad.push_back(QString("#WAV01 a")+QChar(0x2028)+".wav");
    for (const auto& raw : bad) { SCOPED_TRACE(raw.toStdString()); EXPECT_FALSE(ok(prepare(raw))); }
}
TEST_F(ResourceBindingTest, StaleEditUndoRoundtripRejectsAllTargetsBeforeMutation) {
    const auto p = prepare(); ASSERT_TRUE(ok(p)); auto* sb = session(b); sb->set_backup_enabled(false);
    ASSERT_TRUE(sb->exec(std::make_unique<SetSampleFileCommand>(SampleKind::Wav,4,"x.wav"))); ASSERT_TRUE(sb->undo());
    const auto r = controller.commitBindings(planId(p)); EXPECT_FALSE(ok(r));
    EXPECT_TRUE(r.value("error").toString().startsWith("stale_plan")); EXPECT_EQ(session(a)->undo_depth(),0u);
    EXPECT_EQ(file(session(a),SampleKind::Wav,2),"<absent>");
}
TEST_F(ResourceBindingTest, ReloadCloseRecreateAndPathChangesAreStale) {
    auto p = prepare(); ASSERT_TRUE(ok(p)); auto* sb = session(b);
    auto chart = sb->chart(); auto path = sb->path(); sb->load(chart,path);
    EXPECT_FALSE(ok(controller.commitBindings(planId(p)))); EXPECT_EQ(session(a)->undo_depth(),0u);
    p = prepare(); ASSERT_TRUE(ok(p)); const auto id = DocumentService::findSessionId(b).toStdString();
    ASSERT_TRUE(session_registry().close(id)); ASSERT_TRUE(session_registry().create(id,false));
    session_registry().by_id(id)->load(chart,path); EXPECT_FALSE(ok(controller.commitBindings(planId(p))));
    p = prepare(); ASSERT_TRUE(ok(p)); session(b)->set_path(path+".renamed");
    EXPECT_FALSE(ok(controller.commitBindings(planId(p)))); EXPECT_EQ(session(a)->undo_depth(),0u);
}
TEST_F(ResourceBindingTest, ClosedTargetAndRootRenameRejectWholePlan) {
    auto p = prepare(); ASSERT_TRUE(ok(p));
    const auto id = DocumentService::findSessionId(b).toStdString();
    ASSERT_TRUE(session_registry().close(id));
    EXPECT_FALSE(ok(controller.commitBindings(planId(p)))); EXPECT_EQ(session(a)->undo_depth(),0u);
    p = prepare(); ASSERT_TRUE(ok(p));
    const QString moved = root+"-moved";
    ASSERT_TRUE(QDir().rename(root,moved));
    const auto result = controller.commitBindings(planId(p));
    EXPECT_FALSE(ok(result)); EXPECT_EQ(session(a)->undo_depth(),0u);
    EXPECT_TRUE(QDir().rename(moved,root));
}
TEST_F(ResourceBindingTest, PortableSubdirectoryMissingPathsAndCrLfAreAccepted) {
    const auto p = prepare(" \r\n#WAV02 media\\missing sound.wav\r\n\t#BMP02 images/x.png\r\n");
    ASSERT_TRUE(ok(p)); ASSERT_TRUE(ok(controller.commitBindings(planId(p))));
    EXPECT_EQ(file(session(b),SampleKind::Wav,2),"../media/missing sound.wav");
}
TEST_F(ResourceBindingTest, AutosaveAndBackupHooksCannotWriteDuringCommit) {
    ASSERT_TRUE(write(a+".bak", "existing backup"));
    const auto p = prepare(); ASSERT_TRUE(ok(p)); const auto before = bytes(a), beforeBackup = bytes(a+".bak"); int writes = 0;
    auto* sa = session(a); sa->set_autosave_enabled(true); sa->set_backup_enabled(true);
    sa->set_persist_hook([&](const Chart&,const std::string&,std::string*){++writes;return true;});
    ASSERT_TRUE(ok(controller.commitBindings(planId(p)))); EXPECT_EQ(writes,0); EXPECT_EQ(bytes(a),before);
    EXPECT_EQ(bytes(a+".bak"),beforeBackup);
    EXPECT_TRUE(sa->autosave_enabled()); EXPECT_TRUE(sa->backup_enabled()); EXPECT_TRUE(sa->is_dirty());
}
TEST_F(ResourceBindingTest, LowercaseKeywordsPreserveIdCaseAndBmp00IsValid) {
    const auto p = prepare("#wav0a missing.wav\n#bmp00 poor.png"); ASSERT_TRUE(ok(p));
    ASSERT_TRUE(ok(controller.commitBindings(planId(p))));
    EXPECT_EQ(file(session(a),SampleKind::Bmp,0),"poor.png");
    EXPECT_EQ(file(session(b),SampleKind::Bmp,0),"../poor.png");
    EXPECT_EQ(file(session(b),SampleKind::Wav,36),"../missing.wav");
}
TEST_F(ResourceBindingTest, PreviewBudgetsRejectWithoutOpeningTargets) {
    EXPECT_FALSE(ok(prepare(QString(1024*1024+1,'x'))));
    QStringList targets;
    for (int i = 0; i < 20001; ++i) targets.push_back(a);
    const auto result = controller.prepareBindings("#WAV02 a.wav",root,targets);
    EXPECT_FALSE(ok(result)); EXPECT_TRUE(result.value("error").toString().startsWith("preview_limit"));
    EXPECT_TRUE(DocumentService::findSessionId(a).isEmpty());
}
TEST_F(ResourceBindingTest, InvalidTargetsAndConditionalTargetsAreRejected) {
    EXPECT_FALSE(ok(controller.prepareBindings("#WAV02 a.wav",root,{})));
    EXPECT_FALSE(ok(controller.prepareBindings("#WAV02 a.wav",root,{a,a})));
    EXPECT_FALSE(ok(controller.prepareBindings("#WAV02 a.wav",root,{"../outside.bms"})));
    EXPECT_FALSE(ok(controller.prepareBindings("#WAV02 a.wav",root,{"missing.bms"})));
    ASSERT_TRUE(ok(documents.openReference(a))); session(a)->chart_mut().raw_lines.push_back("#RANDOM 2");
    EXPECT_FALSE(ok(prepare()));
}
} // namespace
int main(int argc, char** argv) {
    QCoreApplication application(argc,argv);
    ::testing::InitGoogleTest(&argc,argv);
    return RUN_ALL_TESTS();
}
