// SPDX-License-Identifier: GPL-3.0-only
// DocumentService：真实 core 会话/命令，零 GUI/音频设备；临时目录可由 BB_TEST_TMPDIR 指定。
#include <gtest/gtest.h>

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>

#include <algorithm>
#include <memory>
#include <string>
#include <vector>

#include "bridge/DocumentService.hpp"
#include "beatbench/core/command/Command.hpp"
#include "beatbench/core/bms/BmsCodec.hpp"
#include "beatbench/core/edit/EditorSession.hpp"
#include "beatbench/core/edit/SessionRegistry.hpp"

namespace {
using beatbench::app::DocumentService;
using beatbench::edit::session_registry;

class DocumentServiceTest : public ::testing::Test {
protected:
    void SetUp() override {
        auto& reg = session_registry();
        previousActive_ = reg.active_id();
        previousIds_ = reg.ids();
        const QString base = qEnvironmentVariable("BB_TEST_TMPDIR", QDir::tempPath());
        ASSERT_TRUE(QDir().mkpath(base));
        temporary_ = std::make_unique<QTemporaryDir>(QDir(base).filePath(QStringLiteral("document-service-XXXXXX")));
        ASSERT_TRUE(temporary_->isValid());
        aPath_ = temporary_->filePath(QStringLiteral("A.bms"));
        bPath_ = temporary_->filePath(QStringLiteral("B.bms"));
        ASSERT_TRUE(writeChart(aPath_, "A"));
        ASSERT_TRUE(writeChart(bPath_, "B"));
    }

    void TearDown() override {
        // 只清理由本用例创建的会话；不删除/重置测试前已有用户或其它测试会话。
        auto& reg = session_registry();
        for (const std::string& id : reg.ids()) {
            if (std::find(previousIds_.begin(), previousIds_.end(), id) == previousIds_.end())
                reg.close(id);
        }
        reg.activate(previousActive_);
        temporary_.reset();  // 仅移除独占的随机临时子目录
    }

    static bool writeChart(const QString& path, const QByteArray& title) {
        QFile file(path);
        if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) return false;
        const QByteArray body = "#PLAYER 1\n#TITLE " + title + "\n#BPM 120\n";
        return file.write(body) == body.size();
    }

    static QByteArray readBytes(const QString& path) {
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly)) return {};
        return file.readAll();
    }

    static QString idOf(const QVariantMap& response) { return response.value("id").toString(); }
    static bool ok(const QVariantMap& response) { return response.value("ok").toBool(); }

    static beatbench::edit::EditorSession* session(const QString& id) {
        return session_registry().by_id(id.toUtf8().toStdString());
    }

    static bool edit(const QString& id) {
        auto* s = session(id);
        if (!s) return false;
        s->set_backup_enabled(false);  // 本用例无需崩溃备份，亦不修改原谱面文件
        return s->exec(std::make_unique<beatbench::edit::PutNoteCommand>(
            1, beatbench::Rational(0, 1), beatbench::Lane{0, beatbench::LaneKind::Key, 1}, 1));
    }

    static bool containsId(const QVariantList& documents, const QString& id) {
        for (const QVariant& doc : documents)
            if (doc.toMap().value("id").toString() == id) return true;
        return false;
    }

    QString newUnnamed() {
        const std::string id = "document-service-unnamed";
        if (!session_registry().create(id)) return {};
        session_registry().by_id(id)->load(beatbench::Chart{});
        return QString::fromStdString(id);
    }

    DocumentService service_;
    std::unique_ptr<QTemporaryDir> temporary_;
    QString aPath_, bPath_;
    std::string previousActive_;
    std::vector<std::string> previousIds_;
};

TEST_F(DocumentServiceTest, CreatesSelectedModesWithoutOverwritingExistingDocuments) {
    const auto a = service_.open(aPath_);
    const auto b = service_.openReference(bPath_);
    ASSERT_TRUE(ok(a)); ASSERT_TRUE(ok(b)); ASSERT_TRUE(edit(idOf(a)));
    auto* aCore = session(idOf(a)); auto* bCore = session(idOf(b));
    const auto aNotes = aCore->chart().notes;
    const auto aUndo = aCore->undo_depth();
    for (const auto* mode : {"sp7k", "dp", "battle", "pms9k"}) {
        ASSERT_TRUE(ok(service_.activate(idOf(a))));
        const auto count = service_.instances().size();
        const auto created = service_.create(QString::fromLatin1(mode));
        ASSERT_TRUE(ok(created)) << created.value("error").toString().toStdString();
        const auto id = idOf(created);
        ASSERT_NE(id, idOf(a)); ASSERT_NE(id, idOf(b));
        ASSERT_NE(session(id), nullptr);
        EXPECT_EQ(service_.instances().size(), count + 1);
        EXPECT_EQ(session(id)->chart().mode_id.value_or(""), mode);
        EXPECT_EQ(service_.activeId(), id);
        EXPECT_EQ(aCore->chart().notes, aNotes);
        EXPECT_EQ(aCore->undo_depth(), aUndo);
        EXPECT_TRUE(aCore->is_dirty()); EXPECT_FALSE(bCore->is_dirty());
        EXPECT_EQ(bCore->chart().meta.at("TITLE"), "B");
        EXPECT_EQ(service_.session(id)->playKeyCount(), std::string(mode) == "pms9k" ? 9 : 7);
        EXPECT_FALSE(service_.session(id)->codecModes().contains("pms9k-bmesp"));
        ASSERT_TRUE(edit(id));
        const auto target = temporary_->filePath(QString::fromLatin1(mode) +
            (std::string(mode) == "pms9k" ? ".pms" : ".bms"));
        ASSERT_TRUE(ok(service_.save(id, target)));
        const auto reopened = beatbench::bms::read_bms_file(target.toUtf8().toStdString());
        EXPECT_EQ(reopened.chart.mode_id.value_or(""), mode);
        EXPECT_EQ(reopened.chart.notes.size(), 1u);
        EXPECT_EQ(service_.activeId(), id);
    }
    ASSERT_TRUE(ok(service_.activate(idOf(a))));
    const auto ids = session_registry().ids();
    for (const auto* mode : {"invalid", "pms9k-bmesp"}) {
        const auto rejected = service_.create(QString::fromLatin1(mode));
        EXPECT_FALSE(ok(rejected));
        EXPECT_EQ(session_registry().ids(), ids);
        EXPECT_EQ(service_.activeId(), idOf(a));
        EXPECT_EQ(aCore->chart().notes, aNotes); EXPECT_EQ(aCore->undo_depth(), aUndo);
        EXPECT_EQ(bCore->chart().meta.at("TITLE"), "B");
    }
}

TEST_F(DocumentServiceTest, OpensIndependentDocumentsAndPreservesDirtyUndo) {
    const auto a = service_.open(aPath_);
    ASSERT_TRUE(ok(a));
    const QString aId = idOf(a);
    ASSERT_TRUE(edit(aId));
    const QByteArray diskA = readBytes(aPath_);
    const auto b = service_.open(bPath_);
    ASSERT_TRUE(ok(b));
    EXPECT_NE(idOf(b), aId);
    EXPECT_FALSE(b.value("reused").toBool());
    EXPECT_EQ(session_registry().active_id(), idOf(b).toStdString());
    ASSERT_NE(session(aId), nullptr);
    EXPECT_TRUE(session(aId)->is_dirty());
    EXPECT_EQ(session(aId)->undo_depth(), 1u);
    EXPECT_EQ(session(aId)->chart().notes.size(), 1u);
    EXPECT_EQ(readBytes(aPath_), diskA);
    EXPECT_TRUE(session(aId)->undo());
    EXPECT_TRUE(session(aId)->chart().notes.empty());
    EXPECT_EQ(session(idOf(b))->chart().meta.at("TITLE"), "B");
}

TEST_F(DocumentServiceTest, ReferenceOpenNeverActivatesAndReusesDirtyDocument) {
    const auto a = service_.open(aPath_);
    ASSERT_TRUE(ok(a));
    const auto aId = idOf(a);
    ASSERT_TRUE(edit(aId));
    auto* primary = service_.currentSession();
    int changes = 0;
    const auto connection = QObject::connect(&service_, &DocumentService::currentSessionChanged,
                                             [&changes] { ++changes; });
    const auto b = service_.openReference(bPath_);
    ASSERT_TRUE(ok(b));
    EXPECT_FALSE(b.value("reused").toBool());
    EXPECT_EQ(service_.activeId(), aId);
    EXPECT_EQ(service_.currentSession(), primary);
    ASSERT_TRUE(edit(idOf(b)));
    auto* reference = service_.session(idOf(b));
    const auto reused = service_.openReference(bPath_);
    ASSERT_TRUE(ok(reused));
    EXPECT_TRUE(reused.value("reused").toBool());
    EXPECT_EQ(idOf(reused), idOf(b));
    EXPECT_EQ(service_.session(idOf(reused)), reference);
    EXPECT_TRUE(session(idOf(b))->is_dirty());
    EXPECT_EQ(session(idOf(b))->undo_depth(), 1u);
    const auto ids = session_registry().ids();
    EXPECT_FALSE(ok(service_.openReference(bPath_ + ".missing")));
    EXPECT_FALSE(ok(service_.openReference({})));
    EXPECT_EQ(session_registry().ids(), ids);
    EXPECT_EQ(service_.activeId(), aId);
    EXPECT_TRUE(session(aId)->is_dirty());
    EXPECT_EQ(session(aId)->undo_depth(), 1u);
    EXPECT_EQ(changes, 0);
    QObject::disconnect(connection);
}

TEST_F(DocumentServiceTest, ReusesNormalizedPathWithoutReloadingDirtyDocument) {
    const auto a = service_.open(aPath_);
    ASSERT_TRUE(ok(a));
    ASSERT_TRUE(edit(idOf(a)));
    ASSERT_TRUE(writeChart(aPath_, "External Change"));
    ASSERT_TRUE(ok(service_.open(bPath_)));
    const auto count = session_registry().ids().size();
    const QString alias = QDir::current().relativeFilePath(
        temporary_->path() + QStringLiteral("/./A.bms"));
    const auto reused = service_.open(alias);
    ASSERT_TRUE(ok(reused));
    EXPECT_TRUE(reused.value("reused").toBool());
    EXPECT_EQ(idOf(reused), idOf(a));
    EXPECT_EQ(session_registry().ids().size(), count);
    EXPECT_EQ(session_registry().active_id(), idOf(a).toStdString());
    EXPECT_EQ(session(idOf(a))->chart().meta.at("TITLE"), "A");
    EXPECT_TRUE(session(idOf(a))->is_dirty());
    EXPECT_EQ(session(idOf(a))->undo_depth(), 1u);
    EXPECT_TRUE(QDir::isAbsolutePath(reused.value("path").toString()));
}

#ifdef Q_OS_WIN
TEST_F(DocumentServiceTest, WindowsPathCaseAndNativeSeparatorsReuseSameSession) {
    const auto a = service_.open(aPath_);
    ASSERT_TRUE(ok(a));
    // 兼容旧入口保存的原生分隔符/大小写路径，不只测试本服务自己写入的规范路径。
    session(idOf(a))->set_path(QDir::toNativeSeparators(aPath_.toUpper()).toUtf8().toStdString());
    const auto reused = service_.open(aPath_);
    ASSERT_TRUE(ok(reused));
    EXPECT_TRUE(reused.value("reused").toBool());
    EXPECT_EQ(idOf(reused), idOf(a));
}
#endif

TEST_F(DocumentServiceTest, OpenFailureRestoresActiveAndRemovesNewSession) {
    const auto a = service_.open(aPath_);
    ASSERT_TRUE(ok(a));
    ASSERT_TRUE(edit(idOf(a)));
    const auto ids = session_registry().ids();
    const auto failed = service_.open(temporary_->filePath(QStringLiteral("missing.bms")));
    EXPECT_FALSE(ok(failed));
    EXPECT_FALSE(failed.value("error").toString().isEmpty());
    EXPECT_TRUE(idOf(failed).isEmpty());
    EXPECT_EQ(session_registry().active_id(), idOf(a).toStdString());
    EXPECT_EQ(session_registry().ids(), ids);
    EXPECT_TRUE(session(idOf(a))->is_dirty());
    EXPECT_EQ(session(idOf(a))->undo_depth(), 1u);
    EXPECT_FALSE(ok(service_.open({})));
    EXPECT_EQ(session_registry().active_id(), idOf(a).toStdString());
}

TEST_F(DocumentServiceTest, ActivateIsExplicitAndInvalidIdNeverFallsBackToActive) {
    const auto a = service_.open(aPath_);
    ASSERT_TRUE(ok(a));
    const auto b = service_.open(bPath_);
    ASSERT_TRUE(ok(b));
    EXPECT_TRUE(ok(service_.activate(idOf(a))));
    EXPECT_EQ(session_registry().active_id(), idOf(a).toStdString());
    EXPECT_FALSE(ok(service_.activate(QStringLiteral("not-a-document"))));
    EXPECT_FALSE(ok(service_.activate({})));
    EXPECT_EQ(session_registry().active_id(), idOf(a).toStdString());
}

TEST_F(DocumentServiceTest, DirtyDocumentsIncludeInactiveAndUnnamed) {
    const auto a = service_.open(aPath_);
    ASSERT_TRUE(ok(a));
    ASSERT_TRUE(edit(idOf(a)));
    const auto b = service_.open(bPath_);
    ASSERT_TRUE(ok(b));
    const QString unnamed = newUnnamed();
    ASSERT_FALSE(unnamed.isEmpty());
    ASSERT_TRUE(edit(unnamed));
    ASSERT_TRUE(ok(service_.activate(idOf(b))));
    const auto docs = service_.dirtyDocuments();
    EXPECT_TRUE(containsId(docs, idOf(a)));
    EXPECT_TRUE(containsId(docs, unnamed));
    EXPECT_FALSE(containsId(docs, idOf(b)));
    for (const QVariant& value : docs) {
        const QVariantMap doc = value.toMap();
        EXPECT_FALSE(doc.value("label").toString().isEmpty());
        if (doc.value("id").toString() == unnamed) {
            EXPECT_TRUE(doc.value("path").toString().isEmpty());
        }
    }
}

TEST_F(DocumentServiceTest, TargetedSavePreservesActiveAndOtherDirtyDocument) {
    const auto a = service_.open(aPath_);
    ASSERT_TRUE(ok(a));
    ASSERT_TRUE(edit(idOf(a)));
    const auto b = service_.open(bPath_);
    ASSERT_TRUE(ok(b));
    ASSERT_TRUE(edit(idOf(b)));
    const auto saved = service_.save(idOf(a));
    ASSERT_TRUE(ok(saved));
    EXPECT_EQ(idOf(saved), idOf(a));
    EXPECT_EQ(saved.value("path").toString(), QFileInfo(aPath_).canonicalFilePath());
    EXPECT_EQ(session_registry().active_id(), idOf(b).toStdString());
    EXPECT_FALSE(session(idOf(a))->is_dirty());
    EXPECT_TRUE(session(idOf(b))->is_dirty());
    EXPECT_TRUE(readBytes(aPath_).contains("#00111:01"));
    EXPECT_TRUE(ok(service_.open(aPath_)));  // 保存后仍复用内存文档
}

TEST_F(DocumentServiceTest, SavedStateDistinguishesLoadedCleanDocumentFromSuccessfulSave) {
    const auto opened = service_.open(aPath_);
    ASSERT_TRUE(ok(opened));
    const QString id = idOf(opened);
    auto* doc = session(id);
    ASSERT_NE(doc, nullptr);
    EXPECT_FALSE(doc->is_dirty());
    EXPECT_FALSE(doc->saved_since_load());  // 仅打开 = 未修改，而非已保存
    const QString blocked = temporary_->filePath("cannot-save.bms");
    ASSERT_TRUE(QDir().mkpath(blocked));
    EXPECT_FALSE(ok(service_.save(id, blocked)));
    EXPECT_FALSE(doc->saved_since_load());
    ASSERT_TRUE(edit(id));
    ASSERT_TRUE(doc->undo());
    EXPECT_FALSE(doc->is_dirty());
    EXPECT_FALSE(doc->saved_since_load());  // 编辑再撤回初始内容仍是未修改
    ASSERT_TRUE(ok(service_.save(id)));
    EXPECT_TRUE(doc->saved_since_load());
    ASSERT_TRUE(ok(service_.open(aPath_)));
    EXPECT_TRUE(doc->saved_since_load());  // 切换/复用保留保存记录
    ASSERT_TRUE(edit(id));
    EXPECT_TRUE(doc->is_dirty());
    ASSERT_TRUE(doc->undo());
    EXPECT_FALSE(doc->is_dirty());
    EXPECT_TRUE(doc->saved_since_load());  // 撤回保存点 = 已保存
    doc->load(beatbench::Chart{}, aPath_.toUtf8().toStdString());
    EXPECT_FALSE(doc->saved_since_load());  // 重新加载开始新的会话记录
}

TEST_F(DocumentServiceTest, SaveFailureDoesNotMarkCleanOrChangeActiveAndPath) {
    const auto a = service_.open(aPath_);
    ASSERT_TRUE(ok(a));
    ASSERT_TRUE(edit(idOf(a)));
    const auto b = service_.open(bPath_);
    ASSERT_TRUE(ok(b));
    const QString blocked = temporary_->filePath(QStringLiteral("directory.bms"));
    ASSERT_TRUE(QDir().mkpath(blocked));
    const auto failed = service_.save(idOf(a), blocked);
    EXPECT_FALSE(ok(failed));
    EXPECT_FALSE(failed.value("error").toString().isEmpty());
    EXPECT_TRUE(session(idOf(a))->is_dirty());
    EXPECT_EQ(QString::fromStdString(session(idOf(a))->path()), a.value("path").toString());
    EXPECT_EQ(session_registry().active_id(), idOf(b).toStdString());
    EXPECT_FALSE(ok(service_.save({})));
    EXPECT_FALSE(ok(service_.save(QStringLiteral("missing-id"))));
    EXPECT_EQ(session_registry().active_id(), idOf(b).toStdString());
}

TEST_F(DocumentServiceTest, SaveAsRejectsPathOwnedByAnotherOpenDocument) {
    const auto a = service_.open(aPath_);
    const auto b = service_.open(bPath_);
    ASSERT_TRUE(ok(a));
    ASSERT_TRUE(ok(b));
    ASSERT_TRUE(edit(idOf(a)));
    const auto bytes = readBytes(bPath_);
    const auto rejected = service_.save(idOf(a), bPath_);
    EXPECT_FALSE(ok(rejected));
    EXPECT_TRUE(rejected.value("error").toString().startsWith("path_in_use:"));
    EXPECT_EQ(readBytes(bPath_), bytes);
    EXPECT_TRUE(session(idOf(a))->is_dirty());
    EXPECT_EQ(session_registry().active_id(), idOf(b).toStdString());
    EXPECT_EQ(service_.activeId(), idOf(b));
}

TEST_F(DocumentServiceTest, UnnamedRequiresSaveAsAndSaveAsDoesNotActivateIt) {
    const QString unnamed = newUnnamed();
    ASSERT_FALSE(unnamed.isEmpty());
    ASSERT_TRUE(edit(unnamed));
    const auto a = service_.open(aPath_);
    ASSERT_TRUE(ok(a));
    EXPECT_FALSE(ok(service_.save(unnamed)));
    EXPECT_TRUE(session(unnamed)->is_dirty());
    const QString target = temporary_->filePath(QStringLiteral("new-chart.bms"));
    const auto saved = service_.save(unnamed, target);
    ASSERT_TRUE(ok(saved));
    EXPECT_EQ(saved.value("path").toString(), QFileInfo(target).canonicalFilePath());
    EXPECT_FALSE(session(unnamed)->is_dirty());
    EXPECT_EQ(session_registry().active_id(), idOf(a).toStdString());
    const auto reused = service_.open(target);
    EXPECT_TRUE(ok(reused));
    EXPECT_TRUE(reused.value("reused").toBool());
    EXPECT_EQ(idOf(reused), unnamed);
}

TEST_F(DocumentServiceTest, SampleAvailabilityUsesExplicitInMemoryDocument) {
    const auto a = service_.open(aPath_);
    const auto b = service_.open(bPath_);
    ASSERT_TRUE(ok(a));
    ASSERT_TRUE(ok(b));
    auto* aSession = session(idOf(a));
    auto* bSession = session(idOf(b));
    aSession->set_backup_enabled(false);
    bSession->set_backup_enabled(false);
    QFile audio(temporary_->filePath("exists.wav"));
    ASSERT_TRUE(audio.open(QIODevice::WriteOnly));
    audio.write("resource existence only");
    audio.close();
    ASSERT_TRUE(aSession->exec(std::make_unique<beatbench::edit::SetSampleFileCommand>(
        beatbench::SampleKind::Wav, 1, "missing.wav")));
    ASSERT_TRUE(bSession->exec(std::make_unique<beatbench::edit::SetSampleFileCommand>(
        beatbench::SampleKind::Wav, 1, "exists.wav")));
    const auto missingCount = [](const QString& id) {
        using beatbench::json::Json;
        Json request = Json::object(), args = Json::object();
        request.set("command", "session.lint");
        args.set("session_id", id.toUtf8().toStdString());
        request.set("args", std::move(args));
        const Json response = beatbench::cmd::global_registry().dispatch(request);
        return response.at("result").at("lint").at("missing_wav").as_array().size();
    };
    EXPECT_EQ(missingCount(idOf(a)), 1u);
    EXPECT_EQ(missingCount(idOf(b)), 0u);
    ASSERT_TRUE(aSession->exec(std::make_unique<beatbench::edit::SetSampleFileCommand>(
        beatbench::SampleKind::Wav, 1, "exists.wav")));
    EXPECT_EQ(missingCount(idOf(a)), 0u);  // 未保存的绑定更新也立刻生效
    EXPECT_EQ(service_.activeId(), idOf(b));
}

TEST_F(DocumentServiceTest, SaveDirtyNamedSavesAllNamedWithoutChangingActive) {
    const auto a = service_.open(aPath_);
    ASSERT_TRUE(ok(a));
    ASSERT_TRUE(edit(idOf(a)));
    const auto b = service_.open(bPath_);
    ASSERT_TRUE(ok(b));
    ASSERT_TRUE(edit(idOf(b)));
    const auto saved = service_.saveDirtyNamed();
    EXPECT_TRUE(ok(saved));
    EXPECT_EQ(saved.value("saved").toInt(), 2);
    EXPECT_TRUE(saved.value("errors").toList().isEmpty());
    EXPECT_TRUE(saved.value("unnamed").toList().isEmpty());
    EXPECT_FALSE(session(idOf(a))->is_dirty());
    EXPECT_FALSE(session(idOf(b))->is_dirty());
    EXPECT_EQ(session_registry().active_id(), idOf(b).toStdString());
}

TEST_F(DocumentServiceTest, SaveDirtyNamedReportsUnresolvedUnnamedThenCompletes) {
    const auto a = service_.open(aPath_);
    ASSERT_TRUE(ok(a));
    ASSERT_TRUE(edit(idOf(a)));
    const auto b = service_.open(bPath_);
    ASSERT_TRUE(ok(b));
    ASSERT_TRUE(edit(idOf(b)));
    const QString unnamed = newUnnamed();
    ASSERT_FALSE(unnamed.isEmpty());
    ASSERT_TRUE(edit(unnamed));
    ASSERT_TRUE(ok(service_.activate(idOf(b))));
    const auto saved = service_.saveDirtyNamed();
    EXPECT_FALSE(ok(saved));
    EXPECT_EQ(saved.value("saved").toInt(), 2);
    EXPECT_TRUE(saved.value("errors").toList().isEmpty());
    EXPECT_TRUE(containsId(saved.value("unnamed").toList(), unnamed));
    EXPECT_EQ(session_registry().active_id(), idOf(b).toStdString());
    EXPECT_TRUE(session(unnamed)->is_dirty());
    ASSERT_TRUE(ok(service_.save(unnamed, temporary_->filePath(QStringLiteral("unnamed.bms")))));
    const auto complete = service_.saveDirtyNamed();
    EXPECT_TRUE(ok(complete));
    EXPECT_EQ(complete.value("saved").toInt(), 0);
    EXPECT_TRUE(complete.value("errors").toList().isEmpty());
    EXPECT_TRUE(complete.value("unnamed").toList().isEmpty());
}

TEST_F(DocumentServiceTest, SaveDirtyNamedReportsFailureAndContinuesOtherDocuments) {
    const auto a = service_.open(aPath_);
    ASSERT_TRUE(ok(a));
    ASSERT_TRUE(edit(idOf(a)));
    const auto b = service_.open(bPath_);
    ASSERT_TRUE(ok(b));
    ASSERT_TRUE(edit(idOf(b)));
    const QString blocked = temporary_->filePath(QStringLiteral("blocked.bms"));
    ASSERT_TRUE(QDir().mkpath(blocked));
    session(idOf(a))->set_path(blocked.toUtf8().toStdString());
    const auto saved = service_.saveDirtyNamed();
    EXPECT_FALSE(ok(saved));
    EXPECT_EQ(saved.value("saved").toInt(), 1);
    EXPECT_TRUE(containsId(saved.value("errors").toList(), idOf(a)));
    EXPECT_TRUE(saved.value("unnamed").toList().isEmpty());
    EXPECT_TRUE(session(idOf(a))->is_dirty());
    EXPECT_FALSE(session(idOf(b))->is_dirty());
    EXPECT_EQ(session_registry().active_id(), idOf(b).toStdString());
}
}  // namespace
