// SPDX-License-Identifier: GPL-3.0-only
// Standalone Qt Core event-loop tests; never use the author's QSettings/store.
#include <gtest/gtest.h>
#include <QCoreApplication>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QSettings>
#include <QTemporaryDir>
#include <QTimer>
#include <QUuid>
#include <algorithm>
#include <memory>
#include <string>
#include <vector>
#include "bridge/ChartHistoryController.hpp"
#include "bridge/ChartSnapshotStore.hpp"
#include "bridge/DocumentService.hpp"
#include "beatbench/core/edit/EditorSession.hpp"
#include "beatbench/core/edit/SessionRegistry.hpp"

namespace {
using beatbench::app::ChartHistoryController;
using beatbench::app::ChartSnapshotStore;
using beatbench::app::DocumentService;
using beatbench::edit::session_registry;

bool waitIdle(ChartHistoryController& controller) {
    if (!controller.busy()) return true;
    QEventLoop loop;
    QTimer timeout;
    timeout.setSingleShot(true);
    QObject::connect(&controller, &ChartHistoryController::operationFinished, &loop, &QEventLoop::quit);
    QObject::connect(&timeout, &QTimer::timeout, &loop, &QEventLoop::quit);
    timeout.start(5000);
    loop.exec();
    return !controller.busy();
}

class ChartHistoryControllerTest : public ::testing::Test {
protected:
    void SetUp() override {
        previousIds = session_registry().ids();
        previousActive = session_registry().active_id();
        oldOrganization = QCoreApplication::organizationName();
        oldApplication = QCoreApplication::applicationName();
        oldFormat = QSettings::defaultFormat();
        // Local runs may override the temporary root; never depend on private workspace paths.
        const QString base = qEnvironmentVariable("BB_TEST_TMPDIR", QDir::tempPath());
        ASSERT_TRUE(QDir().mkpath(base));
        const QString canonical = QFileInfo(base).canonicalFilePath();
        temporary = std::make_unique<QTemporaryDir>(QDir(canonical.isEmpty() ? base : canonical)
            .filePath(QStringLiteral("history-controller-XXXXXX")));
        ASSERT_TRUE(temporary->isValid());
        QCoreApplication::setOrganizationName(QStringLiteral("BeAtBench-isolated-tests"));
        QCoreApplication::setApplicationName(QUuid::createUuid().toString(QUuid::WithoutBraces));
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, temporary->filePath("settings-user"));
        QSettings::setPath(QSettings::IniFormat, QSettings::SystemScope, temporary->filePath("settings-system"));
        QSettings settings;
        settings.setFallbacksEnabled(false);
        settings.clear();
        settings.sync();
        ASSERT_EQ(settings.status(), QSettings::NoError);
        root = temporary->filePath("store");
        aPath = temporary->filePath("A.bms");
        bPath = temporary->filePath("B.bms");
        ASSERT_TRUE(writeFile(aPath, QByteArray("#TITLE A\n#BPM 120\n")));
        ASSERT_TRUE(writeFile(bPath, QByteArray("#TITLE B\n#BPM 120\n")));
        documents = std::make_unique<DocumentService>();
        // Populate core directly: opening through the GUI bridge would start audio rendering.
        aId = addDocument(aPath);
        bId = addDocument(bPath);
        ASSERT_FALSE(aId.isEmpty());
        ASSERT_FALSE(bId.isEmpty());
        ASSERT_TRUE(session_registry().activate(aId.toUtf8().toStdString()));
        controller = std::make_unique<ChartHistoryController>(documents.get(), nullptr, root);
    }
    void TearDown() override {
        controller.reset(); // joins worker before removing temp/settings/session resources
        QCoreApplication::sendPostedEvents();
        documents.reset();
        auto& registry = session_registry();
        for (const auto& id : registry.ids())
            if (std::find(previousIds.begin(), previousIds.end(), id) == previousIds.end()) registry.close(id);
        registry.activate(previousActive);
        QSettings::setDefaultFormat(oldFormat);
        QCoreApplication::setOrganizationName(oldOrganization);
        QCoreApplication::setApplicationName(oldApplication);
        temporary.reset();
    }
    QString addDocument(const QString& path) {
        const QString id = QUuid::createUuid().toString(QUuid::WithoutBraces);
        if (!session_registry().create(id.toUtf8().toStdString())) return {};
        auto* session = session_registry().by_id(id.toUtf8().toStdString());
        session->load(beatbench::Chart{}, path.toUtf8().toStdString());
        session->set_backup_enabled(false);
        return id;
    }
    static bool writeFile(const QString& path, const QByteArray& bytes) {
        QFile file(path);
        return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size();
    }
    static QByteArray bytes(const QString& path) {
        QFile file(path);
        return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray{};
    }
    void enableAndSelect() {
        controller->setEnabled(true);
        controller->showHistory(aId);
        ASSERT_TRUE(waitIdle(*controller));
    }
    std::unique_ptr<QTemporaryDir> temporary;
    std::unique_ptr<DocumentService> documents;
    std::unique_ptr<ChartHistoryController> controller;
    QString root, aPath, bPath, aId, bId, oldOrganization, oldApplication;
    QSettings::Format oldFormat = QSettings::NativeFormat;
    std::vector<std::string> previousIds;
    std::string previousActive;
};

TEST_F(ChartHistoryControllerTest, DisabledByDefaultDoesNotCreateStore) {
    EXPECT_FALSE(controller->enabled());
    EXPECT_FALSE(controller->createOnClose());
    controller->showHistory(aId);
    controller->createSnapshot();
    controller->requestSavedSnapshot();
    EXPECT_TRUE(controller->snapshotAllSaved());
    EXPECT_FALSE(controller->busy());
    EXPECT_FALSE(QFileInfo::exists(root));
}

TEST_F(ChartHistoryControllerTest, SaveCancellationOnlyConsumesOriginalDocument) {
    enableAndSelect();
    int completed = 0;
    QString requested;
    QObject::connect(controller.get(), &ChartHistoryController::saveRequested,
                     [&](const QString& id) { requested = id; });
    QObject::connect(controller.get(), &ChartHistoryController::operationFinished,
                     [&](bool ok) { ++completed; EXPECT_FALSE(ok); });
    controller->requestSavedSnapshot("pending");
    EXPECT_EQ(requested, aId);
    EXPECT_TRUE(controller->busy());
    controller->finishSave(bId, true);
    EXPECT_TRUE(controller->busy());
    EXPECT_EQ(completed, 0);
    controller->finishSave(aId, false);
    EXPECT_FALSE(controller->busy());
    EXPECT_EQ(completed, 1);
    controller->finishSave(aId, true);
    EXPECT_EQ(completed, 1);
    EXPECT_TRUE(controller->entries().isEmpty());
}

TEST_F(ChartHistoryControllerTest, SaveAsPathChangeRejectsPendingSnapshot) {
    enableAndSelect();
    controller->requestSavedSnapshot();
    auto* session = session_registry().by_id(aId.toUtf8().toStdString());
    session->set_path(temporary->filePath("renamed.bms").toUtf8().toStdString());
    controller->finishSave(aId, true);
    EXPECT_FALSE(controller->busy());
    EXPECT_TRUE(controller->entries().isEmpty());
    EXPECT_TRUE(ChartSnapshotStore(root).history(aPath).value("entries").toList().isEmpty());
}

TEST_F(ChartHistoryControllerTest, RemovedDocumentRejectsPendingSnapshot) {
    enableAndSelect();
    controller->requestSavedSnapshot();
    session_registry().close(aId.toUtf8().toStdString());
    controller->finishSave(aId, true);
    EXPECT_FALSE(controller->busy());
    EXPECT_TRUE(controller->entries().isEmpty());
}

TEST_F(ChartHistoryControllerTest, ActiveTabChangeDoesNotRetargetSnapshotOrRestore) {
    enableAndSelect();
    const QByteArray beforeA = bytes(aPath), beforeB = bytes(bPath);
    auto* a = session_registry().by_id(aId.toUtf8().toStdString());
    const auto revision = a->revision();
    const auto undoDepth = a->undo_depth();
    controller->requestSavedSnapshot();
    ASSERT_TRUE(session_registry().activate(bId.toUtf8().toStdString()));
    controller->finishSave(aId, true);
    ASSERT_TRUE(waitIdle(*controller));
    ASSERT_EQ(controller->entries().size(), 1);
    const QString snapshotId = controller->entries().first().toMap().value("id").toString();
    ASSERT_FALSE(snapshotId.isEmpty());
    controller->restoreCopy(snapshotId);
    ASSERT_TRUE(waitIdle(*controller));
    EXPECT_EQ(bytes(aPath), beforeA);
    EXPECT_EQ(bytes(bPath), beforeB);
    EXPECT_EQ(a->path(), aPath.toUtf8().toStdString());
    EXPECT_EQ(a->revision(), revision);
    EXPECT_EQ(a->undo_depth(), undoDepth);
    EXPECT_FALSE(a->is_dirty());
    EXPECT_EQ(session_registry().active_id(), bId.toUtf8().toStdString());
    EXPECT_EQ(documents->instances().size(), 2);
    const auto copies = QDir(temporary->path()).entryList({"*.bms"}, QDir::Files);
    ASSERT_EQ(copies.size(), 3);
    for (const auto& name : copies)
        if (name != "A.bms" && name != "B.bms") EXPECT_EQ(bytes(temporary->filePath(name)), beforeA);
}

TEST_F(ChartHistoryControllerTest, PendingSaveRejectsRetargetAndExit) {
    enableAndSelect();
    controller->setCreateOnClose(true);
    controller->requestSavedSnapshot();
    controller->showHistory(bId);
    EXPECT_EQ(controller->sourcePath(), DocumentService::normalizedPath(aPath));
    EXPECT_FALSE(controller->snapshotAllSaved());
    controller->finishSave(aId, false);
    EXPECT_FALSE(controller->busy());
}

TEST_F(ChartHistoryControllerTest, ExitSnapshotFailureDoesNotChangeSessionCleanState) {
    enableAndSelect();
    controller->setCreateOnClose(true);
    ASSERT_TRUE(QFile::remove(aPath));
    EXPECT_FALSE(controller->snapshotAllSaved());
    EXPECT_FALSE(controller->busy());
    EXPECT_FALSE(session_registry().by_id(aId.toUtf8().toStdString())->is_dirty());
}

TEST_F(ChartHistoryControllerTest, ExitSnapshotsSavedDocumentsDeduplicateAndSkipDirtyOrUnnamed) {
    enableAndSelect();
    controller->setCreateOnClose(true);
    auto* b = session_registry().by_id(bId.toUtf8().toStdString());
    ASSERT_TRUE(b->exec(std::make_unique<beatbench::edit::MetaEditCommand>("TITLE", "dirty")));
    const QString unnamed = addDocument({});
    ASSERT_FALSE(unnamed.isEmpty());
    ASSERT_TRUE(controller->snapshotAllSaved());
    ASSERT_TRUE(controller->snapshotAllSaved());
    EXPECT_EQ(ChartSnapshotStore(root).history(aPath).value("entries").toList().size(), 1);
    EXPECT_TRUE(ChartSnapshotStore(root).history(bPath).value("entries").toList().isEmpty());
    EXPECT_TRUE(b->is_dirty());
    EXPECT_EQ(bytes(bPath), QByteArray("#TITLE B\n#BPM 120\n"));
}

TEST_F(ChartHistoryControllerTest, ExitPartialFailureRetriesWithoutDuplicatesAndCanBeDisabled) {
    enableAndSelect();
    controller->setCreateOnClose(true);
    ASSERT_TRUE(QFile::remove(bPath));
    EXPECT_FALSE(controller->snapshotAllSaved());
    EXPECT_EQ(ChartSnapshotStore(root).history(aPath).value("entries").toList().size(), 1);
    ASSERT_TRUE(writeFile(bPath, QByteArray("#TITLE repaired\n")));
    EXPECT_TRUE(controller->snapshotAllSaved());
    EXPECT_EQ(ChartSnapshotStore(root).history(aPath).value("entries").toList().size(), 1);
    EXPECT_EQ(ChartSnapshotStore(root).history(bPath).value("entries").toList().size(), 1);
    ASSERT_TRUE(QFile::remove(bPath));
    controller->setCreateOnClose(false);
    EXPECT_TRUE(controller->snapshotAllSaved());
}

TEST_F(ChartHistoryControllerTest, DestructionJoinsActiveWorkerAndDropsQueuedDelivery) {
    enableAndSelect();
    controller->createSnapshot();
    ASSERT_TRUE(controller->busy());
    controller.reset();
    QCoreApplication::sendPostedEvents();
    const auto history = ChartSnapshotStore(root).history(aPath);
    ASSERT_TRUE(history.value("ok").toBool());
    EXPECT_EQ(history.value("entries").toList().size(), 1);
    EXPECT_EQ(bytes(aPath), QByteArray("#TITLE A\n#BPM 120\n"));
}
} // namespace

int main(int argc, char** argv) {
    QCoreApplication application(argc, argv);
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
