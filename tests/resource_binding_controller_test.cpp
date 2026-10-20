// SPDX-License-Identifier: GPL-3.0-only
#include <gtest/gtest.h>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>
#include <algorithm>
#include <set>
#include "bridge/ResourceBindingController.hpp"
#include "bridge/DocumentService.hpp"
#include "beatbench/core/edit/SessionRegistry.hpp"
#include "beatbench/core/bms/BmsCodec.hpp"

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
TEST_F(ResourceBindingTest, CustomMetaDefinitionsKeepUntouchedSourceModelDiskAndUndo) {
    ASSERT_TRUE(write(a,"#TITLE Old\n#ARTIST Keep\n#BPM 120\n#WAV01 old.wav\n#BPM01 150\n#STOP01 48\n#00108:01\n#00109:01\n#00251:0102\n#00311:01\n#004ZZ:keep\n// original comment\n"));
    ASSERT_TRUE(ok(documents.openReference(a))); auto* s = session(a); ASSERT_NE(s,nullptr);
    const auto before = s->chart(); const auto disk = bytes(a); const auto activeId = session_registry().active_id();
    s->set_backup_enabled(false);
    ASSERT_TRUE(s->exec(std::make_unique<SetSampleFileCommand>(SampleKind::Wav,9,"draft.wav")));
    const auto depth = s->undo_depth(); int persisted = 0;
    s->set_autosave_enabled(true); s->set_backup_enabled(true);
    s->set_persist_hook([&](const Chart&,const std::string&,std::string*) { ++persisted; return true; });
    auto p = controller.prepareCustomContent("#TITLE New\n#WAV01 media/new.wav\n#BMP02 images/new.png\n#BPM01 180.25\n#STOP01 96\n#STAGEFILE images/stage.png",root,{a});
    ASSERT_TRUE(ok(p)) << p.value("error").toString().toStdString();
    EXPECT_EQ(p.value("changedCount").toInt(),6); EXPECT_EQ(p.value("collisionCount").toInt(),4);
    ASSERT_TRUE(ok(controller.commitBindings(planId(p),true)));
    EXPECT_EQ(persisted,0); EXPECT_EQ(bytes(a),disk); EXPECT_EQ(session_registry().active_id(),activeId);
    EXPECT_EQ(s->undo_depth(),depth+1); EXPECT_EQ(s->chart().meta.at("TITLE"),"New"); EXPECT_EQ(s->chart().meta.at("ARTIST"),"Keep");
    EXPECT_EQ(s->chart().notes,before.notes); EXPECT_EQ(s->chart().raw_lines,before.raw_lines); EXPECT_EQ(s->chart().mode_id,before.mode_id);
    EXPECT_EQ(s->chart().bpm_events.front().value.value,180.25); EXPECT_TRUE(s->chart().bpm_events.front().value.ch08);
    EXPECT_EQ(s->chart().bpm_events.front().value.ref_id,1u); EXPECT_EQ(s->chart().stop_events.front().value.count,96);
    s->set_autosave_enabled(false); s->set_backup_enabled(false);
    ASSERT_TRUE(s->undo()); EXPECT_EQ(s->chart().notes,before.notes); EXPECT_EQ(s->chart().meta.at("TITLE"),"Old");
    EXPECT_EQ(file(s,SampleKind::Wav,9),"draft.wav"); ASSERT_TRUE(s->redo()); EXPECT_EQ(s->chart().meta.at("TITLE"),"New");
}
TEST_F(ResourceBindingTest, CustomDataReplacesEntireBgmGroupNotAppendAndPreservesOtherSources) {
    ASSERT_TRUE(write(a,"#BPM 120\n#WAV01 a.wav\n#WAV02 b.wav\n#00101:01\n#00101:0001\n#00111:01\n#00151:0101\n#00201:01\n#003ZZ:untouched\n"));
    ASSERT_TRUE(ok(documents.openReference(a))); auto* s = session(a); const auto before=s->chart();
    auto p=controller.prepareCustomContent("#00101:02\n#00101:0002",root,{a}); ASSERT_TRUE(ok(p));
    const auto row=p.value("rows").toList().front().toMap(); EXPECT_EQ(row.value("key").toString(),"001/01");
    EXPECT_TRUE(row.value("summary").toString().contains("整组替换")); EXPECT_EQ(p.value("collisionCount").toInt(),1);
    EXPECT_FALSE(ok(controller.commitBindings(planId(p)))); EXPECT_EQ(s->chart().notes,before.notes);
    p=controller.prepareCustomContent("#00101:02\n#00101:0002",root,{a}); ASSERT_TRUE(ok(p)); ASSERT_TRUE(ok(controller.commitBindings(planId(p),true)));
    unsigned bgmCount=0; std::set<unsigned> sublines;
    for (const auto& e:s->chart().notes) {
        if (e.measure==1 && e.value.lane.kind==LaneKind::Bgm) { ++bgmCount; EXPECT_EQ(e.value.sample.id,2u); sublines.insert(e.value.sub_line); }
        else {
            const auto original=std::find_if(before.notes.begin(),before.notes.end(),[&](const auto& n) { return n.measure==e.measure && n.pos==e.pos && n.value.lane==e.value.lane && n.value.ln_channel==e.value.ln_channel; });
            ASSERT_NE(original,before.notes.end()); EXPECT_EQ(e.value.sample,original->value.sample); EXPECT_EQ(e.value.sub_line,original->value.sub_line);
        }
    }
    EXPECT_EQ(bgmCount,2u); EXPECT_EQ(sublines,(std::set<unsigned>{0,1})); EXPECT_EQ(s->chart().raw_lines,before.raw_lines);
    s->set_backup_enabled(false); ASSERT_TRUE(s->undo()); EXPECT_EQ(s->chart().notes,before.notes); ASSERT_TRUE(s->redo());
    p=controller.prepareCustomContent("#00101:02\n#00101:0002",root,{a}); ASSERT_TRUE(ok(p)); EXPECT_EQ(p.value("changedCount").toInt(),0);
}
TEST_F(ResourceBindingTest, CustomSameValueReferenceIdentityAndBgmSublineChangesAreNotNoop) {
    ASSERT_TRUE(write(a,"#BPM 120\n#BPM01 180\n#BPM02 180\n#STOP01 48\n#STOP02 48\n#00108:01\n#00109:01\n#00201:00\n#00201:01\n"));
    auto p=controller.prepareCustomContent("#00108:02\n#00109:02\n#00201:01",root,{a}); ASSERT_TRUE(ok(p)) << p.value("error").toString().toStdString();
    EXPECT_EQ(p.value("changedCount").toInt(),3); ASSERT_TRUE(ok(controller.commitBindings(planId(p),true)));
    auto* s=session(a); ASSERT_EQ(s->chart().bpm_events.size(),1u); EXPECT_EQ(s->chart().bpm_events[0].value.ref_id,2u);
    ASSERT_EQ(s->chart().stop_events.size(),1u); EXPECT_EQ(s->chart().stop_events[0].value.ref_id,2u);
    ASSERT_EQ(s->chart().notes.size(),1u); EXPECT_EQ(s->chart().notes[0].value.sub_line,0u);
}
TEST_F(ResourceBindingTest, CustomTimingBgaAndMeasureGroupsRetainOtherChannels) {
    ASSERT_TRUE(write(a,"#BPM 120\n#BPM01 150\n#STOP01 48\n#BMP01 old.png\n#00103:8C\n#00108:01\n#00109:01\n#00104:01\n#00107:01\n#00102:1.5\n"));
    auto p=controller.prepareCustomContent("#BPM02 180\n#STOP02 96\n#BMP02 image.png\n#00108:02\n#00109:02\n#00104:02\n#00102:0.5",root,{a});
    ASSERT_TRUE(ok(p)) << p.value("error").toString().toStdString(); ASSERT_TRUE(ok(controller.commitBindings(planId(p),true)));
    const auto& c=session(a)->chart(); ASSERT_EQ(c.bpm_events.size(),2u);
    EXPECT_EQ(std::count_if(c.bpm_events.begin(),c.bpm_events.end(),[](const auto& e){return !e.value.ch08 && e.value.value==140;}),1);
    EXPECT_EQ(c.stop_events.front().value.count,96); EXPECT_EQ(c.measure_events.front().value.beats,2);
    ASSERT_EQ(c.bga_events.size(),2u); for(const auto& e:c.bga_events) EXPECT_EQ(e.value.image.id,e.value.layer==0?2u:1u);
}
TEST_F(ResourceBindingTest, CustomUnknownHeadersAndRawRoundTripWithoutLosingUnrelatedRaw) {
    ASSERT_TRUE(write(a,"#BPM 120\n#MYSTERY old\n#EXT-RAW old\n#WAV001 legacy.wav\n#003ZZ:old\n// Keep comment\nopaque original\n"));
    auto p=controller.prepareCustomContent("#MYSTERY new\n#EXT-RAW new\n#WAV001 updated.wav\n#003ZZ:new\nopaque new\n// new comment",root,{a});
    ASSERT_TRUE(ok(p)) << p.value("error").toString().toStdString();
    EXPECT_TRUE(p.value("warnings").toStringList().join(' ').contains("未知"));
    ASSERT_TRUE(ok(controller.commitBindings(planId(p),true))); const auto& c=session(a)->chart();
    EXPECT_EQ(c.meta.at("MYSTERY"),"new");
    EXPECT_EQ(std::count(c.raw_lines.begin(),c.raw_lines.end(),"#EXT-RAW old"),0);
    EXPECT_EQ(std::count(c.raw_lines.begin(),c.raw_lines.end(),"#EXT-RAW new"),1);
    EXPECT_EQ(std::count(c.raw_lines.begin(),c.raw_lines.end(),"opaque original"),1);
    EXPECT_EQ(std::count(c.raw_lines.begin(),c.raw_lines.end(),"// Keep comment"),1);
    const auto saved=bms::write_bms(c); const auto reloaded=bms::read_bms(saved).chart;
    EXPECT_EQ(reloaded.meta.at("MYSTERY"),"new");
    for(const auto& line:c.raw_lines) EXPECT_NE(std::find(reloaded.raw_lines.begin(),reloaded.raw_lines.end(),line),reloaded.raw_lines.end()) << line;
}
TEST_F(ResourceBindingTest, CustomMixedBasesInterpretInputPerTargetAndRootRelativePaths) {
    const auto p=controller.prepareCustomContent("#TITLE Both\n#WAV0a media/missing.wav\n#BMP0a images/missing.png\n#BANNER images/banner.png\n#00211:0a",root,{a,b});
    ASSERT_TRUE(ok(p)) << p.value("error").toString().toStdString(); ASSERT_TRUE(ok(controller.commitBindings(planId(p))));
    EXPECT_EQ(file(session(a),SampleKind::Wav,10),"media/missing.wav"); EXPECT_EQ(file(session(b),SampleKind::Wav,36),"../media/missing.wav");
    EXPECT_EQ(session(a)->chart().meta.at("BANNER"),"images/banner.png"); EXPECT_EQ(session(b)->chart().meta.at("BANNER"),"../images/banner.png");
    for(const auto& pair:std::vector<std::pair<EditorSession*,unsigned>>{{session(a),10},{session(b),36}}) {
        const auto note=std::find_if(pair.first->chart().notes.begin(),pair.first->chart().notes.end(),[](const auto& e){return e.measure==2;});
        ASSERT_NE(note,pair.first->chart().notes.end()); EXPECT_EQ(note->value.sample.id,pair.second);
    }
    EXPECT_EQ(session(a)->chart().id_base,IdBase::Base36); EXPECT_EQ(session(b)->chart().id_base,IdBase::Base62);
}
TEST_F(ResourceBindingTest, CustomPmsProfilesAndLnSourceIdentityArePreserved) {
    const auto standard=QDir(root).filePath("standard.pms"), bmesp=QDir(root).filePath("bmesp.pms");
    ASSERT_TRUE(write(standard,"#BPM 120\n#00122:01\n#00262:0101\n#00311:01\n"));
    ASSERT_TRUE(write(bmesp,"#BPM 120\n#00118:01\n#00258:0101\n#00311:01\n"));
    auto p=controller.prepareCustomContent("#TITLE PMS\n#WAV02 updated.wav",root,{standard,bmesp}); ASSERT_TRUE(ok(p));
    const auto stdBefore=session(standard)->chart(), bmeBefore=session(bmesp)->chart(); ASSERT_TRUE(ok(controller.commitBindings(planId(p))));
    EXPECT_EQ(session(standard)->chart().mode_id,std::optional<std::string>{"pms9k"});
    EXPECT_EQ(session(bmesp)->chart().mode_id,std::optional<std::string>{"pms9k-bmesp"});
    EXPECT_EQ(session(standard)->chart().notes,stdBefore.notes); EXPECT_EQ(session(bmesp)->chart().notes,bmeBefore.notes);
    p=controller.prepareCustomContent("#00118:02",root,{bmesp}); ASSERT_TRUE(ok(p)); ASSERT_TRUE(ok(controller.commitBindings(planId(p),true)));
    EXPECT_EQ(session(bmesp)->chart().mode_id,bmeBefore.mode_id);
    const auto saved=bms::write_bms(session(bmesp)->chart()); EXPECT_NE(saved.find("#00118:02"),std::string::npos); EXPECT_NE(saved.find("#00258:"),std::string::npos);
    EXPECT_FALSE(ok(controller.prepareCustomContent("#00118:02",root,{standard})));
}
TEST_F(ResourceBindingTest, CustomRejectsStructuralChangesControlsAndMalformedData) {
    const QStringList bad{"#BASE 62","#BASE 36","#PLAYER 3","#LNOBJ ZZ","#LNTYPE 2","#LNMODE 1","#ENCODING UTF-8",
        "#RANDOM 2","#IF 1","#ENDIF","#ENDRANDOM","#SWITCH 1","#ENDSWITCH","#DEFAULT","/* comment */",
        "#00111:012","#00111:0!","#00103:ZZ","#00108:ZZ","#00109:ZZ","#00102:NaN","#BPM01 NaN","#STOP01 1.5","#WAV01 ../escape.wav","#WAV01","#BPM01","#STOP01"};
    for(const auto& raw:bad) { SCOPED_TRACE(raw.toStdString()); EXPECT_FALSE(ok(controller.prepareCustomContent(raw,root,{a}))); }
    ASSERT_TRUE(ok(documents.openReference(a))); session(a)->chart_mut().raw_lines.push_back("#SWITCH 1");
    EXPECT_FALSE(ok(controller.prepareCustomContent("#TITLE Safe",root,{a}))); EXPECT_EQ(session(a)->undo_depth(),0u);
}
TEST_F(ResourceBindingTest, CustomStaleCancelReplacementAndNoopAreSafe) {
    auto p=controller.prepareCustomContent("#TITLE Custom",root,{a,b}); ASSERT_TRUE(ok(p)); controller.discardPlan();
    EXPECT_FALSE(ok(controller.commitBindings(planId(p),true))); EXPECT_EQ(session(a)->undo_depth(),0u);
    p=controller.prepareCustomContent("#TITLE Custom",root,{a,b}); ASSERT_TRUE(ok(p));
    auto* sb=session(b); sb->set_backup_enabled(false); ASSERT_TRUE(sb->exec(std::make_unique<SetSampleFileCommand>(SampleKind::Wav,8,"draft.wav"))); ASSERT_TRUE(sb->undo());
    EXPECT_FALSE(ok(controller.commitBindings(planId(p),true))); EXPECT_EQ(session(a)->undo_depth(),0u);
    p=controller.prepareCustomContent("#BPM 120",root,{a,b}); ASSERT_TRUE(ok(p)); EXPECT_EQ(p.value("changedCount").toInt(),0);
    ASSERT_TRUE(ok(controller.commitBindings(planId(p)))); EXPECT_EQ(session(a)->undo_depth(),0u);
    p=controller.prepareCustomContent("#TITLE Custom",root,{a}); ASSERT_TRUE(ok(p));
    const auto newer=controller.prepareBindings("#WAV02 other.wav",root,{a}); ASSERT_TRUE(ok(newer));
    EXPECT_FALSE(ok(controller.commitBindings(planId(p),true)));
}
TEST_F(ResourceBindingTest, CustomBudgetsRejectBeforeOpeningTargets) {
    EXPECT_FALSE(ok(controller.prepareCustomContent(QString(1024*1024+1,'x'),root,{a})));
    QString many; for(int i=0;i<20001;++i) many+="// x\n";
    EXPECT_FALSE(ok(controller.prepareCustomContent(many,root,{a})));
    QStringList targets; for(int i=0;i<20001;++i) targets.push_back(a);
    EXPECT_FALSE(ok(controller.prepareCustomContent("#TITLE x",root,targets))); EXPECT_TRUE(DocumentService::findSessionId(a).isEmpty());
}
TEST_F(ResourceBindingTest, SimpleWavHelperAcceptsRootRelativeAndAbsoluteButRejectsInjection) {
    auto p=controller.prepareWavBinding("02",QDir(root).filePath("media/new.wav"),root,{a,b}); ASSERT_TRUE(ok(p));
    ASSERT_TRUE(ok(controller.commitBindings(planId(p)))); EXPECT_EQ(file(session(b),SampleKind::Wav,2),"../media/new.wav");
    EXPECT_FALSE(ok(controller.prepareWavBinding("03\n#WAV04","x.wav",root,{a})));
    EXPECT_FALSE(ok(controller.prepareWavBinding("03","x.wav\n#WAV04 other.wav",root,{a})));
    EXPECT_FALSE(ok(controller.prepareWavBinding("03",QDir(root).absoluteFilePath("../outside.wav"),root,{a})));
    EXPECT_FALSE(ok(controller.prepareWavBinding("00","x.wav",root,{a})));
    p=controller.prepareWavBinding("02","other.wav",root,{a}); ASSERT_TRUE(ok(p)); EXPECT_EQ(p.value("collisionCount").toInt(),1);
    EXPECT_FALSE(ok(controller.commitBindings(planId(p)))); EXPECT_EQ(file(session(a),SampleKind::Wav,2),"media/new.wav");
}
} // namespace
int main(int argc, char** argv) {
    QCoreApplication application(argc,argv);
    ::testing::InitGoogleTest(&argc,argv);
    return RUN_ALL_TESTS();
}
